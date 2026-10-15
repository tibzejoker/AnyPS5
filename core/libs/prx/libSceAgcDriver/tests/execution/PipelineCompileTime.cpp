#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using namespace AgcDriver::Graphics;
using ShaderRecompiler::ShaderStage;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000};

std::string ReadText(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(std::string("cannot read ") + path);
    std::ostringstream stream;
    stream << file.rdbuf();
    auto text = stream.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

// An OpString with a fresh random literal in the debug section: the module hashes differently, so no
// driver or pipeline cache serves the compile being timed.
void MakeUncached(std::vector<std::uint32_t>& spirv) {
    std::size_t at = 5;
    while (at < spirv.size()) {
        const auto opcode = spirv[at] & 0xffffu;
        const auto count = spirv[at] >> 16u;
        if (opcode != spv::OpCapability && opcode != spv::OpExtension && opcode != spv::OpExtInstImport && opcode != spv::OpMemoryModel && opcode != spv::OpEntryPoint && opcode != spv::OpExecutionMode && opcode != spv::OpExecutionModeId) break;
        at += count;
    }
    std::random_device random;
    const auto text = "compile-time " + std::to_string(random()) + "-" + std::to_string(random());
    std::vector<std::uint32_t> literal((text.size() + 4u) / 4u, 0u);
    std::memcpy(literal.data(), text.data(), text.size());
    const auto id = spirv[3]++;
    std::vector<std::uint32_t> instruction{(static_cast<std::uint32_t>(2u + literal.size()) << 16u) | spv::OpString, id};
    instruction.insert(instruction.end(), literal.begin(), literal.end());
    spirv.insert(spirv.begin() + static_cast<std::ptrdiff_t>(at), instruction.begin(), instruction.end());
}

// Pipeline creation time of a replayed pixel shader request (APS5_DUMP_SHADERS) with a minimal vertex
// shader, on uncached SPIR-V: cold on the draw thread, after a registration-style background prebuild
// of its fragment program, and while that prebuild still compiles (the draw must wait for it, not
// compile the program again).
void Time(AgcDriver::VulkanDevice& device, const char* path) {
    auto saved = ShaderRecompiler::RequestSerializer{}.Deserialize(ReadText(path));
    auto& request = saved.request;
    if (request.shader.stage != ShaderStage::Fragment || !request.context.pixel.has_value()) throw std::runtime_error(std::string(path) + " is not a pixel shader request");
    const auto target = device.Target();
    request.target = target;
    request.useCache = false;
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    const std::vector<std::uint32_t> vertexUserData(4, 0u);
    ShaderRecompiler::RecompileRequest vertex{{ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}}, {request.context.waveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory}, target, {0, 0, 0, 64}};
    vertex.useCache = false;
    auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    request.layout = PixelPushLayout(vertexPush, target);
    const auto pixelPush = PixelPushOffset(vertexPush, target);
    const auto recompileStart = std::chrono::steady_clock::now();
    auto pixel = ShaderRecompiler::Recompile(request);
    const auto recompileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - recompileStart).count();
    for (auto* result : {&pixel, &vertexResult}) {
        auto words = result->spirv.Words();
        MakeUncached(words);
        result->spirv = std::move(words);
    }
    const std::array<CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertexResult, 0}, {ShaderStage::Fragment, &pixel, pixelPush}}};
    State state{};
    state.stages = {ShaderPath::Vertex, 0u, request.context.waveSize, request.context.waveSize, std::nullopt, std::nullopt};
    std::uint32_t targets = 0;
    for (std::uint32_t index = 0; index < request.context.pixel->targetOutputMode.size(); ++index) {
        if (request.context.pixel->targetOutputMode[index] != 0u) targets = index + 1u;
    }
    for (std::uint32_t index = 0; index < std::max(targets, 1u); ++index) {
        ColorTarget color{0x100000u * (index + 1u), {64, 64}, VK_FORMAT_R8G8B8A8_UNORM, 64u * 64u * 4u, 0xe4u};
        color.exportIndex = index;
        if (index == 0) state.color = color;
        state.colors.push_back(color);
    }
    state.hasColorTarget = true;
    state.renderExtent = {64, 64};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, 64.0f, 64.0f, -64.0f, 0, 1};
    state.scissor = {{0, 0}, {64, 64}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends.assign(state.colors.size(), state.blend);
    const auto layouts = DescribeSetLayouts(context, shaders);
    const auto milliseconds = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
    const auto fresh = [&] {
        auto words = pixel.spirv.Words();
        MakeUncached(words);
        pixel.spirv = std::move(words);
        (pixel.specializationId != 0 ? pixel.specializationId : pixel.variantId) += 0x9e3779b97f4a7c15ull;
    };
    const auto build = [&] {
        const auto start = std::chrono::steady_clock::now();
        Pipeline pipeline(context, state, VertexInputLayout{}, layouts.View(), shaders);
        return milliseconds(start, std::chrono::steady_clock::now());
    };
    const auto fragmentParts = [&] { return PipelineLibraryCountersOf(context.device).built[2]; };
    const auto prebuild = [&] {
        const std::array<CompiledShader, 1> fragment{{{ShaderStage::Fragment, &pixel, pixelPush}}};
        static_cast<void>(device.PrebuildGraphicsLibraries(state.stages, fragment));
    };
    // (1) Cold, on the draw thread, as a draw without a prebuild compiles it.
    const auto cold = build();
    WaitForOptimizedPipelines(context.device);
    // (2) After a background prebuild of the fragment program: the draw only links.
    fresh();
    const auto prebuildStart = std::chrono::steady_clock::now();
    std::thread(prebuild).join();
    const auto prebuildMs = milliseconds(prebuildStart, std::chrono::steady_clock::now());
    const auto before = fragmentParts();
    const auto prebuilt = build();
    const bool reused = fragmentParts() == before;
    WaitForOptimizedPipelines(context.device);
    // (3) A draw arriving while the prebuild still compiles waits for that compile, not a second one.
    fresh();
    const auto overlapBefore = fragmentParts();
    std::thread background(prebuild);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto overlapped = build();
    background.join();
    const auto compiles = fragmentParts() - overlapBefore;
    WaitForOptimizedPipelines(context.device);
    std::printf("%s: %zu SPIR-V words, recompile %.1f ms; pipeline cold %.1f ms; background prebuild %.1f ms then draw pipeline %.1f ms (fragment part reused %d); draw during the prebuild %.1f ms, fragment compiles %llu\n", path, pixel.spirv.size(), recompileMs, cold, prebuildMs, prebuilt, reused ? 1 : 0, overlapped, static_cast<unsigned long long>(compiles));
    Require(reused, "the draw compiled the fragment program its prebuild had compiled");
    Require(compiles == 1u, "a draw during a prebuild of its fragment program compiled it a second time");
}

}

// Arguments: replayed pixel shader requests; without any, APS5_PIPELINE_TIME_REQUESTS (paths
// separated by ';'), else skipped (the requests are game data, not part of the tree).
int main(int argc, char** argv) {
    try {
        std::vector<std::string> paths;
        for (int index = 1; index < argc; ++index) paths.emplace_back(argv[index]);
        if (paths.empty()) {
            if (const char* list = std::getenv("APS5_PIPELINE_TIME_REQUESTS")) {
                std::stringstream stream(list);
                for (std::string path; std::getline(stream, path, ';');) {
                    if (!path.empty()) paths.push_back(path);
                }
            }
        }
        if (paths.empty()) {
            std::printf("skipped, no shader requests (arguments or APS5_PIPELINE_TIME_REQUESTS)\n");
            return VulkanTestSkipped;
        }
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (const auto& path : paths) Time(*device, path.c_str());
        std::cout << "pipeline compile times measured\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
