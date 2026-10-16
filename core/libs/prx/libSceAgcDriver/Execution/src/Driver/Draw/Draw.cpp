#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <set>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace AgcDriver::DriverDetail {

namespace {

// POC (on by default, APS5_POC_NO_DRAW_THREAD=1 disables): queue 0's draws are recorded on a thread of their own while the queue
// worker prepares the next packet (decode, capture, shader lookup run unlocked; the recording takes
// the GPU mutex). At most APS5_POC_DRAW_THREAD_DEPTH (default 6) draws are in flight; every packet other than a draw, a register write
// or a WAIT_REG_MEM, the end of a submission and any draw the worker records itself wait for them
// (DrainDrawPipeline).
std::size_t DrawThreadDepth() {
    static const std::size_t depth = [] { const char* text = std::getenv("APS5_POC_DRAW_THREAD_DEPTH"); return text != nullptr ? std::max<std::size_t>(1, std::strtoull(text, nullptr, 10)) : std::size_t{6}; }();
    return depth;
}

class DrawPipeline {
public:
    void Push(std::function<void()> job) {
        std::unique_lock lock(mutex);
        if (!thread.joinable()) thread = std::thread([this] { run(); });
        space.wait(lock, [&] { return jobs.size() + running < DrawThreadDepth(); });
        jobs.push_back(std::move(job));
        ++inFlight;
        ready.notify_one();
    }
    void Drain() {
        if (inFlight.load(std::memory_order_acquire) == 0) return;
        std::unique_lock lock(mutex);
        idle.wait(lock, [&] { return jobs.empty() && running == 0; });
    }

private:
    void run() {
        PinWorkerThread("draw thread");
        if (std::getenv("APS5_SAMPLE_DRAW_THREAD") != nullptr) StartWorkerSampler();
        std::unique_lock lock(mutex);
        for (;;) {
            ready.wait(lock, [&] { return !jobs.empty(); });
            auto job = std::move(jobs.front());
            jobs.pop_front();
            running = 1;
            lock.unlock();
            job();
            // POC: the job's captures (snapshots, shared objects) are released on a reclaim thread, not
            // on the draw thread (APS5_POC_NO_JOB_RECLAIM=1 releases them here).
            static const bool reclaim = std::getenv("APS5_POC_NO_JOB_RECLAIM") == nullptr;
            if (reclaim) {
                std::lock_guard graveyard(reclaimMutex);
                if (!reclaimThread.joinable()) reclaimThread = std::thread([this] { reclaimLoop(); });
                reclaimed.push_back(std::move(job));
                reclaimReady.notify_one();
            }
            lock.lock();
            running = 0;
            --inFlight;
            space.notify_all();
            if (jobs.empty()) idle.notify_all();
        }
    }

    void reclaimLoop() {
        std::unique_lock lock(reclaimMutex);
        for (;;) {
            reclaimReady.wait(lock, [&] { return !reclaimed.empty(); });
            auto batch = std::move(reclaimed);
            reclaimed.clear();
            lock.unlock();
            batch.clear();
            lock.lock();
        }
    }

    std::mutex reclaimMutex;
    std::condition_variable reclaimReady;
    std::vector<std::function<void()>> reclaimed;
    std::thread reclaimThread;
    std::mutex mutex;
    std::condition_variable ready, space, idle;
    std::deque<std::function<void()>> jobs;
    std::size_t running = 0;
    std::atomic<std::size_t> inFlight{0};
    std::thread thread;
};

DrawPipeline& drawPipeline() {
    static auto* pipeline = new DrawPipeline();
    return *pipeline;
}

}

namespace {

class PrepPool {
public:
    void Start(std::size_t count) {
        std::lock_guard lock(mutex);
        if (!threads.empty()) return;
        for (std::size_t index = 0; index < count; ++index) threads.emplace_back([this] { run(); });
    }
    void Submit(std::function<void()> job) {
        std::lock_guard lock(mutex);
        jobs.push_back(std::move(job));
        ready.notify_one();
    }

private:
    void run() {
        PinWorkerThread("draw prep");
        std::unique_lock lock(mutex);
        for (;;) {
            ready.wait(lock, [&] { return !jobs.empty(); });
            auto job = std::move(jobs.front());
            jobs.pop_front();
            lock.unlock();
            job();
            lock.lock();
        }
    }
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> threads;
};

PrepPool& prepPool() {
    static auto* pool = new PrepPool();
    return *pool;
}

struct DrawTurns {
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t issued = 0;
    std::uint64_t finished = 0;
};

DrawTurns& drawTurns() {
    static auto* turns = new DrawTurns();
    return *turns;
}

// The ticket of the preparation running on this thread, and whether it holds the turn.
thread_local std::uint64_t currentTicket = 0;
thread_local bool ticketActive = false;
thread_local bool ticketServed = false;

}

std::size_t ParallelPrepThreads() {
    static const std::size_t threads = [] { const char* text = std::getenv("APS5_POC_PARALLEL_PREP"); return text != nullptr ? static_cast<std::size_t>(std::strtoull(text, nullptr, 10)) : std::size_t{3}; }();
    return DrawThreadEnabled() ? threads : 0;
}

std::uint64_t TakeDrawTicket() {
    auto& turns = drawTurns();
    std::lock_guard lock(turns.mutex);
    return turns.issued++;
}

void ServeDrawTurn() {
    if (!ticketActive || ticketServed) return;
    auto& turns = drawTurns();
    std::unique_lock lock(turns.mutex);
    turns.changed.wait(lock, [&] { return turns.finished == currentTicket; });
    ticketServed = true;
}

void SubmitDrawPrep(std::uint64_t ticket, std::function<void()> job) {
    auto& pool = prepPool();
    pool.Start(ParallelPrepThreads());
    pool.Submit([ticket, job = std::move(job)] {
        currentTicket = ticket;
        ticketActive = true;
        ticketServed = false;
        try {
            job();
        } catch (const std::exception& error) {
            static std::atomic<std::uint64_t> failures{0};
            if (failures.fetch_add(1) < 50) std::fprintf(stderr, "[draw-prep] draw failed: %s\n", error.what());
        }
        ServeDrawTurn();
        ticketActive = false;
        auto& turns = drawTurns();
        {
            std::lock_guard lock(turns.mutex);
            ++turns.finished;
        }
        turns.changed.notify_all();
    });
}

bool LabelJobsEnabled() {
    static const bool enabled = DrawThreadEnabled() && std::getenv("APS5_POC_NO_LABEL_JOBS") == nullptr;
    return enabled;
}

std::atomic<std::uint64_t>& PendingLabelJobs() {
    static std::atomic<std::uint64_t> pending{0};
    return pending;
}

namespace {
struct PendingLabel {
    std::uint64_t address = 0;
    std::size_t size = 0;
    std::uint64_t value = 0;
};
struct PendingLabels {
    std::mutex mutex;
    std::deque<PendingLabel> labels;
};
PendingLabels& pendingLabels() {
    static auto* labels = new PendingLabels();
    return *labels;
}
}

void NotePendingLabel(std::uint64_t address, std::span<const std::byte> bytes, bool add) {
    auto& pending = pendingLabels();
    std::lock_guard lock(pending.mutex);
    if (add) {
        PendingLabel label{address, bytes.size(), 0};
        std::memcpy(&label.value, bytes.data(), std::min<std::size_t>(bytes.size(), sizeof(label.value)));
        pending.labels.push_back(label);
        return;
    }
    const auto found = std::find_if(pending.labels.begin(), pending.labels.end(), [&](const PendingLabel& label) { return label.address == address && label.size == bytes.size(); });
    if (found != pending.labels.end()) pending.labels.erase(found);
}

PendingLabelWait CheckPendingLabelWait(std::span<const std::uint32_t> packet) {
    if (PendingLabelJobs().load(std::memory_order_acquire) == 0 || packet.size() < 4) return PendingLabelWait::None;
    const std::uint64_t awaited = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
    const auto awaitedBytes = Pm4::WaitAwaitedBytes(packet);
    auto& pending = pendingLabels();
    std::lock_guard lock(pending.mutex);
    for (auto label = pending.labels.rbegin(); label != pending.labels.rend(); ++label) {
        if (label->address >= awaited + awaitedBytes || awaited >= label->address + label->size) continue;
        if (label->address != awaited || label->size < awaitedBytes) return PendingLabelWait::Overlaps;
        const auto value = awaitedBytes == 8 ? label->value : (label->value & 0xffffffffu);
        return Pm4::WaitComparesValue(packet, value) ? PendingLabelWait::Ordered : PendingLabelWait::Overlaps;
    }
    return PendingLabelWait::None;
}

void DrainDrawWork() {
    if (ParallelPrepThreads() != 0) {
        auto& turns = drawTurns();
        std::unique_lock lock(turns.mutex);
        turns.changed.wait(lock, [&] { return turns.finished == turns.issued; });
    }
    DrainDrawPipeline();
}

bool DrawThreadEnabled() {
    static const bool enabled = std::getenv("APS5_POC_NO_DRAW_THREAD") == nullptr;
    return enabled;
}

bool DispatchThreadEnabled() {
    static const bool enabled = DrawThreadEnabled() && std::getenv("APS5_POC_DISPATCH_THREAD") != nullptr;
    return enabled;
}

void PushDrawPipeline(std::function<void()> job) {
    drawPipeline().Push(std::move(job));
}

void DrainDrawPipeline() {
    if (DrawThreadEnabled()) drawPipeline().Drain();
}

DrawVerdict Driver::draw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::string& rejected) {
    PerformanceTimer timing("Driver.Draw");
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    std::array<double, DrawDriverPhaseCount> phaseMs{};
    std::uint64_t captures = 0;
    auto phaseLap = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    DrawPhaseTiming phaseTiming{profile, phaseMs, phaseLap};
    if (profile && packetStartedAt() != std::chrono::steady_clock::time_point{}) phaseMs[DrawRowPrologue] = std::chrono::duration<double, std::milli>(phaseLap - packetStartedAt()).count();

    const auto drawn = [&] {
        phaseTiming.Phase(DrawRowVectors);
        if (!profile) return DrawVerdict::Drawn;
        auto& pending = pendingDrawPhases();
        pending.phases = true;
        pending.captures = captures;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
        return DrawVerdict::Drawn;
    };
    auto drawParameters = Pm4::ResolveDraw(packet, queue);
    bool traceIndirect = false;
    if (const auto verdict = precheckDraw(queue, submission, packet, drawParameters, rejected, traceIndirect)) return *verdict;
    phaseTiming.Phase(DrawRowPrecheck);
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;

    static const std::uint64_t dumpTarget = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SHADERS"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();

    static const std::uint64_t dumpSlot1 = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SLOT1"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();

    static const bool lockedPrepare = std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr;
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    std::shared_ptr<VulkanDevice> localDevice;
    if (lockedPrepare) {

        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        timing.Mark("gpu_mutex_wait");
        phaseTiming.Phase(DrawRowLockWait);
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(DrawRowLabels);
    } else if ((localDevice = device.Load()) == nullptr) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        std::lock_guard createLock(GuestMemory::GpuMutex());
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;
    }
    timing.Mark("device_setup");
    phaseTiming.Phase(DrawRowVectors);

    const bool useDrawEntries = drawEntries() && !ShaderRecompiler::DebugProbeActive() && dumpTarget == 0 && dumpSlot1 == 0;
    const bool registerKey = useDrawEntries && registerKeyEnabled();
    std::uint64_t drawKey = 0;
    std::shared_ptr<DrawEntry> entry;
    std::shared_ptr<const DrawDecode> decode;
    if (registerKey) {
        const auto keyStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        drawKey = drawRegisterKey(queue, *submission.shaders, localDevice->Serial());
        std::lock_guard cacheLock(drawCacheMutex);
        ++drawEntryCounters.lookups;
        ++drawEntryCounters.registerKeyLookups;
        if (profile) drawEntryCounters.keyUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - keyStart).count();
        const auto found = drawCache.find(drawKey);
        if (found != drawCache.end()) {
            entry = found->second;
            decode = entry->decode;
        } else {
            ++drawEntryCounters.absent;
        }
    }
    if (decode != nullptr && !DecodeProgramsCurrent(*decode, *submission.shaders)) {
        std::lock_guard cacheLock(drawCacheMutex);
        eraseDrawEntry(drawKey, entry);
        ++drawEntryCounters.absent;
        entry = nullptr;
        decode = nullptr;
    }
    phaseTiming.Phase(DrawRowKeyLookupValidate);

    resolveDrawDecode(queue, submission, decode, registerKey, drawKey, profile);
    const auto& graphics = decode->state;
    auto pixel = decode->pixel;
    std::vector<DrawProgram> programs = decode->programs;
    if (const auto skipped = SkippedShader(); skipped != 0 && std::any_of(programs.begin(), programs.end(), [skipped](const DrawProgram& program) { return program.binary.codeAddress == skipped; })) return drawn();
    const auto setMeshIndexBuffer = [&](const Pm4::DrawParameters& parameters) {
        if (!graphics.stages.mesh) return;
        auto& words = programs.front().userData;
        require(programs.front().firstUserSgpr == 0 && words.size() >= ShaderRecompiler::MeshIndexBufferUserWord + 4, "mesh program lacks the hidden user words");
        const auto descriptor = Graphics::MeshIndexBufferDescriptor(parameters);
        std::copy(descriptor.begin(), descriptor.end(), words.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    };
    if (!drawParameters.indirect) setMeshIndexBuffer(drawParameters);
    else if (graphics.stages.mesh) setMeshIndexBuffer(Pm4::DrawParameters{drawParameters.indexAddress, std::max(drawParameters.indexCount, 1u), drawParameters.indexSize, 1, 0, drawParameters.indexed});
    const std::vector<Role>& roles = decode->roles;
    phaseTiming.Phase(DrawRowDecode);

    const auto locate = [&](std::uint32_t location) -> std::optional<std::pair<std::size_t, std::size_t>> {
        if (location == 0x280u) return std::nullopt;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (roles[i] == Role::Fragment || roles[i] == Role::GeometryBack || location < programs[i].userDataBase) continue;
            const auto word = location - programs[i].userDataBase + (8u - programs[i].firstUserSgpr);
            if (word < programs[i].userData.size()) return std::make_pair(i, static_cast<std::size_t>(word));
        }
        return std::nullopt;
    };
    if (drawParameters.indirect) {
        auto& indirect = *drawParameters.indirect;
        const auto sgprOf = [&](std::uint32_t location) -> std::int32_t {
            const auto word = locate(location);
            if (!word || word->first != 0) return -1;
            return static_cast<std::int32_t>(programs.front().firstUserSgpr + word->second);
        };
        indirect.baseVertexSgpr = sgprOf(indirect.baseVertexLocation);
        indirect.startInstanceSgpr = sgprOf(indirect.startInstanceLocation);
        indirect.drawIndexSgpr = sgprOf(indirect.drawIndexLocation);
    }
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    std::vector<ShaderRecompiler::LinkedProgram> linked;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        for (const auto& region : program.memory) {
            if (!region.bytes.empty()) memory.push_back(region);
        }
        linked.push_back({roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
    }
    timing.Mark("prepare");
    phaseTiming.Phase(DrawRowProgramPrepare);

    std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>> vertexInfos(programs.size());
    std::vector<std::vector<Graphics::DecodeRead>> decodeReads(programs.size());
    const auto decodeVertexInfo = [&](std::size_t i) {
        const auto& program = programs[i];
        if (program.binary.stage == Stage::Fragment || roles[i] == Role::GeometryBack) return;
        decodeReads[i].clear();
        vertexInfos[i] = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, &decodeReads[i], false, 8u - program.firstUserSgpr);
        if (vertexInfos[i]) vertexInfos[i]->paClVsOutCntl = PositionExportLayout(program.snapshot.get(), program.codeOffset, program.binary.code, readRegister(queue.context, 0x207)) | graphics.layerExports;
    };
    if (!registerKey) {
        for (std::size_t i = 0; i < programs.size(); ++i) decodeVertexInfo(i);
        phaseTiming.Phase(DrawRowDecode);
    }
    ShaderMemory shaderMemory(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
    std::vector<ShaderRecompiler::RecompileResult> results;
    std::vector<Graphics::CompiledShader> stages;
    results.reserve(programs.size() + 2u);
    stages.reserve(programs.size());
    std::uint32_t pushCursorBytes = 0;

    std::vector<const ShaderRecompiler::RecompileResult*> programResults(programs.size(), nullptr);

    std::vector<StageCapture> stageCaptures(programs.size());
    std::vector<std::shared_ptr<DispatchVariant>> matched(programs.size());
    std::vector<std::vector<ShaderRecompiler::MemoryRegion>> matchedRegions(programs.size());

    std::vector<std::shared_ptr<DispatchVariant>> fresh(programs.size());

    std::vector<bool> recompiled(programs.size(), false);
    bool drawHit = false;
    bool verifyHit = false;
    lookupDraw(submission, localDevice, graphics, pixel, programs, roles, vertexInfos, useDrawEntries, registerKey, profile, drawKey, entry, matched, matchedRegions, drawHit, verifyHit, phaseTiming, phaseMs);

    if (registerKey) {

        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (matched[i] != nullptr && drawHit && !verifyDrawRecipe()) {
                if (matched[i]->vertexInfo != nullptr) vertexInfos[i] = *matched[i]->vertexInfo;
                continue;
            }
            decodeVertexInfo(i);
            if (matched[i] != nullptr && verifyDrawRecipe() && (vertexInfos[i].has_value() != (matched[i]->vertexInfo != nullptr) || (vertexInfos[i] && !sameVertexInfo(*vertexInfos[i], *matched[i]->vertexInfo)))) {
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit has a vertex stage info unlike its variant's\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress));
                std::lock_guard cacheLock(drawCacheMutex);
                ++drawEntryCounters.verifyDecodeMismatches;
            }
        }
        phaseTiming.Phase(DrawRowDecode);
    }

    const auto fold = [&](const ShaderRecompiler::RecompileResult& main, Pm4::DrawParameters& parameters) { FoldDrawOffsets(main, programs.front(), parameters); };

    std::optional<Graphics::IndirectDrawPath> indirectCpu;
    std::vector<std::uint32_t> pushOffsets(programs.size(), 0);
    std::vector<std::size_t> resultIndex(programs.size(), 0);
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (roles[i] == Role::GeometryBack) continue;
        const auto& program = programs[i];
        pushCursorBytes = Graphics::StagePushOffset(pushCursorBytes, program.binary.stage, localDevice->GraphicsPipelineLibraries());
        pushOffsets[i] = pushCursorBytes;
        if (drawHit) {

            programResults[i] = matched[i]->compiled.get();
            memory.insert(memory.end(), matchedRegions[i].begin(), matchedRegions[i].end());
        } else {
            resultIndex[i] = results.size();
            if (program.binary.stage == ShaderRecompiler::ShaderStage::Fragment) {
                for (auto previous = i; previous-- > 0;) {
                    if (programResults[previous] == nullptr) continue;
                    pixel = Graphics::DefaultUnexportedInputs(decode->pixel, programResults[previous]->parameterExports);
                    break;
                }
            }
            results.push_back(materializeDrawStage(i, pushCursorBytes, queue, submission, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected));
            if (!rejected.empty()) return DrawVerdict::Rejected;
            programResults[i] = &results.back();
        }
        const auto& result = *programResults[i];
        if (i == 0 && drawParameters.indirect) {
            indirectCpu = ClassifyIndirectDraw(result, graphics, programs.front(), localDevice, drawParameters, traceIndirect);
        } else if (i == 0) {
            fold(result, drawParameters);
        }
        const auto slotEnd = (pushCursorBytes / Graphics::PipelinePushSlotBytes + 1u) * Graphics::PipelinePushSlotBytes;
        require(result.pushConstants.size() <= slotEnd - pushCursorBytes, "stage push constants exceed the pipeline push constant block");
        stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushCursorBytes});
        pushCursorBytes += static_cast<std::uint32_t>(result.pushConstants.size());
    }

    cacheDrawStages(useDrawEntries, drawHit, drawParameters, indirectCpu, programs, stageCaptures, vertexInfos, decodeReads, verifyHit, matched, fresh, drawKey, registerKey, decode, phaseTiming);
    timing.Mark("shader_compile_and_link");

    for (const auto& reads : decodeReads) {
        for (const auto& read : reads) memory.push_back({read.address, std::as_bytes(std::span(read.bytes))});
    }

    if (recordQueuedLabelsAfterCapture(submission.queue, memory)) return draw(queue, packet, submission, rejected);

    bool rectListBuilt = false;

    std::size_t rectIndex = 0;
    const auto buildRectList = [&] {
        phaseTiming.Phase(DrawRowVectors);
        require(programs.size() == 2 && programResults[0] != nullptr && programResults[1] != nullptr, "rect-list requires vertex and fragment programs");
        auto rectangle = DrawRectangle(*programs[0].snapshot, programs[1].snapshot, programResults[0]->variantId, programResults[1]->variantId, localDevice->Target());
        if (rectListBuilt) {
            results[rectIndex] = std::move(rectangle.control);
            results[rectIndex + 1] = std::move(rectangle.evaluation);
            phaseTiming.Phase(DrawRowRectList);
            return;
        }
        require(stages.size() == 2, "rect-list requires vertex and fragment programs");
        rectIndex = results.size();
        results.push_back(std::move(rectangle.control));
        results.push_back(std::move(rectangle.evaluation));
        stages.insert(stages.begin() + 1, {{Stage::TessellationControl, &results[rectIndex], 0}, {Stage::TessellationEvaluation, &results[rectIndex + 1], 0}});
        rectListBuilt = true;
        phaseTiming.Phase(DrawRowRectList);
    };
    if (graphics.rectList) buildRectList();
    if (!programs.empty() && programResults.back() != nullptr && programResults.back()->barycentricEmulation.active) {
        require(!graphics.rectList && graphics.stages.path == Graphics::ShaderPath::Vertex && programs.size() == 2 && programResults[0] != nullptr && stages.size() == 2, "a pixel shader that reads barycentrics without VK_KHR_fragment_shader_barycentric needs a vertex shader before it");
        results.push_back(ShaderRecompiler::BuildBarycentricGeometryShader(*programResults[0], *programResults[1], localDevice->Target(), localDevice->GeometryLimits()));
        stages.insert(stages.begin() + 1, Graphics::CompiledShader{Stage::Geometry, &results.back(), 0});
    }
    std::vector<Graphics::GuestMemorySnapshot> snapshots;
    const auto snapshot = [&] {
        snapshots.clear();
        for (const auto& region : memory) snapshots.push_back({region.guestAddress, region.bytes});
    };
    snapshot();
    timing.Mark("post_compile_prepare");
    const auto lockForDraw = [&] {
        if (gpuLock.owns_lock()) return;
        ServeDrawTurn();
        DrainDrawPipeline();
        phaseTiming.Phase(DrawRowVectors);
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        timing.Mark("gpu_mutex_wait");
        phaseTiming.Phase(DrawRowLockWait);

        if (auto current = device.Load(); current != nullptr && current != localDevice) {
            static std::atomic<std::uint64_t> replaced{0};
            std::fprintf(stderr, "[draw] device replaced during unlocked preparation (%llu)\n", static_cast<unsigned long long>(++replaced));
            localDevice = std::move(current);
        }

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(DrawRowLabels);
    };
    if (drawParameters.indirect && indirectCpu) {

        const auto indirect = *drawParameters.indirect;
        if (drawHit) {

            for (std::size_t i = 0; i < programs.size(); ++i) {
                if (programResults[i] == nullptr) continue;
                resultIndex[i] = results.size();
                results.push_back(ShaderRecompiler::RecompileResult(*programResults[i]));
                for (auto& stage : stages) {
                    if (stage.program == programResults[i]) stage.program = &results[resultIndex[i]];
                }
                programResults[i] = &results[resultIndex[i]];
            }
        }
        recordQueuedLabelsBeforeRead(submission.queue);
        const auto readStart = std::chrono::steady_clock::now();
        const auto count = std::min(indirect.countIndirect ? Pm4::ReadDrawCount(indirect) : indirect.count, indirect.count);
        std::vector<Pm4::DrawArguments> records;
        for (std::uint32_t record = 0; record < count; ++record) records.push_back(Pm4::ReadDrawArguments(indirect, record));
        Graphics::CountIndirectDraw(*indirectCpu, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
        const auto baseVertexWord = locate(indirect.baseVertexLocation);
        const auto startInstanceWord = locate(indirect.startInstanceLocation);
        const auto drawIndexWord = indirect.drawIndexEnabled ? locate(indirect.drawIndexLocation) : std::nullopt;
        for (std::uint32_t record = 0; record < records.size(); ++record) {
            const auto& arguments = records[record];
            if (traceIndirect) std::fprintf(stderr, "[draw]   record %u: count %u instances %u first %u vertexOffset %u startInstance %u\n", record, arguments.count, arguments.instances, arguments.firstVertexOrIndex, arguments.vertexOffset, arguments.firstInstance);
            if (arguments.count == 0 || arguments.instances == 0) continue;
            std::set<std::size_t> patched;
            const auto patch = [&](const std::optional<std::pair<std::size_t, std::size_t>>& word, std::uint32_t value) {
                if (!word) return;
                programs[word->first].userData[word->second] = value;
                patched.insert(word->first);
            };
            patch(baseVertexWord, indirect.recordBytes == 20 ? arguments.vertexOffset : arguments.firstVertexOrIndex);
            patch(startInstanceWord, arguments.firstInstance);
            patch(drawIndexWord, record);
            Pm4::DrawParameters direct{0, arguments.count, 0, arguments.instances, drawParameters.flags, drawParameters.indexed, 0, 0};
            if (drawParameters.indexed) {

                if (arguments.firstVertexOrIndex >= drawParameters.indexCount) continue;
                direct.indexAddress = drawParameters.indexAddress + static_cast<std::uint64_t>(arguments.firstVertexOrIndex) * drawParameters.indexSize;
                direct.indexCount = std::min(arguments.count, drawParameters.indexCount - arguments.firstVertexOrIndex);
                direct.indexSize = drawParameters.indexSize;
            } else {
                direct.firstVertex = indirect.indxOffset;
            }
            if (graphics.stages.mesh) {
                setMeshIndexBuffer(direct);
                patched.insert(0);
            }
            for (const auto programIndex : patched) {
                auto& result = results[resultIndex[programIndex]];
                const auto pushBytes = result.pushConstants.size();
                decodeVertexInfo(programIndex);
                result = materializeDrawStage(programIndex, pushOffsets[programIndex], queue, submission, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected);
                if (!rejected.empty()) return DrawVerdict::Rejected;
                require(result.pushConstants.size() == pushBytes, "patched program changed its push constant layout");
            }
            fold(*programResults[0], direct);
            if (graphics.rectList && patched.contains(0)) buildRectList();
            snapshot();
            if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
                rejected = std::move(*known);
                return DrawVerdict::Rejected;
            }
            lockForDraw();
            noteDrawWriters(stages, submission.queue);
            phaseTiming.Phase(DrawRowVectors);
            localDevice->Draw(graphics, direct, stages, snapshots);
            phaseTiming.Phase(DrawRowGraphics);
        }
        timing.Mark("draw_and_resource_release");
        return drawn();
    }

    std::vector<std::shared_ptr<DispatchVariant>> recipeStages;
    if (registerKey && !drawParameters.indirect && Graphics::DrawRecipes()) {
        recipeStages.reserve(programs.size());
        for (std::size_t i = 0; i < programs.size(); ++i) recipeStages.push_back(drawHit ? matched[i] : fresh[i]);
        if (std::all_of(recipeStages.begin(), recipeStages.end(), [](const std::shared_ptr<DispatchVariant>& variant) { return variant == nullptr; })) recipeStages.clear();
    }
    std::shared_ptr<const DrawRecipe> recipe;
    if (drawHit && !recipeStages.empty()) {
        recipe = findDrawRecipe(drawKey, recipeStages);
        if (recipe == nullptr) VulkanDevice::NoteDrawRecipeMiss(VulkanDevice::DrawRecipePrecheck::NoRecipe);
    }
    if (recipe == nullptr) {
        if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
            rejected = std::move(*known);
            return DrawVerdict::Rejected;
        }
    }
    if (DrawThreadEnabled() && submission.queue == 0 && !gpuLock.owns_lock() && deferredLabels().labels.empty()) {
        noteDrawWriters(stages, submission.queue);
        struct Job {
            std::shared_ptr<VulkanDevice> device;
            std::shared_ptr<const DrawDecode> decode;
            Pm4::DrawParameters parameters;
            std::vector<ShaderRecompiler::RecompileResult> results;
            std::vector<std::shared_ptr<DispatchVariant>> matched;
            std::vector<Graphics::CompiledShader> stages;
            std::vector<std::vector<std::byte>> bytes;
            std::vector<Graphics::GuestMemorySnapshot> snapshots;
            std::shared_ptr<const DrawRecipe> recipe;
            std::vector<std::shared_ptr<DispatchVariant>> recipeStages;
            std::uint64_t drawKey = 0;
            std::uint64_t epoch = 0;
            std::uint32_t queue = 0;
        };
        auto job = std::make_shared<Job>();
        job->device = localDevice;
        job->decode = decode;
        job->parameters = drawParameters;
        job->results = std::move(results);
        job->matched = matched;
        job->stages = stages;
        job->bytes.reserve(snapshots.size());
        job->snapshots.reserve(snapshots.size());
        for (const auto& item : snapshots) {
            job->bytes.emplace_back(item.bytes.begin(), item.bytes.end());
            job->snapshots.push_back({item.address, std::span<const std::byte>(job->bytes.back())});
        }
        job->recipe = recipe;
        job->recipeStages = recipeStages;
        job->drawKey = drawKey;
        job->epoch = GuestMemory::ThreadCollectEpoch();
        job->queue = submission.queue;
        ServeDrawTurn();
        drawPipeline().Push([this, job] {
            std::shared_lock deviceUse(deviceReplacement);
            GuestMemory::AdoptCollectEpoch(job->epoch);
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
            std::unique_lock lock(GuestMemory::GpuMutex());
            auto localDevice = job->device;
            if (auto current = device.Load(); current != nullptr && current != localDevice) localDevice = std::move(current);
            try {
                recordLabelsForPacket(localDevice.get(), job->queue);
                if (job->recipe != nullptr) {
                    if (localDevice->DrawFromRecipe(job->decode->state, job->parameters, job->stages, job->snapshots, job->recipe) == RecipeOutcome::Recorded) return;
                    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
                }
                std::shared_ptr<const DrawRecipe> built;
                localDevice->Draw(job->decode->state, job->parameters, job->stages, job->snapshots, job->recipeStages.empty() ? nullptr : &built);
                if (built != nullptr) attachDrawRecipe(job->drawKey, job->recipeStages, std::move(built));
            } catch (const std::exception& error) {
                static std::atomic<std::uint64_t> failures{0};
                if (failures.fetch_add(1) < 50) std::fprintf(stderr, "[draw-thread] draw failed: %s\n", error.what());
            }
        });
        timing.Mark("draw_and_resource_release");
        return drawn();
    }
    lockForDraw();
    noteDrawWriters(stages, submission.queue);
    phaseTiming.Phase(DrawRowVectors);
    if (recipe != nullptr) {
        if (localDevice->DrawFromRecipe(graphics, drawParameters, stages, snapshots, recipe) == RecipeOutcome::Recorded) {
            phaseTiming.Phase(DrawRowGraphics);
            timing.Mark("draw_and_resource_release");
            return drawn();
        }

        VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
    }
    std::shared_ptr<const DrawRecipe> built;
    localDevice->Draw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &built);
    phaseTiming.Phase(DrawRowGraphics);
    if (built != nullptr) attachDrawRecipe(drawKey, recipeStages, std::move(built));
    timing.Mark("draw_and_resource_release");
    return drawn();
}

}
