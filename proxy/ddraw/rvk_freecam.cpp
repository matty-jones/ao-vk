// A free camera for debugging (Ctrl+Shift+C): flying through the scene with the mouse and WASD while the character
// stays put.
//
// - Where it is: FreeCamWorld, from the native viewport's scene update (proxy/native/viewport.cpp Process), puts it in
//   the game's own camera before its view and frustum are made - what the game draws and culls follows it. It starts
//   where the game's camera is. (Without the native scene: the renderer's own override, Device::SetFreeCam - the view
//   only, the game culling from its own camera.)
// - Its controls (FreeCamFrame, each presented frame, while the game's window has the focus): W / S forward and back,
//   A / D sideways, E / Q up and down, Shift four times as fast, the mouse wheel faster or slower; the mouse looks about
//   (the cursor hidden and kept in the window's middle).
// - The game kept from the input meanwhile: DirectInput (DisplaySystem.dll's keyboard and mouse) reads nothing - the
//   device method table every DirectInput device of the process shares is found by making a device of our own, and
//   its GetDeviceState / GetDeviceData give an idle state; and the game's window gets no key, mouse or raw input
//   messages (rvk_input.cpp, shared with the console). Our own keys come from GetAsyncKeyState, which neither
//   touches.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "rvk_backend.h"

namespace rvkproxy {

namespace {

struct State {
    bool on = false, placed = false;            // on; placed: started from the game's camera
    float pos[3] = {}, yaw = 0.0f, pitch = 0.0f, speed = 8.0f;
    LARGE_INTEGER last{};
    uint64_t nativeFrame = 0;                   // the last present the native scene took it (FreeCamWorld)
    uint64_t onSince = 0;                        // the present it was turned on at
    bool deviceOverride = false;                 // the renderer's own override in use (no native scene)
    float wheel = 0.0f;                          // wheel notches since the last frame (the window procedure)
} g;
uint64_t g_presents = 0;

}  // namespace

bool FreeCamOn() { return g.on; }

void FreeCamWheel(float notches) { g.wheel += notches; }

void FreeCamToggle(HWND window)
{
    g.on = !g.on;
    if (g.on) {
        InputAttach(window);
        g.placed = false;
        g.onSince = g_presents;
        g.last.QuadPart = 0;
        g.wheel = 0.0f;
        ShowCursor(FALSE);
    } else {
        ShowCursor(TRUE);
    }
    InputBlock(kInputFreeCam, g.on, g.on);
    RvkLog("free camera %s (mouse: look; W/S/A/D: move, E/Q: up/down, Shift: faster, wheel: speed)", g.on ? "on" : "off");
}

// The native scene's camera, once a frame before its view and frustum: false while off; else its world matrix the
// free camera's (D3D rows: right, up, forward, position) - started, the first time, where `game` (its own) is.
bool FreeCamWorld(const float game[16], float out[16])
{
    if (!g.on) return false;
    g.nativeFrame = g_presents;
    if (!g.placed) {
        g.pos[0] = game[12];
        g.pos[1] = game[13];
        g.pos[2] = game[14];
        const float fl = std::sqrt(game[8] * game[8] + game[9] * game[9] + game[10] * game[10]);
        if (fl < 1e-6f) return false;
        g.yaw = std::atan2(game[8], game[10]);
        g.pitch = std::asin(std::clamp(game[9] / fl, -1.0f, 1.0f));
        g.placed = true;
    }
    const float cy = std::cos(g.yaw), sy = std::sin(g.yaw), cp = std::cos(g.pitch), sp = std::sin(g.pitch);
    const float f[3] = {cp * sy, sp, cp * cy}, r[3] = {cy, 0.0f, -sy};
    const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    std::memset(out, 0, 64);
    for (int i = 0; i < 3; ++i) {
        out[i] = r[i];
        out[4 + i] = u[i];
        out[8 + i] = f[i];
        out[12 + i] = g.pos[i];
    }
    out[15] = 1.0f;
    return true;
}

// Each presented frame: the keys and the mouse into the free camera (placed by the native scene; without it, the
// renderer's own override).
void FreeCamFrame(rvk::ThreadedDevice* device, HWND window)
{
    ++g_presents;
    static bool wasOn = false;
    if (!g.on && !wasOn) return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = g.last.QuadPart ? float(double(now.QuadPart - g.last.QuadPart) / double(freq.QuadPart)) : 0.0f;
    g.last = now;
    float move[3] = {}, look[2] = {};
    if (g.on && InputFocused() && !ConsoleOpen()) {
        auto key = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
        move[0] = (key('D') ? 1.0f : 0.0f) - (key('A') ? 1.0f : 0.0f);
        move[1] = (key('E') ? 1.0f : 0.0f) - (key('Q') ? 1.0f : 0.0f);
        move[2] = (key('W') ? 1.0f : 0.0f) - (key('S') ? 1.0f : 0.0f);
        if (g.wheel != 0.0f) {
            g.speed = std::clamp(g.speed * std::pow(1.25f, g.wheel), 0.5f, 400.0f);
            g.wheel = 0.0f;
        }
        // The mouse: how far from the window's middle since last frame, and back there.
        RECT rc;
        if (window && GetClientRect(window, &rc)) {
            POINT mid{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2}, cursor;
            ClientToScreen(window, &mid);
            if (GetCursorPos(&cursor) && dt > 0.0f) {
                look[0] = float(cursor.x - mid.x) * 0.0035f;
                look[1] = -float(cursor.y - mid.y) * 0.0035f;
            }
            SetCursorPos(mid.x, mid.y);
        }
    }
    const float step = g.speed * (GetAsyncKeyState(VK_SHIFT) & 0x8000 ? 4.0f : 1.0f) * std::clamp(dt, 0.0f, 0.1f);
    // (The native scene takes it within a frame or two of turning on; only without one, the renderer's override.)
    const bool native = g.nativeFrame + 2 >= g_presents && g.placed;
    const bool fallback = g.on && !native && g_presents > g.onSince + 3;
    if (!fallback) {
        if (g.on) {
            g.yaw += look[0];
            g.pitch = std::clamp(g.pitch + look[1], -1.55f, 1.55f);
            const float cy = std::cos(g.yaw), sy = std::sin(g.yaw), cp = std::cos(g.pitch), sp = std::sin(g.pitch);
            const float f[3] = {cp * sy, sp, cp * cy}, r[3] = {cy, 0.0f, -sy};
            for (int i = 0; i < 3; ++i)
                g.pos[i] += (r[i] * move[0] + (i == 1 ? move[1] : 0.0f) + f[i] * move[2]) * step;
        }
        if (g.deviceOverride) {                  // (the renderer's override off, if it was the one in use)
            rvk::Device::FreeCamInput in;
            device->SetFreeCam(in);
            g.deviceOverride = false;
        }
    } else {                                     // no native scene: the renderer's own override (the view only)
        g.deviceOverride = true;
        rvk::Device::FreeCamInput in;
        in.on = true;
        std::memcpy(in.move, move, sizeof(move));
        std::memcpy(in.look, look, sizeof(look));
        in.speed = g.speed * (GetAsyncKeyState(VK_SHIFT) & 0x8000 ? 4.0f : 1.0f);
        in.dt = std::clamp(dt, 0.0f, 0.1f);
        device->SetFreeCam(in);
    }
    wasOn = g.on;
}

}  // namespace rvkproxy
