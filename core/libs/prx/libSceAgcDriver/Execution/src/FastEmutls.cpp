#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

// POC: thread_local storage of this module without libgcc's emulated TLS (MinGW GCC has no native TLS:
// every thread_local access calls __emutls_get_address, which goes through pthread_getspecific and a
// table lookup; 6 % of the draw thread). The definition below replaces libgcc's for the objects of this
// module: each thread keeps one array of its objects in a Windows TLS slot read straight from the TEB.
// Objects of other modules (libstdc++'s, reached through inline code) still go to libgcc's function. The
// per-thread storage is never released (thread_local destructors run at thread exit and must find it).

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

struct EmutlsObject {
    std::size_t size;
    std::size_t align;
    std::atomic<std::uintptr_t> index;
    const void* initial;
};

struct ThreadArray {
    std::size_t count;
    void* items[1];
};

using LibgccGet = void* (*)(EmutlsObject*);

std::uintptr_t moduleBegin() {
    return reinterpret_cast<std::uintptr_t>(&__ImageBase);
}

std::uintptr_t moduleEnd() {
    static const std::uintptr_t end = [] {
        const auto* dos = &__ImageBase;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(dos) + dos->e_lfanew);
        return moduleBegin() + nt->OptionalHeader.SizeOfImage;
    }();
    return end;
}

std::atomic<DWORD> slot{TLS_OUT_OF_INDEXES};
std::atomic<std::uintptr_t> nextIndex{1};
std::atomic<LibgccGet> libgcc{nullptr};

DWORD tlsSlot() {
    auto current = slot.load(std::memory_order_acquire);
    if (current != TLS_OUT_OF_INDEXES) return current;
    const auto fresh = TlsAlloc();
    if (!slot.compare_exchange_strong(current, fresh, std::memory_order_acq_rel)) {
        TlsFree(fresh);
        return current;
    }
    return fresh;
}

inline ThreadArray* threadArray(DWORD index) {
    if (index < 64) {
        ThreadArray* value;
        __asm__("movq %%gs:(%1), %0" : "=r"(value) : "r"(static_cast<std::uintptr_t>(0x1480 + index * sizeof(void*))));
        return value;
    }
    return static_cast<ThreadArray*>(TlsGetValue(index));
}

void* foreign(EmutlsObject* object) {
    auto get = libgcc.load(std::memory_order_acquire);
    if (get == nullptr) {
        get = reinterpret_cast<LibgccGet>(GetProcAddress(GetModuleHandleW(L"libgcc_s_seh-1.dll"), "__emutls_get_address"));
        if (get == nullptr) std::abort();
        libgcc.store(get, std::memory_order_release);
    }
    return get(object);
}

std::uintptr_t indexOf(EmutlsObject* object) {
    auto index = object->index.load(std::memory_order_acquire);
    if (index != 0) return index;
    static std::mutex assign;
    std::lock_guard lock(assign);
    index = object->index.load(std::memory_order_relaxed);
    if (index == 0) {
        index = nextIndex.fetch_add(1, std::memory_order_relaxed);
        object->index.store(index, std::memory_order_release);
    }
    return index;
}

__attribute__((noinline)) void* slowPath(EmutlsObject* object, std::uintptr_t index, DWORD tls) {
    auto* array = static_cast<ThreadArray*>(TlsGetValue(tls));
    if (array == nullptr || index >= array->count) {
        const std::size_t count = std::max<std::size_t>(index + 32, array != nullptr ? array->count * 2 : 64);
        auto* grown = static_cast<ThreadArray*>(std::calloc(1, sizeof(ThreadArray) + count * sizeof(void*)));
        if (grown == nullptr) std::abort();
        grown->count = count;
        if (array != nullptr) std::memcpy(grown->items, array->items, array->count * sizeof(void*));
        TlsSetValue(tls, grown);
        array = grown;
    }
    if (array->items[index] == nullptr) {
        const std::size_t align = std::max<std::size_t>(object->align, sizeof(void*));
        void* storage = _aligned_malloc(object->size == 0 ? 1 : object->size, align);
        if (storage == nullptr) std::abort();
        if (object->initial != nullptr) std::memcpy(storage, object->initial, object->size);
        else std::memset(storage, 0, object->size);
        array->items[index] = storage;
    }
    return array->items[index];
}

}

extern "C" void* __emutls_get_address(EmutlsObject* object) {
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    if (address < moduleBegin() || address >= moduleEnd()) return foreign(object);
    const auto index = indexOf(object);
    const auto tls = tlsSlot();
    if (auto* array = threadArray(tls); array != nullptr && index < array->count) {
        if (void* storage = array->items[index]) return storage;
    }
    return slowPath(object, index, tls);
}

#endif
