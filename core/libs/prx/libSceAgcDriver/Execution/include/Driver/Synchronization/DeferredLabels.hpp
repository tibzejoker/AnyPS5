#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEFERREDLABELS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEFERREDLABELS_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace AgcDriver::DriverDetail {

inline constexpr std::chrono::microseconds ReapInterval{250};

struct DeferredLabel {
    static constexpr std::size_t Capacity = 32;
    std::uint64_t address;
    std::size_t size;
    std::array<std::byte, Capacity> bytes;
};

struct DeferredLabels {
    std::vector<DeferredLabel> labels;

    std::chrono::steady_clock::time_point since;
};

DeferredLabels& deferredLabels();
// POC draw thread (Draw.cpp): whether queue 0's draws are recorded on their own thread, and the wait
// for the draws still in flight there.
bool DrawThreadEnabled();
void DrainDrawPipeline();
// POC: direct dispatches of queue 0 prepared on the draw preparation pool and recorded on the draw thread
// (APS5_POC_NO_DISPATCH_THREAD=1 disables, APS5_POC_NO_DISPATCH_PREP=1 prepares on the draw thread).
bool DispatchThreadEnabled();
// POC: DISPATCH_INDIRECT of queue 0 whose arguments the GPU reads in place keeps the draw work in flight and
// takes the dispatch job path (APS5_POC_NO_INDIRECT_JOBS=1 drains as before).
bool IndirectJobsEnabled();
void PushDrawPipeline(std::function<void()> job);
// POC parallel draw preparation (APS5_POC_PARALLEL_PREP=<threads>, default 3, 0 disables): queue 0's draw packets are prepared on a pool
// of threads, each on a copy of the queue state; a ticket per draw orders what reaches the draw thread (the
// preparation waits for its turn before handing its recording over or drawing synchronously, ServeDrawTurn).
std::size_t ParallelPrepThreads();
std::uint64_t TakeDrawTicket();
void SubmitDrawPrep(std::uint64_t ticket, std::function<void()> job);
void ServeDrawTurn();
// Waits for every submitted preparation, then for the draws still on the draw thread.
void DrainDrawWork();
// A draw thread job queued by the packet worker behind the preparations submitted before it (it reaches
// the draw thread once they have handed their recordings over, in ticket order).
void PushOrderedDrawJob(std::function<void()> job);
// POC: guest ranges that queued draw thread jobs (labels, CP stores) write and have not run yet. A
// preparation or packet that reads such a range first waits for the jobs ordered before it
// (WaitPendingJobWrites); the job forgets its range once it ran (FinishPendingJobWrite).
std::uint64_t NotePendingJobWrite(std::uint64_t address, std::size_t bytes);
void FinishPendingJobWrite(std::uint64_t id);
void WaitPendingJobWrites(std::uint64_t address, std::size_t bytes);
// Whether CP stores (COPY_DATA, DMA_DATA, WRITE_CONST_RAM to memory) of queue 0 become draw thread jobs
// (APS5_POC_NO_STORE_JOBS=1 disables).
bool StoreJobsEnabled();
// POC: RELEASE_MEM / WRITE_DATA labels of queue 0 (no end-of-pipe interrupt) are recorded by the draw thread
// behind the draws before them instead of draining it (APS5_POC_NO_LABEL_JOBS=1 disables); the count of label
// jobs not run yet (a WAIT_REG_MEM drains while any is pending).
bool LabelJobsEnabled();
std::atomic<std::uint64_t>& PendingLabelJobs();
// The label jobs not run yet. A WAIT_REG_MEM whose compare holds for the newest pending value at its address
// is ordered behind that label job like a wait on a recorded label; one that overlaps a pending label
// otherwise drains the draw work first.
enum class PendingLabelWait { None, Ordered, Overlaps };
void NotePendingLabel(std::uint64_t address, std::span<const std::byte> bytes, bool add);
PendingLabelWait CheckPendingLabelWait(std::span<const std::uint32_t> packet);

bool DeferLabels();

bool QueuedLabelTable();

bool LabelBatchSubmit();

std::size_t& queuedLabelsNoted();

bool NeedsRecordedLabels(std::uint32_t header);

bool PacketLocksItself(std::uint32_t header);

}

#endif
