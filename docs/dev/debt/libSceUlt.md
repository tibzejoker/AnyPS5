# libSceUlt technical debt

### Unknown function info

- [libSceUlt ulthreads](../../../core/libs/prx/libSceUlt/Export.cpp) - `_sceUltUlthreadRuntimeOptParamInitialize`, `_sceUltUlthreadRuntimeCreate` and `_sceUltUlthreadCreate` are assumed to take the same arguments as the existing exports without the underscore, including the trailing build version. `sceUltUlthreadTryJoin` is assumed to return `ULT_ERROR_BUSY` while the ulthread runs, and `sceUltUlthreadRuntimeDestroy` to return `ULT_ERROR_BUSY` while an ulthread created on the runtime is not joined. Neither error has been confirmed on PS5 hardware
- [libSceUlt queues](../../../core/libs/prx/libSceUlt/Export.cpp) - the added `sceUltQueuePop`, `sceUltQueueTryPush`, `sceUltQueueDestroy` and `sceUltQueueDataResourcePoolDestroy` signatures are inferred from the existing push/pop and object APIs. Queue destruction is assumed to reject blocked callers with `ULT_ERROR_BUSY` and otherwise discard queued data; pool exhaustion returns `ULT_ERROR_AGAIN`. These rules, alignment checks and shared slot limits have not been confirmed on PS5 hardware or a title. Non-null queue and data-pool option parameters throw because their layouts are unknown.

### Functional

- [libSceUlt queues](../../../core/libs/prx/libSceUlt/Export.cpp) use preallocated host storage instead of the supplied guest work area; its existing size formula and the 512-byte object layouts remain unverified. Waiting-queue pool thread limits and waiter priority ordering are not implemented. Finalization wakes blocked queue callers with `ULT_ERROR_STATE`.
