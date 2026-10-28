#include <windows.h>

#include "native/native.h"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    // The process is exiting (not a FreeLibrary): the crash log says so, so a log that stops without it was ended
    // from outside (crash.cpp).
    if (reason == DLL_PROCESS_DETACH && reserved)
        rnative::CrashLogExit();
    return TRUE;
}
