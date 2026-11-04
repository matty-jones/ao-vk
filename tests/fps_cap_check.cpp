// Deterministic deadlines, then the real Windows wait path (also run under Wine).
#define NOMINMAX
#include "ddraw/frame_pacer.h"
#include "ddraw/rvk_settings.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>

namespace {

using rvkproxy::FramePacer;
using rvkproxy::FrameSchedule;
using Clock = FrameSchedule::Clock;
using Time = FrameSchedule::Time;
using namespace std::chrono_literals;
int g_failed = 0;

void Check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++g_failed; }
}

void Deadlines()
{
    static_assert(Clock::is_steady);
    for (uint32_t fps : {1u, 30u, 60u, 144u, 1000u}) {
        FrameSchedule schedule;
        Time now = Time{} + 10s;
        const Time start = now;
        for (uint32_t i = 0; i <= fps * 3; ++i) {
            const Time deadline = schedule.Begin(now, fps);
            Check(deadline >= now, "no deadline in the past");
            const double elapsed = std::chrono::duration<double>(deadline - start).count();
            Check(std::abs(elapsed - double(i) / fps) < 0.00001, "stable cadence, including fractional milliseconds");
            // A small scheduler oversleep is absorbed by the next deadline, rather than accumulating.
            schedule.Finish(deadline + 100us);
            now = deadline + 200us;
        }
    }

    FrameSchedule schedule;
    Time now = Time{} + 10s;
    schedule.Begin(now, 60);
    schedule.Finish(now);
    now += 2s;
    Check(schedule.Begin(now, 60) == now, "stalled or paused frame starts immediately");
    schedule.Finish(now);
    Check(schedule.Begin(now + 1ms, 60) > now + 16ms, "no catch-up burst after a stall");
    Check(schedule.Begin(now + 2ms, 30) == now + 2ms, "a changed cap discards the old deadline");
    schedule.Finish(now + 2ms);
    Check(schedule.Begin(now + 3ms, 0) == now + 3ms, "zero disables the cap");
    Check(schedule.Begin(now + 4ms, 30) == now + 4ms, "re-enabling starts a fresh schedule");
    schedule.Finish(now + 4ms);
    schedule.Reset();
    Check(schedule.Begin(now + 5ms, 30) == now + 5ms, "focus or window reset discards the deadline");
    schedule.Finish(now + 5ms);
    Time deadline = schedule.Begin(now + 6ms, 30);
    schedule.Finish(deadline + 500ms);
    Check(schedule.Begin(deadline + 501ms, 30) > deadline + 533ms, "long timer oversleep rebases the schedule");

    schedule.Reset();
    for (int i = 0; i < 100; ++i) {
        now += 50ms;
        Check(schedule.Begin(now, 60) == now, "work slower than the cap does not add another interval");
        schedule.Finish(now);
    }
}

uint64_t ThreadTicks()
{
    FILETIME created, exited, kernel, user;
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}

void Sleeping()
{
    FramePacer pacer;
    for (uint32_t fps : {30u, 60u, 144u}) {
        pacer.Reset();
        pacer.Wait(fps, nullptr);
        const auto start = Clock::now();
        const auto cpuStart = ThreadTicks();
        for (uint32_t i = 0; i < fps; ++i) pacer.Wait(fps, nullptr);
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        const double cpu = double(ThreadTicks() - cpuStart) * 1e-7;
        std::printf("%u FPS: %.4f s wall, %.4f s thread CPU\n", fps, seconds, cpu);
        Check(seconds >= 0.99 && seconds < 1.15, "sleeping cadence stays near the requested rate");
        Check(cpu < seconds * 0.25, "waiting sleeps instead of consuming a core");
    }
    pacer.Wait(1, nullptr);                              // a one-second deadline must not survive disabling/reset
    auto start = Clock::now();
    for (int i = 0; i < 1000; ++i) pacer.Wait(0, nullptr);
    Check(Clock::now() - start < 100ms, "unlimited does not sleep");
    pacer.Wait(1, nullptr);
    pacer.Reset();
    start = Clock::now();
    pacer.Wait(1, nullptr);
    Check(Clock::now() - start < 100ms, "activation reset does not wait on a stale deadline");
    std::thread reset([&] { Sleep(30); pacer.Reset(); });
    start = Clock::now();
    pacer.Wait(1, nullptr);
    reset.join();
    Check(Clock::now() - start < 250ms, "activation interrupts a long interval promptly");

    HWND window = CreateWindowExW(0, L"STATIC", L"ao-vk pacing check", WS_OVERLAPPEDWINDOW,
                                   0, 0, 320, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "test window created");
    if (window) {
        ShowWindow(window, SW_SHOW);
        pacer.Wait(1, window);
        ShowWindow(window, SW_MINIMIZE);
        start = Clock::now();
        pacer.Wait(1, window);
        Check(Clock::now() - start < 100ms, "minimizing discards the old deadline");
        ShowWindow(window, SW_RESTORE);
        start = Clock::now();
        pacer.Wait(1, window);
        Check(Clock::now() - start < 100ms, "restoring discards the old deadline");
        DestroyWindow(window);
    }
}

// --settings <dll>: use a copy in a temporary folder; loading it creates that folder's ini.
void Settings(const char* path)
{
    HMODULE dll = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Check(dll != nullptr, "built renderer DLL loads");
    if (!dll) return;
    auto count = reinterpret_cast<decltype(&RvkSettings_Count)>(GetProcAddress(dll, "RvkSettings_Count"));
    auto get = reinterpret_cast<decltype(&RvkSettings_Get)>(GetProcAddress(dll, "RvkSettings_Get"));
    auto set = reinterpret_cast<decltype(&RvkSettings_Set)>(GetProcAddress(dll, "RvkSettings_Set"));
    Check(count && get && set, "settings API exported");
    if (count && get && set) {
        bool found = false;
        for (uint32_t i = 0; i < count(); ++i) {
            RvkSettingInfo info{};
            info.size = sizeof(info);
            if (!get(i, &info) || std::strcmp(info.name, "RVK_FpsCap") != 0) continue;
            found = true;
            Check(info.defaultValue == 0 && info.min == 0 && info.max == 1000 && info.type == 1 && !info.parent,
                  "frame cap is an independent integer setting, default unlimited");
            const float saved = info.value;
            char ini[MAX_PATH];
            GetModuleFileNameA(dll, ini, MAX_PATH);
            char* filename = std::strrchr(ini, '\\') + 1;
            std::snprintf(filename, sizeof(ini) - size_t(filename - ini), "randy-vk.ini");
            for (float value : {60.0f, 0.0f, 144.0f}) {
                Check(set("RVK_FpsCap", value) && get(i, &info) && info.value == value, "frame cap changes via settings API");
                Check(GetPrivateProfileIntA("Renderer", "RVK_FpsCap", -1, ini) == int(value), "frame cap saved under Renderer");
            }
            set("RVK_FpsCap", saved);
        }
        Check(found, "frame cap appears in the in-game settings list");
    }
    FreeLibrary(dll);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--settings") == 0) Settings(argv[2]);
    else { Deadlines(); Sleeping(); }
    std::printf("fps_cap_check: %s (%d failures)\n", g_failed ? "FAILED" : "passed", g_failed);
    return g_failed ? 1 : 0;
}
