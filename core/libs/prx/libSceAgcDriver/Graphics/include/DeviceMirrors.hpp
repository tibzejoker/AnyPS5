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
// where the PS5 reads its unified memory locally.
//
// Chunks and the hot set. The address space's import-served ranges enter the BDA table cut into
// chunks (APS5_DEVICE_MIRROR_CHUNK_MIB, default 4; a smaller range is one chunk), each chunk its own
// entry, served from the import or from a device mirror of its own. A chunk is mirrored once BDA work
// used it: a lookup that resolves an entry on its slow path sets the entry's bit in the use's fault
// buffer (BdaAbi::TouchedWord), the driver reads it at completion (NoteDeviceMirrorTouched), and the
// next address space (DeviceMirrorEpoch moves) serves the chunk from a new mirror within the budget
// (APS5_DEVICE_MIRROR_MIB, else a quarter of the largest device-local heap, less when
// VK_EXT_memory_budget reports it short); the least recently used mirrors make room. Consecutive hot
// chunks of one import share a mirror and one entry (up to 64 MiB), since changing entries costs a
// table search; a run that grows is mirrored anew. A 4.6 GiB heap thus mirrors the chunks its
// shaders read, not 4.6 GiB.
//
// Stores and atomics. A mirrored entry serves reads from the mirror (deviceAddress) and stores and
// atomics from the guest bytes (hostAddress); the shaders store to both (SpirvBdaRead), so the guest
// bytes stay what the CPU and every other GPU path read. For atomics the mirror is the source of truth
// (the value returned and later read through BDA); the guest bytes take the same operation, which
// leaves both equal for the commutative operations; after a contended exchange or compare-exchange the
// guest bytes may hold another lane's outcome until the block is uploaded again.
//
// Coherence. Before a use (once per mirror and recorded batch while nothing staled it since), a
// mirror whose range stamp moved (a CPU write: collected write stamps of watched memory, write-watch
// hints of unwatched imports) has the moved 64 KiB blocks copied from the import again, ahead of the
// work; a GPU write the driver reports, recorded (Recorder::noteWrite) or not (GuestMemory::MarkWritten,
// the synchronous write-backs), stales the blocks it covers first. Ranges neither stamps nor hints
// cover stay on the import, for good (touching them again rebuilds no space). Integrated GPUs (every heap device-local) take no mirrors.
// APS5_NO_DEVICE_MIRRORS=1 turns them off. [mirror] every 10 s.
struct DeviceMirror;

struct DeviceMirrorSource {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
};

bool DeviceMirrorsEnabled(const Context& context);
// For an address space being built: its table entries (sorted, the import of each in `sources`, a
// null buffer for none), cut into chunks and served from mirrors where the hot set and the budget
// allow. Returns the entries; `held` takes the mirrors the space must keep.
std::vector<ShaderRecompiler::BdaAbi::Range> ChunkDeviceMirrors(const Context& context, std::vector<ShaderRecompiler::BdaAbi::Range> ranges, std::span<const DeviceMirrorSource> sources, std::vector<std::shared_ptr<DeviceMirror>>& held);
// Moves when the mirror set an address space would take changed (new hot chunks, evictions): the
// cached space is rebuilt.
std::uint64_t DeviceMirrorEpoch();
// Records the copies that bring the mirrors up to date with the guest bytes, ahead of work that
// reads them (they are kept by the recorder's batch).
void RefreshDeviceMirrors(const Context& context, Recorder& recorder, std::span<const std::shared_ptr<DeviceMirror>> mirrors);
// A GPU write of [begin, end) into guest memory: the mirrored blocks it overlaps are stale.
void NoteDeviceMirrorGpuWrite(std::uint64_t begin, std::uint64_t end);
// BDA work resolved the table entry [begin, end) on its slow path: the entry joins the hot set.
void NoteDeviceMirrorTouched(std::uint64_t begin, std::uint64_t end);
void ClearDeviceMirrors(VkDevice device);

struct DeviceMirrorCounts {
    std::uint64_t mirrors = 0;
    std::uint64_t mirroredBytes = 0;
    std::uint64_t uploadedBytes = 0;
    std::uint64_t uploads = 0;
    std::uint64_t evictions = 0;
    std::uint64_t assigned = 0;
    std::uint64_t chunks = 0;
    std::uint64_t touched = 0;
    std::uint64_t checks = 0;
    std::uint64_t checksSkipped = 0;
    std::uint64_t skippedUntracked = 0;
    std::uint64_t skippedBudget = 0;
    std::uint64_t gpuInvalidations = 0;
};
DeviceMirrorCounts DeviceMirrorCounters();

}

#endif
