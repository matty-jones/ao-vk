// Game-thread frame deadlines and a sleeping Windows timer.
#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rvkproxy {

class FrameSchedule {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    Time Begin(Time now, uint32_t fps)
    {
        if (!fps) {
            Reset();
            return now;
        }
        if (fps != m_fps) {
            m_fps = fps;
            m_period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
            m_next = now;
        }
        // Work or a pause has already used the interval: start now, without catching up.
        if (now >= m_next) m_next = now;
        return m_next;
    }

    void Finish(Time now)
    {
        if (!m_fps) return;
        m_next += m_period;
        if (m_next <= now) m_next = now + m_period;
    }

    void Reset() { m_fps = 0; }

private:
    uint32_t m_fps = 0;
    Clock::duration m_period{};
    Time m_next{};
};

class FramePacer {
public:
    ~FramePacer();
    void Wait(uint32_t fps, HWND window);
    void Reset() { m_reset.fetch_add(1, std::memory_order_relaxed); }

private:
    void SleepFor(FrameSchedule::Clock::duration remaining);
    void StopTimer();
    bool Changed(HWND window);

    FrameSchedule m_schedule;
    HANDLE m_timer = nullptr;
    bool m_timerTried = false;
    bool m_fineSleep = false;
    HWND m_window = nullptr;
    bool m_focused = false, m_minimized = false;
    std::atomic<uint32_t> m_reset{0};
    uint32_t m_seenReset = 0;
};

}  // namespace rvkproxy
