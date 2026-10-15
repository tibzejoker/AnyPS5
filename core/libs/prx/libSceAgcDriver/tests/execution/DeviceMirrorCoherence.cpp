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
#include <array>
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
constexpr std::size_t RangeBytes = 2u << 20u;

// v_lshlrev_b32 v5, 2, v0; v[2:3] = s[0:1] + v5 (source), v[0:1] = s[2:3] + v5 (destination);
// global_load_dword v4, v[2:3], off; s_waitcnt; v_add_nc_u32 v4, 1, v4;
// global_store_dword v[0:1], v4, off; s_endpgm: every lane reads one dword through BDA and stores
// it plus one, also through BDA.
constexpr std::array<std::uint32_t, 15> Code{
    0x340a0082u, 0x7e040200u, 0x7e060201u, 0x7e000202u, 0x7e020203u, 0x4a040b02u, 0x4a000b00u,
    0xdc308000u, 0x047d0002u, 0xbf8c3f70u, 0x4a080881u, 0xdc708000u, 0x007d0400u, 0xbf810000u, 0xbf810000u,
};

// Guest memory as the title gets it: an arena allocation (write tracked), registered for the GPU.
struct Range {
    std::uint32_t* words = nullptr;
    Range() {
        words = static_cast<std::uint32_t*>(GuestArena::GuestArenaAllocate_nid_postfix(RangeBytes, 65536));
        Require(words != nullptr, "device mirror test: no arena block");
        GuestArena::GuestArenaCommit_nid_postfix(words, RangeBytes, PAGE_READWRITE, RangeBytes);
        std::memset(words, 0, RangeBytes);
        GuestAllocations::Mutation().Add(words, RangeBytes, true, true, true);
    }
    ~Range() {
        GuestAllocations::Mutation().Remove(words);
        GuestArena::GuestArenaReset_nid_postfix(words, RangeBytes);
        GuestArena::GuestArenaRelease_nid_postfix(words, RangeBytes);
    }
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(words); }
};

void Copy(AgcDriver::VulkanDevice& device, const Range& from, const Range& to) {
    const std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(from.Address()), static_cast<std::uint32_t>(from.Address() >> 32u), static_cast<std::uint32_t>(to.Address()), static_cast<std::uint32_t>(to.Address() >> 32u)};
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), Code, 0, {}}, {64, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}};
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
    device.WaitIdle();
}

void Expect(const Range& range, std::uint32_t first, const char* step) {
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
        Require(range.words[lane] == first + lane, std::string(step) + ": lane " + std::to_string(lane) + " holds " + std::to_string(range.words[lane]) + ", expected " + std::to_string(first + lane));
    }
}

// Device mirrors keep BDA reads and writes coherent with guest memory: a CPU write after the GPU
// read a range reaches the next GPU read, a GPU write reaches the CPU and the next GPU read, and a
// range past the budget stays on its import with the same results.
void Run(AgcDriver::VulkanDevice& device, bool smallBudget) {
    Range source, middle, last;
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) source.words[lane] = lane;
    Copy(device, source, middle);
    Expect(middle, 1, "first GPU read");
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) source.words[lane] = 1000u + lane;
    Copy(device, source, middle);
    Expect(middle, 1001, "GPU read after a CPU write");
    Copy(device, middle, last);
    Expect(last, 1002, "GPU read of GPU-written memory");
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) middle.words[lane] = 5000u + lane;
    Copy(device, middle, last);
    Expect(last, 5001, "GPU read after a CPU write over GPU-written memory");
    const auto counts = AgcDriver::Graphics::DeviceMirrorCounters();
    std::printf("%s: %llu mirrors (%.1f MiB), %llu ranges served from mirrors, %llu uploads (%.1f MiB), %llu blocks staled by GPU writes, left on imports: %llu untracked, %llu past the budget\n", smallBudget ? "budget of 1 MiB" : "default budget", static_cast<unsigned long long>(counts.mirrors), static_cast<double>(counts.mirroredBytes) / 1048576.0, static_cast<unsigned long long>(counts.assigned), static_cast<unsigned long long>(counts.uploads), static_cast<double>(counts.uploadedBytes) / 1048576.0, static_cast<unsigned long long>(counts.gpuInvalidations), static_cast<unsigned long long>(counts.skippedUntracked), static_cast<unsigned long long>(counts.skippedBudget));
    // An integrated GPU takes no mirrors at all (nothing to skip for the budget either).
    if (smallBudget) Require(counts.assigned == 0u, "a 2 MiB range was mirrored under a 1 MiB budget");
}

}

int main(int argc, char** argv) {
    try {
        const bool smallBudget = argc > 1 && std::string(argv[1]) == "--small-budget";
        if (smallBudget) _putenv("APS5_DEVICE_MIRROR_MIB=1");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device, smallBudget);
        if (!smallBudget) {
            // The budget is read once per process: the over-budget case runs in a child.
            const std::string child = std::string("\"\"") + argv[0] + "\" --small-budget\"";
            Require(std::system(child.c_str()) == 0, "the over-budget run failed");
        }
        std::puts("device mirror coherence tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
