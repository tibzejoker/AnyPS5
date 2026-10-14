#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include <windows.h>
#include <new>
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <cstdio>
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Extent = 64u;
// Guest memory: the vertex buffer, the V# table and a tiled (resident) color target.
struct GuestData {
    alignas(256) std::array<std::byte, 256> vertices{};
    alignas(256) std::array<std::uint32_t, 4> descriptor{};
    alignas(4096) std::array<std::byte, 65536> target{};
};
// In a 64 KiB aligned allocation of its own (the tiled target's base alignment).
GuestData& guest = *new (VirtualAlloc(nullptr, sizeof(GuestData), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) GuestData{};
constexpr std::array<std::array<float, 4>, 3> positions{{{-1.0f, -1.0f, 0.0f, 1.0f}, {3.0f, -1.0f, 0.0f, 1.0f}, {-1.0f, 3.0f, 0.0f, 1.0f}}};
constexpr std::array<std::uint32_t, 11> vertexCode{0xf4080100u, 0xfa000000u, 0x4a0a0a02u, 0x4a0a0b08u, 0x4a0a0a03u, 0xe00c2000u, 0x80010005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
constexpr std::array<std::uint32_t, 5> pixelCode{0x7e0002f2u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};

std::array<std::uint32_t, 4> BufferDescriptor(const void* pointer, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, bytes, 0x31016facu};
}

// Two identical recorded draws whose vertex shader reads its vertex V# from guest memory (BDA): the
// second binds the first one's resources (resource cache template hit) instead of rebuilding them.
void Run(AgcDriver::VulkanDevice& device) {
    GuestAllocations::Mutation().Add(&guest, sizeof(guest), true, true, true);
    constexpr std::uint32_t stride = 16u;
    constexpr std::uint32_t firstRecord = 2u;
    for (std::size_t index = 0; index < positions.size(); ++index) std::memcpy(guest.vertices.data() + (firstRecord + index) * stride, positions[index].data(), 16u);
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(guest.vertices.data());
    guest.descriptor = {static_cast<std::uint32_t>(vertexAddress), static_cast<std::uint32_t>(vertexAddress >> 32u) | (stride << 16u), 8u, 0x01000facu | (77u << 12u)};
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(guest.descriptor.data());
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(tableAddress), static_cast<std::uint32_t>(tableAddress >> 32u), 0u, 0u};
    ShaderRecompiler::ShaderVertexStageInfo vertexInfo{};
    vertexInfo.fetchEmbedded = true;
    vertexInfo.resourcesNum = 1u;
    vertexInfo.resourcesDst[0] = {0, 4, 0, 0};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(vertexCode.data()), vertexCode, 0, {}}, {32u, 0u, userData, std::nullopt, std::nullopt, vertexInfo, {}}, device.Target(), {0u, 0u, 0u, 96u}};
    const auto vertex = ShaderRecompiler::Recompile(request);
    const auto pushBytes = static_cast<std::uint32_t>(vertex.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixelInfo{};
    pixelInfo.wave32 = true;
    pixelInfo.targetOutputMode[0] = 9u;
    pixelInfo.targetExportMapping.fill(0xe4u);
    ShaderRecompiler::RecompileRequest pixelRequest{{ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(pixelCode.data()), pixelCode, 0, {}}, {32u, 0u, {}, std::nullopt, pixelInfo, std::nullopt, {}}, device.Target(), PixelPushLayout(pushBytes, device.Target())};
    const auto pixel = ShaderRecompiler::Recompile(pixelRequest);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertex, 0u}, {ShaderStage::Fragment, &pixel, PixelPushOffset(pushBytes, device.Target())}}};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(guest.target.data()), {Extent, Extent}, VK_FORMAT_R8G8B8A8_UNORM, 0, 0xe4u};
    state.color.tileMode = AgcDriver::Graphics::ColorTileMode::RenderTarget;
    state.color.bytes = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::SurfaceForTarget(state.color)).guestBytes;
    Require(state.color.bytes <= guest.target.size(), "the color target does not fit its guest block");
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Extent, Extent};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0.0f, static_cast<float>(Extent), static_cast<float>(Extent), -static_cast<float>(Extent), 0.0f, 1.0f};
    state.scissor = {{0, 0}, {Extent, Extent}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15u;
    state.blends = {state.blend};
    AgcDriver::Pm4::DrawParameters draw{0u, 3u, 0u, 1u, 0u, false};
    draw.firstVertex = firstRecord;
    std::uint64_t hits = 0;
    for (std::uint32_t round = 0; round < 3u; ++round) {
        const auto before = AgcDriver::Graphics::ResourceTemplateHits();
        device.Draw(state, draw, shaders);
        device.SubmitRecorded(false);
        device.WaitIdle();
        const auto hit = AgcDriver::Graphics::ResourceTemplateHits() - before;
        std::printf("draw %u: resource cache hits %llu\n", round, static_cast<unsigned long long>(hit));
        if (round != 0u) hits += hit;
    }
    Require(hits == 2u, "identical recorded BDA draws rebuilt their resources instead of reusing the first draw's");

}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::cout << "BDA draw resource reuse tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
