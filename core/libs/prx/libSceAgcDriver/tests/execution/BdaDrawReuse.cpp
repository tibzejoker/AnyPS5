#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
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
    alignas(256) std::array<std::byte, 256> otherVertices{};
    alignas(256) std::array<std::uint32_t, 4> descriptor{};
    alignas(256) std::array<std::uint32_t, 4> otherDescriptor{};
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

// Recorded draws whose vertex shader reads its vertex V# from a guest table (BDA). Repeats bind the
// first draw's resources (resource cache template hits), and must still follow what changed: user
// data naming another table (the reused build's shader data is refreshed per draw) and vertex data
// rewritten in place at the same address (read live through BDA).
void Run(AgcDriver::VulkanDevice& device) {
    GuestAllocations::Mutation().Add(&guest, sizeof(guest), true, true, true);
    constexpr std::uint32_t stride = 16u;
    constexpr std::uint32_t firstRecord = 2u;
    const auto writeVertices = [&](std::array<std::byte, 256>& vertices, float shift) {
        for (std::size_t index = 0; index < positions.size(); ++index) {
            auto position = positions[index];
            position[0] += shift;
            std::memcpy(vertices.data() + (firstRecord + index) * stride, position.data(), 16u);
        }
    };
    const auto vertexDescriptor = [&](const std::array<std::byte, 256>& vertices) {
        const auto address = reinterpret_cast<std::uintptr_t>(vertices.data());
        return std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (stride << 16u), 8u, 0x01000facu | (77u << 12u)};
    };
    writeVertices(guest.vertices, 0.0f);
    writeVertices(guest.otherVertices, 8.0f);
    guest.descriptor = vertexDescriptor(guest.vertices);
    guest.otherDescriptor = vertexDescriptor(guest.otherVertices);
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(guest.descriptor.data());
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(tableAddress), static_cast<std::uint32_t>(tableAddress >> 32u), 0u, 0u};
    ShaderRecompiler::ShaderVertexStageInfo vertexInfo{};
    vertexInfo.fetchEmbedded = true;
    vertexInfo.resourcesNum = 1u;
    vertexInfo.resourcesDst[0] = {0, 4, 0, 0};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(vertexCode.data()), vertexCode, 0, {}}, {32u, 0u, userData, std::nullopt, std::nullopt, vertexInfo, {}}, device.Target(), {0u, 0u, 0u, 0u}};
    auto vertex = ShaderRecompiler::Recompile(request);
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
    std::array<std::uint32_t, 4> roundUserData{};
    const auto table = [&](const std::array<std::uint32_t, 4>& descriptor) {
        const auto address = reinterpret_cast<std::uintptr_t>(descriptor.data());
        roundUserData = {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 0u, 0u};
        request.context.userData = roundUserData;
        vertex = ShaderRecompiler::Recompile(request);
    };
    const auto redPixels = [&] {
        std::vector<std::byte> bytes(state.color.bytes);
        AgcDriver::GuestMemory::Read(state.color.address, bytes, 4);
        std::size_t red = 0;
        for (std::size_t at = 0; at + 4u <= bytes.size(); at += 4u) red += bytes[at] == std::byte{255} && bytes[at + 1u] == std::byte{0} && bytes[at + 2u] == std::byte{0} && bytes[at + 3u] == std::byte{255};
        return red;
    };
    struct Round {
        const char* what;
        bool otherTable;
        float shift;
        bool drawn;
    };
    constexpr std::array<Round, 5> rounds{{{"first draw", false, 0.0f, true}, {"same draw again", false, 0.0f, true}, {"user data naming another table", true, 0.0f, false}, {"vertex data moved off screen in place", false, 8.0f, false}, {"vertex data moved back in place", false, 0.0f, true}}};
    std::uint64_t hits = 0;
    for (std::size_t index = 0; index < rounds.size(); ++index) {
        const auto& round = rounds[index];
        table(round.otherTable ? guest.otherDescriptor : guest.descriptor);
        writeVertices(guest.vertices, round.shift);
        std::memset(guest.target.data(), 0, state.color.bytes);
        const auto before = AgcDriver::Graphics::ResourceTemplateHits();
        device.Draw(state, draw, shaders);
        device.SubmitRecorded(false);
        device.WaitIdle();
        const auto hit = AgcDriver::Graphics::ResourceTemplateHits() - before;
        const auto red = redPixels();
        std::printf("%s: resource cache hits %llu, red pixels %zu\n", round.what, static_cast<unsigned long long>(hit), red);
        if (index != 0u) hits += hit;
        Require(round.drawn ? red == static_cast<std::size_t>(Extent) * Extent : red == 0u, std::string(round.what) + ": the draw did not follow guest memory (" + std::to_string(red) + " red pixels)");
    }
    Require(hits == rounds.size() - 1u, "identical recorded BDA draws rebuilt their resources instead of reusing the first draw's");
    // Back to back in one batch: the second draw reuses the first one's build while the target the
    // first one rendered is still pending on the GPU. The reuse must not store that target (or any
    // other pending image under the address space) into guest memory, which would note a pending
    // write over it and make the next CPU read of it wait for the GPU.
    table(guest.descriptor);
    writeVertices(guest.vertices, 0.0f);
    std::memset(guest.target.data(), 0, state.color.bytes);
    device.Draw(state, draw, shaders);
    const auto before = AgcDriver::Graphics::ResourceTemplateHits();
    device.Draw(state, draw, shaders);
    const auto hit = AgcDriver::Graphics::ResourceTemplateHits() - before;
    auto* recorder = AgcDriver::Graphics::Recorder::Active();
    const bool stored = recorder != nullptr && recorder->PendingWriteOverlaps(state.color.address, state.color.bytes);
    std::printf("back to back: resource cache hits %llu, target stored by the reuse %d\n", static_cast<unsigned long long>(hit), stored ? 1 : 0);
    Require(hit == 1u, "the second of two back-to-back BDA draws rebuilt its resources");
    Require(!stored, "a reused BDA draw stored the pending render target into guest memory");
    device.SubmitRecorded(false);
    device.WaitIdle();
    Require(redPixels() == static_cast<std::size_t>(Extent) * Extent, "back-to-back BDA draws did not draw");
    // The flush hook names the code that read guest memory (the [gpu-cpu-reads] caller chains).
    static std::string chain;
    static const void* caller = nullptr;
    AgcDriver::GuestMemory::SetFlushHook([](std::uint64_t, std::size_t) {
        caller = AgcDriver::GuestMemory::CurrentFlushCaller();
        chain = AgcDriver::GuestMemory::DescribeCallers(0u, 4u);
    });
    std::array<std::byte, 4> word{};
    AgcDriver::GuestMemory::Read(state.color.address, word, 4);
    std::printf("flush hook caller chain: %s\n", chain.c_str());
    Require(caller != nullptr && chain.find("agc_driver_bda_draw_reuse") != std::string::npos, "the flush hook does not name the guest memory reader: [" + chain + "]");

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
