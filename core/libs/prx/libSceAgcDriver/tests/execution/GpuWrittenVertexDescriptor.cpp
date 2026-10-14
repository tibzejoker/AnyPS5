#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
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
alignas(256) std::array<std::byte, Extent * Extent * 4u> pixels{};
// Guest memory: the vertex buffer and the V# table a compute dispatch writes before the draw.
struct alignas(4096) GuestData {
    alignas(256) std::array<std::byte, 256> vertices{};
    alignas(256) std::array<std::uint32_t, 4> descriptor{};
};
GuestData guest;
// v_mov_b32 v0..v3, s4..s7; buffer_store_dwordx4 v[0:3], off, s[0:3]; s_endpgm
constexpr std::array<std::uint32_t, 7> writerCode{0x7e000204u, 0x7e020205u, 0x7e040206u, 0x7e060207u, 0xe0780000u, 0x80000000u, 0xbf810000u};
constexpr std::array<std::array<float, 4>, 3> positions{{{-1.0f, -1.0f, 0.0f, 1.0f}, {3.0f, -1.0f, 0.0f, 1.0f}, {-1.0f, 3.0f, 0.0f, 1.0f}}};
constexpr std::array<std::uint32_t, 11> vertexCode{0xf4080100u, 0xfa000000u, 0x4a0a0a02u, 0x4a0a0b08u, 0x4a0a0a03u, 0xe00c2000u, 0x80010005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
constexpr std::array<std::uint32_t, 5> pixelCode{0x7e0002f2u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};

std::array<std::uint32_t, 4> BufferDescriptor(const void* pointer, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, bytes, 0x31016facu};
}

// The vertex shader reads its vertex V# from a table that a compute dispatch recorded just before
// the draw writes: the draw resolves the V# and fetches the vertices on the GPU (BDA), the CPU
// neither reads the table nor waits for the dispatch.
void Run(AgcDriver::VulkanDevice& device) {
    GuestAllocations::Mutation().Add(&guest, sizeof(guest), true, true, true);
    constexpr std::uint32_t stride = 16u;
    constexpr std::uint32_t firstRecord = 2u;
    for (std::size_t index = 0; index < positions.size(); ++index) std::memcpy(guest.vertices.data() + (firstRecord + index) * stride, positions[index].data(), 16u);
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(guest.vertices.data());
    const std::array<std::uint32_t, 4> vertexDescriptor{static_cast<std::uint32_t>(vertexAddress), static_cast<std::uint32_t>(vertexAddress >> 32u) | (stride << 16u), 8u, 0x01000facu | (77u << 12u)};
    guest.descriptor = {0u, 0u, 0u, 0u};

    const auto table = BufferDescriptor(guest.descriptor.data(), sizeof(guest.descriptor));
    const std::array<std::uint32_t, 8> writerUserData{table[0], table[1], table[2], table[3], vertexDescriptor[0], vertexDescriptor[1], vertexDescriptor[2], vertexDescriptor[3]};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{1u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest writerRequest{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(writerCode.data()), writerCode, 0u, {}}, {32u, 0u, writerUserData, compute, std::nullopt, std::nullopt, {}}, device.Target(), {0u, 0u, 0u, 128u}};
    const auto writer = ShaderRecompiler::Recompile(writerRequest);

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
    state.color = {reinterpret_cast<std::uintptr_t>(pixels.data()), {Extent, Extent}, VK_FORMAT_R8G8B8A8_UNORM, pixels.size(), 0xe4u};
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
    for (std::uint32_t round = 0; round < 2u; ++round) {
        guest.descriptor = {0u, 0u, 0u, 0u};
        pixels.fill(std::byte{0});
        device.Dispatch(writer, 1u, 1u, 1u);
        const auto before = AgcDriver::Graphics::DrawCpuTotals();
        device.Draw(state, draw, shaders);
        const auto after = AgcDriver::Graphics::DrawCpuTotals();
        device.WaitIdle();
        std::printf("round %u: draw made %llu CPU guest reads and %llu GPU syncs\n", round, static_cast<unsigned long long>(after.reads - before.reads), static_cast<unsigned long long>(after.syncs - before.syncs));
        Require(guest.descriptor == vertexDescriptor, "the compute dispatch did not write the vertex V#");
        for (std::size_t index = 0; index < pixels.size(); index += 4u) {
            Require(pixels[index] == std::byte{255} && pixels[index + 1u] == std::byte{0} && pixels[index + 2u] == std::byte{0} && pixels[index + 3u] == std::byte{255}, "a draw reading its vertex V# from a buffer the previous dispatch wrote produced the wrong triangle at byte " + std::to_string(index));
        }
        Require(after.syncs == before.syncs, "the draw waited for the GPU to read its descriptors");
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::cout << "GPU-written vertex descriptor tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
