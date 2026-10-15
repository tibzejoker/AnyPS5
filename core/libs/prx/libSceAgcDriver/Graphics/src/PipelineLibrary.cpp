#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <set>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <thread>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>

namespace AgcDriver::Graphics {

namespace {

constexpr std::array<VkDynamicState, 15> dynamicStates{
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_DEPTH_BIAS,
    VK_DYNAMIC_STATE_DEPTH_BOUNDS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE_EXT,
    VK_DYNAMIC_STATE_FRONT_FACE_EXT,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT,
    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,
    VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT,
    VK_DYNAMIC_STATE_STENCIL_OP_EXT,
};

using Key = std::vector<std::byte>;

struct DeviceLibraries {
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyPipelineLayout destroyLayout = nullptr;
    PFN_vkDestroyShaderModule destroyModule = nullptr;
    std::vector<VkShaderModule> modules;
    std::map<Key, VkPipelineLayout> layouts;
    std::array<std::map<Key, VkPipeline>, 4> parts;
    // Parts some thread is compiling now (another thread needing one waits for it, see below), and
    // parts a background prebuild made that no draw has used yet (the [pipelines] prebuilt hits).
    std::array<std::set<Key>, 4> compiling;
    std::array<std::set<Key>, 4> prebuilt;
    PipelineLibraryCounters counters;
};

// [pipelines], every 10 s: library parts compiled on draw threads (the draw waits for them) and in
// the background (registration prebuilds), with their compile time; draws that found a prebuilt part,
// and draws that waited for a part another thread was compiling.
struct PipelineTotals {
    std::array<std::uint64_t, 4> drawParts{};
    std::array<std::uint64_t, 4> backgroundParts{};
    double drawMs = 0;
    double backgroundMs = 0;
    double maxDrawMs = 0;
    std::uint64_t prebuiltHits = 0;
    std::uint64_t waits = 0;
    double waitMs = 0;
    std::uint64_t links = 0;
    double linkMs = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};

struct LibraryStore {
    std::mutex mutex;
    std::map<VkDevice, DeviceLibraries> devices;
    PipelineTotals totals;
    std::condition_variable compiled;
    std::condition_variable idle;
    std::condition_variable work;
    std::deque<std::pair<VkDevice, std::function<void()>>> jobs;
    std::map<VkDevice, std::size_t> pending;
    bool started = false;
};

LibraryStore& Libraries() {
    static auto* store = new LibraryStore();
    return *store;
}

void runJobs(LibraryStore& store) {
    std::unique_lock lock(store.mutex);
    for (;;) {
        store.work.wait(lock, [&] { return !store.jobs.empty(); });
        auto [device, job] = std::move(store.jobs.front());
        store.jobs.pop_front();
        lock.unlock();
        job();
        lock.lock();
        if (--store.pending[device] == 0) store.idle.notify_all();
    }
}

void enqueue(LibraryStore& store, VkDevice device, std::function<void()> job) {
    if (!store.started) {
        std::thread(runJobs, std::ref(store)).detach();
        store.started = true;
    }
    ++store.pending[device];
    store.jobs.emplace_back(device, std::move(job));
    store.work.notify_one();
}

void reportPipelines(PipelineTotals& totals) {
    const auto now = std::chrono::steady_clock::now();
    if (now - totals.last < std::chrono::seconds(10)) return;
    totals.last = now;
    const auto sum = [](const std::array<std::uint64_t, 4>& parts) { return parts[0] + parts[1] + parts[2] + parts[3]; };
    if (sum(totals.drawParts) + sum(totals.backgroundParts) + totals.prebuiltHits + totals.waits + totals.links == 0) return;
    std::fprintf(stderr, "[pipelines] last 10 s: library parts compiled on draw threads %llu (vertex input %llu, pre-rasterization %llu, fragment %llu, output %llu) in %.0f ms (longest %.0f ms), in the background %llu (vertex input %llu, pre-rasterization %llu, fragment %llu, output %llu) in %.0f ms; draws using a prebuilt part %llu; waits for a part compiling elsewhere %llu (%.0f ms); links %llu (%.0f ms)\n",
        static_cast<unsigned long long>(sum(totals.drawParts)), static_cast<unsigned long long>(totals.drawParts[0]), static_cast<unsigned long long>(totals.drawParts[1]), static_cast<unsigned long long>(totals.drawParts[2]), static_cast<unsigned long long>(totals.drawParts[3]), totals.drawMs, totals.maxDrawMs,
        static_cast<unsigned long long>(sum(totals.backgroundParts)), static_cast<unsigned long long>(totals.backgroundParts[0]), static_cast<unsigned long long>(totals.backgroundParts[1]), static_cast<unsigned long long>(totals.backgroundParts[2]), static_cast<unsigned long long>(totals.backgroundParts[3]), totals.backgroundMs,
        static_cast<unsigned long long>(totals.prebuiltHits), static_cast<unsigned long long>(totals.waits), totals.waitMs, static_cast<unsigned long long>(totals.links), totals.linkMs);
    const auto last = totals.last;
    totals = {};
    totals.last = last;
}

Key Combined(const Key& part, const Key& context) {
    Key key;
    const auto append = [&](const Key& value) {
        const auto size = value.size();
        const auto bytes = std::as_bytes(std::span(&size, 1));
        key.insert(key.end(), bytes.begin(), bytes.end());
        key.insert(key.end(), value.begin(), value.end());
    };
    append(part);
    append(context);
    return key;
}

}

std::span<const VkDynamicState> PipelineLibraryDynamicStates() {
    return dynamicStates;
}

VkPipeline LinkPipelineFromLibraries(const Context& context, const VkGraphicsPipelineCreateInfo& info, const VkPipelineRenderingCreateInfoKHR& rendering, const VkPipelineLayoutCreateInfo& layout, const PipelineLibraryKeys& keys, std::shared_ptr<OptimizedPipeline>* optimized, bool partsOnly, std::span<VkShaderModule> modules) {
    Require(info.renderPass == VK_NULL_HANDLE && info.pDynamicState != nullptr, "pipeline library parts need dynamic rendering and the library dynamic states");
    const bool split = (layout.flags & VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) != 0;
    Require(!split || ((layout.setLayoutCount == 2u || layout.setLayoutCount == 3u) && layout.pSetLayouts != nullptr && ((keys.preRasterizationSets | keys.fragmentSets) & ~3u) == 0u), "split pipeline library layout requires two descriptor set slots and a valid mask");
    const std::uint32_t registrySet = layout.setLayoutCount == 3u ? 4u : 0u;
    auto& store = Libraries();
    std::unique_lock lock(store.mutex);
    auto& device = store.devices[context.device];
    if (device.destroyPipeline == nullptr) {
        device.destroyPipeline = context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        device.destroyLayout = context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
        device.destroyModule = context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    }
    const auto layoutFor = [&](const Key& key, std::optional<std::uint32_t> mask) {
        auto found = device.layouts.find(key);
        if (found != device.layouts.end()) return found->second;
        auto info = layout;
        std::array<VkDescriptorSetLayout, 3> sets{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (mask.has_value()) {
            for (std::uint32_t set = 0; set < layout.setLayoutCount; ++set) {
                if (((*mask | registrySet) & (1u << set)) != 0u) sets[set] = layout.pSetLayouts[set];
            }
            info.pSetLayouts = sets.data();
        }
        VkPipelineLayout handle = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &info, nullptr, &handle), "vkCreatePipelineLayout library");
        return device.layouts.emplace(key, handle).first->second;
    };
    const auto fullLayout = layoutFor(keys.layout, std::nullopt);
    const std::array<VkPipelineLayout, 4> partLayouts{VK_NULL_HANDLE, split ? layoutFor(keys.preRasterizationLayout, keys.preRasterizationSets) : fullLayout, split ? layoutFor(keys.fragmentLayout, keys.fragmentSets) : fullLayout, VK_NULL_HANDLE};
    std::vector<VkPipelineShaderStageCreateInfo> preRasterization;
    const VkPipelineShaderStageCreateInfo* fragment = nullptr;
    bool mesh = false;
    for (std::uint32_t index = 0; index < info.stageCount; ++index) {
        const auto& stage = info.pStages[index];
        if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) fragment = &stage;
        else preRasterization.push_back(stage);
        mesh |= stage.stage == VK_SHADER_STAGE_MESH_BIT_EXT;
    }
    Require(fragment != nullptr && !preRasterization.empty(), "a pipeline built from libraries needs a fragment and a pre-rasterization stage");
    constexpr std::array<VkGraphicsPipelineLibraryFlagsEXT, 4> flags{
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
    };
    const std::array<const Key*, 4> partKeys{&keys.vertexInput, &keys.preRasterization, &keys.fragmentShader, &keys.fragmentOutput};
    const Key none;
    VkPipelineRenderingCreateInfoKHR shaderRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    shaderRendering.viewMask = rendering.viewMask;
    shaderRendering.depthAttachmentFormat = rendering.depthAttachmentFormat;
    shaderRendering.stencilAttachmentFormat = rendering.stencilAttachmentFormat;
    const std::array<std::uint32_t, 3> attachments{shaderRendering.viewMask, static_cast<std::uint32_t>(shaderRendering.depthAttachmentFormat), static_cast<std::uint32_t>(shaderRendering.stencilAttachmentFormat)};
    const auto attachmentBytes = std::as_bytes(std::span(attachments));
    const Key attachmentKey(attachmentBytes.begin(), attachmentBytes.end());
    const auto preRasterizationContext = Combined(keys.preRasterizationLayout, attachmentKey);
    const auto fragmentContext = Combined(keys.fragmentLayout, attachmentKey);
    const std::array<const Key*, 4> contextKeys{&none, &preRasterizationContext, &fragmentContext, &keys.renderPass};
    VkPipelineDepthStencilStateCreateInfo dynamicDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    static const bool backgroundLinking = std::getenv("APS5_NO_PIPELINE_LTO") == nullptr;
    const bool optimize = backgroundLinking && context.pipelineLinkTimeOptimization;
    std::vector<VkPipeline> libraries;
    for (std::size_t part = 0; part < flags.size(); ++part) {
        if (part == 0 && mesh) continue;
        const auto key = Combined(*partKeys[part], *contextKeys[part]);
        auto& cache = device.parts[part];
        // A part another thread is compiling (a prebuild, or a draw of another queue) is waited for,
        // not compiled again: the wait overlaps every other compile in flight.
        if (device.compiling[part].contains(key)) {
            const auto waitStart = std::chrono::steady_clock::now();
            store.compiled.wait(lock, [&] { return !device.compiling[part].contains(key); });
            if (!partsOnly) {
                ++store.totals.waits;
                store.totals.waitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
            }
        }
        if (const auto found = cache.find(key); found != cache.end()) {
            if (!partsOnly && device.prebuilt[part].erase(key) != 0u) ++store.totals.prebuiltHits;
            libraries.push_back(found->second);
            continue;
        }
        PerformanceTimer timing("Vulkan.GraphicsPipelineLibrary");
        VkGraphicsPipelineLibraryCreateInfoEXT library{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, flags[part] == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT ? &rendering : &shaderRendering};
        library.flags = flags[part];
        VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &library};
        create.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
        create.pDynamicState = info.pDynamicState;
        create.layout = partLayouts[part];
        switch (flags[part]) {
        case VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT:
            create.pVertexInputState = info.pVertexInputState;
            create.pInputAssemblyState = info.pInputAssemblyState;
            create.layout = VK_NULL_HANDLE;
            break;
        case VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT:
            create.stageCount = static_cast<std::uint32_t>(preRasterization.size());
            create.pStages = preRasterization.data();
            create.pViewportState = info.pViewportState;
            create.pRasterizationState = info.pRasterizationState;
            create.pTessellationState = info.pTessellationState;
            break;
        case VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT:
            create.stageCount = 1;
            create.pStages = fragment;
            create.pMultisampleState = info.pMultisampleState;
            create.pDepthStencilState = info.pDepthStencilState != nullptr ? info.pDepthStencilState : &dynamicDepth;
            break;
        default:
            create.pMultisampleState = info.pMultisampleState;
            create.pColorBlendState = info.pColorBlendState;
            create.layout = VK_NULL_HANDLE;
            break;
        }
        VkPipeline handle = VK_NULL_HANDLE;
        // Compiled without the store's lock (other parts compile meanwhile), marked as in flight.
        device.compiling[part].insert(key);
        lock.unlock();
        const auto compileStart = std::chrono::steady_clock::now();
        VkResult created;
        {
            auto pipelineCacheLock = context.LockPipelineCacheForUse();
            created = context.Function<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(context.device, context.pipelineCache, 1, &create, nullptr, &handle);
        }
        const auto compileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count();
        lock.lock();
        device.compiling[part].erase(key);
        store.compiled.notify_all();
        if (partsOnly) {
            ++store.totals.backgroundParts[part];
            store.totals.backgroundMs += compileMs;
        } else {
            ++store.totals.drawParts[part];
            store.totals.drawMs += compileMs;
            store.totals.maxDrawMs = std::max(store.totals.maxDrawMs, compileMs);
        }
        Check(created, "vkCreateGraphicsPipelines library");
        if (const auto raced = cache.find(key); raced != cache.end()) {
            device.destroyPipeline(context.device, handle, nullptr);
            libraries.push_back(raced->second);
            continue;
        }
        cache.emplace(key, handle);
        if (partsOnly) device.prebuilt[part].insert(key);
        if (optimize) {
            for (std::uint32_t stage = 0; stage < create.stageCount; ++stage) {
                for (auto& module : modules) {
                    if (module == VK_NULL_HANDLE || module != create.pStages[stage].module) continue;
                    device.modules.push_back(module);
                    module = VK_NULL_HANDLE;
                }
            }
        }
        ++device.counters.built[part];
        libraries.push_back(handle);
    }
    if (partsOnly) return VK_NULL_HANDLE;
    reportPipelines(store.totals);
    PerformanceTimer timing("Vulkan.GraphicsPipelineLink");
    const auto linkStart = std::chrono::steady_clock::now();
    VkPipelineLibraryCreateInfoKHR linked{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    linked.libraryCount = static_cast<std::uint32_t>(libraries.size());
    linked.pLibraries = libraries.data();
    VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &linked};
    create.layout = info.layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    lock.unlock();
    VkResult linkedResult;
    {
        auto pipelineCacheLock = context.LockPipelineCacheForUse();
        linkedResult = context.Function<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(context.device, context.pipelineCache, 1, &create, nullptr, &pipeline);
    }
    lock.lock();
    Check(linkedResult, "vkCreateGraphicsPipelines link");
    ++device.counters.linked;
    ++store.totals.links;
    store.totals.linkMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - linkStart).count();
    if (optimize && optimized != nullptr) {
        auto slot = std::make_shared<OptimizedPipeline>();
        slot->device = context.device;
        slot->destroy = device.destroyPipeline;
        *optimized = slot;
        const auto createPipelines = context.Function<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
        const auto cache = context.pipelineCache;
        const auto cacheSynchronization = context.pipelineCacheSynchronization;
        const auto libraryLayout = fullLayout;
        enqueue(store, context.device, [slot, createPipelines, cache, cacheSynchronization, libraryLayout, libraries, &store, devicePointer = &device] {
            if (slot->released.load()) return;
            VkPipelineLibraryCreateInfoKHR linked{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
            linked.libraryCount = static_cast<std::uint32_t>(libraries.size());
            linked.pLibraries = libraries.data();
            VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &linked};
            create.flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT;
            create.layout = libraryLayout;
            VkPipeline result = VK_NULL_HANDLE;
            const auto pipelineCreationResult = [&] {
                auto pipelineCacheLock = cacheSynchronization != nullptr ? cacheSynchronization->LockForPipelineUse() : PipelineCacheSynchronization::PipelineUseLock{};
                return createPipelines(slot->device, cache, 1, &create, nullptr, &result);
            }();
            if (pipelineCreationResult != VK_SUCCESS) return;
            slot->handle.store(result);
            if (slot->released.load()) {
                if (const auto handle = slot->handle.exchange(VK_NULL_HANDLE)) slot->destroy(slot->device, handle, nullptr);
            }
            std::lock_guard lock(store.mutex);
            ++devicePointer->counters.optimized;
        });
    }
    return pipeline;
}

void OptimizedPipeline::Release() noexcept {
    released.store(true);
    if (const auto pipeline = handle.exchange(VK_NULL_HANDLE)) destroy(device, pipeline, nullptr);
}

void WaitForOptimizedPipelines(VkDevice device) {
    auto& store = Libraries();
    std::unique_lock lock(store.mutex);
    store.idle.wait(lock, [&] { return store.pending[device] == 0; });
}

void ClearPipelineLibraries(VkDevice device) {
    auto& store = Libraries();
    std::unique_lock lock(store.mutex);
    store.idle.wait(lock, [&] { return store.pending[device] == 0; });
    store.compiled.wait(lock, [&] {
        const auto devices = store.devices.find(device);
        return devices == store.devices.end() || std::all_of(devices->second.compiling.begin(), devices->second.compiling.end(), [](const auto& parts) { return parts.empty(); });
    });
    const auto found = store.devices.find(device);
    if (found == store.devices.end()) return;
    auto& libraries = found->second;
    for (auto& part : libraries.parts) {
        for (const auto& [key, handle] : part) libraries.destroyPipeline(device, handle, nullptr);
    }
    for (const auto& [key, handle] : libraries.layouts) libraries.destroyLayout(device, handle, nullptr);
    for (const auto module : libraries.modules) libraries.destroyModule(device, module, nullptr);
    store.devices.erase(found);
}

PipelineLibraryCounters PipelineLibraryCountersOf(VkDevice device) {
    auto& store = Libraries();
    std::lock_guard lock(store.mutex);
    const auto found = store.devices.find(device);
    return found == store.devices.end() ? PipelineLibraryCounters{} : found->second.counters;
}

}
