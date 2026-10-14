#ifndef CORE_SHADER_RECOMPILER_IMAGETABLEREGISTRY_HPP
#define CORE_SHADER_RECOMPILER_IMAGETABLEREGISTRY_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <vector>

#include "IntermediateRepresentation/IrMetadata/ShaderStage.hpp"
#include "ImageTableAbi.hpp"

namespace ShaderRecompiler::ImageTableRegistry {

using ShaderRecompiler::IrShaderStage;

inline constexpr std::uint32_t SlotWords = 9u;
inline constexpr std::uint32_t SlotCodeWord = 8u;
inline constexpr std::uint32_t Probes = 8u;
inline constexpr std::uint32_t RingEntries = 256u;
inline constexpr std::uint32_t RingEntryWords = 8u;
inline constexpr std::uint32_t Marker = 0x52454749u;
inline constexpr std::uint32_t MaxEntries = 16384u;

inline std::uint32_t Hash(const std::uint32_t* words, std::uint32_t count) {
    std::uint32_t hash = 2166136261u;
    for (std::uint32_t index = 0; index < count; ++index) {
        hash ^= words[index];
        hash *= 16777619u;
    }
    return hash;
}

struct State {
    std::mutex mutex;
    std::vector<std::array<std::uint32_t, 8>> images;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::set<std::array<std::uint32_t, 8>> imageSet;
    std::set<std::array<std::uint32_t, 4>> samplerSet;
    std::atomic<std::uint64_t> generation{1};
    std::atomic<std::uint64_t> drained{0};
    std::atomic<std::uint64_t> addedImages{0};
    std::atomic<std::uint64_t> addedSamplers{0};
    std::atomic<std::uint64_t> registryColumns{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> faultedTables{0};
    std::atomic<std::uint64_t> insertedSlots{0};
    std::atomic<std::uint64_t> builtTables{0};
    std::atomic<long long> lastReport{0};
};

inline bool GpuResolvedStage(IrShaderStage stage) {
    return stage == IrShaderStage::Compute;
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
    if (state.images.size() >= MaxEntries || !state.imageSet.insert(words).second) return false;
    state.images.push_back(words);
    state.addedImages.fetch_add(1, std::memory_order_relaxed);
    state.generation.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

inline bool AddSampler(const std::array<std::uint32_t, 4>& words) {
    auto& state = Global();
    std::lock_guard lock(state.mutex);
    if (state.samplers.size() >= MaxEntries || !state.samplerSet.insert(words).second) return false;
    state.samplers.push_back(words);
    state.addedSamplers.fetch_add(1, std::memory_order_relaxed);
    state.generation.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

inline std::uint64_t Images(std::vector<std::array<std::uint32_t, 8>>& out) {
    auto& state = Global();
    std::lock_guard lock(state.mutex);
    out = state.images;
    return state.generation.load(std::memory_order_acquire);
}

inline std::uint64_t Samplers(std::vector<std::array<std::uint32_t, 4>>& out) {
    auto& state = Global();
    std::lock_guard lock(state.mutex);
    out = state.samplers;
    return state.generation.load(std::memory_order_acquire);
}

inline std::uint32_t Capacity(std::uint32_t entries) {
    std::uint32_t capacity = 64u;
    while (capacity < entries * 4u) capacity <<= 1u;
    return capacity;
}

struct RegistryEntry {
    const std::uint32_t* words = nullptr;
    std::uint32_t count = 0;
    std::uint32_t code = 0;
};

inline std::uint32_t BuildRegistryRegion(std::vector<std::uint32_t>& map, std::uint32_t header, const std::vector<RegistryEntry>& entries, bool sampler) {
    const auto capacity = Capacity(static_cast<std::uint32_t>(entries.size()));
    const auto slotsStart = static_cast<std::uint32_t>(map.size());
    map.resize(map.size() + static_cast<std::size_t>(capacity) * SlotWords, 0u);
    std::uint32_t inserted = 0;
    for (const auto& entry : entries) {
        if (entry.code == ImageTableAbi::NullCode || (entry.code & ImageTableAbi::PoisonFlag) != 0u) continue;
        const auto hash = Hash(entry.words, entry.count);
        for (std::uint32_t probe = 0; probe < Probes; probe++) {
            const auto slot = (hash + probe) & (capacity - 1u);
            const auto base = slotsStart + slot * SlotWords;
            if (map[base + SlotCodeWord] != ImageTableAbi::NullCode) continue;
            for (std::uint32_t dword = 0; dword < entry.count; dword++) map[base + dword] = entry.words[dword];
            map[base + SlotCodeWord] = entry.code;
            ++inserted;
            break;
        }
    }
    const auto ringStart = static_cast<std::uint32_t>(map.size());
    map.resize(map.size() + 1u + static_cast<std::size_t>(RingEntries) * RingEntryWords, 0u);
    map[header + ImageTableAbi::TableMapStart] = slotsStart;
    map[header + ImageTableAbi::TableKeyCount] = capacity;
    map[header + ImageTableAbi::TableSizeLow] = ringStart;
    map[header + ImageTableAbi::TableSizeHigh] = RingEntries;
    map[header + ImageTableAbi::TableBaseLow] = sampler ? 1u : 0u;
    map[header + ImageTableAbi::TableBaseHigh] = Marker;
    return inserted;
}

struct ProbeResult {
    std::uint32_t code = 0;
    bool found = false;
    bool reported = false;
};

inline std::uint32_t MapAt(const std::vector<std::uint32_t>& map, std::uint32_t index) {
    return index < map.size() ? map[index] : map[0];
}

inline ProbeResult ProbeMirror(std::vector<std::uint32_t>& map, std::uint32_t header, const std::uint32_t* words, std::uint32_t count, bool sampler, bool exec) {
    const auto length = static_cast<std::uint32_t>(map.size());
    const auto field = [&](std::uint32_t index) { return MapAt(map, header + index); };
    const auto slotsStart = field(ImageTableAbi::TableMapStart);
    const auto capacity = field(ImageTableAbi::TableKeyCount);
    const auto ringStart = field(ImageTableAbi::TableSizeLow);
    const auto ringCapacity = field(ImageTableAbi::TableSizeHigh);
    const auto outsideCode = field(ImageTableAbi::TableOutsideCode);
    const auto tableFault = field(ImageTableAbi::TableFault);
    std::uint32_t hash = 2166136261u;
    for (std::uint32_t dword = 0; dword < count; dword++) {
        hash ^= words[dword];
        hash *= 16777619u;
    }
    const bool powerOfTwo = (capacity & (capacity - 1u)) == 0u;
    const auto slotsEnd = slotsStart + capacity * SlotWords;
    const bool usable = powerOfTwo && capacity != 0u && slotsEnd <= length;
    const auto mask = (usable ? capacity : 1u) - 1u;
    ProbeResult result;
    std::uint32_t code = ImageTableAbi::NullCode;
    for (std::uint32_t probe = 0; probe < Probes; probe++) {
        const auto slot = (hash + probe) & mask;
        const auto base = slotsStart + slot * SlotWords;
        const auto slotCode = MapAt(map, base + SlotCodeWord);
        bool match = usable && slotCode != ImageTableAbi::NullCode;
        for (std::uint32_t dword = 0; dword < count; dword++) match = match && MapAt(map, base + dword) == words[dword];
        if (match && !result.found) code = slotCode;
        result.found = result.found || match;
    }
    const bool missed = !result.found && capacity != 0u;
    const bool ringValid = ringStart < length && ringCapacity <= RingEntries;
    if (missed && exec && ringValid) {
        const auto index = map[ringStart]++;
        if (index < ringCapacity) {
            const auto entry = ringStart + 1u + index * RingEntryWords;
            if (entry + RingEntryWords - 1u < length) {
                for (std::uint32_t dword = 0; dword < count; dword++) map[entry + dword] = words[dword];
                result.reported = true;
            }
        }
    }
    const auto missCode = sampler ? outsideCode : ImageTableAbi::NullCode;
    result.code = tableFault != 0u ? tableFault : (result.found ? code : missCode);
    return result;
}

inline void Report() {
    auto& state = Global();
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10;
    auto previous = state.lastReport.load();
    if (now == previous || !state.lastReport.compare_exchange_strong(previous, now)) return;
    std::size_t images = 0;
    std::size_t samplers = 0;
    {
        std::lock_guard lock(state.mutex);
        images = state.images.size();
        samplers = state.samplers.size();
    }
    std::fprintf(stderr, "[gpu-reg2] registry: %zu images, %zu samplers, generation %llu; feedback entries drained %llu (added %llu images, %llu samplers, %llu rejected); registry table columns snapshotted %llu; image/sampler handles planned on the GPU %llu; registry maps built %llu (with a table fault %llu), slots inserted %llu\n",images, samplers, static_cast<unsigned long long>(state.generation.load()), static_cast<unsigned long long>(state.drained.load()), static_cast<unsigned long long>(state.addedImages.load()), static_cast<unsigned long long>(state.addedSamplers.load()), static_cast<unsigned long long>(state.rejected.load()), static_cast<unsigned long long>(state.registryColumns.load()), static_cast<unsigned long long>(RegistryHandles().load()), static_cast<unsigned long long>(state.builtTables.load()), static_cast<unsigned long long>(state.faultedTables.load()), static_cast<unsigned long long>(state.insertedSlots.load()));
}

}

#endif
