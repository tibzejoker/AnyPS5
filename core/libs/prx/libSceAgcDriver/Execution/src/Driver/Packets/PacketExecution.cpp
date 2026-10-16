#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "ThreadOwned.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include <cstdlib>
#include <functional>
#include <shared_mutex>

namespace AgcDriver::DriverDetail {

template <typename TWork>
void Driver::timed(double WorkerProfile::*bucket, TWork&& work) {
    static thread_local WorkerProfile profile;
    const auto begin = std::chrono::steady_clock::now();
    work();
    const auto end = std::chrono::steady_clock::now();
    profile.*bucket += std::chrono::duration<double, std::milli>(end - begin).count();
    static const bool report = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (report && end - profile.reported > std::chrono::seconds(10)) {
        profile.reported = end;
        AgcDriver::ProfilePrint_nid_no_patch( "[gpu] worker at %.0f s: dispatch %.1f s, draw %.1f s, wait %.1f s\n", std::chrono::duration<double>(end - profile.start).count(), profile.dispatchMs / 1000, profile.drawMs / 1000, profile.waitMs / 1000);
    }
}

void Driver::execute(const Submission& submission) {
    auto* submissionTiming = includeTimingSubmission(submission, true);
    struct FlipHold {
        Driver& driver;
        bool held;
        void Release() {
            if (!held) return;
            held = false;
            driver.releaseFlipHold();
        }
        ~FlipHold() { Release(); }
    } flipHold{*this, submission.holdsFlip};
    if (submission.suspend) {
        PerformanceContext timingContext(submissionTiming);
        PerformanceTimer timing("Driver.Suspend");

        static const bool suspendDrain = std::getenv("APS5_SUSPEND_DRAIN") != nullptr;
        static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        ++suspendPoints;
        auto& costs = submissionCosts(submission.queue);

        if (suspendDrain || !deferredLabels().labels.empty() || Graphics::Recorder::PendingLabelSince().has_value() || Graphics::Recorder::RecordedWorkSinceSubmit() != 0) {
            const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            const auto localDevice = device.Load();
            recordDeferredLabels(localDevice.get(), submission.queue);
            if (localDevice != nullptr) {
                if (suspendDrain) localDevice->WaitIdle();
                else localDevice->SubmitRecorded(submission.queue == 0);
            }
            ++costs.suspends;
            if (profile) costs.suspendNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        } else {
            ++costs.suspendsSkipped;
        }
        resetGraphics = true;
        return;
    }
    QueueState* state = nullptr;
    {
        std::lock_guard lock(mutex);
        if (submission.queue == 0 && resetGraphics) {
            queues.erase(0);
            resetGraphics = false;
        }
        state = &queues[submission.queue];
    }
    auto& queue = *state;
    static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
    if (traceGpu) std::fprintf(stderr, "[gpu] %.1f execute serial=%llu queue=0x%x dwords=%zu\n", TraceMs(), static_cast<unsigned long long>(submission.serial), submission.queue, submission.commands.size());

    static const long dumpQueue = [] { const char* text = std::getenv("APS5_DUMP_QUEUE"); return text ? std::strtol(text, nullptr, 16) : -1L; }();
    if (static_cast<long>(submission.queue) == dumpQueue) {

        static int dumped = 0;
        if (dumped++ < 40) {
            std::string text = "[queue] submission " + std::to_string(submission.serial) + ":\n";
            for (std::size_t cursor = 0; cursor < submission.commands.size();) {
                const auto header = submission.commands[cursor];
                const auto count = Pm4::PacketWords(header);
                char line[200];
                int length = std::snprintf(line, sizeof(line), "[queue]   %s", Pm4::Name(header).c_str());
                for (std::size_t i = 1; i < count && i < 10 && length < 180; ++i) length += std::snprintf(line + length, sizeof(line) - length, " %08x", submission.commands[cursor + i]);
                text += line;
                text += "\n";
                cursor += count;
            }
            std::fputs(text.c_str(), stderr);
        }
    }
    PacketHistory recent{submission.commands};

    static const bool profilePackets = std::getenv("APS5_PROFILE_DRAW") != nullptr;

    thread_local PacketProfile* packetProfileSlot = nullptr;
    auto& packetProfile = ShaderRecompiler::ThreadOwned(packetProfileSlot);
    ++packetProfile.submissions;

    bumpEpoch(&EpochBumps::submissions);
    clearResolvedAhead();
    gateOpened() = true;
    struct ClearAhead {
        ~ClearAhead() { clearResolvedAhead(); }
    } clearAhead;
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        if (packetEpoch()) bumpEpoch(&EpochBumps::packets);
        CheckFailure();
        const auto header = submission.commands[cursor];
        if (Pm4::FillerPacket(header)) { ++cursor; continue; }
        const auto count = Pm4::PacketWords(header);
        const auto packet = std::span(submission.commands).subspan(cursor, count);
        const auto opcode = (header >> 8u) & 0xffu;
        auto nextCursor = cursor + count;
        {
            // POC: [activity] every 10 s, the draws and dispatches of all queues (benchmarks tell the game scene
            // from loading screens with it).
            static std::atomic<std::uint64_t> draws{0}, dispatches{0};
            static std::atomic<long long> shown{0};
            if (Pm4::DrawOpcode(opcode)) draws.fetch_add(1, std::memory_order_relaxed);
            else if (opcode == 0x15 || opcode == 0x16) dispatches.fetch_add(1, std::memory_order_relaxed);
            const auto window = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10;
            if (auto last = shown.load(std::memory_order_relaxed); last != window && shown.compare_exchange_strong(last, window)) {
                std::fprintf(stderr, "[activity] last 10 s: draws=%llu dispatches=%llu\n", static_cast<unsigned long long>(draws.exchange(0)), static_cast<unsigned long long>(dispatches.exchange(0)));
                std::fprintf(stderr, "[recipe-stages] %s\n", Graphics::RecipeStageStats().c_str());
                VulkanDevice::ReportRecipeLine();
            }
        }
        if (submission.queue == 0 && DrawThreadEnabled()) {
            // EVENT_WRITE (but a counter sample), ACQUIRE_MEM, PFP_SYNC_ME, SET_BASE and SET_UCONFIG_REG_INDEX do
            // nothing the draws in flight must precede; a RELEASE_MEM / WRITE_DATA label without an end-of-pipe
            // interrupt becomes a draw thread job (PacketMemory.cpp); a WAIT_REG_MEM on a pending label job is ordered
            // behind it when the pending value satisfies it, and drains otherwise.
            const bool quietEvent = LabelJobsEnabled() && ((opcode == 0x46 && packet.size() >= 2 && (packet[1] & 0x3fu) != 0x39u) || opcode == 0x58 || opcode == 0x42 || opcode == 0x11 || opcode == 0x7a || opcode == 0x59);
            const bool labelJob = LabelJobsEnabled() && (opcode == 0x37 || (opcode == 0x49 && packet.size() >= 3 && [&] { const auto select = (packet[2] >> 24u) & 7u; return select == 0 || select == 3; }())) && [&] {
                const auto label = Pm4::DecodeLabelWrite(packet);
                return label.has_value() && label->Bytes().size() <= DeferredLabel::Capacity && label->Bytes().size() % 4 == 0 && label->address % 4 == 0;
            }();
            const auto pendingWait = opcode == 0x3c && !Pm4::Predicated(header) ? CheckPendingLabelWait(packet) : PendingLabelWait::None;
            if (pendingWait == PendingLabelWait::Ordered) {
                cursor = nextCursor;
                continue;
            }
            const bool waitWithLabels = pendingWait == PendingLabelWait::Overlaps;
            const bool keepsDraws = !waitWithLabels && (quietEvent || labelJob || (opcode == 0x15 && DispatchThreadEnabled()) || Pm4::DrawOpcode(opcode) || Pm4::IndirectRegisterOpcode(opcode) || opcode == 0x69 || opcode == 0x76 || opcode == 0x79 || opcode == 0x68 || opcode == 0x10 || opcode == 0x2a || opcode == 0x2f || opcode == 0x26 || opcode == 0x13 || opcode == 0x3c);
            if (!keepsDraws || header == FlipPacketHeader || header == RenderingWaitPacketHeader) {
                // POC: which packets drain the draw work, and the time they wait ([drain] every 10 s).
                static std::array<std::atomic<std::uint64_t>, 257> counts{};
                static std::array<std::atomic<std::uint64_t>, 257> waitedUs{};
                static std::atomic<long long> reported{0};
                const auto slot = header == FlipPacketHeader ? 256u : opcode;
                const auto started = std::chrono::steady_clock::now();
                DrainDrawWork();
                counts[slot].fetch_add(1, std::memory_order_relaxed);
                waitedUs[slot].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
                const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10;
                if (auto last = reported.load(std::memory_order_relaxed); last != now && reported.compare_exchange_strong(last, now)) {
                    std::string line = "[drain] last 10 s by opcode (count/ms waited):";
                    for (std::size_t index = 0; index < counts.size(); ++index) {
                        const auto count = counts[index].exchange(0);
                        const auto us = waitedUs[index].exchange(0);
                        if (count == 0) continue;
                        char item[64];
                        std::snprintf(item, sizeof(item), " %s=%llu/%llu", index == 256 ? "flip" : Pm4::Name(static_cast<std::uint32_t>(index) << 8u).c_str(), static_cast<unsigned long long>(count), static_cast<unsigned long long>(us / 1000));
                        line += item;
                    }
                    std::fprintf(stderr, "%s\n", line.c_str());
                }
            }
        }
        if (APS5_ENABLE_TIMING_LOG && submission.queue == 0 && pendingFrameTiming == nullptr) includeTimingSubmission(submission, false);
        PerformanceContext timingContext(submission.queue == 0 ? pendingFrameTiming.get() : FrameTiming::Async());
        PerformanceTimer timing("Driver.Packet");
        std::shared_lock deviceUse(deviceReplacement, std::defer_lock);
        if (opcode != 0x3c && opcode != 0x93 && header != RenderingWaitPacketHeader && header != FlipPacketHeader) deviceUse.lock();

        timing.Mark("device_lock");
        GuestMemory::SetCurrentPacket(header == FlipPacketHeader ? 0xffffu : opcode, submission.queue);
        CaptureTrace::Log("packet submission=%llu queue=%x offset=%zu header=%08x words=%zu", static_cast<unsigned long long>(submission.serial), submission.queue, cursor, header, packet.size());
        if (Pm4::Predicated(header) && queue.predication.operation != 0) {
            landPendingWaits(submission.queue);
            recordQueuedLabelsBeforeRead(submission.queue);
            if (!Pm4::PredicationPasses(queue)) {
                cursor = opcode == 0x3f ? submission.conditionalEnds.at(cursor) : nextCursor;
                continue;
            }
        }
        if (opcode == 0x3f) {
            cursor = nextCursor;
            continue;
        }

        const auto flushStart = profilePackets ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        flushBetweenPackets(submission.queue, header, opcode == 0x49 || opcode == 0x37);
        timing.Mark("flush_between_packets");
        PacketTimer packetTimer{profilePackets, header == FlipPacketHeader ? 0xffffu : opcode, submission.queue, packetProfile, std::chrono::steady_clock::now()};

        if (profilePackets) {
            packetStartedAt() = packetTimer.start;
            pendingDispatchPhases() = {};
            pendingDrawPhases() = {};
        }
        const auto finishDispatchPacket = [&](bool indirect) {
            if (!profilePackets) return;
            const auto now = std::chrono::steady_clock::now();
            auto& pending = pendingDispatchPhases();
            if (pending.phases) {
                pending.ms[PhaseEpilogue] = std::chrono::duration<double, std::milli>(now - pending.tailAt).count();
                addDriverPhases(submission.queue != 0 ? OtherQueues : indirect ? Queue0Indirect : Queue0Direct, pending.ms, pending.hit, pending.validated);
            }
            if (indirect) return;
            auto& row = packetProfile.dispatchOutcomes[static_cast<std::size_t>(pending.outcome)];
            ++row.first;
            row.second += std::chrono::duration<double, std::milli>(now - packetTimer.start).count();
        };
        const auto finishDrawPacket = [&](bool drawn) {
            if (!profilePackets) return;
            const auto now = std::chrono::steady_clock::now();
            auto& pending = pendingDrawPhases();
            std::array<double, DrawDriverPhaseCount> ms{};
            if (drawn && pending.phases) {
                ms = pending.ms;
                ms[DrawRowEpilogue] = std::chrono::duration<double, std::milli>(now - pending.tailAt).count();
            } else {
                ms[DrawRowSkipped] = std::chrono::duration<double, std::milli>(now - packetTimer.start).count();
            }
            addDrawPhases(ms, drawn && pending.phases, pending.captures);
        };
        if (profilePackets) packetProfile.flushMs += std::chrono::duration<double, std::milli>(packetTimer.start - flushStart).count();

        bool wroteOnGpu = false, endOfPipeInterrupt = false, interruptDeferred = false, drawPacket = false, sampleDump = false;
        const bool drains = preparePacketMemory(submission, queue, packet, header, opcode, wroteOnGpu, endOfPipeInterrupt, interruptDeferred, drawPacket, sampleDump);
        traceLabel(packet, submission.queue);
        timing.Mark("prepare_memory");

        const bool waitPacket = opcode == 0x3c || opcode == 0x93 || header == RenderingWaitPacketHeader;
        struct Progress {
            Driver& driver;
            bool counted;
            ~Progress() {
                if (!counted) return;
                --driver.packetsInFlight;
                ++driver.packetsDone;
            }
        } progress{*this, !waitPacket};
        if (!waitPacket) ++packetsInFlight;
        recent.Record(cursor);
        if (header == RenderingWaitPacketHeader) {
            timed(&WorkerProfile::waitMs, [&] { submission.renderingWaits.at(cursor)->Wait(); });
        } else if (header == FlipPacketHeader) {
            CheckFailure();
            std::uint64_t batchesAtFlip = 0, unsignaledAtFlip = 0;
            bool flipInterruptDeferred = false;
            if (!drains) {

                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
                std::lock_guard gpuLock(GuestMemory::GpuMutex());
                const auto localDevice = device.Load();

                if (!recordLabelsForPacket(localDevice.get(), submission.queue) && localDevice != nullptr) localDevice->SubmitRecorded(submission.queue == 0);
                if (localDevice != nullptr) {
                    localDevice->FlipBatches(batchesAtFlip, unsignaledAtFlip);
                    const auto queueId = submission.queue;
                    flipInterruptDeferred = localDevice->AfterRecordedWork([queueId] { AgcDriverDeliverEopInterrupt(queueId); }, false);
                }
            }
            if (!flipInterruptDeferred) AgcDriverDeliverEopInterrupt(submission.queue);
            ++flipsCounted;
            if (batchesAtFlip != 0) flipSerial = batchesAtFlip;
            flipBatchesUnsignaled += unsignaledAtFlip;

            timing.Mark("flip_flush");
            timing.FinishPacket(submission.serial, cursor, 0xffffu, count);
            auto frame = APS5_ENABLE_TIMING_LOG ? std::move(pendingFrameTiming) : std::make_shared<FrameTiming>(++frameSerial);
            require(frame != nullptr, "flip has no frame timing");
            const auto now = FrameTiming::Clock::now();
            if (!APS5_ENABLE_TIMING_LOG) frame->IncludeSubmission(submission.serial, now, now, now, true);
            frame->SetFlip(submission.serial, cursor, APS5_ENABLE_TIMING_LOG ? submission.receivedAt : now, now);
            if (APS5_ENABLE_TIMING_LOG) frame->CollectBackground();
            frame->NoteFlipBatches(batchesAtFlip, unsignaledAtFlip);
            CaptureTrace::Log("flip frame=%llu submission=%llu offset=%zu batch=%llu unsignaled=%llu", static_cast<unsigned long long>(frameSerial), static_cast<unsigned long long>(submission.serial), cursor, static_cast<unsigned long long>(batchesAtFlip), static_cast<unsigned long long>(unsignaledAtFlip));
            flipHold.Release();
            submission.flips.at(cursor)->GpuReady(frame);
        } else if (opcode == 0x15 || opcode == 0x16) {
            landPendingWaits(submission.queue);
            currentPacketOffset() = cursor;
            if (gateOpened()) {
                gateOpened() = false;
                timed(&WorkerProfile::dispatchMs, [&] { resolveGroupAhead(submission, queue, cursor); });
            }
            if (opcode == 0x15) timed(&WorkerProfile::dispatchMs, [&] { dispatch(queue, packet, submission); });
            else timed(&WorkerProfile::dispatchMs, [&] { dispatchIndirect(queue, packet, submission); });
            Graphics::Recorder::CountRecordedWork();
            finishDispatchPacket(opcode == 0x16);
        } else if (opcode == 0x3c || opcode == 0x93) {
            static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
            const auto waitStart = std::chrono::steady_clock::now();
            timed(&WorkerProfile::waitMs, [&] { waitMemory(packet, submission.queue, recent, submission.received, submission.heldAtSubmit.contains(cursor)); });
            gateOpened() = true;
            if (traceGpu && std::chrono::steady_clock::now() - waitStart > std::chrono::milliseconds(200)) {

                for (std::size_t next = cursor + count, shown = 0; next < submission.commands.size() && shown < 48; ++shown) {
                    const auto nextHeader = submission.commands[next];
                    const auto nextCount = Pm4::PacketWords(nextHeader);
                    const auto nextPacket = std::span(submission.commands).subspan(next, nextCount);
                    std::fprintf(stderr, "[gpu]   then %s", Pm4::Name(nextHeader).c_str());
                    for (std::size_t i = 1; i < nextPacket.size() && i < 7; ++i) std::fprintf(stderr, " %08x", nextPacket[i]);
                    std::fprintf(stderr, "\n");
                    next += nextCount;
                }
            }
        } else if (drawPacket) {
            landPendingWaits(submission.queue);
            bool drawn = false;
            timed(&WorkerProfile::drawMs, [&] {
                static const bool traceDraws = std::getenv("APS5_TRACE_DRAWS") != nullptr;
                static const bool profileDraws = std::getenv("APS5_PROFILE_DRAW") != nullptr;
                const auto color = (static_cast<std::uint64_t>(readRegister(queue.context, 0x390)) << 40u) | (static_cast<std::uint64_t>(readRegister(queue.context, 0x318)) << 8u);
                const auto started = profileDraws ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const auto countSkip = [&](Graphics::DrawSkip kind) {
                    if (profileDraws) Graphics::CountDrawSkip(kind, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
                };

                if (submission.queue == 0 && ParallelPrepThreads() != 0 && deferredLabels().labels.empty()) {
                    auto state = std::make_shared<QueueState>(queue);
                    const auto epoch = GuestMemory::ThreadCollectEpoch();
                    SubmitDrawPrep(TakeDrawTicket(), [this, state, packet, &submission, epoch] {
                        std::shared_lock prepDeviceUse(deviceReplacement);
                        GuestMemory::AdoptCollectEpoch(epoch);
                        std::string rejected;
                        const Graphics::DrawCpuScope cpuScope;
                        if (draw(*state, packet, submission, rejected) == DrawVerdict::Rejected) {
                            static std::atomic<std::uint64_t> rejects{0};
                            if (rejects.fetch_add(1) < 50) std::fprintf(stderr, "[draw-prep] draw rejected: %.256s\n", rejected.c_str());
                        }
                    });
                    drawn = true;
                    return;
                }
                try {
                    std::string rejected;
                    const Graphics::DrawCpuScope cpuScope;
                    const auto verdict = draw(queue, packet, submission, rejected);
                    drawn = verdict == DrawVerdict::Drawn;
                    CaptureTrace::Log("draw submission=%llu queue=%x offset=%zu target=%llx mask=%x verdict=%d reason=%.256s", static_cast<unsigned long long>(submission.serial), submission.queue, cursor, static_cast<unsigned long long>(color), readRegister(queue.context, 0x8e), static_cast<int>(verdict), rejected.c_str());
                    if (verdict == DrawVerdict::Rejected) {
                        countSkip(Graphics::DrawSkip::Prechecked);
                        throw std::runtime_error(rejected);
                    } else if (verdict == DrawVerdict::Nothing) {
                        countSkip(Graphics::DrawSkip::Nothing);
                    } else if (traceDraws) {
                        std::fprintf(stderr, "[draw] target 0x%llx mask 0x%x ok\n",static_cast<unsigned long long>(color), readRegister(queue.context, 0x8e));
                    }
                } catch (const std::exception& error) {
                    CaptureTrace::Log("draw-error submission=%llu offset=%zu reason=%.256s", static_cast<unsigned long long>(submission.serial), cursor, error.what());
                    countSkip(Graphics::DrawSkip::Thrown);
                    throw;
                }
            });
            finishDrawPacket(drawn);
        } else if (opcode == 0x22) {
            landPendingWaits(submission.queue);
            recordQueuedLabelsBeforeRead(submission.queue);
            const auto condition = Pm4::ReadCondition(packet);
            if (condition == 0) nextCursor = submission.conditionalEnds.at(cursor);
            if (traceGpu) std::fprintf(stderr, "[gpu] %.1f queue 0x%x COND_EXEC at DWORD %zu reads 0x%x at 0x%llx: %s %zu dwords\n", TraceMs(), submission.queue, cursor, condition, static_cast<unsigned long long>(packet[1] | (static_cast<std::uint64_t>(packet[2]) << 32u)), condition == 0 ? "skips" : "executes", submission.conditionalEnds.at(cursor) - cursor - count);
        } else if (opcode == 0x91) {
            landPendingWaits(submission.queue);
            recordQueuedLabelsBeforeRead(submission.queue);
            Pm4::Execute(packet, queue);
        } else if (sampleDump && !wroteOnGpu) {
            dumpSampleCounters(packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u));
        } else if (opcode != 0x42 && opcode != 0x46 && opcode != 0x58) {
            if (!wroteOnGpu) {
                if (Pm4::IndirectRegisterOpcode(opcode)) Pm4::ExecuteIndirectRegisters(packet, submission.registerLists.at(cursor), queue);
                else Pm4::Execute(packet, queue);
                if (opcode == 0x49 || opcode == 0x37) {
                    if (const auto label = Pm4::DecodeLabelWrite(packet)) noteLabelStore(label->address, label->Bytes(), ++eventSerial, submission.queue);
                }
            }
            if (endOfPipeInterrupt && !interruptDeferred) AgcDriverDeliverEopInterrupt(submission.queue);
        }
        if (drawPacket || (sampleDump && wroteOnGpu)) Graphics::Recorder::CountRecordedWork();
        timing.Mark(header == RenderingWaitPacketHeader ? "rendering_wait" : opcode == 0x3c || opcode == 0x93 ? "memory_wait" : opcode == 0x15 || opcode == 0x16 ? "dispatch" : drawPacket ? "draw" : "execute");
        if (submission.queue == 0) timing.FinishPacket(submission.serial, cursor, opcode, count);
        else timing.Finish();
        cursor = nextCursor;
    }

    static const bool submitAtEnd = std::getenv("APS5_SUBMIT_AT_END") != nullptr;
    // POC: when the next submission is queued already (the end submit below is skipped), draws still
    // in flight on the draw thread carry over into it (APS5_POC_DRAW_THREAD_KEEP_END=1).
    static const bool keepEnd = std::getenv("APS5_POC_DRAW_THREAD_KEEP_END") != nullptr;
    const bool nextQueued = submission.queue == 0 && !submitAtEnd && submission.rewindTail == nullptr && workerQueued() != nullptr && workerQueued()->load(std::memory_order_acquire) != 0;
    // POC: the end of a queue 0 submission is a draw thread job (its submit runs behind the draws in flight)
    // instead of a drain, so the next chunk is decoded while they record (APS5_POC_END_JOB=1; no gain measured).
    static const bool endJob = std::getenv("APS5_POC_END_JOB") != nullptr;
    if (submission.queue == 0 && endJob && LabelJobsEnabled() && !submitAtEnd && deferredLabels().labels.empty()) {
        PushDrawPipeline([this] {
            if (!Graphics::Recorder::PendingLabelSince().has_value() && Graphics::Recorder::RecordedWorkSinceSubmit() == 0) return;
            ++submissionCosts(0).endSubmits;
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::End);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            const auto localDevice = device.Load();
            if (localDevice != nullptr) localDevice->SubmitRecorded(true);
        });
        if (submission.rewindTail != nullptr) executeRewindTail(submission);
        return;
    }
    if (submission.queue == 0 && !(keepEnd && nextQueued)) DrainDrawWork();
    PerformanceContext endContext(submission.queue == 0 ? frameTiming() : FrameTiming::Async());
    PerformanceTimer endTiming("Driver.EndSubmission");
    if (!deferredLabels().labels.empty() || Graphics::Recorder::PendingLabelSince().has_value() || Graphics::Recorder::RecordedWorkSinceSubmit() != 0) {
        auto& costs = submissionCosts(submission.queue);
        if (!submitAtEnd && submission.rewindTail == nullptr && submission.queue == 0 && workerQueued() != nullptr && workerQueued()->load(std::memory_order_acquire) != 0) {
            ++costs.endSkipped;
            return;
        }
        ++costs.endSubmits;
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::End);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        const auto localDevice = device.Load();

        recordDeferredLabels(localDevice.get(), submission.queue);
        if (localDevice != nullptr) localDevice->SubmitRecorded(submission.queue == 0);
    }
    endTiming.Finish();
    if (submission.rewindTail != nullptr) executeRewindTail(submission);
}

}
