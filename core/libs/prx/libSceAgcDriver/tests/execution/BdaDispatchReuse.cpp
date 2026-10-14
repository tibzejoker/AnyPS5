#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>

namespace {

using AgcDriver::Graphics::Require;
using namespace ShaderRecompiler;

struct alignas(4096) GuestData {
    std::array<std::array<std::uint32_t, 8>, 4> heap{};
    std::array<std::uint32_t, 12> material{};
    std::array<std::uint32_t, 16> srt{};
    std::array<std::uint32_t, 64> output{};
};

// One page-aligned allocation of whole pages: imported in place, so the address space has no copies.
struct alignas(4096) GuestMemoryBlock {
    std::array<GuestData, 8> guest;
    alignas(256) std::array<std::array<std::uint32_t, 64>, 2> textures{};
};
GuestMemoryBlock block;
auto& guest = block.guest;
auto& textures = block.textures;
// The BindlessImages kernel: the T# comes from a heap selected by a key read through a V#.
constexpr std::array<std::uint32_t, 23> code{0xbf068002u, 0xbf850013u, 0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080300u, 0xfa000020u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x93109010u, 0xf4200406u, 0x20000004u, 0x8f108510u, 0xf42c0502u, 0x20000000u, 0xf09c0f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u, 0xbf800000u};

std::array<std::uint32_t, 4> Buffer(const void* pointer, std::uint32_t stride, std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (stride << 16u), records, stride == 0u ? 0x31016facu : 0x01016facu};
}

// A dispatch reading its V#s from guest memory through BDA: the identical later dispatches bind the
// first one's resources (resource cache hit, the build is reusable under the cached address space)
// and still see guest memory changes in place.
void Run(AgcDriver::VulkanDevice& device) {
    textures[0].fill(0x40000000u);
    textures[1].fill(0x40400000u);
    GuestAllocations::Mutation().Add(&block, sizeof(block), true, true, true);
    const auto threads = std::min(32u, device.Target().subgroupSize);
    auto& data = guest[0];
    const auto table = Buffer(data.heap.data(), 32u, 4u);
    const auto material = Buffer(data.material.data(), 16u, 3u);
    std::copy(table.begin(), table.end(), data.srt.begin());
    std::copy(material.begin(), material.end(), data.srt.begin() + 8u);
    const auto srtAddress = reinterpret_cast<std::uintptr_t>(data.srt.data());
    const std::array<std::uint32_t, 3> userData{static_cast<std::uint32_t>(srtAddress), static_cast<std::uint32_t>(srtAddress >> 32u), 1u};
    const std::array regions{MemoryRegion{reinterpret_cast<std::uintptr_t>(&data), std::as_bytes(std::span(&data, 1u))}};
    for (std::uint32_t texture = 0u; texture < textures.size(); ++texture) {
        const auto address = reinterpret_cast<std::uintptr_t>(textures[texture].data());
        data.heap[texture] = {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u) | ((texture == 0u ? 20u : 22u) << 20u) | (3u << 30u), 7u, 0x90000facu, 0u, 0u, 0u, 0u};
    }
    data.material[1] = 1u;
    data.material[5] = 0u;
    data.material[9] = 1u;
    const auto output = Buffer(data.output.data(), 0u, 256u);
    std::copy(output.begin(), output.end(), data.srt.begin() + 12u);
    const ShaderComputeStageInfo compute{{threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}}, {32u, 0u, userData, compute, std::nullopt, std::nullopt, regions}, device.Target(), {0u, 0u, 0u, 128u}};
    const auto shader = Recompile(request);
    std::uint64_t hits = 0;
    for (std::uint32_t round = 0; round < 6u; ++round) {
        data.output.fill(0xdeadbeefu);
        if (round == 3u) data.material[1] = 0u;
        const auto before = AgcDriver::Graphics::ResourceTemplateHits();
        device.Dispatch(shader, 1u, 1u, 1u);
        device.SubmitRecorded(false);
        device.WaitIdle();
        const auto hit = AgcDriver::Graphics::ResourceTemplateHits() - before;
        hits += round >= 2u ? hit : 0u;
        std::cout << "round " << round << " output 0x" << std::hex << data.output[0] << std::dec << " resource cache hits " << hit << '\n';
    }
    Require(hits >= 1u, "identical BDA dispatches never reused their resources");
    Require(data.output[0] == textures[0][0], "a reused BDA dispatch did not see the key changed in guest memory");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::cout << "BDA dispatch resource reuse tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
