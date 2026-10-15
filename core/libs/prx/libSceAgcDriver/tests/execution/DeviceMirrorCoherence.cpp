#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
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

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 64;
constexpr std::size_t RangeBytes = 2u << 20u;
constexpr std::size_t HeapBytes = 64u << 20u;
constexpr std::size_t ChunkBytes = 4u << 20u;

// v_lshlrev_b32 v5, 2, v0; v[2:3] = s[0:1] + v5 (source), v[0:1] = s[2:3] + v5 (destination);
// global_load_dword v4, v[2:3], off; s_waitcnt; v_add_nc_u32 v4, 1, v4;
// global_store_dword v[0:1], v4, off; s_endpgm: every lane reads one dword through BDA and stores
// it plus one, also through BDA.
constexpr std::array<std::uint32_t, 15> Code{
    0x340a0082u, 0x7e040200u, 0x7e060201u, 0x7e000202u, 0x7e020203u, 0x4a040b02u, 0x4a000b00u,
    0xdc308000u, 0x047d0002u, 0xbf8c3f70u, 0x4a080881u, 0xdc708000u, 0x007d0400u, 0xbf810000u, 0xbf810000u,
};

// Guest memory as the title gets it: an arena allocation (write tracked), registered for the GPU;
// or, `arena` false, plain host memory the write tracking does not cover.
struct Range {
    std::uint32_t* words = nullptr;
    std::size_t bytes = 0;
    bool arena = true;
    explicit Range(std::size_t bytes, bool arena = true) : bytes(bytes), arena(arena) {
        if (arena) {
            words = static_cast<std::uint32_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
            if (words != nullptr) GuestArena::GuestArenaCommit_nid_postfix(words, bytes, PAGE_READWRITE, bytes);
        } else {
            words = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
        Require(words != nullptr, "device mirror test: no guest block");
        std::memset(words, 0, bytes);
        GuestAllocations::Mutation().Add(words, bytes, true, true, true);
    }
    ~Range() {
        GuestAllocations::Mutation().Remove(words);
        if (arena) {
            GuestArena::GuestArenaReset_nid_postfix(words, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(words, bytes);
        } else {
            VirtualFree(words, 0, MEM_RELEASE);
        }
    }
    std::uint64_t Address(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(words) + offset; }
};

void Copy(AgcDriver::VulkanDevice& device, std::uint64_t from, std::uint64_t to) {
    const std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(from >> 32u), static_cast<std::uint32_t>(to), static_cast<std::uint32_t>(to >> 32u)};
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), Code, 0, {}}, {64, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}};
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
    device.WaitIdle();
}

void Copy(AgcDriver::VulkanDevice& device, const Range& from, const Range& to) {
    Copy(device, from.Address(), to.Address());
}

void Expect(const std::uint32_t* words, std::uint32_t first, const std::string& step) {
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
        Require(words[lane] == first + lane, step + ": lane " + std::to_string(lane) + " holds " + std::to_string(words[lane]) + ", expected " + std::to_string(first + lane));
    }
}

void Fill(std::uint32_t* words, std::uint32_t first) {
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) words[lane] = first + lane;
}

AgcDriver::Graphics::DeviceMirrorCounts Counts() {
    return AgcDriver::Graphics::DeviceMirrorCounters();
}

void Print(const char* step, const AgcDriver::Graphics::DeviceMirrorCounts& counts) {
    std::printf("%s: %llu mirrors (%.1f MiB), %llu entries served from mirrors of %llu chunks, %llu hot, %llu uploads (%.1f MiB), %llu blocks staled by GPU writes, %llu checks (%llu skipped), left on imports: %llu untracked, %llu past the budget\n", step, static_cast<unsigned long long>(counts.mirrors), static_cast<double>(counts.mirroredBytes) / 1048576.0, static_cast<unsigned long long>(counts.assigned), static_cast<unsigned long long>(counts.chunks), static_cast<unsigned long long>(counts.touched), static_cast<unsigned long long>(counts.uploads), static_cast<double>(counts.uploadedBytes) / 1048576.0, static_cast<unsigned long long>(counts.gpuInvalidations), static_cast<unsigned long long>(counts.checks), static_cast<unsigned long long>(counts.checksSkipped), static_cast<unsigned long long>(counts.skippedUntracked), static_cast<unsigned long long>(counts.skippedBudget));
}

// Device mirrors keep BDA reads and writes coherent with guest memory, on chunks of the ranges BDA
// work used: a CPU write after the GPU read a range reaches the next GPU read, a GPU write reaches
// the CPU and the next GPU read, a write the driver reports outside the recorder stales the blocks,
// one chunk of a large heap is mirrored and not the heap, memory outside the write tracking stays on
// its import, and (child run) a range past the budget stays on its import with the same results.
void Run(AgcDriver::VulkanDevice& device, bool smallBudget, bool mirrors) {
    Range source(RangeBytes), middle(RangeBytes), last(RangeBytes);
    Fill(source.words, 0);
    Copy(device, source, middle);
    Expect(middle.words, 1, "first GPU read");
    Fill(source.words, 1000);
    Copy(device, source, middle);
    Expect(middle.words, 1001, "GPU read after a CPU write");
    Copy(device, middle, last);
    Expect(last.words, 1002, "GPU read of GPU-written memory");
    Fill(middle.words, 5000);
    Copy(device, middle, last);
    Expect(last.words, 5001, "GPU read after a CPU write over GPU-written memory");
    // The address space takes new hot chunks at most every 50 ms.
    Sleep(60);
    Copy(device, middle, last);
    Copy(device, middle, last);
    Expect(last.words, 5001, "GPU read of mirrored memory");
    auto counts = Counts();
    Print(smallBudget ? "budget of 1 MiB" : "after the small ranges", counts);
    if (smallBudget) {
        Require(counts.assigned == 0u, "a 2 MiB range was mirrored under a 1 MiB budget");
        return;
    }
    if (mirrors) Require(counts.mirrors == 3u && counts.assigned != 0u, "the three ranges BDA work used were not mirrored");
    // A write the driver reports outside the recorder (a synchronous write-back) stales the blocks.
    const auto staled = Counts().gpuInvalidations;
    static_cast<void>(AgcDriver::GuestMemory::MarkWritten(source.Address(), 4096));
    if (mirrors) Require(Counts().gpuInvalidations > staled, "MarkWritten of a mirrored range staled no block");
    // A 64 MiB heap of which the work reads one 4 MiB chunk: that chunk is mirrored, the rest not.
    Range heap(HeapBytes);
    const std::size_t offset = 5 * ChunkBytes + 4096;
    Fill(heap.words + offset / 4u, 7000);
    const auto before = Counts();
    Copy(device, heap.Address(offset), middle.Address());
    Sleep(60);
    Copy(device, heap.Address(offset), middle.Address());
    Copy(device, heap.Address(offset), middle.Address());
    Expect(middle.words, 7001, "GPU read of a heap chunk");
    Fill(heap.words + offset / 4u, 9000);
    Copy(device, heap.Address(offset), middle.Address());
    Expect(middle.words, 9001, "GPU read of a heap chunk after a CPU write");
    counts = Counts();
    Print("after the heap", counts);
    if (mirrors) Require(counts.mirroredBytes - before.mirroredBytes == ChunkBytes, "the heap was not mirrored one chunk");
    // The next chunk turns hot: the two share one mirror, read across their boundary.
    const std::size_t boundary = 6 * ChunkBytes - 128;
    Fill(heap.words + boundary / 4u, 11000);
    Copy(device, heap.Address(boundary), middle.Address());
    Sleep(60);
    Copy(device, heap.Address(boundary), middle.Address());
    Expect(middle.words, 11001, "GPU read across two hot chunks");
    Fill(heap.words + boundary / 4u, 12000);
    Copy(device, heap.Address(boundary), middle.Address());
    Expect(middle.words, 12001, "GPU read across two hot chunks after a CPU write");
    counts = Counts();
    Print("after the run grew", counts);
    if (mirrors) Require(counts.mirroredBytes - before.mirroredBytes >= 2 * ChunkBytes, "the grown run was not mirrored");
    // Plain host memory: outside the write tracking, it stays on its import.
    Range plain(RangeBytes, false);
    Fill(plain.words, 300);
    Copy(device, plain, middle);
    Sleep(60);
    Copy(device, plain, middle);
    Fill(plain.words, 400);
    Copy(device, plain, middle);
    Expect(middle.words, 401, "GPU read of untracked memory after a CPU write");
    counts = Counts();
    Print("after untracked memory", counts);
    if (mirrors) Require(counts.skippedUntracked != 0u, "untracked memory was mirrored");
    // Refused once, it no longer rebuilds the address space when the work touches it again.
    const auto epoch = AgcDriver::Graphics::DeviceMirrorEpoch();
    Sleep(60);
    Copy(device, plain, middle);
    Copy(device, plain, middle);
    Require(AgcDriver::Graphics::DeviceMirrorEpoch() == epoch, "untracked memory touched again moved the mirror epoch");
    // Within one collect epoch (one submission) a use finds the mirror checked already.
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto checksBefore = Counts();
    Copy(device, middle, last);
    Copy(device, middle, last);
    Print("in one epoch", Counts());
    if (mirrors) Require(Counts().checksSkipped > checksBefore.checksSkipped, "uses within one collect epoch checked the mirrors again");
}

}

int main(int argc, char** argv) {
    try {
        const bool smallBudget = argc > 1 && std::string(argv[1]) == "--small-budget";
        if (smallBudget) _putenv("APS5_DEVICE_MIRROR_MIB=1");
        _putenv("APS5_DEVICE_MIRROR_CHUNK_MIB=4");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const bool mirrors = AgcDriver::Graphics::DeviceMirrorsEnabled(AgcDriver::VulkanDeviceTestAccess::ContextOf(*device));
        Run(*device, smallBudget, mirrors);
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
