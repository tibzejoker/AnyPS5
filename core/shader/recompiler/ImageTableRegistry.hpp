#ifndef CORE_SHADER_RECOMPILER_IMAGETABLEREGISTRY_HPP
#define CORE_SHADER_RECOMPILER_IMAGETABLEREGISTRY_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <vector>

#include "IntermediateRepresentation/IrMetadata/Resources.hpp"
#include "IntermediateRepresentation/IrMetadata/ShaderStage.hpp"
#include "ImageTableAbi.hpp"

namespace ShaderRecompiler::ImageTableRegistry {

using ShaderRecompiler::IrShaderStage;

inline constexpr std::uint32_t DescriptorSet = 2u;
inline constexpr std::uint32_t MapBinding = 0u;
inline constexpr std::uint32_t ImageBinding = 1u;
inline constexpr std::uint32_t SamplerBinding = 2u;

inline constexpr std::uint32_t SlotWords = 9u;
inline constexpr std::uint32_t SlotCodeWord = 8u;
inline constexpr std::uint32_t Probes = 8u;
inline constexpr std::uint32_t RingEntries = 256u;
inline constexpr std::uint32_t RingEntryWords = 8u;
inline constexpr std::uint32_t RingWords = 1u + RingEntries * RingEntryWords;
inline constexpr std::uint32_t Marker = 0x52454749u;

inline constexpr std::uint32_t MaxImages = 4096u;
inline constexpr std::uint32_t MaxSamplers = 1024u;
inline constexpr std::uint32_t MaxImageElements = 16384u;
inline constexpr std::uint32_t SamplerElements = ImageTableAbi::SamplerEntryElements;
inline constexpr std::uint32_t MaxSamplerElements = MaxSamplers * SamplerElements;
inline constexpr std::uint32_t ImageSlotCapacity = 16384u;
inline constexpr std::uint32_t SamplerSlotCapacity = 4096u;
inline constexpr std::uint32_t DirectoryCapacity = 8192u;
inline constexpr std::uint32_t DirectoryWords = 3u;
inline constexpr std::uint32_t MaxRegions = 2048u;
inline constexpr std::uint32_t RegionWords = MaxImages;

inline constexpr std::uint32_t HeaderMarker = 0u;
inline constexpr std::uint32_t HeaderImageSlots = 1u;
inline constexpr std::uint32_t HeaderImageCapacity = 2u;
inline constexpr std::uint32_t HeaderSamplerSlots = 3u;
inline constexpr std::uint32_t HeaderSamplerCapacity = 4u;
inline constexpr std::uint32_t HeaderImageRing = 5u;
inline constexpr std::uint32_t HeaderSamplerRing = 6u;
inline constexpr std::uint32_t HeaderDirectory = 7u;
inline constexpr std::uint32_t HeaderDirectoryCapacity = 8u;
inline constexpr std::uint32_t HeaderWords = 16u;

inline constexpr std::uint32_t ImageSlotsStart = HeaderWords;
inline constexpr std::uint32_t SamplerSlotsStart = ImageSlotsStart + ImageSlotCapacity * SlotWords;
inline constexpr std::uint32_t ImageRingStart = SamplerSlotsStart + SamplerSlotCapacity * SlotWords;
inline constexpr std::uint32_t SamplerRingStart = ImageRingStart + RingWords;
inline constexpr std::uint32_t DirectoryStart = SamplerRingStart + RingWords;
inline constexpr std::uint32_t RegionsStart = DirectoryStart + DirectoryCapacity * DirectoryWords;
inline constexpr std::uint32_t MapWords = RegionsStart + MaxRegions * RegionWords;

inline constexpr std::uint32_t RecordModeMask = 0xffu;
inline constexpr std::uint32_t RecordValid = 1u << 8u;
inline constexpr std::uint32_t RecordElementShift = 9u;

inline std::uint32_t Hash(const std::uint32_t* words, std::uint32_t count) {
    std::uint32_t hash = 2166136261u;
    for (std::uint32_t index = 0; index < count; ++index) {
        hash ^= words[index];
        hash *= 16777619u;
    }
    return hash;
}

inline void InitMap(std::span<std::uint32_t> map) {
    std::fill(map.begin(), map.end(), 0u);
    map[HeaderMarker] = Marker;
    map[HeaderImageSlots] = ImageSlotsStart;
    map[HeaderImageCapacity] = ImageSlotCapacity;
    map[HeaderSamplerSlots] = SamplerSlotsStart;
    map[HeaderSamplerCapacity] = SamplerSlotCapacity;
    map[HeaderImageRing] = ImageRingStart;
    map[HeaderSamplerRing] = SamplerRingStart;
    map[HeaderDirectory] = DirectoryStart;
    map[HeaderDirectoryCapacity] = DirectoryCapacity;
}

// The slot's code is written last: a shader probing while the CPU inserts sees either no code or
// the complete words.
inline bool InsertSlot(std::span<std::uint32_t> map, std::uint32_t start, std::uint32_t capacity, const std::uint32_t* words, std::uint32_t count, std::uint32_t code) {
    const auto hash = Hash(words, count);
    for (std::uint32_t probe = 0; probe < Probes; probe++) {
        const auto base = start + ((hash + probe) & (capacity - 1u)) * SlotWords;
        if (map[base + SlotCodeWord] != 0u) continue;
        for (std::uint32_t dword = 0; dword < count; dword++) map[base + dword] = words[dword];
        std::atomic_thread_fence(std::memory_order_release);
        map[base + SlotCodeWord] = code;
        return true;
    }
    return false;
}

inline std::uint32_t FindSlot(std::span<const std::uint32_t> map, std::uint32_t start, std::uint32_t capacity, const std::uint32_t* words, std::uint32_t count) {
    const auto hash = Hash(words, count);
    for (std::uint32_t probe = 0; probe < Probes; probe++) {
        const auto base = start + ((hash + probe) & (capacity - 1u)) * SlotWords;
        const auto code = map[base + SlotCodeWord];
        if (code == 0u) continue;
        bool match = true;
        for (std::uint32_t dword = 0; dword < count; dword++) match = match && map[base + dword] == words[dword];
        if (match) return code;
    }
    return 0u;
}

inline bool DirectoryInsert(std::span<std::uint32_t> map, std::uint64_t key, std::uint32_t region) {
    const std::array<std::uint32_t, 2> words{static_cast<std::uint32_t>(key), static_cast<std::uint32_t>(key >> 32u)};
    const auto hash = Hash(words.data(), 2u);
    for (std::uint32_t probe = 0; probe < Probes; probe++) {
        const auto base = DirectoryStart + ((hash + probe) & (DirectoryCapacity - 1u)) * DirectoryWords;
        if (map[base + 2u] != 0u) continue;
        map[base] = words[0];
        map[base + 1u] = words[1];
        std::atomic_thread_fence(std::memory_order_release);
        map[base + 2u] = region;
        return true;
    }
    return false;
}

inline std::uint32_t DirectoryFind(std::span<const std::uint32_t> map, std::uint64_t key) {
    const std::array<std::uint32_t, 2> words{static_cast<std::uint32_t>(key), static_cast<std::uint32_t>(key >> 32u)};
    const auto hash = Hash(words.data(), 2u);
    for (std::uint32_t probe = 0; probe < Probes; probe++) {
        const auto base = DirectoryStart + ((hash + probe) & (DirectoryCapacity - 1u)) * DirectoryWords;
        if (map[base + 2u] != 0u && map[base] == words[0] && map[base + 1u] == words[1]) return map[base + 2u];
    }
    return 0u;
}

// Identity of an image resource's runtime mode list, shared by every program whose resource has the
// same list: the shader embeds it and the driver fills one record region per key.
inline std::uint64_t ModeSetKey(const ImageResource& image, std::span<const ImageResource> modes) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 0x100000001b3ull;
    };
    const auto resource = [&](const ImageResource& value) {
        mix(static_cast<std::uint64_t>(value.resourceClass));
        mix(static_cast<std::uint64_t>(value.numericClass));
        mix(static_cast<std::uint64_t>(value.dimension));
        mix(static_cast<std::uint64_t>(value.mipMode));
        mix(value.mipCount);
        mix(static_cast<std::uint64_t>(value.conversionFormat));
        mix(value.shaderSwizzle);
        mix((value.read ? 1u : 0u) | (value.written ? 2u : 0u) | (value.atomic ? 4u : 0u) | (value.atomic64 ? 8u : 0u) | (value.depthCompare ? 16u : 0u) | (value.cube ? 32u : 0u) | (value.r128 ? 64u : 0u) | (value.srgbDecode ? 128u : 0u));
        mix((value.srgbDecodeCompatible ? 1u : 0u) | (value.depthBits ? 2u : 0u) | (value.depthUnorm16 ? 4u : 0u) | (value.packed ? 8u : 0u) | (value.fmaskCompatible ? 16u : 0u) | (value.depthBitsCompatible ? 32u : 0u) | (value.constantSwizzle ? 64u : 0u) | (value.constantSwizzleCompatible ? 128u : 0u));
        mix((value.flatVolumeCompatible ? 1u : 0u) | (value.flatLineCompatible ? 2u : 0u) | (value.foldTexelOffsetsOnly ? 4u : 0u));
        mix(value.srgbDecodeFormats);
        mix(value.byElements);
        mix(value.byComponents);
        mix(static_cast<std::uint64_t>(value.packedFormat));
        mix(value.emulatedCompare);
        mix(static_cast<std::uint64_t>(value.tableOperation));
    };
    resource(image);
    mix(modes.size());
    for (const auto& mode : modes) resource(mode);
    return hash == 0u ? 1u : hash;
}

struct ModeSet {
    ImageResource image;
    std::vector<ImageResource> modes;
};

struct State {
    std::mutex mutex;
    std::vector<std::array<std::uint32_t, 8>> images;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::set<std::array<std::uint32_t, 8>> imageSet;
    std::set<std::array<std::uint32_t, 4>> samplerSet;
    std::map<std::uint64_t, ModeSet> modeSets;
    std::vector<std::uint64_t> pendingModeSets;
    std::atomic<std::uint64_t> generation{1};
    std::atomic<std::uint64_t> drained{0};
    std::atomic<std::uint64_t> addedImages{0};
    std::atomic<std::uint64_t> addedSamplers{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> regions{0};
    std::atomic<std::uint64_t> records{0};
    std::atomic<std::uint64_t> failedRecords{0};
    std::atomic<std::uint64_t> elements{0};
    std::atomic<std::uint64_t> refreshed{0};
    std::atomic<std::uint64_t> builds{0};
    std::atomic<std::uint64_t> zeroCpuBuilds{0};
    std::atomic<std::uint64_t> binds{0};
    std::atomic<std::uint64_t> commandBuffers{0};
    std::atomic<long long> lastReport{0};
    std::uint64_t reportedBuilds = 0;
    std::uint64_t reportedZeroCpu = 0;
    std::uint64_t reportedBinds = 0;
    std::uint64_t reportedCommandBuffers = 0;
};

// POC: APS5_POC_NO_GRAPHICS_REGISTRY=1 resolves vertex and pixel image/sampler handles on the CPU
// again (the registry probes run per pixel otherwise).
inline bool GpuResolvedStage(IrShaderStage stage) {
    static const bool noGraphics = std::getenv("APS5_POC_NO_GRAPHICS_REGISTRY") != nullptr;
    static const bool noCompute = std::getenv("APS5_POC_NO_COMPUTE_REGISTRY") != nullptr;
    if (noCompute && stage == IrShaderStage::Compute) return false;
    if (noGraphics) return stage == IrShaderStage::Compute;
    return stage == IrShaderStage::Compute || stage == IrShaderStage::Vertex || stage == IrShaderStage::Pixel;
}

// Buffer and address handles read from guest memory are resolved on the GPU (BDA) in compute only:
// graphics draws have no BDA page table.
// TEMPORARY switches (POC): vertex and pixel buffer/address handles stay on the CPU path (v8
// behaviour; per-pixel BDA resolution cost GTA Story 5.2 vs 8.7 FPS once GPU-bound) unless
// APS5_POC_GRAPHICS_BDA=1; APS5_POC_NO_VERTEX_PULL keeps the vertex stage on the CPU path alone.
inline bool PocNoGraphicsBda() {
    static const bool off = std::getenv("APS5_POC_GRAPHICS_BDA") == nullptr;
    return off;
}

inline bool PocNoVertexPull() {
    static const bool off = std::getenv("APS5_POC_NO_VERTEX_PULL") != nullptr;
    return off;
}

inline bool GpuResolvedBufferStage(IrShaderStage stage) {
    static const bool noComputeBda = std::getenv("APS5_POC_NO_COMPUTE_BDA") != nullptr;
    if (stage == IrShaderStage::Compute) return !noComputeBda;
    if (PocNoGraphicsBda()) return false;
    if (stage == IrShaderStage::Vertex) return !PocNoVertexPull();
    return stage == IrShaderStage::Pixel;
}

inline std::atomic<std::uint64_t>& RegistryHandles() {
    static std::atomic<std::uint64_t> handles{0};
    return handles;
}

inline State& Global() {
    static auto* state = new State();
    return *state;
}

inline std::uint64_t Generation() {
    return Global().generation.load(std::memory_order_acquire);
}

inline bool AddImage(const std::array<std::uint32_t, 8>& words) {
    auto& state = Global();
    std::lock_guard lock(state.mutex);
    if (state.images.size() >= MaxImages || !state.imageSet.insert(words).second) return false;
    state.images.push_back(words);
    state.addedImages.fetch_add(1, std::memory_order_relaxed);
    state.generation.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

inline bool AddSampler(const std::array<std::uint32_t, 4>& words) {
    auto& state = Global();
    std::lock_guard lock(state.mutex);
    if (state.samplers.size() >= MaxSamplers || !state.samplerSet.insert(words).second) return false;
    state.samplers.push_back(words);
    state.addedSamplers.fetch_add(1, std::memory_order_relaxed);
    state.generation.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

inline void RegisterModeSet(std::uint64_t key, const ImageResource& image, std::span<const ImageResource> modes) {
    auto& state = Global();
    std::lock_guard lock(state.mutex);
    if (state.modeSets.contains(key)) return;
    state.modeSets.emplace(key, ModeSet{image, std::vector<ImageResource>(modes.begin(), modes.end())});
    state.pendingModeSets.push_back(key);
}

inline void Report() {
    auto& state = Global();
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10;
    auto previous = state.lastReport.load();
    if (now == previous || !state.lastReport.compare_exchange_strong(previous, now)) return;
    std::size_t images = 0;
    std::size_t samplers = 0;
    std::size_t modeSets = 0;
    std::uint64_t builds = 0;
    std::uint64_t zeroCpu = 0;
    std::uint64_t binds = 0;
    std::uint64_t commandBuffers = 0;
    {
        std::lock_guard lock(state.mutex);
        images = state.images.size();
        samplers = state.samplers.size();
        modeSets = state.modeSets.size();
        builds = state.builds.load() - state.reportedBuilds;
        zeroCpu = state.zeroCpuBuilds.load() - state.reportedZeroCpu;
        binds = state.binds.load() - state.reportedBinds;
        commandBuffers = state.commandBuffers.load() - state.reportedCommandBuffers;
        state.reportedBuilds += builds;
        state.reportedZeroCpu += zeroCpu;
        state.reportedBinds += binds;
        state.reportedCommandBuffers += commandBuffers;
    }
    std::fprintf(stderr, "[gpu-reg3] registry: %zu images, %zu samplers, %llu elements, %zu mode sets in %llu regions (%llu records, %llu refused); feedback drained %llu (added %llu images, %llu samplers, %llu samplers refused); refreshed %llu; handles planned on the GPU %llu; last 10 s: %llu draws/dispatches prepared, %llu with zero CPU descriptor work, %llu registry binds over %llu command buffers (%.1f per command buffer)\n",
        images, samplers, static_cast<unsigned long long>(state.elements.load()), modeSets, static_cast<unsigned long long>(state.regions.load()), static_cast<unsigned long long>(state.records.load()), static_cast<unsigned long long>(state.failedRecords.load()),
        static_cast<unsigned long long>(state.drained.load()), static_cast<unsigned long long>(state.addedImages.load()), static_cast<unsigned long long>(state.addedSamplers.load()), static_cast<unsigned long long>(state.rejected.load()),
        static_cast<unsigned long long>(state.refreshed.load()), static_cast<unsigned long long>(RegistryHandles().load()),
        static_cast<unsigned long long>(builds), static_cast<unsigned long long>(zeroCpu), static_cast<unsigned long long>(binds), static_cast<unsigned long long>(commandBuffers), commandBuffers != 0u ? static_cast<double>(binds) / static_cast<double>(commandBuffers) : 0.0);
}

struct ProbeResult {
    std::uint32_t code = 0;
    bool found = false;
    bool reported = false;
};

// CPU mirror of the shader probe (registry_probe_image / registry_probe_sampler).
inline ProbeResult ProbeMirror(std::span<std::uint32_t> map, const std::uint32_t* words, std::uint32_t count, bool sampler, bool exec) {
    const auto length = static_cast<std::uint32_t>(map.size());
    const auto at = [&](std::uint32_t index) { return index < length ? map[index] : map[0]; };
    const auto slotsStart = at(sampler ? HeaderSamplerSlots : HeaderImageSlots);
    const auto capacity = at(sampler ? HeaderSamplerCapacity : HeaderImageCapacity);
    const auto ringStart = at(sampler ? HeaderSamplerRing : HeaderImageRing);
    const bool usable = at(HeaderMarker) == Marker && capacity != 0u && (capacity & (capacity - 1u)) == 0u && slotsStart + capacity * SlotWords <= length;
    ProbeResult result;
    if (usable) {
        const auto hash = Hash(words, count);
        for (std::uint32_t probe = 0; probe < Probes && !result.found; probe++) {
            const auto base = slotsStart + ((hash + probe) & (capacity - 1u)) * SlotWords;
            const auto code = at(base + SlotCodeWord);
            bool match = code != 0u;
            for (std::uint32_t dword = 0; dword < count; dword++) match = match && at(base + dword) == words[dword];
            if (match) {
                result.found = true;
                result.code = code;
            }
        }
    }
    if (usable && !result.found && exec && ringStart + RingWords <= length) {
        const auto index = map[ringStart]++;
        if (index < RingEntries) {
            const auto entry = ringStart + 1u + index * RingEntryWords;
            for (std::uint32_t dword = 0; dword < count; dword++) map[entry + dword] = words[dword];
            result.reported = true;
        }
    }
    return result;
}

}

#endif
