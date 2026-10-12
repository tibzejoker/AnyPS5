# libSceNpAuth technical debt

### Unknown function info

- [sceNpAuthGetAuthorizedAppCode](../../../core/libs/prx/libSceNpAuth/Export.cpp) (libSceNpAuth) - declared without parameters as a throwing stub; Minecraft (PPSA17221) imports it and fails to load without it. The request parameter and code output are not known, and there is no PSN sign-in for it to succeed against.
