#define NOMINMAX
#include "frame_pacer.h"
#include <mmsystem.h>

#include <algorithm>

namespace rvkproxy {

FramePacer::~FramePacer()
{
    StopTimer();
}

void FramePacer::StopTimer()
{
    if (m_timer) CloseHandle(m_timer);
    if (m_fineSleep) timeEndPeriod(1);
    m_timer = nullptr;
    m_timerTried = m_fineSleep = false;
}

bool FramePacer::Changed(HWND window)
{
    const bool focused = !window || GetForegroundWindow() == window;
    const bool minimized = window && IsIconic(window);
    const uint32_t reset = m_reset.load(std::memory_order_relaxed);
    const bool changed = window != m_window || focused != m_focused || minimized != m_minimized || reset != m_seenReset;
    m_window = window;
    m_focused = focused;
    m_minimized = minimized;
    m_seenReset = reset;
    return changed;
}

void FramePacer::SleepFor(FrameSchedule::Clock::duration remaining)
{
    if (!m_timerTried) {
        m_timerTried = true;
        m_fineSleep = timeBeginPeriod(1) == TIMERR_NOERROR;
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!m_timer) m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    using HundredNs = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
    LARGE_INTEGER due;
    due.QuadPart = -std::chrono::ceil<HundredNs>(remaining).count();
    if (m_timer && SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE) &&
        WaitForSingleObject(m_timer, INFINITE) == WAIT_OBJECT_0)
        return;
    Sleep(DWORD(std::chrono::ceil<std::chrono::milliseconds>(remaining).count()));
}

void FramePacer::Wait(uint32_t fps, HWND window)
{
    if (!fps) {
        m_schedule.Reset();
        if (m_timerTried) StopTimer();
        return;
    }
    if (Changed(window)) m_schedule.Reset();
    using Clock = FrameSchedule::Clock;
    auto now = Clock::now();
    auto deadline = m_schedule.Begin(now, fps);
    while (now < deadline) {
        // Recheck activation during long intervals without pumping game messages inside the renderer.
        SleepFor(std::min(deadline - now, std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(50))));
        now = Clock::now();
        if (Changed(window)) {
            m_schedule.Reset();
            m_schedule.Begin(now, fps);
            break;
        }
    }
    m_schedule.Finish(now);
}

}  // namespace rvkproxy
