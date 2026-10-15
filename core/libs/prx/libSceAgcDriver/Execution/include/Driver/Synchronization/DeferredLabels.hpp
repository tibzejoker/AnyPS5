#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEFERREDLABELS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEFERREDLABELS_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

bool DeferLabels();

bool QueuedLabelTable();

bool LabelBatchSubmit();

std::size_t& queuedLabelsNoted();

bool NeedsRecordedLabels(std::uint32_t header);

bool PacketLocksItself(std::uint32_t header);

}

#endif
