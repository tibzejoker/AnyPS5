#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

// The handler is recorded but never invoked: host crashes are not turned into guest core dumps.
static std::atomic<uint64_t> g_coredumpHandler{0};
static std::atomic<uint64_t> g_coredumpContext{0};

static constexpr int SCE_COREDUMP_ERROR_NOT_IN_COREDUMP_HANDLER = static_cast<int>(0x81180003u);

static int attachOutsideCoredumpHandler(const char* function, const void* mem, size_t size) {
    if (mem == nullptr || size == 0 || size > UINTPTR_MAX - reinterpret_cast<uintptr_t>(mem))
        throw std::runtime_error(std::string(function) + ": null, empty or wrapping region outside the core dump handler");
    return SCE_COREDUMP_ERROR_NOT_IN_COREDUMP_HANDLER;
}

extern "C" {

int APS5_VABI sceCoredumpRegisterCoredumpHandler(uint64_t handler, size_t stack_size, uint64_t context) {
    (void)stack_size;
    g_coredumpHandler.store(handler, std::memory_order_relaxed);
    g_coredumpContext.store(context, std::memory_order_relaxed);
    return 0;
}

int APS5_VABI sceCoredumpUnregisterCoredumpHandler(void) {
    g_coredumpHandler.store(0, std::memory_order_relaxed);
    g_coredumpContext.store(0, std::memory_order_relaxed);
    return 0;
}

int APS5_VABI sceKernelDebugWriteCppExceptionInfo(const void* exception, uint64_t unknown, const char* typeName, const char* what) {
    (void)unknown;
    std::fprintf(stderr, "[coredump] uncaught C++ exception %p of type %s%s%s\n", exception, typeName ? typeName : "(unknown)", what ? ", what(): " : "", what ? what : "");
    return 0;
}


int APS5_VABI sceCoredumpAttachUserFile(void) {
    NotImplemented_nid_no_patch("5nc2gdLNsok");
    return 0;
}

int APS5_VABI sceCoredumpGetStopInfoGpu_Agc(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceCoredumpAttachMemoryRegionAsUserFile(void) {
    NotImplemented_nid_no_patch("MEJ7tc7ThwM");
    return 0;
}

int APS5_VABI sceCoredumpSetUserDataType(void) {
    NotImplemented_nid_no_patch("Uxqkdta7wEg");
    return 0;
}

int APS5_VABI sceCoredumpDebugTextOut(void) {
    NotImplemented_nid_no_patch("dei8oUx6DbU");
    return 0;
}

int APS5_VABI sceCoredumpGetStopInfoCpu(void) {
    NotImplemented_nid_no_patch("kK0DUW1Ukgc");
    return 0;
}

int APS5_VABI sceCoredumpGetThreadContextInfo(void* threadContextInfo, size_t threadContextInfoSize) {
    (void)threadContextInfo;
    (void)threadContextInfoSize;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceCoredumpWriteUserString() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceCoredumpAttachUserMemoryFile(uint32_t userValue, const void* mem, size_t size) {
    (void)userValue;
    return attachOutsideCoredumpHandler(__func__, mem, size);
}

int APS5_VABI sceCoredumpAttachMemoryRegion(uint32_t userValue, const void* mem, size_t size) {
    (void)userValue;
    return attachOutsideCoredumpHandler(__func__, mem, size);
}
}
