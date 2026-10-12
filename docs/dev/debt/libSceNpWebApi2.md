# libSceNpWebApi2 technical debt

### Unknown function info

- [sceNpWebApi2PushEventAbortHandle](../../../core/libs/prx/libSceNpWebApi2/Export.cpp) (libSceNpWebApi2, NID `1OLgvahaSco`) is a throwing stub. The two signed 32-bit handle arguments and return type follow [shadPS4](https://github.com/shadps4-emu/shadPS4/blob/443f022450f6857256e780daa3d5a14029a748f7/src/core/libraries/np/np_web_api2/np_web_api2.cpp) (PS4); the PS5 signature and abort behavior are unverified. PPSA01736 imports it.
- [sceNpWebApi2GetMemoryPoolStats](../../../core/libs/prx/libSceNpWebApi2/Export.cpp) (libSceNpWebApi2) - imported by PPSA21703's Unity PSN plugin. The signature `(int libCtxId, stats*)` and the layout of the Http2 statistics above (measured on a PS5 for Http2, Ssl and Net) are assumed for this library, not measured. The pool size is the one given to `sceNpWebApi2Initialize`, and the peak and current use are 0. The error codes are unknown, so a null output or an unknown context throws
