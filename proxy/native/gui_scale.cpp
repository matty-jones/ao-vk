// Per-window logical coordinates: layout and input share the scale used to draw and clip each view.
#include "native/gui.h"
#include "native/gui_scale.h"
#include "native/gui_font.h"
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
ViewFn g_top, g_child, g_sibling, g_parent, g_looper, g_windowClient;
ViewFn g_dragObject, g_dragSource;
NameFn g_name;
SizeFn g_screen;
Point g_screenSize{};
FrameFn g_frame, g_bounds, g_borders;
SizeFn g_windowMouse;
using ScreenFn = Point*(__cdecl*)(Point*);
ScreenFn g_windowScreen;

struct Window {
    void* owner;
    Transform transform;
    bool fullScreen;
};
std::unordered_map<void*, Window> g_windows;
std::unordered_map<void*, Transform> g_views;
struct Clip { Rect logical, screen; Point origin; };
std::unordered_map<void*, Clip> g_clips;
float g_windowScale = 1.0f, g_hudScale = 1.0f;
float g_wantedWindow = 1.0f, g_wantedHud = 1.0f;
bool g_installed, g_redraw;
int g_coordinates;
float g_placementScale;
using PlacementFn = void(__fastcall*)(void*, void*, uint32_t);
PlacementFn g_placement;
using VerticesFn = void(__fastcall*)(void*, void*, void*, const Rect*);
VerticesFn g_vertices;
using MouseDownFn = void(__fastcall*)(void*, void*, const Point*, int, int);
using MouseMoveFn = void(__fastcall*)(void*, void*, const Point*, uint32_t, uint32_t, void*);
MouseDownFn g_borderDown;
MouseMoveFn g_borderMove;
using SetFrameFn = void(__fastcall*)(void*, void*, const Rect*);
SetFrameFn g_borderFrame;
constexpr uint32_t kScreenFrameCalls[] = {0x15731B, 0x1578F0};

struct Coordinates {
    bool outer = g_coordinates++ == 0;
    ~Coordinates() { --g_coordinates; }
};

void __fastcall BorderDownHook(void* view, void* edx, const Point* point, int button, int clicks)
{
    Coordinates scope;
    g_borderDown(view, edx, point, button, clicks);
}

void __fastcall BorderMoveHook(void* view, void* edx, const Point* point, uint32_t transition, uint32_t buttons, void* message)
{
    // Dispatch already supplied logical coordinates; native dragging also needs a logical screen and frame.
    Coordinates scope;
    g_borderMove(view, edx, point, transition, buttons, message);
}

void __fastcall BorderFrameHook(void* view, void* edx, const Rect* frame)
{
    Coordinates scope;
    g_borderFrame(view, edx, frame);
}

bool Hud(void* window);

Transform For(void* view)
{
    if (!g_installed || !view || (g_windowScale == 1.0f && g_hudScale == 1.0f)) return {};
    for (void* ancestor = view; ancestor; ancestor = g_parent(ancestor)) {
        auto it = g_views.find(ancestor);
        if (it != g_views.end()) return it->second;
    }
    return {};
}

template <class T> void ParentSpace(void* view, T& coordinates, bool outward);

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

Rect* __fastcall ScreenFrameHook(void* view, void* edx, Rect* out)
{
    g_frame(view, edx, out);
    if (!g_coordinates) {
        auto clip = g_clips.find(view);
        if (clip != g_clips.end()) {
            const Point parent{out->left - clip->second.origin.x, out->top - clip->second.origin.y};
            *out = clip->second.screen;
            out->left += parent.x; out->right += parent.x; out->top += parent.y; out->bottom += parent.y;
        } else ParentSpace(view, *out, true);
    }
    return out;
}

Point* __cdecl WindowScreenHook(Point* out)
{
    g_windowScreen(out);
    if (g_installed && g_placementScale > 0.0f) Scale{g_placementScale}.FromScreen(*out);
    return out;
}

Point* __fastcall WindowMouseHook(void* window, void*, Point* out)
{
    g_windowMouse(window, out);
    void* client = g_windowClient(window);
    const Transform scale = For(client);
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
    if (g_installed) {
        void* root = g_parent(g_windowClient(window));
        Rect frame;
        Point screen;
        g_frame(root, nullptr, &frame); g_windowScreen(&screen);
        const Transform map = Place(frame, screen, g_placementScale, false);
        g_windows.insert_or_assign(root, Window{window, map, false});
        g_views.insert_or_assign(root, map);
        g_redraw = true;
    }
    g_placementScale = saved;
}

Transform DrawingScale(void* view)
{
    Transform scale = For(view);
    if (g_placementScale > 0.0f) {
        scale.value /= g_placementScale;
        scale.offset.x /= g_placementScale; scale.offset.y /= g_placementScale;
    }
    return scale;
}

void __fastcall VerticesHook(void* area, void* edx, void* view, const Rect* clip)
{
    const Transform scale = For(view);
    if (scale.Identity()) {
        g_vertices(area, edx, view, clip);
        return;
    }
    Rect logical = *clip;
    scale.FromScreen(logical);
    {
        // Native clipping derives atlas coordinates from logical distances.
        Coordinates scope;
        g_vertices(area, edx, view, &logical);
    }
    if (!*(static_cast<const uint8_t*>(area) + 0x7C)) return;
    const auto* bytes = static_cast<const uint8_t*>(area);
    const auto* node = *reinterpret_cast<const uint8_t* const*>(bytes + 0xA0);
    Rect centres{};
    if (node) {
        const auto* material = **reinterpret_cast<const uint8_t* const* const*>(node + 0xC);
        const Point extent{float(*reinterpret_cast<const uint32_t*>(material + 8)),
                           float(*reinterpret_cast<const uint32_t*>(material + 12))};
        centres = AtlasCentres(*reinterpret_cast<const Rect*>(bytes + 0x80),
                               *reinterpret_cast<const Point*>(node + 0x10), extent);
    }
    for (int i = 0; i < 4; ++i) {
        auto* position = reinterpret_cast<float*>(static_cast<uint8_t*>(area) + i * 28);
        Point point{position[0] + 0.5f, position[1] + 0.5f};
        scale.ToScreen(point);
        position[0] = point.x - 0.5f; position[1] = point.y - 0.5f;
        // Packed images have no gutter; magnification must not sample a neighbouring allocation.
        if (node) GuardAtlas(*reinterpret_cast<Point*>(position + 5), centres);
    }
}

void ClipBounds(void*, const Point&, Point&) {}
void ClipBounds(void* view, const Rect& input, Rect& output)
{
    auto it = g_clips.find(view);
    if (it != g_clips.end() && std::memcmp(&input, &it->second.logical, sizeof(input)) == 0)
        output = it->second.screen;
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
        const Type original = *in; \
        g_to##Tag##Value(view, edx, out, in, scaled); \
        if (scope.outer && scaled) { DrawingScale(view).ToScreen(*out); ClipBounds(view, original, *out); } \
        return out; \
    } \
    void __fastcall To##Tag##Pointer(void* view, void* edx, Type* in, uint32_t scaled) \
    { \
        Coordinates scope; \
        const Type original = *in; \
        g_to##Tag##Pointer(view, edx, in, scaled); \
        if (scope.outer && scaled) { DrawingScale(view).ToScreen(*in); ClipBounds(view, original, *in); } \
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

template <class T> void Translate(T& coordinates, Point delta);
template <> void Translate(Point& p, Point delta) { p.x += delta.x; p.y += delta.y; }
template <> void Translate(Rect& r, Point delta)
{
    r.left += delta.x; r.right += delta.x; r.top += delta.y; r.bottom += delta.y;
}

template <class T> void ParentSpace(void* view, T& coordinates, bool outward)
{
    void* parent = g_parent(view);
    const Transform childMap = For(view), parentMap = For(parent);
    if (childMap == parentMap) return;
    Point origin{}, zero{};
    if (parent) {
        Coordinates scope;
        g_toPointValue(parent, nullptr, &origin, &zero, 1);
    }
    Translate(coordinates, origin);
    if (outward) { childMap.ToScreen(coordinates); parentMap.FromScreen(coordinates); }
    else { parentMap.ToScreen(coordinates); childMap.FromScreen(coordinates); }
    Translate(coordinates, Point{-origin.x, -origin.y});
}

#define PARENT_HOOKS(Type, Tag) \
    using Tag##ParentValueFn = Type*(__fastcall*)(void*, void*, Type*, const Type*); \
    using Tag##ParentPointerFn = void(__fastcall*)(void*, void*, Type*); \
    Tag##ParentValueFn g_parent##Tag##Value; \
    Tag##ParentPointerFn g_parent##Tag##Pointer; \
    Type* __fastcall Parent##Tag##Value(void* view, void* edx, Type* out, const Type* in) \
    { \
        Coordinates scope; \
        Type logical = *in; \
        if (scope.outer) ParentSpace(view, logical, false); \
        return g_parent##Tag##Value(view, edx, out, &logical); \
    } \
    void __fastcall Parent##Tag##Pointer(void* view, void* edx, Type* in) \
    { \
        Coordinates scope; \
        if (scope.outer) ParentSpace(view, *in, false); \
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
    {"?DoSetFrame@WndBorder@@AAEXABVRect@@@Z", 0x159888, "", 10,
      reinterpret_cast<void*>(&BorderFrameHook), reinterpret_cast<void**>(&g_borderFrame)},
    {"?MouseDown@WndBorder@@UAEXABVPoint@@HH@Z", 0x1595F7, "\x55\x8B\xEC\x83\xEC\x20", 6,
      reinterpret_cast<void*>(&BorderDownHook), reinterpret_cast<void**>(&g_borderDown)},
    {"?MouseMove@WndBorder@@UAEXABVPoint@@W4MouseTransition_e@@IPAVMessage@@@Z", 0x159C27, "", 10,
      reinterpret_cast<void*>(&BorderMoveHook), reinterpret_cast<void**>(&g_borderMove)},
    {"?_UpdateVertices@ViewArea_c@@AAEXPAVView@@ABVRect@@@Z", 0x151898, "", 10,
      reinterpret_cast<void*>(&VerticesHook), reinterpret_cast<void**>(&g_vertices)},
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
    g_parent = reinterpret_cast<ViewFn>(address("?GetParent@View@@QBEPAV1@XZ"));
    g_child = reinterpret_cast<ViewFn>(address("?GetTopChild@View@@QBEPAV1@XZ"));
    g_sibling = reinterpret_cast<ViewFn>(address("?GetLowerSibling@View@@QBEPAV1@XZ"));
    g_windowClient = reinterpret_cast<ViewFn>(address("?_GetClientView@Window@@ABEPAVView@@XZ"));
    g_dragObject = reinterpret_cast<ViewFn>(address("?GetDragObject@WindowController_c@@QBEPAVDragObject_c@@XZ"));
    g_dragSource = reinterpret_cast<ViewFn>(address("?GetSource@DragObject_c@@QBEPAVView@@XZ"));
    g_frame = reinterpret_cast<FrameFn>(address("?GetFrame@View@@QBE?AVRect@@XZ"));
    g_bounds = reinterpret_cast<FrameFn>(address("?GetBounds@View@@QBE?AVRect@@XZ"));
    g_borders = reinterpret_cast<FrameFn>(address("?GetBorderSizes@TabView@@QBE?AVRect@@XZ"));
    g_screen = reinterpret_cast<SizeFn>(address("?GetScreenSize@WindowController_c@@QBE?AVPoint@@XZ"));
    HMODULE utils = GetModuleHandleA("Utils.dll");
    g_looper = utils ? reinterpret_cast<ViewFn>(GetProcAddress(utils, "?GetLooper@Handler@@QBEPAVLooper@@XZ")) : nullptr;
    g_name = utils ? reinterpret_cast<NameFn>(GetProcAddress(utils, "?GetName@Looper@@QBE?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ")) : nullptr;
    if (!g_top || !g_parent || !g_child || !g_sibling || !g_windowClient || !g_dragObject || !g_dragSource || !g_frame ||
        !g_bounds || !g_borders || !g_screen || !g_looper || !g_name) {
        Log("interface scaling: required GUI exports unavailable, left at 100%%");
        return;
    }
    auto* base = reinterpret_cast<uint8_t*>(gui);
    for (uint32_t rva : kScreenFrameCalls) {
        int32_t relative;
        std::memcpy(&relative, base + rva + 1, 4);
        if (base[rva] != 0xE8 || base + rva + 5 + relative != reinterpret_cast<uint8_t*>(g_frame)) {
            Log("interface scaling: screen hit-test query changed, left at 100%%");
            return;
        }
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
    // Only cursor hit tests need displayed frames. Native layout queries always keep their original units.
    for (uint32_t rva : kScreenFrameCalls) {
        DWORD protect;
        if (!VirtualProtect(base + rva, 5, PAGE_EXECUTE_READWRITE, &protect)) return;
        const int32_t relative = int32_t(reinterpret_cast<uintptr_t>(&ScreenFrameHook) - reinterpret_cast<uintptr_t>(base + rva + 5));
        std::memcpy(base + rva + 1, &relative, 4);
        VirtualProtect(base + rva, 5, protect, &protect);
        FlushInstructionCache(GetCurrentProcess(), base + rva, 5);
    }
    g_installed = true;
    Log("interface scaling: window and HUD coordinates hooked");
    InstallFontScaling(gui);
    if (!FontScalingReady()) Log("interface fonts: text hooks unavailable, original cache retained");
}

bool BeginScaleFrame(void* controller)
{
    if (!g_installed) return false;
    // Keep the preferences slider under the mouse until its drag ends.
    const uint32_t buttons = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(controller) + 0x58);
    if (!(buttons & 1) && g_windowScale != g_wantedWindow) {
        g_windowScale = g_wantedWindow;
        g_redraw = true;
    }
    if (g_hudScale != g_wantedHud) g_redraw = true;
    g_hudScale = g_wantedHud;
    Point screen;
    g_screen(controller, &screen);
    const bool resized = screen.x != g_screenSize.x || screen.y != g_screenSize.y;
    g_screenSize = screen;
    if (resized) g_redraw = true;
    std::unordered_map<void*, Window> current;
    std::unordered_map<void*, Transform> views;
    std::unordered_map<void*, Clip> clips;
    void* drag = g_dragObject(controller);
    void* dragWindow = drag ? *reinterpret_cast<void**>(static_cast<uint8_t*>(drag) + 0x54) : nullptr;
    void* dragSource = drag ? g_dragSource(drag) : nullptr;
    void* sourceOwner = dragSource ? g_looper(dragSource) : nullptr;
    auto assign = [&](auto&& self, void* view, void* owner, Transform inherited, bool split) -> Rect {
        Rect frame;
        g_frame(view, nullptr, &frame);
        const float width = frame.right - frame.left + 1, height = frame.bottom - frame.top + 1;
        const bool scaffold = split && (width >= screen.x * 0.9f || height >= screen.y * 0.9f);
        Rect bounds, global;
        g_bounds(view, nullptr, &bounds);
        {
            Coordinates scope;
            g_toRectValue(view, nullptr, &global, &bounds, 1);
        }
        Transform map = inherited;
        if (split && !scaffold) {
            auto previous = g_windows.find(view);
            if (!resized && previous != g_windows.end() && previous->second.owner == owner &&
                previous->second.transform.value == g_hudScale) map = previous->second.transform;
            else {
                map = Place(global, screen, g_hudScale, true);
            }
            current.emplace(view, Window{owner, map, false});
        }
        views.emplace(view, map);
        Rect displayed = global;
        map.ToScreen(displayed);
        if (!scaffold) return displayed;
        for (void* child = g_child(view); child; child = g_sibling(child)) {
            const Rect content = self(self, child, owner, map, scaffold);
            if (scaffold && g_hudScale != 1.0f) {
                displayed.left = std::min(displayed.left, content.left);
                displayed.top = std::min(displayed.top, content.top);
                displayed.right = std::max(displayed.right, content.right);
                displayed.bottom = std::max(displayed.bottom, content.bottom);
            }
        }
        if (scaffold && g_hudScale != 1.0f)
            clips.emplace(view, Clip{bounds, displayed, {global.left, global.top}});
        return displayed;
    };
    for (void* root = g_child(g_top(controller)); root; root = g_sibling(root)) {
        void* owner = g_looper(root);
        if (!owner) continue;
        const bool hud = Hud(owner);
        const float factor = owner == dragWindow && sourceOwner ? (Hud(sourceOwner) ? g_hudScale : g_windowScale) :
                             hud ? g_hudScale : g_windowScale;
        Rect frame;
        g_frame(root, nullptr, &frame);
        auto previous = g_windows.find(root);
        const bool known = previous != g_windows.end() && previous->second.owner == owner;
        const bool full = !std::strcmp(TypeOf(owner), ".?AVControlCenterWindow_c@@") ||
                          (hud && frame.right - frame.left + 1 >= screen.x - 1 && frame.bottom - frame.top + 1 >= screen.y - 1);
        Transform map;
        if (owner == dragWindow) {
            const Point cursor = *reinterpret_cast<const Point*>(static_cast<const uint8_t*>(controller) + 0x48);
            map = {factor, {(1 - factor) * cursor.x, (1 - factor) * cursor.y}};
        } else if (!full) {
            if (!resized && known && previous->second.transform.value == factor) map = previous->second.transform;
            else map = Place(frame, screen, factor, false);
        }
        if (!known || !(previous->second.transform == map)) g_redraw = true;
        current.emplace(root, Window{owner, map, full});
        assign(assign, root, owner, map, full);
    }
    g_windows.swap(current);
    g_views.swap(views);
    g_clips.swap(clips);
    return g_redraw;
}

float ScaleForView(void* view) { return For(view).value; }

}  // namespace rnative::gui
