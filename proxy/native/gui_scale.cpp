// Per-window logical coordinates: layout and input share the scale used to draw and clip each view.
#include "native/gui.h"
#include "native/gui_scale.h"
#include "native/native.h"
#include "native/vc10.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace rnative::gui {
namespace {

using ViewFn = void*(__thiscall*)(void*);
using NameFn = vc10::String*(__thiscall*)(void*, vc10::String*);
using SizeFn = Point*(__thiscall*)(void*, Point*);
using FrameFn = Rect*(__fastcall*)(void*, void*, Rect*);
using MoveFn = void(__thiscall*)(void*, float, float);
ViewFn g_top, g_child, g_sibling, g_looper, g_windowClient;
NameFn g_name;
SizeFn g_screen;
Point g_screenSize{};
FrameFn g_frame, g_borders;
MoveFn g_resize, g_move;
SizeFn g_windowMouse;
using ScreenFn = Point*(__cdecl*)(Point*);
ScreenFn g_windowScreen;

struct Window {
    void* owner;
    Scale scale;
    bool fullScreen;
};
std::unordered_map<void*, Window> g_windows;
std::unordered_map<void*, Scale> g_scales;
float g_windowScale = 1.0f, g_hudScale = 1.0f;
float g_wantedWindow = 1.0f, g_wantedHud = 1.0f;
bool g_installed, g_controllerWork, g_redraw;
int g_coordinates;
float g_placementScale;
using PlacementFn = void(__fastcall*)(void*, void*, uint32_t);
PlacementFn g_placement;

struct Coordinates {
    bool outer = g_coordinates++ == 0;
    ~Coordinates() { --g_coordinates; }
};

bool Hud(void* window);

Scale For(void* view)
{
    if (!g_installed || (g_windowScale == 1.0f && g_hudScale == 1.0f)) return {};
    void* owner = g_looper(view);
    auto it = g_scales.find(owner);
    if (it != g_scales.end()) return it->second;
    return owner ? Scale{Hud(owner) ? g_hudScale : g_windowScale} : Scale{};
}

const char* TypeOf(void* window)
{
    const auto* table = *static_cast<const uintptr_t* const*>(window);
    const auto* locator = table ? reinterpret_cast<const uint32_t*>(table[-1]) : nullptr;
    return locator && locator[3] ? reinterpret_cast<const char*>(locator[3] + 8) : "";
}

bool Hud(void* window)
{
    const char* type = TypeOf(window);
    if (!std::strcmp(type, ".?AVCharBarWindow_c@@") || !std::strcmp(type, ".?AVCompassWindow_c@@") ||
        !std::strcmp(type, ".?AVShortcutBarContainer@@") || !std::strcmp(type, ".?AVShortcutBarWindow_c@@") ||
        !std::strcmp(type, ".?AVControlCenterWindow_c@@") || !std::strcmp(type, ".?AVNotificationWindow_c@@"))
        return true;
    vc10::String text{};
    g_name(window, &text);
    const std::string name(text.c_str(), text.size);
    if (text.capacity > 15) vc10::Free(text.pointer);
    return name == "control_center_window" || name == "compass_window" || name == "shortcutbar_window" ||
           name == "notification_window" || name.compare(0, 3, "cc_") == 0 || name.compare(0, 2, "CC") == 0;
}

Rect* __fastcall FrameHook(void* view, void* edx, Rect* out)
{
    g_frame(view, edx, out);
    if (g_controllerWork && !g_coordinates && g_placementScale == 0.0f) {
        auto it = g_windows.find(view);
        if (it != g_windows.end()) it->second.scale.ToScreen(*out);
    }
    return out;
}

Point* __cdecl WindowScreenHook(Point* out)
{
    g_windowScreen(out);
    Scale{g_installed ? (g_placementScale > 0.0f ? g_placementScale : g_windowScale) : 1.0f}.FromScreen(*out);
    return out;
}

Point* __fastcall WindowMouseHook(void* window, void*, Point* out)
{
    g_windowMouse(window, out);
    void* client = g_windowClient(window);
    const Scale scale = For(client);
    if (scale.value != 1.0f) {
        Rect border;
        g_borders(client, nullptr, &border);
        out->x -= border.left * (1.0f - 1.0f / scale.value);
        out->y -= border.top * (1.0f - 1.0f / scale.value);
    }
    return out;
}

void __fastcall PlacementHook(void* window, void* edx, uint32_t centered)
{
    const float saved = g_placementScale;
    g_placementScale = g_installed ? (Hud(window) ? g_hudScale : g_windowScale) : 1.0f;
    g_placement(window, edx, centered);
    g_placementScale = saved;
}

Scale DrawingScale(void* view)
{
    Scale scale = For(view);
    if (g_placementScale > 0.0f) scale.value /= g_placementScale;
    return scale;
}

// The value and pointer overloads recurse differently in GUI.dll. Transform only at the outermost boundary.
#define SCREEN_HOOKS(Type, Tag) \
    using Tag##ValueFn = Type*(__fastcall*)(void*, void*, Type*, const Type*, uint32_t); \
    using Tag##PointerFn = void(__fastcall*)(void*, void*, Type*, uint32_t); \
    Tag##ValueFn g_to##Tag##Value, g_from##Tag##Value; \
    Tag##PointerFn g_to##Tag##Pointer, g_from##Tag##Pointer; \
    Type* __fastcall To##Tag##Value(void* view, void* edx, Type* out, const Type* in, uint32_t scaled) \
    { \
        Coordinates scope; \
        g_to##Tag##Value(view, edx, out, in, scaled); \
        if (scope.outer && scaled) DrawingScale(view).ToScreen(*out); \
        return out; \
    } \
    void __fastcall To##Tag##Pointer(void* view, void* edx, Type* in, uint32_t scaled) \
    { \
        Coordinates scope; \
        g_to##Tag##Pointer(view, edx, in, scaled); \
        if (scope.outer && scaled) DrawingScale(view).ToScreen(*in); \
    } \
    Type* __fastcall From##Tag##Value(void* view, void* edx, Type* out, const Type* in, uint32_t scaled) \
    { \
        Coordinates scope; \
        Type logical = *in; \
        if (scope.outer && scaled) For(view).FromScreen(logical); \
        return g_from##Tag##Value(view, edx, out, &logical, scaled); \
    } \
    void __fastcall From##Tag##Pointer(void* view, void* edx, Type* in, uint32_t scaled) \
    { \
        Coordinates scope; \
        if (scope.outer && scaled) For(view).FromScreen(*in); \
        g_from##Tag##Pointer(view, edx, in, scaled); \
    }
SCREEN_HOOKS(Point, Point)
SCREEN_HOOKS(Rect, Rect)
#undef SCREEN_HOOKS

#define PARENT_HOOKS(Type, Tag) \
    using Tag##ParentValueFn = Type*(__fastcall*)(void*, void*, Type*, const Type*); \
    using Tag##ParentPointerFn = void(__fastcall*)(void*, void*, Type*); \
    Tag##ParentValueFn g_parent##Tag##Value; \
    Tag##ParentPointerFn g_parent##Tag##Pointer; \
    Type* __fastcall Parent##Tag##Value(void* view, void* edx, Type* out, const Type* in) \
    { \
        Coordinates scope; \
        Type logical = *in; \
        auto it = g_windows.find(view); \
        if (scope.outer && it != g_windows.end()) it->second.scale.FromScreen(logical); \
        return g_parent##Tag##Value(view, edx, out, &logical); \
    } \
    void __fastcall Parent##Tag##Pointer(void* view, void* edx, Type* in) \
    { \
        Coordinates scope; \
        auto it = g_windows.find(view); \
        if (scope.outer && it != g_windows.end()) it->second.scale.FromScreen(*in); \
        g_parent##Tag##Pointer(view, edx, in); \
    }
PARENT_HOOKS(Point, Point)
PARENT_HOOKS(Rect, Rect)
#undef PARENT_HOOKS

struct Hook {
    const char* name;
    uint32_t rva;
    const char* prefix;
    size_t bytes;
    void* replacement;
    void** original;
};

Hook g_hooks[] = {
    {"?MoveToMouse@Window@@QAEX_N@Z", 0x154E16, "", 10,
      reinterpret_cast<void*>(&PlacementHook), reinterpret_cast<void**>(&g_placement)},
    {"?ConvertToScreen@View@@QBE?AVPoint@@ABV2@_N@Z", 0x14BFC7, "", 10,
      reinterpret_cast<void*>(&ToPointValue), reinterpret_cast<void**>(&g_toPointValue)},
    {"?ConvertToScreen@View@@QBEXPAVPoint@@_N@Z", 0x14C123, "", 10,
      reinterpret_cast<void*>(&ToPointPointer), reinterpret_cast<void**>(&g_toPointPointer)},
    {"?ConvertFromScreen@View@@QBE?AVPoint@@ABV2@_N@Z", 0x14C4A0, "\x55\x8b\xec\x83\xec\x18", 6,
      reinterpret_cast<void*>(&FromPointValue), reinterpret_cast<void**>(&g_fromPointValue)},
    {"?ConvertFromScreen@View@@QBEXPAVPoint@@_N@Z", 0x14C512, "\x55\x8b\xec\x83\xec\x10", 6,
      reinterpret_cast<void*>(&FromPointPointer), reinterpret_cast<void**>(&g_fromPointPointer)},
    {"?ConvertFromParent@View@@QBE?AVPoint@@ABV2@@Z", 0x14BA97, "\x55\x8b\xec\x8b\x81\xac\x00\x00\x00", 9,
      reinterpret_cast<void*>(&ParentPointValue), reinterpret_cast<void**>(&g_parentPointValue)},
    {"?ConvertFromParent@View@@QBEXPAVPoint@@@Z", 0x14BAD2, "\x55\x8b\xec\x8b\x81\xac\x00\x00\x00", 9,
      reinterpret_cast<void*>(&ParentPointPointer), reinterpret_cast<void**>(&g_parentPointPointer)},
    {"?ConvertToScreen@View@@QBE?AVRect@@ABV2@_N@Z", 0x14C230, "", 10,
      reinterpret_cast<void*>(&ToRectValue), reinterpret_cast<void**>(&g_toRectValue)},
    {"?ConvertToScreen@View@@QBEXPAVRect@@_N@Z", 0x14C366, "", 10,
      reinterpret_cast<void*>(&ToRectPointer), reinterpret_cast<void**>(&g_toRectPointer)},
    {"?ConvertFromScreen@View@@QBE?AVRect@@ABV2@_N@Z", 0x14C560, "", 10,
      reinterpret_cast<void*>(&FromRectValue), reinterpret_cast<void**>(&g_fromRectValue)},
    {"?ConvertFromScreen@View@@QBEXPAVRect@@_N@Z", 0x14C610, "\x55\x8b\xec\x83\xec\x10", 6,
      reinterpret_cast<void*>(&FromRectPointer), reinterpret_cast<void**>(&g_fromRectPointer)},
    {"?ConvertFromParent@View@@QBE?AVRect@@ABV2@@Z", 0x14BB07, "", 10,
      reinterpret_cast<void*>(&ParentRectValue), reinterpret_cast<void**>(&g_parentRectValue)},
    {"?ConvertFromParent@View@@QBEXPAVRect@@@Z", 0x14BB74, "\x55\x8b\xec\x8b\x81\xac\x00\x00\x00", 9,
      reinterpret_cast<void*>(&ParentRectPointer), reinterpret_cast<void**>(&g_parentRectPointer)},
    {"?GetFrame@View@@QBE?AVRect@@XZ", 0x14AF23, "\x55\x8b\xec\x51\x8b\x45\x08", 7,
      reinterpret_cast<void*>(&FrameHook), reinterpret_cast<void**>(&g_frame)},
    {"?GetMousePosition@Window@@QBE?AVPoint@@XZ", 0x1542FB, "", 10,
      reinterpret_cast<void*>(&WindowMouseHook), reinterpret_cast<void**>(&g_windowMouse)},
};

}  // namespace

void SetScale(float windows, float hud)
{
    auto factor = [](float percent) { return std::isfinite(percent) ? std::clamp(percent, 50.0f, 200.0f) / 100.0f : 1.0f; };
    windows = factor(windows); hud = factor(hud);
    if (windows == g_wantedWindow && hud == g_wantedHud) return;
    g_wantedWindow = windows; g_wantedHud = hud;
    g_redraw = true;
}

bool TakeScaleRedraw()
{
    const bool changed = g_redraw;
    g_redraw = false;
    return changed;
}

void InstallScaling(HMODULE gui)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(gui);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(gui) + dos->e_lfanew);
    if (pe->FileHeader.TimeDateStamp != 0x5CD32BEA || pe->OptionalHeader.SizeOfImage != 0x2B0000) {
        Log("interface scaling: unknown GUI.dll build, left unchanged");
        return;
    }
    auto address = [&](const char* name) { return GetProcAddress(gui, name); };
    g_top = reinterpret_cast<ViewFn>(address("?GetToplevelView@WindowController_c@@QBEPAVView@@XZ"));
    g_child = reinterpret_cast<ViewFn>(address("?GetTopChild@View@@QBEPAV1@XZ"));
    g_sibling = reinterpret_cast<ViewFn>(address("?GetLowerSibling@View@@QBEPAV1@XZ"));
    g_windowClient = reinterpret_cast<ViewFn>(address("?_GetClientView@Window@@ABEPAVView@@XZ"));
    g_borders = reinterpret_cast<FrameFn>(address("?GetBorderSizes@TabView@@QBE?AVRect@@XZ"));
    g_screen = reinterpret_cast<SizeFn>(address("?GetScreenSize@WindowController_c@@QBE?AVPoint@@XZ"));
    g_resize = reinterpret_cast<MoveFn>(address("?ResizeTo@Window@@QAEXMM@Z"));
    g_move = reinterpret_cast<MoveFn>(address("?MoveTo@Window@@QAEXMM@Z"));
    HMODULE utils = GetModuleHandleA("Utils.dll");
    g_looper = utils ? reinterpret_cast<ViewFn>(GetProcAddress(utils, "?GetLooper@Handler@@QBEPAVLooper@@XZ")) : nullptr;
    g_name = utils ? reinterpret_cast<NameFn>(GetProcAddress(utils, "?GetName@Looper@@QBE?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ")) : nullptr;
    if (!g_top || !g_child || !g_sibling || !g_windowClient || !g_borders || !g_screen || !g_resize || !g_move || !g_looper || !g_name) {
        Log("interface scaling: required GUI exports unavailable, left at 100%%");
        return;
    }
    // Check the entire group before patching; a partial group never enables scaling.
    for (const Hook& h : g_hooks) {
        auto* at = reinterpret_cast<uint8_t*>(address(h.name));
        if (!at || at != reinterpret_cast<uint8_t*>(gui) + h.rva) {
            Log("interface scaling: %s unavailable, left at 100%%", h.name);
            return;
        }
        if (*h.prefix) {
            if (std::memcmp(at, h.prefix, h.bytes) != 0) {
                Log("interface scaling: %s changed, left at 100%%", h.name);
                return;
            }
        } else {
            int32_t rel;
            std::memcpy(&rel, at + 6, 4);
            if (at[0] != 0xB8 || at[5] != 0xE8 || at + 10 + rel != reinterpret_cast<uint8_t*>(gui) + 0x1738A4) {
                Log("interface scaling: %s changed, left at 100%%", h.name);
                return;
            }
        }
    }
    auto* screenEntry = reinterpret_cast<uint8_t*>(address("?GetScreenSize@Window@@SA?AVPoint@@XZ"));
    if (screenEntry != reinterpret_cast<uint8_t*>(gui) + 0x154850 ||
        std::memcmp(screenEntry, "\x55\x8b\xec\xe8", 4) != 0) {
        Log("interface scaling: logical screen hook unavailable, left at 100%%");
        return;
    }
    for (const Hook& h : g_hooks) {
        const size_t rel = 6;
        *h.original = HookAt(reinterpret_cast<uint8_t*>(gui) + h.rva, h.bytes, *h.prefix ? nullptr : &rel,
                             *h.prefix ? 0 : 1, h.replacement, h.name);
        if (!*h.original) return;
    }
    const size_t screenRel = 4;
    g_windowScreen = reinterpret_cast<ScreenFn>(HookAt(screenEntry, 8, &screenRel, 1,
                                                     reinterpret_cast<void*>(&WindowScreenHook), "Window::GetScreenSize"));
    if (!g_windowScreen) return;
    g_installed = true;
    Log("interface scaling: window and HUD coordinates hooked");
}

bool BeginScaleFrame(void* controller)
{
    if (!g_installed) return false;
    g_controllerWork = false;
    g_windowScale = g_wantedWindow;
    g_hudScale = g_wantedHud;
    Point screen;
    g_screen(controller, &screen);
    const bool resized = screen.x != g_screenSize.x || screen.y != g_screenSize.y;
    g_screenSize = screen;
    if (resized) g_redraw = true;
    std::unordered_map<void*, Window> current;
    for (void* root = g_child(g_top(controller)); root; root = g_sibling(root)) {
        void* owner = g_looper(root);
        if (!owner) continue;
        const bool hud = Hud(owner);
        const Scale scale{hud ? g_hudScale : g_windowScale};
        Rect frame;
        g_frame(root, nullptr, &frame);
        auto previous = g_windows.find(root);
        const bool known = previous != g_windows.end() && previous->second.owner == owner;
        const bool full = known ? previous->second.fullScreen :
                          !std::strcmp(TypeOf(owner), ".?AVControlCenterWindow_c@@") ||
                          (hud && frame.right - frame.left + 1 >= screen.x - 1 && frame.bottom - frame.top + 1 >= screen.y - 1);
        current.emplace(root, Window{owner, scale, full});
        g_scales[owner] = scale;
        if (full) {
            const float width = screen.x / scale.value, height = screen.y / scale.value;
            if (std::abs(frame.right - frame.left + 1 - width) > 0.5f ||
                std::abs(frame.bottom - frame.top + 1 - height) > 0.5f) {
                g_resize(owner, width, height);
                g_redraw = true;
            }
        } else if (resized || !known || previous->second.scale.value != scale.value) {
            if (scale.value != 1.0f || (known && previous->second.scale.value != 1.0f)) {
                const float oldScale = known ? previous->second.scale.value : scale.value;
                const float x = std::clamp(frame.left * oldScale / scale.value, 0.0f,
                                          std::max(0.0f, screen.x / scale.value - (frame.right - frame.left + 1)));
                const float y = std::clamp(frame.top * oldScale / scale.value, 0.0f,
                                          std::max(0.0f, screen.y / scale.value - (frame.bottom - frame.top + 1)));
                if (x != frame.left || y != frame.top) g_move(owner, x, y);
            }
            g_redraw = true;
        }
    }
    g_windows.swap(current);
    g_scales.clear();
    for (const auto& [root, window] : g_windows) g_scales.emplace(window.owner, window.scale);
    g_controllerWork = true;
    return g_redraw;
}

void ScaleDrawing(bool drawing) { g_controllerWork = !drawing; }
void EndScaleFrame() { g_controllerWork = false; }

}  // namespace rnative::gui
