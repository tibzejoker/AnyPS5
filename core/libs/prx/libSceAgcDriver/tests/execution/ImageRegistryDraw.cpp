#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 16;
constexpr std::uint32_t Size = 16;
constexpr std::uint32_t Mips = 5;
constexpr std::uint32_t Format8888UNorm = 56;
constexpr std::uint32_t Type1D = 8;
constexpr float SecondCoordinateScale = 3.0f;
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};
alignas(256) std::array<std::uint8_t, 16384> Line{};

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

// s_buffer_load_dwordx8 s[4:11] and s_buffer_load_dwordx4 s[12:15] from the descriptor buffer at
// s[0:3] (so the T# and S# come from memory and go through the registry), then the
// ImagePlaneAddressOnLineImplicitLod pixel program.
alignas(256) constexpr std::array<std::uint32_t, 15> PixelCode{
    0xf42c0100, 0xfa000000, 0xf4280300, 0xfa000020, 0xbf8cc07f, 0xbefe0a7e, 0x10040010, 0x06060300, 0x10060611, 0xf0800f08, 0x00610402, 0xbf8c3f70, 0xf800180f, 0x07060504, 0xbf810000,
};

alignas(256) std::array<std::uint32_t, 16> Descriptors{};

constexpr std::array<std::array<float, 4>, 3> Triangle{{{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}}};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

std::array<std::uint32_t, 8> LineDescriptor() {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Line.data()));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format8888UNorm << 20u) | (((Size - 1u) & 3u) << 30u),
        (Size - 1u) >> 2u,
        0xfacu | ((Mips - 1u) << 16u) | (Type1D << 28u),
        0u,
        (Mips - 1u) << 4u,
        0u,
        0u,
    };
}

std::array<std::uint32_t, 4> SamplerDescriptor() {
    return {0u, 0xfffu << 12u, 1u << 26u, 0u};
}

std::uint8_t LevelMark(std::uint32_t level) {
    return static_cast<std::uint8_t>(10u + 40u * level);
}

void FillTexture() {
    const auto geometry = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(LineDescriptor()));
    Require(geometry.guestBytes <= Line.size(), "implicit LOD through a 1D texture: the texture does not fit the texel storage");
    Require(geometry.mips.size() == Mips, "implicit LOD through a 1D texture: the texture has " + std::to_string(geometry.mips.size()) + " mips");
    std::fill(Line.begin(), Line.end(), 0xeeu);
    for (std::uint32_t level = 0; level < Mips; ++level) {
        const auto& mip = geometry.mips[level];
        for (std::uint32_t x = 0; x < (Size >> level); ++x) {
            auto* texel = &Line[geometry.GuestLayerOffset(0) + mip.tiledOffset + x * 4u];
            texel[0] = LevelMark(level);
            texel[1] = static_cast<std::uint8_t>(x * 8u);
            texel[2] = 99u;
            texel[3] = 255u;
        }
    }
}

void Draw(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> pixelCode, float firstCoordinateScale) {
    const auto& target = device.Target();
    Pixels.fill(std::byte{0});

    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(Triangle.data(), 16u, static_cast<std::uint32_t>(Triangle.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.wave32 = false;
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    std::vector<std::uint32_t> pixelUserData(18, 0u);
    const auto texture = LineDescriptor();
    const auto sampler = SamplerDescriptor();
    std::copy(texture.begin(), texture.end(), Descriptors.begin());
    std::copy(sampler.begin(), sampler.end(), Descriptors.begin() + 8);
    const auto descriptors = BufferDescriptor(Descriptors.data(), 0u, static_cast<std::uint32_t>(sizeof(Descriptors)));
    std::copy(descriptors.begin(), descriptors.end(), pixelUserData.begin());
    pixelUserData[16] = std::bit_cast<std::uint32_t>(firstCoordinateScale);
    pixelUserData[17] = std::bit_cast<std::uint32_t>(SecondCoordinateScale);
    const std::array<ShaderRecompiler::MemoryRegion, 2> pixelMemory{{{reinterpret_cast<std::uintptr_t>(pixelCode.data()), std::as_bytes(pixelCode)}, {reinterpret_cast<std::uintptr_t>(Descriptors.data()), std::as_bytes(std::span(Descriptors))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(pixelCode.data()), pixelCode, 0, {}},
        {64, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangle.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

bool Matches(std::uint32_t level) {
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            const std::array<std::uint32_t, 4> expected{LevelMark(level), (x % (Size >> level)) * 8u, 99u, 255u};
            for (std::uint32_t component = 0; component < 4u; ++component) {
                if (std::to_integer<std::uint32_t>(Pixels[(y * Width + x) * 4u + component]) != expected[component]) return false;
            }
        }
    }
    return true;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        FillTexture();
        const float scale = 1.0f / static_cast<float>(Size);
        Draw(*device, PixelCode, scale);
        const bool first = Matches(0u);
        std::printf("first draw %s, pixel 0 = %u %u %u %u\n", first ? "sampled the texture" : "missed the registry", std::to_integer<unsigned>(Pixels[0]), std::to_integer<unsigned>(Pixels[1]), std::to_integer<unsigned>(Pixels[2]), std::to_integer<unsigned>(Pixels[3]));
        Draw(*device, PixelCode, scale);
        Draw(*device, PixelCode, scale);
        Require(Matches(0u), "a pixel shader sampling a T# and S# read from memory does not sample the texture once the registry drained them: pixel 0 = " + std::to_string(std::to_integer<unsigned>(Pixels[0])) + " " + std::to_string(std::to_integer<unsigned>(Pixels[1])) + " " + std::to_string(std::to_integer<unsigned>(Pixels[2])) + " " + std::to_string(std::to_integer<unsigned>(Pixels[3])));
        std::puts("image registry draw tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
