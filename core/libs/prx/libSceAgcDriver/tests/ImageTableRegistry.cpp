#include "ImageTableRegistry.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

namespace Registry = ShaderRecompiler::ImageTableRegistry;
namespace Abi = ShaderRecompiler::ImageTableAbi;

void require(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        std::exit(1);
    }
}

std::array<std::uint32_t, 8> imageWords(std::uint32_t seed) {
    return {0x40000000u + seed * 0x10000u, 0x0812c000u + seed, 0x00ff00ffu, 0x90000fadu ^ seed, 0u, 0u, 0x00010000u, 0u};
}

void registeredImageHits() {
    std::vector<std::uint32_t> map(Abi::HeaderWords + Abi::TableWords * 2u, 0u);
    std::vector<std::array<std::uint32_t, 8>> images;
    for (std::uint32_t seed = 0; seed < 200u; seed++) images.push_back(imageWords(seed));
    std::vector<Registry::RegistryEntry> entries;
    for (std::uint32_t index = 0; index < images.size(); index++) entries.push_back({images[index].data(), 8u, 100u + index * 3u});
    const auto header = Abi::TableHeader(1u);
    const auto inserted = Registry::BuildRegistryRegion(map, header, entries, false);
    require(inserted == images.size(), "every registered image got a slot (" + std::to_string(inserted) + " of " + std::to_string(images.size()) + ")");
    for (std::uint32_t index = 0; index < images.size(); index++) {
        const auto result = Registry::ProbeMirror(map, header, images[index].data(), 8u, false, true);
        require(result.found && result.code == 100u + index * 3u, "registered image " + std::to_string(index) + " is found with its record code");
        require(!result.reported, "a hit appends nothing to the feedback ring");
    }
    const auto ringStart = map[header + Abi::TableSizeLow];
    require(map[ringStart] == 0u, "no feedback after hits only");
    const auto absent = imageWords(5000u);
    const auto miss = Registry::ProbeMirror(map, header, absent.data(), 8u, false, true);
    require(!miss.found && miss.code == Abi::NullCode && miss.reported, "an unregistered image misses, reads the null code and is fed back");
    require(map[ringStart] == 1u && map[ringStart + 1u] == absent[0] && map[ringStart + 8u] == absent[7], "the miss is in the ring");
}

void faultedTableNeverHits() {
    std::vector<std::uint32_t> map(Abi::HeaderWords + Abi::TableWords, 0u);
    const auto words = imageWords(1u);
    const auto header = Abi::TableHeader(0u);
    map[header + Abi::TableFault] = Abi::PoisonCode(9u);
    Registry::BuildRegistryRegion(map, header, {{words.data(), 8u, 42u}}, false);
    const auto result = Registry::ProbeMirror(map, header, words.data(), 8u, false, true);
    require(result.found && result.code == Abi::PoisonCode(9u), "a table fault overrides the hit (what made GTA's storage images read zeros)");
}

void badHeaderStaysInBounds() {
    std::vector<std::uint32_t> map(Abi::HeaderWords + Abi::TableWords, 0u);
    const auto header = Abi::TableHeader(0u);
    map[header + Abi::TableMapStart] = 0xfffff000u;
    map[header + Abi::TableKeyCount] = 1024u;
    map[header + Abi::TableSizeLow] = 0xffffff00u;
    map[header + Abi::TableSizeHigh] = 256u;
    const auto words = imageWords(2u);
    const auto result = Registry::ProbeMirror(map, header, words.data(), 8u, false, true);
    require(!result.found && !result.reported, "a header outside the map neither matches nor writes the ring");
}

void samplerHits() {
    std::vector<std::uint32_t> map(Abi::HeaderWords + Abi::TableWords, 0u);
    const std::array<std::uint32_t, 4> sampler{0x1u, 0x2u, 0x3u, 0x4u};
    const auto header = Abi::TableHeader(0u);
    map[header + Abi::TableOutsideCode] = 77u;
    Registry::BuildRegistryRegion(map, header, {{sampler.data(), 4u, 9u}}, true);
    const auto hit = Registry::ProbeMirror(map, header, sampler.data(), 4u, true, true);
    require(hit.found && hit.code == 9u, "a registered S# is found");
    const std::array<std::uint32_t, 4> other{0x5u, 0x6u, 0x7u, 0x8u};
    const auto miss = Registry::ProbeMirror(map, header, other.data(), 4u, true, true);
    require(!miss.found && miss.code == 77u, "an unregistered S# reads the default sampler record");
}

}

int main() {
    registeredImageHits();
    faultedTableNeverHits();
    badHeaderStaysInBounds();
    samplerHits();
    std::printf("Image table registry map and probe mirror passed\n");
    return 0;
}
