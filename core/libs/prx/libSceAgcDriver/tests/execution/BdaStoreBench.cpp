#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
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
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Groups = 1u << 15u;
constexpr std::uint32_t Stores = 16;
constexpr std::uint32_t Rounds = 10u;
constexpr std::uint32_t GroupBytes = 4096u;
constexpr std::size_t BlockBytes = 65536u + static_cast<std::size_t>(Groups) * GroupBytes;

// s_load_dwordx4 s[8:11], s[0:1], 0 (the output V#, read from guest memory: resolved on the GPU, so
// the stores go through BDA); s_lshl_b32 s12, s4, 12 (the workgroup's page); s_waitcnt;
// v_lshlrev_b32 v1, 6, v0; Stores x buffer_store_dword v0, v1, s[8:11], s12 offen offset 4k;
// s_endpgm. Each workgroup's 64 lanes store into their own 4 KiB page, and each store notes its
// page for the CPU (note_bda_write), as GTA's ray traced passes do per lane.
std::vector<std::uint32_t> Code() {
    std::vector<std::uint32_t> code{0xf4080200u, 0xfa000000u, 0x8f0c8c04u, 0xbf8cc07fu, 0x34020086u};
    for (std::uint32_t store = 0; store < Stores; ++store) {
        code.push_back(0xe0701000u | (store * 4u));
        code.push_back(0x0c020001u);
    }
    code.push_back(0xbf810000u);
    return code;
}

// The GPU time of BDA stores from many lanes into one page (the [gpu-time] cost of GTA's 0x25b456800
// came with six such stores per lane), and that every store landed.
void Run(AgcDriver::VulkanDevice& device) {
    auto* block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Require(block != nullptr, "bda store bench: cannot allocate the guest block");
    std::memset(block, 0, BlockBytes);
    GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    auto* descriptor = reinterpret_cast<std::uint32_t*>(block);
    auto* output = reinterpret_cast<std::uint32_t*>(block + 65536);
    const auto outputAddress = reinterpret_cast<std::uintptr_t>(output);
    const std::array<std::uint32_t, 4> vsharp{static_cast<std::uint32_t>(outputAddress), static_cast<std::uint32_t>((outputAddress >> 32u) & 0xffffu), Groups * GroupBytes, 0x31016facu};
    std::memcpy(descriptor, vsharp.data(), sizeof(vsharp));
    const auto descriptorAddress = reinterpret_cast<std::uintptr_t>(descriptor);
    const std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(descriptorAddress), static_cast<std::uint32_t>(descriptorAddress >> 32u), 0u, 0u};
    const auto code = Code();
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}, {descriptorAddress, std::as_bytes(std::span(descriptor, 4))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}}, {64, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}};
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    const bool bda = std::any_of(result.bindings.begin(), result.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; });
    std::vector<double> times;
    for (std::uint32_t round = 0; round < Rounds; ++round) {
        std::memset(output, 0xff, static_cast<std::size_t>(Groups) * GroupBytes);
        const auto start = std::chrono::steady_clock::now();
        device.Dispatch(result, Groups, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
        device.WaitIdle();
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    for (const std::uint32_t group : {0u, Groups / 2u, Groups - 1u}) {
        for (std::uint32_t thread = 0; thread < Threads; ++thread) {
            for (std::uint32_t store = 0; store < Stores; ++store) Require(output[group * GroupBytes / 4u + thread * Stores + store] == thread, "bda store bench: group " + std::to_string(group) + " thread " + std::to_string(thread) + " store " + std::to_string(store) + " did not land");
        }
    }
    std::sort(times.begin() + 1, times.end());
    std::printf("%s, %u groups of %u lanes, %u stores each into the group's page: median %.3f ms per dispatch (min %.3f) over %u rounds\n", bda ? "stores through BDA" : "stores through a bound buffer", Groups, Threads, Stores, times[1 + (times.size() - 1) / 2], times[1], Rounds - 1u);
    GuestAllocations::Mutation().Remove(block);
    VirtualFree(block, 0, MEM_RELEASE);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::puts("bda store benchmark passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
