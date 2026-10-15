#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEFERREDLABELS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEFERREDLABELS_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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
// POC: direct dispatches of queue 0 recorded on the draw thread too (APS5_POC_DISPATCH_THREAD=1).
bool DispatchThreadEnabled();
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

bool DeferLabels();

bool QueuedLabelTable();

bool LabelBatchSubmit();

std::size_t& queuedLabelsNoted();

bool NeedsRecordedLabels(std::uint32_t header);

bool PacketLocksItself(std::uint32_t header);

}

#endif
