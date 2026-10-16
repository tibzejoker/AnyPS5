#ifdef _WIN32

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#ifndef MI_STATIC_LIB
#define MI_STATIC_LIB 1
#endif
#ifndef MI_WIN_USE_FIXED_TLS
#define MI_WIN_USE_FIXED_TLS 1
#endif
#include "../../../../../../../3rdparty/mimalloc/src/static.c"
#pragma GCC diagnostic pop

// mimalloc's thread attach/detach callbacks: its MSVC section pragmas do nothing under GCC, so the TLS
// callback entries are placed here (process attach allocates the fixed TLS slot before any constructor;
// thread detach returns the thread's heap). Fixed TLS keeps emulated TLS (whose first use per thread
// allocates) out of malloc.
extern "C" {
extern const IMAGE_TLS_DIRECTORY _tls_used;
__attribute__((section(".CRT$XLB"), used)) const PIMAGE_TLS_CALLBACK hostHeapTlsAttach = &mi_win_main_attach;
__attribute__((section(".CRT$XLY"), used)) const PIMAGE_TLS_CALLBACK hostHeapTlsDetach = &mi_win_main_detach;
}
__attribute__((used)) const void* const hostHeapTlsDirectory = &_tls_used;

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// POC (APS5_NO_MIMALLOC=1 disables): the host heap of the whole process (ucrtbase's _malloc_base,
// _free_base, _realloc_base, _calloc_base, _msize, _recalloc, _expand, which malloc, operator new and
// every module of the process end in) is served by mimalloc instead of the Windows process heap. The
// entries are patched with an absolute jump while the other threads are suspended; a block the process
// heap served before the patch is recognized (not in mimalloc's segments) and handled by the process
// heap as before. Only a ucrtbase whose entry bytes are the known ones is patched.

namespace {

HANDLE processHeap() {
    static const HANDLE heap = GetProcessHeap();
    return heap;
}

bool ours(void* block) {
    return mi_is_in_heap_region(block);
}

void* hostMalloc(std::size_t bytes) {
    void* block = mi_malloc(bytes == 0 ? 1 : bytes);
    if (block == nullptr) errno = ENOMEM;
    return block;
}

void hostFree(void* block) {
    if (block == nullptr) return;
    if (ours(block)) mi_free(block);
    else HeapFree(processHeap(), 0, block);
}

void* hostRealloc(void* block, std::size_t bytes) {
    if (block == nullptr) return hostMalloc(bytes);
    if (bytes == 0) {
        hostFree(block);
        return nullptr;
    }
    if (ours(block)) {
        void* moved = mi_realloc(block, bytes);
        if (moved == nullptr) errno = ENOMEM;
        return moved;
    }
    void* moved = mi_malloc(bytes);
    if (moved == nullptr) {
        errno = ENOMEM;
        return nullptr;
    }
    const auto old = HeapSize(processHeap(), 0, block);
    std::memcpy(moved, block, std::min<std::size_t>(old, bytes));
    HeapFree(processHeap(), 0, block);
    return moved;
}

void* hostCalloc(std::size_t count, std::size_t size) {
    void* block = mi_calloc(count, size == 0 ? 1 : size);
    if (block == nullptr) errno = ENOMEM;
    return block;
}

std::size_t hostMsize(void* block) {
    if (block == nullptr) {
        errno = EINVAL;
        return static_cast<std::size_t>(-1);
    }
    return ours(block) ? mi_usable_size(block) : HeapSize(processHeap(), 0, block);
}

void* hostRecalloc(void* block, std::size_t count, std::size_t size) {
    if (size != 0 && count > static_cast<std::size_t>(-1) / size) {
        errno = ENOMEM;
        return nullptr;
    }
    const auto bytes = count * size;
    const auto old = block != nullptr ? hostMsize(block) : 0;
    void* moved = hostRealloc(block, bytes);
    if (moved != nullptr && bytes > old) std::memset(static_cast<char*>(moved) + old, 0, bytes - old);
    return moved;
}

void* hostExpand(void* block, std::size_t bytes) {
    if (block == nullptr) {
        errno = EINVAL;
        return nullptr;
    }
    if (ours(block)) return mi_expand(block, bytes);
    return HeapReAlloc(processHeap(), HEAP_REALLOC_IN_PLACE_ONLY, block, bytes == 0 ? 1 : bytes);
}

struct Target {
    const char* name;
    bool viaThunk;
    void* hook;
    std::array<unsigned char, 14> expected;
    unsigned char* address = nullptr;
};

// The 14 entry bytes of each function in the ucrtbase.dll this was written against (the patch replaces
// exactly them; no branch of ucrtbase lands inside them).
std::array<Target, 7> targets() {
    return {{
        {"_malloc_base", false, reinterpret_cast<void*>(&hostMalloc), {0x48, 0x83, 0xec, 0x28, 0x48, 0x83, 0xf9, 0xe0, 0x77, 0x43, 0x48, 0x89, 0x5c, 0x24}},
        {"_free_base", false, reinterpret_cast<void*>(&hostFree), {0x48, 0x83, 0xec, 0x28, 0x48, 0x85, 0xc9, 0x74, 0x16, 0x4c, 0x8b, 0xc1, 0x33, 0xd2}},
        {"_realloc_base", false, reinterpret_cast<void*>(&hostRealloc), {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xfa, 0x48}},
        {"_calloc_base", false, reinterpret_cast<void*>(&hostCalloc), {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0xc2, 0x48, 0x8b, 0xd9, 0x48, 0x85}},
        {"_msize", true, reinterpret_cast<void*>(&hostMsize), {0x48, 0x83, 0xec, 0x28, 0x48, 0x85, 0xc9, 0x74, 0x17, 0x4c, 0x8b, 0xc1, 0x33, 0xd2}},
        {"_recalloc", true, reinterpret_cast<void*>(&hostRecalloc), {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec}},
        {"_expand", true, reinterpret_cast<void*>(&hostExpand), {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24}},
    }};
}

// Allocates nothing once the first thread is suspended (that thread may hold the heap lock).
std::vector<HANDLE> suspendOthers() {
    std::vector<HANDLE> suspended;
    suspended.reserve(16384);
    const auto self = GetCurrentThreadId();
    const auto process = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return suspended;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
        if (thread == nullptr) continue;
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            CloseHandle(thread);
            continue;
        }
        if (suspended.size() == suspended.capacity()) {
            ResumeThread(thread);
            CloseHandle(thread);
            continue;
        }
        suspended.push_back(thread);
    }
    CloseHandle(snapshot);
    return suspended;
}

// Whether a suspended thread's instruction pointer lies inside a patched window.
bool insideWindow(HANDLE thread, const std::array<Target, 7>& list) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(thread, &context)) return true;
    const auto rip = static_cast<std::uintptr_t>(context.Rip);
    return std::any_of(list.begin(), list.end(), [&](const Target& target) {
        const auto start = reinterpret_cast<std::uintptr_t>(target.address);
        return rip > start && rip < start + 14;
    });
}

void redirect() {
    if (std::getenv("APS5_NO_MIMALLOC") != nullptr) return;
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    if (ucrt == nullptr) return;
    // The fixed slot must be one of the 64 the TEB holds directly (mimalloc reads it at gs:[0x1480 + slot * 8]).
    if (_mi_win_tls_offset == 0 || _mi_win_tls_offset / sizeof(void*) >= 64) {
        std::fprintf(stderr, "[heap] no direct TLS slot for mimalloc (offset 0x%zx): host heap left to Windows\n", _mi_win_tls_offset);
        return;
    }
    mi_free(mi_malloc(16));
    auto list = targets();
    for (auto& target : list) {
        auto* entry = reinterpret_cast<unsigned char*>(GetProcAddress(ucrt, target.name));
        if (entry != nullptr && target.viaThunk) entry = entry[0] == 0xe9 ? entry + 5 + *reinterpret_cast<const std::int32_t*>(entry + 1) : nullptr;
        if (entry == nullptr || std::memcmp(entry, target.expected.data(), target.expected.size()) != 0) {
            std::fprintf(stderr, "[heap] ucrtbase %s does not have the known entry bytes: host heap left to Windows\n", target.name);
            return;
        }
        target.address = entry;
    }
    std::vector<HANDLE> suspended;
    for (int attempt = 0; attempt < 50; ++attempt) {
        suspended = suspendOthers();
        if (std::none_of(suspended.begin(), suspended.end(), [&](HANDLE thread) { return insideWindow(thread, list); })) break;
        for (HANDLE thread : suspended) {
            ResumeThread(thread);
            CloseHandle(thread);
        }
        suspended.clear();
        Sleep(1);
    }
    for (const auto& target : list) {
        std::array<unsigned char, 14> code{0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
        std::memcpy(code.data() + 6, &target.hook, sizeof(void*));
        DWORD previous = 0;
        VirtualProtect(target.address, code.size(), PAGE_EXECUTE_READWRITE, &previous);
        std::memcpy(target.address, code.data(), code.size());
        VirtualProtect(target.address, code.size(), previous, &previous);
        FlushInstructionCache(GetCurrentProcess(), target.address, code.size());
    }
    for (HANDLE thread : suspended) {
        ResumeThread(thread);
        CloseHandle(thread);
    }
    std::fprintf(stderr, "[heap] host heap served by mimalloc %d (APS5_NO_MIMALLOC=1 disables)\n", MI_MALLOC_VERSION);
}

struct Redirect {
    Redirect() { redirect(); }
} redirectAtLoad;

}

#endif
