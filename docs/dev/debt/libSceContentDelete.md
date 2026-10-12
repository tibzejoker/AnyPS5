# libSceContentDelete technical debt

### Unknown function info

- [sceContentDeleteById](../../../core/libs/prx/libSceContentDelete/Export.cpp) (libSceContentDelete) and [sceContentSearchGetContentLastUpdateId, sceContentSearchOpenMetadataByContentId, sceContentSearchGetTotalContentSize, sceContentSearchGetNumOfContent](../../../core/libs/prx/libSceContentSearch/Export.cpp) (libSceContentSearch) - imported by PPSA06465, which fails to load without them; added without an implementation because their signatures are unknown
