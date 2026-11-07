// The software cursor grows about its physical click point, including its detail sprites.
#include "native/gui.h"
#include "native/native.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace rnative::gui {
namespace {

struct IPoint { int x, y; };
using MoveFn = void(__fastcall*)(void*, void*, const IPoint*, uint32_t);
using SpriteMoveFn = void(__fastcall*)(void*, void*, const IPoint*);
using SpriteScaleFn = void(__fastcall*)(void*, void*, const float*, const float*);
MoveFn g_move;
SpriteMoveFn g_spriteMove;
SpriteScaleFn g_spriteScale;
float g_scale = 1;
thread_local const IPoint* g_anchor;

void __fastcall SpriteMoveHook(void* sprite, void* edx, const IPoint* point)
{
    if (!g_anchor) { g_spriteMove(sprite, edx, point); return; }
    const IPoint scaled{
        g_anchor->x + int(std::lround((point->x - g_anchor->x) * g_scale)),
        g_anchor->y + int(std::lround((point->y - g_anchor->y) * g_scale))};
    g_spriteScale(sprite, nullptr, &g_scale, &g_scale);
    g_spriteMove(sprite, edx, &scaled);
}

void __fastcall MoveHook(void* pointer, void* edx, const IPoint* point, uint32_t details)
{
    struct Anchor {
        const IPoint* saved = g_anchor;
        ~Anchor() { g_anchor = saved; }
    } scope;
    g_anchor = point;
    g_move(pointer, edx, point, details);
}

}  // namespace

void SetCursorScale(float percent)
{
    if (std::isfinite(percent)) g_scale = std::clamp(percent, 50.0f, 300.0f) / 100;
}

void InstallCursorScaling(HMODULE gui)
{
    if (g_move || GetMode("UiScaling", Mode::On) == Mode::Off) return;
    auto* base = reinterpret_cast<uint8_t*>(gui);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (pe->FileHeader.TimeDateStamp != 0x5CD32BEA || pe->OptionalHeader.SizeOfImage != 0x2B0000) return;
    auto* entry = base + 0x1FDDD;
    int32_t relative;
    std::memcpy(&relative, base + 0x20288, 4);
    if (GetProcAddress(gui, "?FrameProcess@MousePointerModule_t@@CAXXZ") != reinterpret_cast<FARPROC>(base + 0x202DD) ||
        base[0x20287] != 0xE8 || base + 0x2028C + relative != entry) {
        Log("interface cursor: frame entry changed, left unchanged");
        return;
    }
    std::memcpy(&relative, entry + 6, 4);
    if (entry[0] != 0xB8 || entry[5] != 0xE8 || entry + 10 + relative != base + 0x1738A4) {
        Log("interface cursor: unexpected move entry, left unchanged");
        return;
    }
    HMODULE display = GetModuleHandleA("DisplaySystem.dll");
    auto** slot = reinterpret_cast<void**>(base + 0x1A73E4);
    auto move = display ? GetProcAddress(display, "?Reposition@RenderSprite_t@@QAEXABVIPoint@@@Z") : nullptr;
    auto scale = display ? GetProcAddress(display, "?SetScale@RenderSprite_t@@QAEXABM0@Z") : nullptr;
    if (!move || !scale || *slot != reinterpret_cast<void*>(move) ||
        *reinterpret_cast<void**>(base + 0x1A73D8) != reinterpret_cast<void*>(scale)) {
        Log("interface cursor: sprite imports changed, left unchanged");
        return;
    }
    DWORD protection;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return;
    const size_t rel[] = {6};
    g_move = reinterpret_cast<MoveFn>(HookAt(entry, 10, rel, 1, reinterpret_cast<void*>(&MoveHook), "MousePointer_t move"));
    if (g_move) {
        g_spriteMove = reinterpret_cast<SpriteMoveFn>(move);
        g_spriteScale = reinterpret_cast<SpriteScaleFn>(scale);
        *slot = reinterpret_cast<void*>(&SpriteMoveHook);
    }
    DWORD ignored;
    VirtualProtect(slot, sizeof(void*), protection, &ignored);
    if (g_move) Log("interface cursor: independent size, physical mouse coordinates retained");
}

}  // namespace rnative::gui
