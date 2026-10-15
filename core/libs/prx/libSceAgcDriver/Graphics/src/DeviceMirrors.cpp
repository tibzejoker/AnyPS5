#include "prx/libSceAgcDriver/Graphics/include/DeviceMirrors.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace AgcDriver::Graphics {

namespace {

constexpr std::uint64_t BlockBytes = 65536;

std::uint64_t mib(const char* name, std::uint64_t fallback) {
    const char* text = std::getenv(name);
    return text != nullptr ? std::strtoull(text, nullptr, 10) * 1024ull * 1024ull : fallback;
}

std::uint64_t chunkBytes() {
    static const auto bytes = std::max<std::uint64_t>(BlockBytes, mib("APS5_DEVICE_MIRROR_CHUNK_MIB", 4ull << 20u) / BlockBytes * BlockBytes);
    return bytes;
}

// Consecutive hot chunks of one import share a mirror (one table entry) up to this size: a lookup
// that changes entries takes the table search again, ~2.5x the time of a 16-entry 64 MiB structure
// read at random (agc_driver_bda_mirror_bench).
constexpr std::uint64_t RunBytes = 64ull << 20u;

// What the write tracking knows of [begin, begin + bytes), offset by one (0 never names a state):
// the newest stamp of a watched range after collecting its pending writes, else the newest write-watch
// hint of an unwatched import.
std::uint64_t stampOf(std::uint64_t begin, std::uint64_t bytes) {
    const auto size = static_cast<std::size_t>(bytes);
    if (GuestMemory::Watched(begin, size)) {
        static_cast<void>(GuestMemory::CollectWrites(begin, size));
        return GuestMemory::NewestStamp(begin, size) + 1u;
    }
    return GuestMemory::UntrackedHint(begin, size) + 1u;
}

bool tracked(std::uint64_t begin, std::uint64_t bytes) {
    const auto size = static_cast<std::size_t>(bytes);
    return GuestMemory::Watched(begin, size) || GuestMemory::Hinted(begin, size);
}

}

struct DeviceMirror {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::unique_ptr<Buffer> buffer;
    std::mutex mutex;
    DeviceMirrorSource source;
    // Per 64 KiB block: the stamp it was last uploaded under (0: never), and whether a GPU write the
    // driver reported staled it since.
    std::vector<std::uint64_t> uploaded;
    std::vector<std::uint8_t> written;
    std::uint64_t rangeStamp = 0;
    bool anyWritten = false;
    // The collect epoch (one per guest submission, GuestMemory::CollectEpoch) whose use last found
    // the mirror current.
    std::uint64_t checkedEpoch = 0;
    std::chrono::steady_clock::time_point lastUse = std::chrono::steady_clock::now();
    ~DeviceMirror();
};

namespace {

struct Mirrors {
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    // Mirrors still alive, by begin; a run that grew leaves its former mirror alive (overlapping the
    // new one) until the spaces and batches holding it let it go.
    std::multimap<std::uint64_t, std::weak_ptr<DeviceMirror>> live;
    std::uint64_t largest = 0;
    std::map<std::uint64_t, std::shared_ptr<DeviceMirror>> held;
    // The hot set: chunks BDA work resolved on its slow path, by begin (their end).
    std::map<std::uint64_t, std::uint64_t> wanted;
    // Hot chunks outside the write tracking, left on their import: touching them again does not
    // rebuild the space.
    std::map<std::uint64_t, std::uint64_t> untracked;
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> epoch{1};
    std::atomic<bool> any{false};
    bool epochPending = false;
    std::chrono::steady_clock::time_point lastEpoch{};
    DeviceMirrorCounts totals;
    DeviceMirrorCounts reported;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

Mirrors& mirrors() {
    static auto* state = new Mirrors();
    return *state;
}

void markWritten(std::uint64_t begin, std::uint64_t end) {
    NoteDeviceMirrorGpuWrite(begin, end);
}

// The space is rebuilt (its mirror set changes) at most every 50 ms; a change inside the window
// moves the epoch at the next call after it.
void moveEpoch(Mirrors& state, bool changed) {
    const auto now = std::chrono::steady_clock::now();
    if (changed) state.epochPending = true;
    if (!state.epochPending || now - state.lastEpoch < std::chrono::milliseconds(50)) return;
    state.epochPending = false;
    state.lastEpoch = now;
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
}

std::uint64_t budget(const Context& context) {
    static const auto fixed = mib("APS5_DEVICE_MIRROR_MIB", 0);
    if (fixed != 0) return fixed;
    std::uint64_t largest = 0;
    std::uint32_t heapIndex = 0;
    for (std::uint32_t heap = 0; heap < context.memory.memoryHeapCount; ++heap) {
        if ((context.memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u && context.memory.memoryHeaps[heap].size > largest) {
            largest = context.memory.memoryHeaps[heap].size;
            heapIndex = heap;
        }
    }
    auto limit = largest / 4u;
    if (context.memoryProperties2 != nullptr && context.physical != VK_NULL_HANDLE) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgets{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budgets};
        context.memoryProperties2(context.physical, &properties);
        const auto heapBudget = budgets.heapBudget[heapIndex];
        const auto usage = budgets.heapUsage[heapIndex];
        if (heapBudget != 0) {
            // What the mirrors hold now counts as theirs; of what is free they take at most half.
            const auto own = mirrors().bytes.load();
            const auto free = heapBudget > usage ? heapBudget - usage : 0;
            limit = std::min(limit, own + free / 2u);
        }
    }
    return limit;
}

void report(Mirrors& state) {
    const auto now = std::chrono::steady_clock::now();
    if (now - state.lastReport < std::chrono::seconds(10)) return;
    state.lastReport = now;
    const auto& t = state.totals;
    const auto& r = state.reported;
    if (t.assigned == r.assigned && t.uploads == r.uploads && t.touched == r.touched && state.count.load() == 0) return;
    std::fprintf(stderr, "[mirror] %llu device mirrors, %.1f MiB, hot set %zu chunks; last 10 s: %llu entries served from mirrors (of %llu chunks), %llu newly hot, %llu uploads %.1f MiB, write-backs none (stores write through), %llu blocks staled by GPU writes, %llu evictions, %llu stale checks (%llu skipped: checked in the submission); left on the import: %llu untracked, %llu past the budget\n",
        static_cast<unsigned long long>(state.count.load()), static_cast<double>(state.bytes.load()) / 1048576.0, state.wanted.size(),
        static_cast<unsigned long long>(t.assigned - r.assigned), static_cast<unsigned long long>(t.chunks - r.chunks), static_cast<unsigned long long>(t.touched - r.touched),
        static_cast<unsigned long long>(t.uploads - r.uploads), static_cast<double>(t.uploadedBytes - r.uploadedBytes) / 1048576.0,
        static_cast<unsigned long long>(t.gpuInvalidations - r.gpuInvalidations), static_cast<unsigned long long>(t.evictions - r.evictions),
        static_cast<unsigned long long>(t.checks - r.checks), static_cast<unsigned long long>(t.checksSkipped - r.checksSkipped),
        static_cast<unsigned long long>(t.skippedUntracked - r.skippedUntracked), static_cast<unsigned long long>(t.skippedBudget - r.skippedBudget));
    state.reported = t;
}

// Drops the least recently used held mirrors (and their chunks from the hot set) until `needed` more
// bytes fit `limit`; a dropped mirror lives on in the spaces and batches still holding it.
void makeRoom(Mirrors& state, std::uint64_t needed, std::uint64_t limit) {
    while (state.bytes.load() + needed > limit && !state.held.empty()) {
        auto victim = std::min_element(state.held.begin(), state.held.end(), [](const auto& left, const auto& right) { return left.second->lastUse < right.second->lastUse; });
        const bool freed = victim->second.use_count() == 1;
        state.wanted.erase(state.wanted.lower_bound(victim->second->begin), state.wanted.lower_bound(victim->second->end));
        state.held.erase(victim);
        ++state.totals.evictions;
        moveEpoch(state, true);
        if (!freed) break;
    }
}

}

DeviceMirror::~DeviceMirror() {
    if (buffer == nullptr) return;
    auto& state = mirrors();
    state.bytes.fetch_sub(end - begin);
    state.count.fetch_sub(1);
}

bool DeviceMirrorsEnabled(const Context& context) {
    static const bool off = std::getenv("APS5_NO_DEVICE_MIRRORS") != nullptr;
    if (off || context.memory.memoryHeapCount == 0) return false;
    // An integrated GPU (every heap device-local) reads guest memory where it lives.
    for (std::uint32_t heap = 0; heap < context.memory.memoryHeapCount; ++heap) {
        if ((context.memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0u) return true;
    }
    return false;
}

std::uint64_t DeviceMirrorEpoch() {
    return mirrors().epoch.load(std::memory_order_acquire);
}

std::vector<ShaderRecompiler::BdaAbi::Range> ChunkDeviceMirrors(const Context& context, std::vector<ShaderRecompiler::BdaAbi::Range> ranges, std::span<const DeviceMirrorSource> sources, std::vector<std::shared_ptr<DeviceMirror>>& held) {
    if (!DeviceMirrorsEnabled(context) || ranges.size() != sources.size()) return ranges;
    static const bool hooked = [] {
        GuestMemory::SetMarkWrittenHook(&markWritten);
        return true;
    }();
    static_cast<void>(hooked);
    auto& state = mirrors();
    std::lock_guard lock(state.mutex);
    if (state.device != context.device) {
        state.held.clear();
        state.live.clear();
        state.wanted.clear();
        state.untracked.clear();
        state.device = context.device;
    }
    const auto chunk = chunkBytes();
    const auto limit = budget(context);
    const auto now = std::chrono::steady_clock::now();
    std::vector<ShaderRecompiler::BdaAbi::Range> entries;
    entries.reserve(ranges.size());
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto& range = ranges[index];
        const auto& source = sources[index];
        if (source.buffer == VK_NULL_HANDLE || range.hostAddress != 0 || (range.begin % 4u) != 0u) {
            entries.push_back(range);
            continue;
        }
        const auto chunkEntry = [&](std::uint64_t begin, std::uint64_t end) {
            auto entry = range;
            entry.begin = begin;
            entry.end = end;
            entry.deviceAddress = range.deviceAddress + (begin - range.begin);
            return entry;
        };
        for (auto begin = range.begin; begin < range.end;) {
            const auto end = std::min(range.end, begin + chunk);
            ++state.totals.chunks;
            const auto hot = [&](std::uint64_t first, std::uint64_t last) {
                const auto found = state.wanted.find(first);
                return found != state.wanted.end() && found->second == last;
            };
            if (!hot(begin, end)) {
                entries.push_back(chunkEntry(begin, end));
                begin = end;
                continue;
            }
            // The run of consecutive hot chunks from here, one mirror.
            auto runEnd = end;
            while (runEnd < range.end && runEnd - begin < RunBytes) {
                const auto next = std::min(range.end, runEnd + chunk);
                if (!hot(runEnd, next)) break;
                ++state.totals.chunks;
                runEnd = next;
            }
            std::shared_ptr<DeviceMirror> mirror;
            for (auto [live, last] = state.live.equal_range(begin); live != last && mirror == nullptr; ++live) {
                auto candidate = live->second.lock();
                if (candidate != nullptr && candidate->end == runEnd) mirror = std::move(candidate);
            }
            if (mirror == nullptr && !tracked(begin, runEnd - begin)) {
                ++state.totals.skippedUntracked;
                for (auto at = state.wanted.lower_bound(begin); at != state.wanted.end() && at->first < runEnd;) {
                    state.untracked.insert(*at);
                    at = state.wanted.erase(at);
                }
            } else if (mirror == nullptr) {
                const auto bytes = runEnd - begin;
                if (state.bytes.load() + bytes > limit) makeRoom(state, bytes, limit);
                if (state.bytes.load() + bytes > limit) {
                    ++state.totals.skippedBudget;
                } else {
                    mirror = std::make_shared<DeviceMirror>();
                    mirror->begin = begin;
                    mirror->end = runEnd;
                    try {
                        mirror->buffer = std::make_unique<Buffer>(context, static_cast<std::size_t>(bytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                        const auto blocks = static_cast<std::size_t>((bytes + BlockBytes - 1) / BlockBytes);
                        mirror->uploaded.assign(blocks, 0);
                        mirror->written.assign(blocks, 0);
                        state.bytes.fetch_add(bytes);
                        state.count.fetch_add(1);
                        state.live.emplace(begin, mirror);
                        state.largest = std::max(state.largest, bytes);
                    } catch (const std::exception&) {
                        ++state.totals.skippedBudget;
                        mirror.reset();
                    }
                }
            }
            if (mirror == nullptr) {
                for (auto at = begin; at < runEnd; at = std::min(runEnd, at + chunk)) entries.push_back(chunkEntry(at, std::min(runEnd, at + chunk)));
                begin = runEnd;
                continue;
            }
            {
                std::lock_guard mirrorLock(mirror->mutex);
                mirror->source = {source.buffer, source.offset + (begin - range.begin)};
                mirror->lastUse = now;
            }
            // The run replaces the held mirrors it covers (a shorter run it grew from).
            state.held.erase(state.held.lower_bound(begin), state.held.lower_bound(runEnd));
            state.held[begin] = mirror;
            auto entry = chunkEntry(begin, runEnd);
            entry.hostAddress = entry.deviceAddress;
            entry.deviceAddress = mirror->buffer->DeviceAddress();
            ++state.totals.assigned;
            held.push_back(std::move(mirror));
            entries.push_back(entry);
            begin = runEnd;
        }
    }
    for (auto live = state.live.begin(); live != state.live.end();) live = live->second.expired() ? state.live.erase(live) : std::next(live);
    state.any.store(!state.live.empty(), std::memory_order_release);
    report(state);
    return entries;
}

void RefreshDeviceMirrors(const Context& context, Recorder& recorder, std::span<const std::shared_ptr<DeviceMirror>> mirrorsToRefresh) {
    if (mirrorsToRefresh.empty()) return;
    auto& state = mirrors();
    std::uint64_t uploads = 0;
    std::uint64_t uploadedBytes = 0;
    std::uint64_t checks = 0;
    std::uint64_t skipped = 0;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    const auto epoch = GuestMemory::CollectEpoch();
    const auto now = std::chrono::steady_clock::now();
    for (const auto& mirror : mirrorsToRefresh) {
        std::lock_guard lock(mirror->mutex);
        mirror->lastUse = now;
        // The guest bytes a submission's work reads are the ones the title wrote before submitting
        // it: one look per submission (collect epoch), unless a write the driver made or reported
        // (its own stores, GPU writes) staled blocks since.
        if (epoch != 0 && mirror->checkedEpoch == epoch && !mirror->anyWritten) {
            ++skipped;
            continue;
        }
        ++checks;
        mirror->checkedEpoch = epoch;
        const auto bytes = mirror->end - mirror->begin;
        const auto rangeStamp = stampOf(mirror->begin, bytes);
        const bool never = mirror->rangeStamp == 0;
        if (!never && rangeStamp == mirror->rangeStamp && !mirror->anyWritten) continue;
        std::vector<VkBufferCopy> copies;
        for (std::size_t block = 0; block < mirror->uploaded.size(); ++block) {
            const auto begin = mirror->begin + block * BlockBytes;
            const auto size = std::min<std::uint64_t>(BlockBytes, mirror->end - begin);
            // Unchanged range stamp: only the blocks GPU writes staled.
            if (!never && rangeStamp == mirror->rangeStamp && mirror->written[block] == 0) continue;
            const auto stamp = never ? rangeStamp : stampOf(begin, size);
            if (mirror->uploaded[block] == stamp && mirror->written[block] == 0) continue;
            mirror->uploaded[block] = stamp;
            mirror->written[block] = 0;
            if (!copies.empty() && copies.back().dstOffset + copies.back().size == begin - mirror->begin) copies.back().size += size;
            else copies.push_back({mirror->source.offset + (begin - mirror->begin), begin - mirror->begin, size});
        }
        mirror->rangeStamp = rangeStamp;
        mirror->anyWritten = false;
        if (copies.empty()) continue;
        if (commands == VK_NULL_HANDLE) {
            commands = recorder.Commands();
            // Earlier work's writes of the guest bytes (and earlier reads of the mirror) come first.
            const VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
            context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        }
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, mirror->source.buffer, mirror->buffer->Handle(), static_cast<std::uint32_t>(copies.size()), copies.data());
        recorder.Keep(mirror);
        for (const auto& copy : copies) uploadedBytes += copy.size;
        uploads += copies.size();
    }
    if (commands != VK_NULL_HANDLE) {
        const VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    }
    std::lock_guard lock(state.mutex);
    state.totals.uploads += uploads;
    state.totals.uploadedBytes += uploadedBytes;
    state.totals.checks += checks;
    state.totals.checksSkipped += skipped;
    moveEpoch(state, false);
    report(state);
}

void NoteDeviceMirrorGpuWrite(std::uint64_t begin, std::uint64_t end) {
    auto& state = mirrors();
    if (!state.any.load(std::memory_order_acquire) || end <= begin) return;
    std::vector<std::shared_ptr<DeviceMirror>> hit;
    {
        std::lock_guard lock(state.mutex);
        auto found = state.live.lower_bound(begin > state.largest ? begin - state.largest : 0);
        for (; found != state.live.end() && found->first < end; ++found) {
            auto mirror = found->second.lock();
            if (mirror != nullptr && mirror->begin < end && begin < mirror->end) hit.push_back(std::move(mirror));
        }
    }
    std::uint64_t staled = 0;
    for (const auto& mirror : hit) {
        std::lock_guard lock(mirror->mutex);
        const auto first = (std::max(begin, mirror->begin) - mirror->begin) / BlockBytes;
        const auto last = (std::min(end, mirror->end) - 1 - mirror->begin) / BlockBytes;
        for (auto block = first; block <= last && block < mirror->written.size(); ++block) {
            staled += mirror->written[block] == 0 ? 1u : 0u;
            mirror->written[block] = 1;
        }
        mirror->anyWritten = true;
    }
    if (staled != 0) {
        std::lock_guard lock(state.mutex);
        state.totals.gpuInvalidations += staled;
    }
}

void NoteDeviceMirrorTouched(std::uint64_t begin, std::uint64_t end) {
    auto& state = mirrors();
    if (state.device == VK_NULL_HANDLE || end <= begin) return;
    std::lock_guard lock(state.mutex);
    if (const auto refused = state.untracked.find(begin); refused != state.untracked.end() && refused->second == end) return;
    // A mirrored run's own entry: its chunks are hot already, under their own ends.
    if (const auto held = state.held.find(begin); held != state.held.end() && held->second->end == end) {
        held->second->lastUse = std::chrono::steady_clock::now();
        return;
    }
    const auto [entry, inserted] = state.wanted.try_emplace(begin, end);
    if (!inserted && entry->second == end) {
        if (auto held = state.held.upper_bound(begin); held != state.held.begin() && (--held)->second->end > begin) held->second->lastUse = std::chrono::steady_clock::now();
        return;
    }
    entry->second = end;
    ++state.totals.touched;
    moveEpoch(state, true);
}

void ClearDeviceMirrors(VkDevice device) {
    auto& state = mirrors();
    std::lock_guard lock(state.mutex);
    if (state.device != device) return;
    state.held.clear();
    state.live.clear();
    state.wanted.clear();
    state.untracked.clear();
    state.any.store(false);
    state.device = VK_NULL_HANDLE;
}

DeviceMirrorCounts DeviceMirrorCounters() {
    auto& state = mirrors();
    std::lock_guard lock(state.mutex);
    auto counts = state.totals;
    counts.mirrors = state.count.load();
    counts.mirroredBytes = state.bytes.load();
    return counts;
}

}
