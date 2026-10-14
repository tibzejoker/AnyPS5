#include "ImageTableRegistry.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

namespace Registry = ShaderRecompiler::ImageTableRegistry;

void require(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        std::exit(1);
    }
}

std::array<std::uint32_t, 8> imageWords(std::uint32_t seed) {
    return {0x40000000u + seed * 0x10000u, 0x0812c000u + seed, 0x00ff00ffu, 0x90000fadu ^ seed, 0u, 0u, 0x00010000u, 0u};
}

std::vector<std::uint32_t> freshMap() {
    std::vector<std::uint32_t> map(Registry::MapWords, 0u);
    Registry::InitMap(map);
    return map;
}

void registeredImageHits() {
    auto map = freshMap();
    std::vector<std::array<std::uint32_t, 8>> images;
    for (std::uint32_t seed = 0; seed < 2000u; seed++) images.push_back(imageWords(seed));
    for (std::uint32_t index = 0; index < images.size(); index++) {
        require(Registry::InsertSlot(map, Registry::ImageSlotsStart, Registry::ImageSlotCapacity, images[index].data(), 8u, index + 1u), "image " + std::to_string(index) + " gets a slot");
    }
    for (std::uint32_t index = 0; index < images.size(); index++) {
        const auto result = Registry::ProbeMirror(map, images[index].data(), 8u, false, true);
        require(result.found && result.code == index + 1u, "registered image " + std::to_string(index) + " is found with its entry code");
        require(!result.reported, "a hit appends nothing to the feedback ring");
    }
    require(map[Registry::ImageRingStart] == 0u, "no feedback after hits only");
    const auto absent = imageWords(50000u);
    const auto miss = Registry::ProbeMirror(map, absent.data(), 8u, false, true);
    require(!miss.found && miss.code == 0u && miss.reported, "an unregistered image misses with code 0 and is fed back");
    require(map[Registry::ImageRingStart] == 1u && map[Registry::ImageRingStart + 1u] == absent[0] && map[Registry::ImageRingStart + 8u] == absent[7], "the miss is in the image ring");
    const auto inactive = Registry::ProbeMirror(map, imageWords(50001u).data(), 8u, false, false);
    require(!inactive.reported && map[Registry::ImageRingStart] == 1u, "an inactive lane's miss is not fed back");
}

void samplersUseTheirOwnRegion() {
    auto map = freshMap();
    const std::array<std::uint32_t, 4> sampler{0x1u, 0x2u, 0x3u, 0x4u};
    require(Registry::InsertSlot(map, Registry::SamplerSlotsStart, Registry::SamplerSlotCapacity, sampler.data(), 4u, 2u), "the sampler gets a slot");
    const auto hit = Registry::ProbeMirror(map, sampler.data(), 4u, true, true);
    require(hit.found && hit.code == 2u, "a registered S# is found");
    const auto asImage = Registry::ProbeMirror(map, sampler.data(), 4u, false, false);
    require(!asImage.found, "the image probe does not see sampler slots");
    const std::array<std::uint32_t, 4> other{0x5u, 0x6u, 0x7u, 0x8u};
    const auto miss = Registry::ProbeMirror(map, other.data(), 4u, true, true);
    require(!miss.found && miss.code == 0u && miss.reported && map[Registry::SamplerRingStart] == 1u, "an unregistered S# reads entry 0 (the default sampler) and is fed back");
}

void directoryKeepsRegions() {
    auto map = freshMap();
    for (std::uint64_t key = 1; key <= 1500u; key++) require(Registry::DirectoryInsert(map, key * 0x9e3779b97f4a7c15ull, Registry::RegionsStart + static_cast<std::uint32_t>(key)), "mode set " + std::to_string(key) + " gets a directory slot");
    for (std::uint64_t key = 1; key <= 1500u; key++) require(Registry::DirectoryFind(map, key * 0x9e3779b97f4a7c15ull) == Registry::RegionsStart + key, "mode set " + std::to_string(key) + " finds its region");
    require(Registry::DirectoryFind(map, 42u) == 0u, "an unknown mode set has no region");
}

void modeSetKeyIgnoresProgramPlacement() {
    ShaderRecompiler::ImageResource image;
    image.resourceClass = ShaderRecompiler::ImageResourceClass::Sampled;
    image.numericClass = ShaderRecompiler::IrTextureNumericClass::Float;
    image.dimension = ShaderRecompiler::RdnaImageDimension::Dim2D;
    std::vector<ShaderRecompiler::ImageResource> modes{image};
    auto moved = image;
    moved.source = 7u;
    moved.firstUsePc = 0x400u;
    moved.table = 3u;
    require(Registry::ModeSetKey(image, modes) == Registry::ModeSetKey(moved, modes), "programs with the same image and modes share a region");
    auto other = image;
    other.dimension = ShaderRecompiler::RdnaImageDimension::Dim2DArray;
    require(Registry::ModeSetKey(image, modes) != Registry::ModeSetKey(other, modes), "a different image gets another region");
}

void badMarkerStaysInBounds() {
    std::vector<std::uint32_t> map(64u, 0u);
    map[Registry::HeaderImageSlots] = 0xfffff000u;
    map[Registry::HeaderImageCapacity] = 1024u;
    map[Registry::HeaderImageRing] = 0xffffff00u;
    const auto words = imageWords(2u);
    const auto result = Registry::ProbeMirror(map, words.data(), 8u, false, true);
    require(!result.found && !result.reported, "a map without the marker neither matches nor writes the ring");
}

}

int main() {
    registeredImageHits();
    samplersUseTheirOwnRegion();
    directoryKeepsRegions();
    modeSetKeyIgnoresProgramPlacement();
    badMarkerStaysInBounds();
    std::printf("Image registry map, directory and probe mirror passed\n");
    return 0;
}
