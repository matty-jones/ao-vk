// A zone change as the game goes through it: the research for the zone hand-over (docs/zone-handover-plan.md).
//
// A playfield change is a server teleport. Gamecode.dll's n3EngineClientAnarchy_t::StartTeleportTry / Confirmed /
// Failed and N3.dll's n3EngineClient_t::StartTeleport (which stops the old playfield) and NewPlayfield (the new one's
// id) are hooked to log when they run, on which thread and for how long; GUI.dll's ServerLogin3DModule_t (the login
// and loading screen) through SetLoadingScreen and FrameProcess, and its state - the screen's kind, its windows, the
// closing flag the server's close-loadscreen message sets - read at each of its frames and each presented one. Around
// any of that (a window of 20 s after the last), every presented frame that took long, or whose 3D came or went, gets a
// line too: whether the game keeps presenting through a zone change, and with what.
//
// The hooks only observe: each runs the original whole and writes log lines. Their code is checked first, so a
// different client build is left alone.
#include "native/zone.h"

#include "native/native.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace rnative::zone {

namespace {

constexpr double kWindowMs = 20000.0;              // lines for this long after the last event
constexpr uint32_t kMaxLines = 1500;                // per window

LARGE_INTEGER g_freq;
std::atomic<int64_t> g_windowStart{0}, g_lastEvent{0};
std::atomic<uint32_t> g_lines{0};
DWORD g_gameThread;

// The game's globals, read from the hooked code (moved with the module).
const volatile uint32_t* g_playfieldId;             // N3: n3EngineClient_t's current playfield (GetPlayfield's)
const volatile int32_t* g_screenKind;               // GUI: ServerLogin3DModule_t's LoadingScreen_e
const volatile uint8_t* g_closing;                  // GUI: set by StartClosingLoadscreenMessage, cleared once faded
const void* const volatile* g_screenWindow;         // GUI: the loading screen's window (SetAlpha's first)

int64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double Ms(int64_t ticks) { return double(ticks) * 1000.0 / double(g_freq.QuadPart); }

bool WindowOpen(int64_t now)
{
    const int64_t last = g_lastEvent.load(std::memory_order_relaxed);
    return last && Ms(now - last) < kWindowMs;
}

// A line in the window: its time from the window's start, and the thread when it isn't the game's.
void Line(int64_t now, const char* text)
{
    if (g_lines.fetch_add(1, std::memory_order_relaxed) >= kMaxLines)
        return;
    const DWORD thread = GetCurrentThreadId();
    char other[32] = "";
    if (g_gameThread && thread != g_gameThread)
        std::snprintf(other, sizeof(other), " [thread %lu]", static_cast<unsigned long>(thread));
    Log("zone: +%6.0f ms %s%s", Ms(now - g_windowStart.load(std::memory_order_relaxed)), text, other);
}

// Something happened: opens a window (or keeps it open) and logs it.
void Event(const char* fmt, ...)
{
    char text[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    const int64_t now = Now();
    if (!WindowOpen(now)) {
        g_windowStart.store(now, std::memory_order_relaxed);
        g_lines.store(0, std::memory_order_relaxed);
        SYSTEMTIME t;
        GetLocalTime(&t);
        Log("zone: --- at %02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    }
    g_lastEvent.store(now, std::memory_order_relaxed);
    Line(now, text);
}

// The loading screen's state, compared with the last look: changes are events.
struct Screen {
    bool exists, window, closing;
    int32_t kind;
    uint32_t playfield;
    bool operator==(const Screen&) const = default;
};
Screen g_screen;

Screen Look()
{
    Screen s{};
    s.playfield = g_playfieldId ? *g_playfieldId : 0;
    s.kind = g_screenKind ? *g_screenKind : -1;
    s.closing = g_closing && *g_closing;
    s.window = g_screenWindow && *g_screenWindow;
    return s;
}

void Poll(const char* where)
{
    const Screen s = Look();
    if (s == g_screen)
        return;
    const Screen was = g_screen;
    g_screen = s;
    if (s.playfield != was.playfield)
        Event("playfield %u -> %u (%s)", was.playfield, s.playfield, where);
    if (s.kind != was.kind || s.window != was.window || s.closing != was.closing)
        Event("loading screen: kind %d, window %s, closing %s (%s)", s.kind, s.window ? "up" : "none",
              s.closing ? "yes" : "no", where);
}

// --- the hooks ---

using VoidFn = void(__cdecl*)();
using KindFn = void(__cdecl*)(int32_t kind);
using MemberFn = void(__fastcall*)(void* self, void* edx);
using TryFn = bool(__fastcall*)(void* self, void* edx);
using NewPlayfieldFn = void(__fastcall*)(void* self, void* edx, const void* playfield, const void* position);

KindFn g_setLoadingScreen;
VoidFn g_frameProcess;
TryFn g_teleportTry;
MemberFn g_teleportConfirmed, g_teleportFailed, g_startTeleport;
NewPlayfieldFn g_newPlayfield;

void __cdecl SetLoadingScreenHook(int32_t kind)
{
    Event("SetLoadingScreen(%d)", kind);
    g_setLoadingScreen(kind);
}

void __cdecl FrameProcessHook()
{
    g_frameProcess();
    Poll("loading screen's frame");
}

bool __fastcall TeleportTryHook(void* self, void* edx)
{
    // Called every frame while the game tries: only its outcome changing is logged.
    static int last = -1;
    const bool ok = g_teleportTry(self, edx);
    if (int(ok) != last) {
        last = ok;
        Event("StartTeleportTry -> %s", ok ? "true" : "false");
    }
    return ok;
}

void __fastcall TeleportConfirmedHook(void* self, void* edx)
{
    Event("StartTeleportConfirmed");
    g_teleportConfirmed(self, edx);
}

void __fastcall TeleportFailedHook(void* self, void* edx)
{
    Event("StartTeleportFailed");
    g_teleportFailed(self, edx);
}

void __fastcall StartTeleportHook(void* self, void* edx)
{
    Event("StartTeleport (the old playfield stops)");
    const int64_t t0 = Now();
    g_startTeleport(self, edx);
    Event("StartTeleport done (%.0f ms)", Ms(Now() - t0));
}

void __fastcall NewPlayfieldHook(void* self, void* edx, const void* playfield, const void* position)
{
    uint32_t id = 0;
    if (playfield)
        std::memcpy(&id, static_cast<const uint8_t*>(playfield) + 0x1C, 4);
    Event("NewPlayfield(%u)", id);
    const int64_t t0 = Now();
    g_newPlayfield(self, edx, playfield, position);
    Event("NewPlayfield done (%.0f ms)", Ms(Now() - t0));
}

// --- installing ---

uint8_t* Export(HMODULE module, const char* name)
{
    return reinterpret_cast<uint8_t*>(GetProcAddress(module, name));
}

bool Bytes(const uint8_t* at, std::initializer_list<int> expected)   // -1: any byte
{
    size_t i = 0;
    for (int b : expected) {
        if (b >= 0 && at[i] != uint8_t(b))
            return false;
        ++i;
    }
    return true;
}

template <class T> const T* Operand(const uint8_t* at)
{
    uint32_t address;
    std::memcpy(&address, at, 4);
    return reinterpret_cast<const T*>(uintptr_t(address));
}

template <class Fn> void Hook(Fn& out, uint8_t* at, size_t count, std::initializer_list<size_t> rel, void* target,
                              const char* what)
{
    out = reinterpret_cast<Fn>(HookAt(at, count, rel.size() ? rel.begin() : nullptr, rel.size(), target, what));
    if (!out) Log("zone: %s not hooked", what);
}

void InstallGui(HMODULE gui)
{
    // SetLoadingScreen: push ebp; mov ebp, esp; mov eax, [ebp+8]; mov [kind], eax.
    uint8_t* kind = Export(gui, "?SetLoadingScreen@ServerLogin3DModule_t@@SAXW4LoadingScreen_e@1@@Z");
    // StartClosingLoadscreenMessage: push ebp; mov ebp, esp; push ecx; mov byte [closing], 1.
    uint8_t* closing = Export(gui, "?StartClosingLoadscreenMessage@ServerLogin3DModule_t@@CAXXZ");
    // SetAlpha: push ebp; mov ebp, esp; push esi; mov esi, [window].
    uint8_t* alpha = Export(gui, "?SetAlpha@ServerLogin3DModule_t@@CAXM_N@Z");
    // FrameProcess: mov eax, <handler>; call <_EH_prolog>.
    uint8_t* frame = Export(gui, "?FrameProcess@ServerLogin3DModule_t@@CAXXZ");
    if (!kind || !closing || !alpha || !frame || !Bytes(kind, {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0xA3}) ||
        !Bytes(closing, {0x55, 0x8B, 0xEC, 0x51, 0xC6, 0x05, -1, -1, -1, -1, 0x01}) ||
        !Bytes(alpha, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x35}) || !Bytes(frame, {0xB8, -1, -1, -1, -1, 0xE8})) {
        Log("zone: GUI.dll's loading screen not as expected (another build?) - not watched");
        return;
    }
    g_screenKind = Operand<volatile int32_t>(kind + 7);
    g_closing = Operand<volatile uint8_t>(closing + 6);
    g_screenWindow = Operand<const void* volatile>(alpha + 6);
    Hook(g_setLoadingScreen, kind, 6, {}, reinterpret_cast<void*>(&SetLoadingScreenHook), "SetLoadingScreen");
    Hook(g_frameProcess, frame, 10, {6}, reinterpret_cast<void*>(&FrameProcessHook), "ServerLogin3DModule_t::FrameProcess");
    Log("zone: GUI.dll's loading screen watched");
}

void InstallGamecode(HMODULE gamecode)
{
    // StartTeleportTry: mov eax, <handler>; call <_EH_prolog>; push ecx; push ebx; push esi.
    uint8_t* attempt = Export(gamecode, "?StartTeleportTry@n3EngineClientAnarchy_t@@QAE_NXZ");
    // StartTeleportConfirmed / Failed: call TeleportTrier_t::Get; test eax, eax; je.
    uint8_t* confirmed = Export(gamecode, "?StartTeleportConfirmed@n3EngineClientAnarchy_t@@QAEXXZ");
    uint8_t* failed = Export(gamecode, "?StartTeleportFailed@n3EngineClientAnarchy_t@@QAEXXZ");
    if (!attempt || !confirmed || !failed ||
        !Bytes(attempt, {0xB8, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x51, 0x53, 0x56}) ||
        !Bytes(confirmed, {0xE8, -1, -1, -1, -1, 0x85, 0xC0, 0x74}) ||
        !Bytes(failed, {0xE8, -1, -1, -1, -1, 0x85, 0xC0, 0x74})) {
        Log("zone: Gamecode.dll's teleport not as expected (another build?) - not watched");
        return;
    }
    Hook(g_teleportTry, attempt, 10, {6}, reinterpret_cast<void*>(&TeleportTryHook), "StartTeleportTry");
    Hook(g_teleportConfirmed, confirmed, 5, {1}, reinterpret_cast<void*>(&TeleportConfirmedHook), "StartTeleportConfirmed");
    Hook(g_teleportFailed, failed, 5, {1}, reinterpret_cast<void*>(&TeleportFailedHook), "StartTeleportFailed");
    Log("zone: Gamecode.dll's teleport watched");
}

void InstallN3(HMODULE n3)
{
    // StartTeleport: push esi; mov esi, ecx; call GetPlayfield.
    uint8_t* start = Export(n3, "?StartTeleport@n3EngineClient_t@@QAEXXZ");
    // NewPlayfield: push ebp; mov ebp, esp; mov eax, [ebp+8]; mov eax, [eax+1Ch]; mov [current], eax.
    uint8_t* fresh = Export(n3, "?NewPlayfield@n3EngineClient_t@@UAEXPBVn3Playfield_t@@ABVVector3_t@@@Z");
    if (!start || !fresh || !Bytes(start, {0x56, 0x8B, 0xF1, 0xE8}) ||
        !Bytes(fresh, {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x40, 0x1C, 0xA3})) {
        Log("zone: N3.dll's playfield change not as expected (another build?) - not watched");
        return;
    }
    g_playfieldId = Operand<volatile uint32_t>(fresh + 10);
    Hook(g_startTeleport, start, 8, {4}, reinterpret_cast<void*>(&StartTeleportHook), "n3EngineClient_t::StartTeleport");
    Hook(g_newPlayfield, fresh, 6, {}, reinterpret_cast<void*>(&NewPlayfieldHook), "n3EngineClient_t::NewPlayfield");
    Log("zone: N3.dll's playfield change watched");
}

bool Enabled()
{
    static const bool on = GetMode("Zone", Mode::On) != Mode::Off;
    return on;
}

}  // namespace

void Install()
{
    if (!Enabled())
        return;
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    struct Module {
        const char* name;
        void (*install)(HMODULE);
        int attempts;
    };
    static Module modules[] = {{"GUI.dll", InstallGui, 0}, {"Gamecode.dll", InstallGamecode, 0}, {"N3.dll", InstallN3, 0}};
    for (Module& m : modules) {
        if (m.attempts >= 600)
            continue;
        ++m.attempts;
        if (HMODULE module = GetModuleHandleA(m.name)) {
            m.attempts = 600;
            m.install(module);
        }
    }
}

void Frame(uint32_t framesWithout3D)
{
    if (!Enabled() || !g_freq.QuadPart)
        return;
    if (!g_gameThread) g_gameThread = GetCurrentThreadId();
    static int64_t last;
    static uint32_t lastWithout3D, sinceLine;
    const int64_t now = Now();
    const double gap = last ? Ms(now - last) : 0.0;
    last = now;
    if (gap > 250.0)
        Event("no frame for %.0f ms", gap);
    Poll("presented frame");
    const bool came = (framesWithout3D == 0) != (lastWithout3D == 0);
    lastWithout3D = framesWithout3D;
    ++sinceLine;
    if (!WindowOpen(now) || !(gap > 40.0 || came || sinceLine >= 30))
        return;
    sinceLine = 0;
    char text[160];
    std::snprintf(text, sizeof(text), "frame (%.0f ms since the last), %s", gap,
                  framesWithout3D ? "no 3D" : "3D");
    if (framesWithout3D > 1)
        std::snprintf(text + std::strlen(text), sizeof(text) - std::strlen(text), " for %u frames", framesWithout3D);
    Line(now, text);
}

}  // namespace rnative::zone
