#include "prx/libSceAgcDriver/Graphics/include/DeviceDiagnostics.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace AgcDriver::Graphics {

namespace {

// The checkpoints set so far: a marker is the checkpoint's serial, the ring keeps what it named.
struct Checkpoint {
    std::uint64_t serial = 0;
    std::uint64_t programAddress = 0;
    CheckpointKind kind = CheckpointKind::Draw;
};

constexpr std::size_t RingSize = 1u << 16u;

struct Diagnostics {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    PFN_vkGetDeviceFaultInfoEXT faultInfo = nullptr;
    PFN_vkCmdSetCheckpointNV setCheckpoint = nullptr;
    PFN_vkGetQueueCheckpointDataNV checkpointData = nullptr;
    std::atomic<std::uint64_t> serial{0};
    std::array<Checkpoint, RingSize> ring{};
    std::mutex mutex;
    std::atomic<bool> reported{false};
};

Diagnostics& diagnostics() {
    static auto* state = new Diagnostics();
    return *state;
}

const char* kindName(CheckpointKind kind) {
    return kind == CheckpointKind::Draw ? "draw" : "dispatch";
}

}

bool GpuCheckpointsRequested() {
    static const bool requested = std::getenv("APS5_GPU_CHECKPOINTS") != nullptr;
    return requested;
}

void InitDeviceDiagnostics(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr deviceProc, bool deviceFault, bool checkpoints) {
    auto& state = diagnostics();
    std::lock_guard lock(state.mutex);
    state.device = device;
    state.queue = queue;
    state.reported.store(false);
    state.faultInfo = deviceFault ? reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(deviceProc(device, "vkGetDeviceFaultInfoEXT")) : nullptr;
    state.setCheckpoint = checkpoints ? reinterpret_cast<PFN_vkCmdSetCheckpointNV>(deviceProc(device, "vkCmdSetCheckpointNV")) : nullptr;
    state.checkpointData = checkpoints ? reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(deviceProc(device, "vkGetQueueCheckpointDataNV")) : nullptr;
    std::fprintf(stderr, "[device-lost] diagnostics: device fault %s, checkpoints %s\n", state.faultInfo != nullptr ? "on" : "unavailable", state.setCheckpoint != nullptr ? "on" : "unavailable");
}

void NoteCheckpoint(VkCommandBuffer commands, CheckpointKind kind, std::uint64_t programAddress) {
    auto& state = diagnostics();
    if (state.setCheckpoint == nullptr || commands == VK_NULL_HANDLE) return;
    const auto serial = state.serial.fetch_add(1, std::memory_order_relaxed) + 1;
    state.ring[serial % RingSize] = {serial, programAddress, kind};
    state.setCheckpoint(commands, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(serial)));
}

void ReportDeviceLost() {
    auto& state = diagnostics();
    if (state.device == VK_NULL_HANDLE || state.reported.exchange(true)) return;
    std::fprintf(stderr, "[device-lost] the device was lost; %llu checkpoints set so far\n", static_cast<unsigned long long>(state.serial.load()));
    if (state.faultInfo != nullptr) {
        VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        const auto counted = state.faultInfo(state.device, &counts, nullptr);
        if (counted == VK_SUCCESS || counted == VK_INCOMPLETE) {
            std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
            VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
            info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
            info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
            counts.vendorBinarySize = 0;
            const auto result = state.faultInfo(state.device, &counts, &info);
            std::fprintf(stderr, "[device-lost] device fault (result %d): %s\n", static_cast<int>(result), info.description);
            static constexpr const char* addressTypes[] = {"none", "read invalid", "write invalid", "execute invalid", "instruction pointer unknown", "instruction pointer invalid", "instruction pointer fault"};
            for (const auto& address : addresses) {
                const auto type = static_cast<std::size_t>(address.addressType);
                std::fprintf(stderr, "[device-lost]   address 0x%llx (+-0x%llx): %s\n", static_cast<unsigned long long>(address.reportedAddress), static_cast<unsigned long long>(address.addressPrecision), type < std::size(addressTypes) ? addressTypes[type] : "?");
            }
            for (const auto& vendor : vendors) std::fprintf(stderr, "[device-lost]   vendor fault 0x%llx data 0x%llx: %s\n", static_cast<unsigned long long>(vendor.vendorFaultCode), static_cast<unsigned long long>(vendor.vendorFaultData), vendor.description);
        } else {
            std::fprintf(stderr, "[device-lost] vkGetDeviceFaultInfoEXT failed (%d)\n", static_cast<int>(counted));
        }
    }
    if (state.checkpointData != nullptr && state.queue != VK_NULL_HANDLE) {
        std::uint32_t count = 0;
        state.checkpointData(state.queue, &count, nullptr);
        std::vector<VkCheckpointDataNV> data(count, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        if (count != 0) state.checkpointData(state.queue, &count, data.data());
        std::fprintf(stderr, "[device-lost] %u last checkpoints the queue reached (stage, serial, kind, program):\n", count);
        for (const auto& checkpoint : data) {
            const auto serial = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(checkpoint.pCheckpointMarker));
            const auto& entry = state.ring[serial % RingSize];
            if (entry.serial == serial) std::fprintf(stderr, "[device-lost]   stage 0x%x serial %llu %s 0x%llx\n", static_cast<unsigned>(checkpoint.stage), static_cast<unsigned long long>(serial), kindName(entry.kind), static_cast<unsigned long long>(entry.programAddress));
            else std::fprintf(stderr, "[device-lost]   stage 0x%x serial %llu (no longer in the ring)\n", static_cast<unsigned>(checkpoint.stage), static_cast<unsigned long long>(serial));
        }
        // The checkpoints just before the last ones reached, for context.
        const auto newest = state.serial.load();
        std::fprintf(stderr, "[device-lost] newest checkpoints recorded:");
        for (auto serial = newest > 8u ? newest - 8u : 1u; serial <= newest; ++serial) {
            const auto& entry = state.ring[serial % RingSize];
            if (entry.serial == serial) std::fprintf(stderr, " %llu:%s:0x%llx", static_cast<unsigned long long>(serial), kindName(entry.kind), static_cast<unsigned long long>(entry.programAddress));
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
}

}
