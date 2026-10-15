#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <windows.h>
#include <new>
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DeviceDiagnostics.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 256u;
constexpr std::uint32_t Height = 64u;
constexpr std::uint32_t Loads = 4u;
constexpr std::uint32_t Rounds = 3u;
constexpr std::uint32_t DrawsPerRound = 2u;

struct GuestData {
    alignas(256) std::array<std::byte, 256> vertices{};
    alignas(256) std::array<std::uint32_t, 4> descriptor{};
    alignas(256) std::array<std::uint32_t, Loads> words{};
    alignas(65536) std::array<std::byte, 1u << 20> target{};
};
GuestData& guest = *new (VirtualAlloc(nullptr, sizeof(GuestData), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) GuestData{};
constexpr std::array<std::array<float, 4>, 3> positions{{{-1.0f, -1.0f, 0.0f, 1.0f}, {3.0f, -1.0f, 0.0f, 1.0f}, {-1.0f, 3.0f, 0.0f, 1.0f}}};
constexpr std::array<std::uint32_t, 11> vertexCode{0xf4080100u, 0xfa000000u, 0x4a0a0a02u, 0x4a0a0b08u, 0x4a0a0a03u, 0xe00c2000u, 0x80010005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};

// s_mov_b32 s8, 0; Loads x (s_load_dword s4, s[0:1], 4k; s_waitcnt lgkmcnt(0); s_add_u32 s8, s8, s4);
// v_mov_b32 v0, s8; v_mov_b32 v1, 0; exp mrt0 v0, v1, v1, v0 done vm; s_endpgm. The words sum to
// 1.0f, so every pixel is red: the pixel shader reads its words from guest memory through the
// user-data pointer at s[0:1] (SGPRs: one value per wave), as GTA's pixel shaders read their T#s.
std::vector<std::uint32_t> PixelCode() {
    std::vector<std::uint32_t> code{0xbe880380u};
    for (std::uint32_t index = 0; index < Loads; ++index) {
        code.push_back(0xf4000100u);
        code.push_back(0xfa000000u | (index * 4u));
        code.push_back(0xbf8cc07fu);
        code.push_back(0x80080408u);
    }
    for (const auto word : {0x7e000208u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u}) code.push_back(word);
    return code;
}

std::array<std::uint32_t, 4> BufferDescriptor(const std::array<std::byte, 256>& vertices, std::uint32_t stride) {
    const auto address = reinterpret_cast<std::uintptr_t>(vertices.data());
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (stride << 16u), 8u, 0x01000facu | (77u << 12u)};
}

// APS5_GPU_CHECKPOINTS: recorded draws set checkpoints, and the device-lost report (run here on a
// healthy device: no fault, the last checkpoints the queue reached) names them.
void Run(AgcDriver::VulkanDevice& device) {
    GuestAllocations::Mutation().Add(&guest, sizeof(guest), true, true, true);
    constexpr std::uint32_t stride = 16u;
    constexpr std::uint32_t firstRecord = 2u;
    for (std::size_t index = 0; index < positions.size(); ++index) std::memcpy(guest.vertices.data() + (firstRecord + index) * stride, positions[index].data(), 16u);
    guest.descriptor = BufferDescriptor(guest.vertices, stride);
    guest.words[0] = 0x3f800000u;
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(guest.descriptor.data());
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(tableAddress), static_cast<std::uint32_t>(tableAddress >> 32u), 0u, 0u};
    ShaderRecompiler::ShaderVertexStageInfo vertexInfo{};
    vertexInfo.fetchEmbedded = true;
    vertexInfo.resourcesNum = 1u;
    vertexInfo.resourcesDst[0] = {0, 4, 0, 0};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(vertexCode.data()), vertexCode, 0, {}}, {32u, 0u, userData, std::nullopt, std::nullopt, vertexInfo, {}}, device.Target(), {0u, 0u, 0u, 0u}};
    const auto vertex = ShaderRecompiler::Recompile(request);
    const auto pushBytes = static_cast<std::uint32_t>(vertex.pushConstants.size());
    const auto pixelCode = PixelCode();
    const auto wordsAddress = reinterpret_cast<std::uintptr_t>(guest.words.data());
    const std::array<std::uint32_t, 4> pixelUserData{static_cast<std::uint32_t>(wordsAddress), static_cast<std::uint32_t>(wordsAddress >> 32u), 0u, 0u};
    ShaderRecompiler::ShaderPixelStageInfo pixelInfo{};
    pixelInfo.wave32 = true;
    pixelInfo.targetOutputMode[0] = 9u;
    pixelInfo.targetExportMapping.fill(0xe4u);
    ShaderRecompiler::RecompileRequest pixelRequest{{ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(pixelCode.data()), pixelCode, 0, {}}, {32u, 0u, pixelUserData, std::nullopt, pixelInfo, std::nullopt, {}}, device.Target(), PixelPushLayout(pushBytes, device.Target())};
    const auto pixel = ShaderRecompiler::Recompile(pixelRequest);
    const bool bda = std::any_of(pixel.bindings.begin(), pixel.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; });
    std::printf("pixel program: %zu SPIR-V words, %s\n", pixel.spirv.size(), bda ? "reads through BDA" : "reads without BDA");
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertex, 0u}, {ShaderStage::Fragment, &pixel, PixelPushOffset(pushBytes, device.Target())}}};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(guest.target.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, 0, 0xe4u};
    state.color.tileMode = AgcDriver::Graphics::ColorTileMode::RenderTarget;
    state.color.bytes = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::SurfaceForTarget(state.color)).guestBytes;
    Require(state.color.bytes <= guest.target.size(), "the color target does not fit its guest block");
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0.0f, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0.0f, 1.0f};
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15u;
    state.blends = {state.blend};
    AgcDriver::Pm4::DrawParameters draw{0u, 3u, 0u, 1u, 0u, false};
    draw.firstVertex = firstRecord;
    std::vector<double> times;
    for (std::uint32_t round = 0; round < Rounds; ++round) {
        const auto start = std::chrono::steady_clock::now();
        for (std::uint32_t repeat = 0; repeat < DrawsPerRound; ++repeat) device.Draw(state, draw, shaders);
        device.SubmitRecorded(false);
        device.WaitIdle();
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::vector<std::byte> bytes(16);
    AgcDriver::GuestMemory::Read(state.color.address, bytes, 4);
    Require(bytes[0] == std::byte{255} && bytes[1] == std::byte{0} && bytes[3] == std::byte{255}, "the pixel shader did not sum its loaded words to 1.0");
    AgcDriver::Graphics::ReportDeviceLost();
}

}

int main() {
    try {
        _putenv("APS5_GPU_CHECKPOINTS=1");
        Require(AgcDriver::Graphics::GpuCheckpointsRequested(), "APS5_GPU_CHECKPOINTS was not seen");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::cout << "device diagnostics passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
