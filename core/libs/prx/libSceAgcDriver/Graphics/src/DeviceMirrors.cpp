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

// What the write tracking knows of [begin, begin + bytes): the newest stamp of a watched range (after
// collecting its pending writes), else the newest write-watch hint of an unwatched import; 0 when
// neither tracks it.
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
    // Per 64 KiB block: the stamp it was last uploaded under (0: never), and whether recorded GPU work
    // wrote the guest bytes since.
    std::vector<std::uint64_t> uploaded;
    std::vector<std::uint8_t> written;
    std::uint64_t rangeStamp = 0;
    bool anyWritten = false;
    std::chrono::steady_clock::time_point lastUse = std::chrono::steady_clock::now();
    ~DeviceMirror();
};

namespace {

struct Mirrors {
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    std::map<std::uint64_t, std::weak_ptr<DeviceMirror>> live;
    std::map<std::uint64_t, std::shared_ptr<DeviceMirror>> held;
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> count{0};
    std::atomic<bool> any{false};
    DeviceMirrorCounts totals;
    DeviceMirrorCounts reported;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

Mirrors& mirrors() {
    static auto* state = new Mirrors();
    return *state;
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
            // What the mirrors hold now counts as theirs; the rest of the budget is shared with
            // everything else: the mirrors take at most half of what is free.
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
    if (t.assigned == r.assigned && t.uploads == r.uploads && t.skippedBudget == r.skippedBudget && t.skippedLarge == r.skippedLarge && t.skippedUntracked == r.skippedUntracked && state.count.load() == 0) return;
    std::fprintf(stderr, "[mirror] %llu device mirrors, %.1f MiB; last 10 s: %llu ranges served from mirrors, %llu uploads %.1f MiB, write-backs none (stores write through), %llu blocks staled by GPU writes, %llu evictions; left on the import: %llu untracked, %llu past the size limit, %llu past the budget\n",
        static_cast<unsigned long long>(state.count.load()), static_cast<double>(state.bytes.load()) / 1048576.0,
        static_cast<unsigned long long>(t.assigned - r.assigned), static_cast<unsigned long long>(t.uploads - r.uploads), static_cast<double>(t.uploadedBytes - r.uploadedBytes) / 1048576.0,
        static_cast<unsigned long long>(t.gpuInvalidations - r.gpuInvalidations), static_cast<unsigned long long>(t.evictions - r.evictions),
        static_cast<unsigned long long>(t.skippedUntracked - r.skippedUntracked), static_cast<unsigned long long>(t.skippedLarge - r.skippedLarge), static_cast<unsigned long long>(t.skippedBudget - r.skippedBudget));
    state.reported = t;
}

// Drops the least recently used held mirrors until `needed` more bytes fit `limit`; a dropped mirror
// lives on in the spaces and batches still holding it.
void makeRoom(Mirrors& state, std::uint64_t needed, std::uint64_t limit) {
    while (state.bytes.load() + needed > limit && !state.held.empty()) {
        auto victim = std::min_element(state.held.begin(), state.held.end(), [](const auto& left, const auto& right) { return left.second->lastUse < right.second->lastUse; });
        state.held.erase(victim);
        ++state.totals.evictions;
        if (state.bytes.load() + needed <= limit) break;
        // Still referenced by a space in use: stop evicting more than the policy needs to.
        if (std::none_of(state.held.begin(), state.held.end(), [](const auto& entry) { return entry.second.use_count() == 1; })) break;
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

std::vector<std::shared_ptr<DeviceMirror>> AssignDeviceMirrors(const Context& context, std::span<ShaderRecompiler::BdaAbi::Range> ranges, std::span<const DeviceMirrorSource> sources) {
    std::vector<std::shared_ptr<DeviceMirror>> assigned;
    if (!DeviceMirrorsEnabled(context) || ranges.size() != sources.size()) return assigned;
    static const auto largest = mib("APS5_DEVICE_MIRROR_MAX_MIB", 512ull * 1024ull * 1024ull);
    auto& state = mirrors();
    std::lock_guard lock(state.mutex);
    if (state.device != context.device) {
        state.held.clear();
        state.live.clear();
        state.device = context.device;
    }
    const auto limit = budget(context);
    // Smaller ranges first: the budget goes to as many distinct structures as it can hold.
    std::vector<std::size_t> order(ranges.size());
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) { return ranges[left].end - ranges[left].begin < ranges[right].end - ranges[right].begin; });
    const auto now = std::chrono::steady_clock::now();
    for (const auto index : order) {
        auto& range = ranges[index];
        const auto& source = sources[index];
        if (source.buffer == VK_NULL_HANDLE || range.hostAddress != 0 || (range.begin % 4u) != 0u) continue;
        const auto bytes = range.end - range.begin;
        if (bytes > largest) {
            ++state.totals.skippedLarge;
            continue;
        }
        std::shared_ptr<DeviceMirror> mirror;
        if (const auto found = state.live.find(range.begin); found != state.live.end()) {
            mirror = found->second.lock();
            if (mirror != nullptr && mirror->end != range.end) mirror.reset();
        }
        if (mirror == nullptr) {
            if (!tracked(range.begin, bytes)) {
                ++state.totals.skippedUntracked;
                continue;
            }
            if (state.bytes.load() + bytes > limit) makeRoom(state, bytes, limit);
            if (state.bytes.load() + bytes > limit) {
                ++state.totals.skippedBudget;
                continue;
            }
            mirror = std::make_shared<DeviceMirror>();
            mirror->begin = range.begin;
            mirror->end = range.end;
            try {
                mirror->buffer = std::make_unique<Buffer>(context, static_cast<std::size_t>(bytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            } catch (const std::exception&) {
                ++state.totals.skippedBudget;
                continue;
            }
            const auto blocks = static_cast<std::size_t>((bytes + BlockBytes - 1) / BlockBytes);
            mirror->uploaded.assign(blocks, 0);
            mirror->written.assign(blocks, 0);
            state.bytes.fetch_add(bytes);
            state.count.fetch_add(1);
            state.live[range.begin] = mirror;
        }
        {
            std::lock_guard mirrorLock(mirror->mutex);
            mirror->source = source;
            mirror->lastUse = now;
        }
        state.held[range.begin] = mirror;
        range.hostAddress = range.deviceAddress;
        range.deviceAddress = mirror->buffer->DeviceAddress();
        ++state.totals.assigned;
        assigned.push_back(std::move(mirror));
    }
    state.any.store(!state.live.empty(), std::memory_order_release);
    report(state);
    return assigned;
}

void RefreshDeviceMirrors(const Context& context, Recorder& recorder, std::span<const std::shared_ptr<DeviceMirror>> mirrorsToRefresh) {
    if (mirrorsToRefresh.empty()) return;
    auto& state = mirrors();
    std::uint64_t uploads = 0;
    std::uint64_t uploadedBytes = 0;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    for (const auto& mirror : mirrorsToRefresh) {
        std::lock_guard lock(mirror->mutex);
        mirror->lastUse = std::chrono::steady_clock::now();
        const auto bytes = mirror->end - mirror->begin;
        const auto rangeStamp = stampOf(mirror->begin, bytes);
        const bool never = mirror->rangeStamp == 0;
        if (!never && rangeStamp == mirror->rangeStamp && !mirror->anyWritten) continue;
        std::vector<VkBufferCopy> copies;
        for (std::size_t block = 0; block < mirror->uploaded.size(); ++block) {
            const auto begin = mirror->begin + block * BlockBytes;
            const auto size = std::min<std::uint64_t>(BlockBytes, mirror->end - begin);
            const auto stamp = never ? rangeStamp : stampOf(begin, size);
            if (mirror->uploaded[block] != 0 && mirror->uploaded[block] == stamp && mirror->written[block] == 0) continue;
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
    if (uploads != 0) {
        std::lock_guard lock(state.mutex);
        state.totals.uploads += uploads;
        state.totals.uploadedBytes += uploadedBytes;
        report(state);
    }
}

void NoteDeviceMirrorGpuWrite(std::uint64_t begin, std::uint64_t end) {
    auto& state = mirrors();
    if (!state.any.load(std::memory_order_acquire) || end <= begin) return;
    std::vector<std::shared_ptr<DeviceMirror>> hit;
    {
        std::lock_guard lock(state.mutex);
        auto found = state.live.upper_bound(begin);
        if (found != state.live.begin()) --found;
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

void ClearDeviceMirrors(VkDevice device) {
    auto& state = mirrors();
    std::lock_guard lock(state.mutex);
    if (state.device != device) return;
    state.held.clear();
    state.live.clear();
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
