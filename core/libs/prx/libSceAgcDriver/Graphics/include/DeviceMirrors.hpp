#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEVICEMIRRORS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEVICEMIRRORS_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "BdaAbi.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

class Recorder;

// Device-local mirrors of host-imported guest memory, for the BDA address space. On a discrete GPU
// the imports are system memory reached over PCIe: a dependent chain of reads through them (a BVH
// traversal) ran ~270x slower than from device memory on an RTX 4090 (agc_driver_bda_placement_bench),
// where the PS5 reads its unified memory locally. A mirrored range's BDA entry serves reads from the
// mirror (deviceAddress) and stores and atomics from the guest bytes (hostAddress); the shaders write
// through to both (SpirvBdaRead), so the guest bytes stay what the CPU and every other GPU path read.
// The mirror follows the guest bytes per 64 KiB block: a block stamped since its upload (a CPU write,
// a GPU write the driver marked) or written by recorded GPU work (NoteDeviceMirrorGpuWrite: copies,
// fills, storage write-backs, descriptor-bound stores) is copied from the import again before the
// next use (RefreshDeviceMirrors, a device copy recorded ahead of the work). Ranges outside the write
// tracking, past APS5_DEVICE_MIRROR_MAX_MIB, or past the budget (APS5_DEVICE_MIRROR_MIB, else a
// quarter of the largest device-local heap, less when VK_EXT_memory_budget reports it short) stay on
// the import; least recently used mirrors make room. Integrated GPUs (every heap device-local) take
// no mirrors. APS5_NO_DEVICE_MIRRORS=1 turns them off. [mirror] every 10 s.
struct DeviceMirror;

struct DeviceMirrorSource {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
};

bool DeviceMirrorsEnabled(const Context& context);
// For an address space being built: serves the ranges (sorted, from `sources`, the import of each
// range or a null buffer for none) from device mirrors where the policy allows, and returns the
// mirrors the space must hold.
std::vector<std::shared_ptr<DeviceMirror>> AssignDeviceMirrors(const Context& context, std::span<ShaderRecompiler::BdaAbi::Range> ranges, std::span<const DeviceMirrorSource> sources);
// Records the copies that bring the mirrors up to date with the guest bytes, ahead of work that
// reads them (they are kept by the recorder's batch).
void RefreshDeviceMirrors(const Context& context, Recorder& recorder, std::span<const std::shared_ptr<DeviceMirror>> mirrors);
// A recorded GPU write of [begin, end) into guest memory: the mirrored blocks it overlaps are stale.
void NoteDeviceMirrorGpuWrite(std::uint64_t begin, std::uint64_t end);
void ClearDeviceMirrors(VkDevice device);

struct DeviceMirrorCounts {
    std::uint64_t mirrors = 0;
    std::uint64_t mirroredBytes = 0;
    std::uint64_t uploadedBytes = 0;
    std::uint64_t uploads = 0;
    std::uint64_t evictions = 0;
    std::uint64_t assigned = 0;
    std::uint64_t skippedUntracked = 0;
    std::uint64_t skippedLarge = 0;
    std::uint64_t skippedBudget = 0;
    std::uint64_t gpuInvalidations = 0;
};
DeviceMirrorCounts DeviceMirrorCounters();

}

#endif
