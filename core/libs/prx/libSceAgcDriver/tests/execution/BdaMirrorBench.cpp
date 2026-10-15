#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DeviceMirrors.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t Groups = 1u << 14u;
constexpr std::uint32_t Steps = 16;
constexpr std::uint32_t NodeCount = 1u << 22u;
constexpr std::size_t NodeBytes = static_cast<std::size_t>(NodeCount) * 16u;
constexpr std::size_t OutputBytes = static_cast<std::size_t>(Groups) * Lanes * 4u;
constexpr std::uint32_t Rounds = 6;

// Each lane starts at node (group * 64 + lane) and follows Steps nodes through BDA loads, each node's
// first dword naming the next (the chain of reads of a BVH traversal), then stores where it ended:
// s_lshl_b32 s5, s4, 10; v_lshlrev_b32 v5, 4, v0; v_add_nc_u32 v5, s5, v5; v2 = s0 + v5; v3 = s1;
// Steps x (global_load_dword v4, v[2:3]; s_waitcnt; v_lshlrev_b32 v6, 4, v4; v_add_nc_u32 v2, s0, v6);
// v7 = (s4 << 8) + (v0 << 2); v[0:1] = s[2:3] + v7; global_store_dword v[0:1], v4; s_endpgm.
std::vector<std::uint32_t> Code() {
    std::vector<std::uint32_t> code{0x8f058a04u, 0x340a0084u, 0x4a0a0a05u, 0x4a040a00u, 0x7e060201u};
    for (std::uint32_t step = 0; step < Steps; ++step) {
        for (const auto word : {0xdc308000u, 0x047d0002u, 0xbf8c3f70u, 0x340c0884u, 0x4a040c00u}) code.push_back(word);
    }
    for (const auto word : {0x340e0082u, 0x8f068804u, 0x4a0e0e06u, 0x4a000e02u, 0x7e020203u, 0xdc708000u, 0x007d0400u, 0xbf810000u}) code.push_back(word);
    return code;
}

struct Arena {
    std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
    Arena(std::size_t size, bool writable) : size(size) {
        bytes = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(size, std::size_t{1} << 28u));
        Require(bytes != nullptr, "mirror bench: no arena block");
        GuestArena::GuestArenaCommit_nid_postfix(bytes, size, PAGE_READWRITE, size);
        GuestAllocations::Mutation().Add(bytes, size, true, writable, true);
    }
    ~Arena() {
        GuestAllocations::Mutation().Remove(bytes);
        GuestArena::GuestArenaReset_nid_postfix(bytes, size);
        GuestArena::GuestArenaRelease_nid_postfix(bytes, size);
    }
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(bytes); }
};

// The GPU time of dependent BDA reads from guest memory through the driver, with the range served
// from a device mirror (default) or from its host import (APS5_NO_DEVICE_MIRRORS=1, the child run).
double Run(AgcDriver::VulkanDevice& device) {
    Arena nodes(NodeBytes, false);
    Arena output(OutputBytes, true);
    auto* words = reinterpret_cast<std::uint32_t*>(nodes.bytes);
    std::uint32_t state = 12345u;
    for (std::uint32_t index = 0; index < NodeCount; ++index) {
        state = state * 1664525u + 1013904223u;
        words[index * 4u] = state % NodeCount;
        words[index * 4u + 1u] = 0u;
        words[index * 4u + 2u] = 0u;
        words[index * 4u + 3u] = 0u;
    }
    const auto code = Code();
    const std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(nodes.Address()), static_cast<std::uint32_t>(nodes.Address() >> 32u), static_cast<std::uint32_t>(output.Address()), static_cast<std::uint32_t>(output.Address() >> 32u)};
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}}, {64, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}};
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    std::vector<double> times;
    for (std::uint32_t round = 0; round < Rounds; ++round) {
        const auto start = std::chrono::steady_clock::now();
        device.Dispatch(result, Groups, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
        device.WaitIdle();
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    const auto* ends = reinterpret_cast<const std::uint32_t*>(output.bytes);
    for (const std::uint32_t lane : {0u, 1u, 777u, Groups * Lanes - 1u}) {
        std::uint32_t node = lane;
        for (std::uint32_t step = 0; step < Steps; ++step) node = words[node * 4u];
        Require(ends[lane] == node, "mirror bench: lane " + std::to_string(lane) + " ended at node " + std::to_string(ends[lane]) + ", expected " + std::to_string(node));
    }
    std::sort(times.begin() + 1, times.end());
    const auto counts = AgcDriver::Graphics::DeviceMirrorCounters();
    std::printf("%s: %u lanes x %u dependent 16-byte node reads over 64 MiB: median %.3f ms per dispatch (min %.3f); %llu mirrors %.1f MiB, %llu uploads %.1f MiB\n", std::getenv("APS5_NO_DEVICE_MIRRORS") != nullptr ? "host import" : "device mirror", Groups * Lanes, Steps, times[1 + (times.size() - 1) / 2], times[1], static_cast<unsigned long long>(counts.mirrors), static_cast<double>(counts.mirroredBytes) / 1048576.0, static_cast<unsigned long long>(counts.uploads), static_cast<double>(counts.uploadedBytes) / 1048576.0);
    return times[1 + (times.size() - 1) / 2];
}

}

int main(int argc, char** argv) {
    try {
        const bool child = argc > 1 && std::string(argv[1]) == "--imports";
        if (child) _putenv("APS5_NO_DEVICE_MIRRORS=1");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        static_cast<void>(Run(*device));
        if (!child) {
            const std::string imports = std::string("\"\"") + argv[0] + "\" --imports\"";
            Require(std::system(imports.c_str()) == 0, "the host import run failed");
            std::puts("bda mirror benchmark passed");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
