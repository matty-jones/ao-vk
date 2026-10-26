// The crash and hang reports (proxy/native/crash.cpp) in the cases that matter: tools/crash-check.sh.
// 1. A fault in our own code. 2. A fault while this thread holds the process heap lock and the loader lock (a report
// that allocates or asks the loader would hang here). 3. A hang: one thread holds the heap lock forever, the main thread
// blocks on it - the watchdog must still write every thread's place. Exits by itself, never with a dialog.
// The ends without a plain fault, each its own run: --throw (a C++ exception, caught), --unhandled (a fault nothing
// catches: the last-chance filter, then the one before it), --abort (our runtime's abort), --exit (ExitProcess's line).
#include "native/native.h"

#include <intrin.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace {

void Print(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

// clang-cl catches a hardware fault in __try only from a call (no /EHa), so the faulting store is its own function.
// Two of them: reports are one per fault address.
__declspec(noinline) void Poke(volatile int* p) { *p = 1; }
__declspec(noinline) void PokeLocked(volatile int* p) { *p = 2; }

void FaultAt(void (*poke)(volatile int*), uintptr_t address)
{
    __try {
        poke(reinterpret_cast<volatile int*>(address));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

using LdrLockFn = LONG(NTAPI*)(ULONG flags, ULONG* state, ULONG_PTR* cookie);
using LdrUnlockFn = LONG(NTAPI*)(ULONG flags, ULONG_PTR cookie);

// The frozen sessions' picture: one thread holds the process heap (and a lock others want) and never lets go.
CRITICAL_SECTION g_held;

DWORD WINAPI HoldHeap(void*)
{
    EnterCriticalSection(&g_held);
    HeapLock(GetProcessHeap());
    Sleep(INFINITE);
    return 0;
}

}  // namespace

// The filter before ours (the game's, in the game): ends the process quietly, as no dialog may show.
LONG WINAPI QuietEnd(EXCEPTION_POINTERS* e)
{
    Print("check: the earlier filter ran (code %08lx)", e->ExceptionRecord->ExceptionCode);
    TerminateProcess(GetCurrentProcess(), 7);
    return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) void Throw(int kind)
{
    if (kind == 0) throw std::runtime_error("the check's own exception");
    throw std::bad_alloc();
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const char* mode = argc > 1 ? argv[1] : "";
    SetUnhandledExceptionFilter(&QuietEnd);
    rnative::SetLog(&Print);
    rnative::InstallCrashLog();

    if (!std::strcmp(mode, "--throw")) {
        for (int kind = 0; kind < 2; ++kind)
            for (int twice = 0; twice < 2; ++twice) {             // one report per type
                try {
                    Throw(kind);
                } catch (const std::exception& e) {
                    Print("check: caught %s", e.what());
                }
            }
        return 0;
    }
    if (!std::strcmp(mode, "--unhandled")) {
        Print("check: a fault nothing catches");
        *reinterpret_cast<volatile int*>(0x18) = 3;
        return 1;
    }
    if (!std::strcmp(mode, "--abort")) {
        Print("check: abort()");
        std::abort();
    }
    if (!std::strcmp(mode, "--exit")) {
        Print("check: exit(3)");
        rnative::CrashLogExit();                 // (what the DLL's process detach calls)
        std::exit(3);
    }

    Print("check: fault in our code");
    FaultAt(&Poke, 0);

    Print("check: fault holding the heap and loader locks");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    auto lock = reinterpret_cast<LdrLockFn>(GetProcAddress(ntdll, "LdrLockLoaderLock"));
    auto unlock = reinterpret_cast<LdrUnlockFn>(GetProcAddress(ntdll, "LdrUnlockLoaderLock"));
    ULONG_PTR cookie = 0;
    HeapLock(GetProcessHeap());
    lock(0, nullptr, &cookie);
    FaultAt(&PokeLocked, 0x14);                                  // qnix's address
    unlock(0, cookie);
    HeapUnlock(GetProcessHeap());
    Print("check: still running after both");

    if (argc > 1 && !std::strcmp(argv[1], "--heap-smash")) {   // does the heap checking in use catch an overrun?
        auto* p = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, 40));
        std::memset(p, 0xCD, 160);                     // (not 0xAB: Wine fills block tails with it)
        const BOOL valid = HeapValidate(GetProcessHeap(), 0, nullptr);
        const BOOL freed = HeapFree(GetProcessHeap(), 0, p);
        Print("check: HeapValidate %d, HeapFree %d", valid, freed);
        Print("check: heap smashed (40-byte block, 160 written); NtGlobalFlag %08x, heap flags %08x",
              *reinterpret_cast<const uint32_t*>(__readfsdword(0x30) + 0x68),
              *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(GetProcessHeap()) + 0x40));
        return 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "--no-hang")) return 0;
    Print("check: hang (a thread keeps the heap lock and a critical section, this one waits on the section)");
    InitializeCriticalSection(&g_held);
    rnative::HangBeat();
    CreateThread(nullptr, 0, &HoldHeap, nullptr, 0, nullptr);
    Sleep(200);
    // Kills the process once the watchdog had its time (RANDYVK_HANG_SECONDS + 5 s): this thread is stuck in HeapAlloc.
    CreateThread(nullptr, 0, [](void*) -> DWORD {
        char s[8] = "20";
        GetEnvironmentVariableA("RANDYVK_HANG_SECONDS", s, sizeof(s));
        Sleep(DWORD(std::atoi(s) + 5) * 1000);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }, nullptr, 0, nullptr);
    EnterCriticalSection(&g_held);                 // blocks for good
    Print("check: unexpected - the section was free");
    return 1;
}
