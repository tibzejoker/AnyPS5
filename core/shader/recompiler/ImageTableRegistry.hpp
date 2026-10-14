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

namespace ShaderRecompiler::ImageTableRegistry {

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
    std::atomic<long long> lastReport{0};
};

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
    std::fprintf(stderr, "[gpu-texreg] registry: %zu images, %zu samplers, generation %llu; feedback entries drained %llu (added %llu images, %llu samplers, %llu rejected); registry table columns snapshotted %llu\n", images, samplers, static_cast<unsigned long long>(state.generation.load()), static_cast<unsigned long long>(state.drained.load()), static_cast<unsigned long long>(state.addedImages.load()), static_cast<unsigned long long>(state.addedSamplers.load()), static_cast<unsigned long long>(state.rejected.load()), static_cast<unsigned long long>(state.registryColumns.load()));
}

}

#endif
