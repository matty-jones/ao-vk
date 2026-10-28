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
#include <vector>

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
using WindowFrameFn = Rect*(__fastcall*)(void*, void*, Rect*, uint32_t);
WindowFrameFn g_windowFrame;
SizeFn g_windowMouse;
using ScreenFn = Point*(__cdecl*)(Point*);
ScreenFn g_windowScreen;

struct Window {
    void* owner;
    Transform transform;
};
std::unordered_map<void*, Window> g_windows;
std::unordered_map<void*, Transform> g_views;
struct Clip { Rect logical, screen; Point origin; };
std::unordered_map<void*, Clip> g_clips;
float g_windowScale = 1.0f, g_hudScale = 1.0f;
float g_wantedWindow = 1.0f, g_wantedHud = 1.0f;
bool g_installed, g_redraw;
bool g_active;
int g_coordinates;
const Transform* g_mapDrawing;
using MapRenderFn = void(__fastcall*)(void*, void*, void*, float, uint32_t);
MapRenderFn g_mapRender, g_mapGroundRender;
using PrimitiveFn = void(__fastcall*)(void*, void*, uint32_t, void*, uint32_t, uint32_t);
PrimitiveFn g_fan, g_lines, g_strip;
float g_placementScale, g_popupPlacementScale;
using PlacementFn = void(__fastcall*)(void*, void*, uint32_t);
PlacementFn g_placement;
using PopupGoFn = void(__fastcall*)(void*, void*, void*, const Point*, const vc10::String*);
using PopupMoveFn = void(__fastcall*)(void*, void*, const Point*);
PopupGoFn g_popupGo;
PopupMoveFn g_popupMove;
using VerticesFn = void(__fastcall*)(void*, void*, void*, const Rect*);
VerticesFn g_vertices;
using MouseDownFn = void(__fastcall*)(void*, void*, const Point*, int, int);
using MouseMoveFn = void(__fastcall*)(void*, void*, const Point*, uint32_t, uint32_t, void*);
MouseDownFn g_borderDown;
MouseMoveFn g_borderMove;
using MouseUpFn = void(__fastcall*)(void*, void*, const Point*, int, void*);
MouseUpFn g_borderUp;
void* g_draggedBorder;
Rect g_draggedFrame;
struct IPoint { int x, y; };
struct TimerWindow { IPoint position; Point scale; };
std::unordered_map<void*, TimerWindow> g_timers;
using TimerCtorFn = void*(__fastcall*)(void*, void*, int, int, const void*, const char*, uint32_t);
using TimerMoveFn = void(__fastcall*)(void*, void*, const IPoint*);
using SpriteScaleFn = void(__fastcall*)(void*, void*, const float*, const float*);
using SpriteDestroyFn = void(__fastcall*)(void*, void*);
TimerCtorFn g_timerCtor;
TimerMoveFn g_timerMove, g_spriteMove;
SpriteScaleFn g_spriteScale;
SpriteDestroyFn g_spriteDestroy;
using SetFrameFn = void(__fastcall*)(void*, void*, const Rect*);
SetFrameFn g_borderFrame;

struct Coordinates {
    bool outer = g_coordinates++ == 0;
    ~Coordinates() { --g_coordinates; }
};

void __fastcall BorderDownHook(void* view, void* edx, const Point* point, int button, int clicks)
{
    if (!g_active) { g_borderDown(view, edx, point, button, clicks); return; }
    Coordinates scope;
    g_frame(view, nullptr, &g_draggedFrame);
    g_borderDown(view, edx, point, button, clicks);
    g_draggedBorder = view;
}

void __fastcall BorderMoveHook(void* view, void* edx, const Point* point, uint32_t transition, uint32_t buttons, void* message)
{
    if (!g_active) { g_borderMove(view, edx, point, transition, buttons, message); return; }
    // Dispatch already supplied logical coordinates; native dragging also needs a logical screen and frame.
    Coordinates scope;
    g_borderMove(view, edx, point, transition, buttons, message);
}

void __fastcall BorderUpHook(void* view, void* edx, const Point* point, int button, void* message)
{
    g_borderUp(view, edx, point, button, message);
    if (g_draggedBorder != view) return;
    g_draggedBorder = nullptr;
    auto window = g_windows.find(view);
    if (!g_active || window == g_windows.end()) return;
    Transform map = window->second.transform;
    if (map.offset == Point{}) return;
    Rect frame; g_frame(view, nullptr, &frame);
    if (std::fabs(frame.left - g_draggedFrame.left) < 0.001f && std::fabs(frame.top - g_draggedFrame.top) < 0.001f &&
        std::fabs(frame.right - g_draggedFrame.right) < 0.001f && std::fabs(frame.bottom - g_draggedFrame.bottom) < 0.001f) return;
    // Save user movement independently of the initial display-only edge clamp.
    const Point origin{frame.left + map.offset.x / map.value, frame.top + map.offset.y / map.value};
    using Move = void(__thiscall*)(void*, const Point*);
    const auto move = reinterpret_cast<Move>((*static_cast<void***>(view))[0xC8 / 4]);
    map.offset = {};
    window->second.transform = map;
    g_views.insert_or_assign(view, map);
    {
        Coordinates scope;
        move(view, &origin);
    }
    g_redraw = true;
}

void __fastcall BorderFrameHook(void* view, void* edx, const Rect* frame)
{
    if (!g_active) { g_borderFrame(view, edx, frame); return; }
    Coordinates scope;
    g_borderFrame(view, edx, frame);
}

void ApplyTimer(void* window, const TimerWindow& logical)
{
    const Point scale{logical.scale.x * g_hudScale, logical.scale.y * g_hudScale};
    const IPoint position{int(std::lround(logical.position.x * g_hudScale)), int(std::lround(logical.position.y * g_hudScale))};
    g_spriteScale(window, nullptr, &scale.x, &scale.y);
    g_spriteMove(window, nullptr, &position);
}

void* __fastcall TimerCtorHook(void* timer, void* edx, int kind, int slot, const void* identity, const char* label, uint32_t colour)
{
    void* result = g_timerCtor(timer, edx, kind, slot, identity, label, colour);
    void* window = *reinterpret_cast<void**>(static_cast<uint8_t*>(timer) + 4);
    if (window) {
        TimerWindow logical;
        std::memcpy(&logical.position, static_cast<uint8_t*>(window) + 0x2C, sizeof(logical.position));
        std::memcpy(&logical.scale, static_cast<uint8_t*>(window) + 0x3C, sizeof(logical.scale));
        g_timers.insert_or_assign(window, logical);
        ApplyTimer(window, logical);
    }
    return result;
}

void __fastcall TimerMoveHook(void* timer, void* edx, const IPoint* point)
{
    void* window = *reinterpret_cast<void**>(static_cast<uint8_t*>(timer) + 4);
    auto found = g_timers.find(window);
    if (found == g_timers.end()) { g_timerMove(timer, edx, point); return; }
    found->second.position = *point;
    ApplyTimer(window, found->second);
}

void __fastcall SpriteScaleHook(void* window, void* edx, const float* x, const float* y)
{
    auto found = g_timers.find(window);
    if (found == g_timers.end()) { g_spriteScale(window, edx, x, y); return; }
    found->second.scale = {*x, *y};
    ApplyTimer(window, found->second);
}

void __fastcall SpriteDestroyHook(void* window, void* edx)
{
    g_timers.erase(window);
    g_spriteDestroy(window, edx);
}

bool Hud(void* window);
bool NativeScreen(void* window);

Transform For(void* view)
{
    if (!g_active || !view) return {};
    for (void* ancestor = view; ancestor; ancestor = g_parent(ancestor)) {
        auto it = g_views.find(ancestor);
        if (it != g_views.end()) return it->second;
    }
    return {};
}

void DrawMap(MapRenderFn original, void* view, void* edx, void* viewport, float time, uint32_t flags)
{
    const Transform scale = For(view);
    if (scale.Identity() && !g_mapDrawing) { original(view, edx, viewport, time, flags); return; }
    // Map zoom, tile clipping and marker placement share native units until submission.
    struct Restore {
        const Transform* saved = g_mapDrawing;
        ~Restore() { g_mapDrawing = saved; }
    } restore;
    g_mapDrawing = scale.Identity() ? nullptr : &scale;
    original(view, edx, viewport, time, flags);
}

void __fastcall MapRenderHook(void* view, void* edx, void* viewport, float time, uint32_t flags)
{
    DrawMap(g_mapRender, view, edx, viewport, time, flags);
}

void __fastcall MapGroundRenderHook(void* view, void* edx, void* viewport, float time, uint32_t flags)
{
    DrawMap(g_mapGroundRender, view, edx, viewport, time, flags);
}

void MapPrimitive(PrimitiveFn original, void* renderer, void* edx, uint32_t fvf, void* vertices,
                  uint32_t count, uint32_t flags)
{
    if (!g_mapDrawing || (fvf != 0x44 && fvf != 0x144)) {
        original(renderer, edx, fvf, vertices, count, flags);
        return;
    }
    const size_t stride = fvf == 0x144 ? 7 : 5;
    uint32_t local[64 * 7];
    std::vector<uint32_t> large;
    if (count > 64) large.resize(size_t(count) * stride);
    uint32_t* copy = count > 64 ? large.data() : local;
    std::memcpy(copy, vertices, size_t(count) * stride * 4);
    for (uint32_t i = 0; i < count; ++i) {
        Point point;
        std::memcpy(&point, copy + size_t(i) * stride, sizeof(point));
        g_mapDrawing->ToScreen(point);
        std::memcpy(copy + size_t(i) * stride, &point, sizeof(point));
    }
    original(renderer, edx, fvf, copy, count, flags);
}

#define MAP_PRIMITIVE(Name, Original) \
    void __fastcall Name(void* renderer, void* edx, uint32_t fvf, void* vertices, uint32_t count, uint32_t flags) \
    { MapPrimitive(Original, renderer, edx, fvf, vertices, count, flags); }
MAP_PRIMITIVE(MapFanHook, g_fan)
MAP_PRIMITIVE(MapLinesHook, g_lines)
MAP_PRIMITIVE(MapStripHook, g_strip)
#undef MAP_PRIMITIVE

template <class T> void ParentSpace(void* view, T& coordinates, bool outward);

const char* TypeOf(void* window)
{
    const auto* table = *static_cast<const uintptr_t* const*>(window);
    const auto* locator = table ? reinterpret_cast<const uint32_t*>(table[-1]) : nullptr;
    return locator && locator[3] ? reinterpret_cast<const char*>(locator[3] + 8) : "";
}

bool NativeScreen(void* window)
{
    const char* type = TypeOf(window);
    return !std::strcmp(type, ".?AVLoginWindow_c@@") || !std::strcmp(type, ".?AVLoginBrowserWindow_c@@") ||
           !std::strcmp(type, ".?AVCharSelectWindow_c@@") || !std::strcmp(type, ".?AVLimboWindow_c@@") ||
           !std::strcmp(type, ".?AVCharDeleteWindow_c@@") || !std::strcmp(type, ".?AVCharActivateWindow_c@@") ||
           !std::strcmp(type, ".?AVProgressWindow_c@@");
}

bool Hud(void* window)
{
    const char* type = TypeOf(window);
    if (!std::strcmp(type, ".?AVCharBarWindow_c@@") || !std::strcmp(type, ".?AVCompassWindow_c@@") ||
        !std::strcmp(type, ".?AVShortcutBarContainer@@") || !std::strcmp(type, ".?AVShortcutBarWindow_c@@") ||
        !std::strcmp(type, ".?AVControlCenterWindow_c@@") || !std::strcmp(type, ".?AVNotificationWindow_c@@") ||
        !std::strcmp(type, ".?AVMailIconWindow_c@@") || !std::strcmp(type, ".?AVHelpButtonWindow_c@@") ||
        !std::strcmp(type, ".?AVTipButtonWindow_c@@") || !std::strcmp(type, ".?AVRaidBarWindow_c@@"))
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
    if (g_active && !g_coordinates) {
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
    const float factor = g_placementScale > 0.0f ? g_placementScale : g_popupPlacementScale;
    if (g_installed && factor > 0.0f) Scale{factor}.FromScreen(*out);
    return out;
}

Point* __fastcall WindowMouseHook(void* window, void*, Point* out)
{
    g_windowMouse(window, out);
    if (!g_active) return out;
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

void PlacedWindow(void* window, float factor)
{
    void* root = g_parent(g_windowClient(window));
    Rect frame;
    Point screen;
    g_frame(root, nullptr, &frame); g_windowScreen(&screen);
    const Transform map = Place(frame, screen, factor, false);
    g_windows.insert_or_assign(root, Window{window, map});
    g_views.insert_or_assign(root, map);
    g_redraw = true;
}

void __fastcall PlacementHook(void* window, void* edx, uint32_t centered)
{
    if (!g_active || NativeScreen(window)) { g_placement(window, edx, centered); return; }
    const float saved = g_placementScale;
    g_placementScale = g_installed ? (Hud(window) ? g_hudScale : g_windowScale) : 1.0f;
    g_placement(window, edx, centered);
    if (g_installed) PlacedWindow(window, g_placementScale);
    g_placementScale = saved;
}

void __fastcall PopupGoHook(void* menu, void* edx, void* parent, const Point* point, const vc10::String* title)
{
    if (!g_active) { g_popupGo(menu, edx, parent, point, title); return; }
    const float saved = g_popupPlacementScale;
    g_popupPlacementScale = g_windowScale;
    Point logical = *point;
    Scale{g_windowScale}.FromScreen(logical);
    g_popupGo(menu, edx, parent, &logical, title);
    void* window = *reinterpret_cast<void**>(static_cast<uint8_t*>(menu) + 0x158);
    if (window) PlacedWindow(window, g_windowScale);
    g_popupPlacementScale = saved;
}

void __fastcall PopupMoveHook(void* menu, void* edx, const Point* point)
{
    if (!g_active) { g_popupMove(menu, edx, point); return; }
    const float saved = g_popupPlacementScale;
    g_popupPlacementScale = g_windowScale;
    Point logical = *point;
    Scale{g_windowScale}.FromScreen(logical);
    g_popupMove(menu, edx, &logical);
    void* window = *reinterpret_cast<void**>(static_cast<uint8_t*>(menu) + 0x158);
    if (window) PlacedWindow(window, g_windowScale);
    g_popupPlacementScale = saved;
}

Rect* __fastcall PopupFrameHook(void* window, void* edx, Rect* out, uint32_t client)
{
    g_windowFrame(window, edx, out, client);
    if (g_active && !g_coordinates) For(g_windowClient(window)).ToScreen(*out);
    return out;
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
    if (g_mapDrawing) { Coordinates scope; g_vertices(area, edx, view, clip); return; }
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
    for (int i = 0; i < 4; ++i) {
        auto* position = reinterpret_cast<float*>(static_cast<uint8_t*>(area) + i * 28);
        Point point{position[0] + 0.5f, position[1] + 0.5f};
        scale.ToScreen(point);
        position[0] = point.x - 0.5f; position[1] = point.y - 0.5f;
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
        if (!g_active || g_coordinates || g_mapDrawing) return g_to##Tag##Value(view, edx, out, in, scaled); \
        Coordinates scope; \
        const Type original = *in; \
        g_to##Tag##Value(view, edx, out, in, scaled); \
        if (scope.outer && scaled) { DrawingScale(view).ToScreen(*out); ClipBounds(view, original, *out); } \
        return out; \
    } \
    void __fastcall To##Tag##Pointer(void* view, void* edx, Type* in, uint32_t scaled) \
    { \
        if (!g_active || g_coordinates || g_mapDrawing) { g_to##Tag##Pointer(view, edx, in, scaled); return; } \
        Coordinates scope; \
        const Type original = *in; \
        g_to##Tag##Pointer(view, edx, in, scaled); \
        if (scope.outer && scaled) { DrawingScale(view).ToScreen(*in); ClipBounds(view, original, *in); } \
    } \
    Type* __fastcall From##Tag##Value(void* view, void* edx, Type* out, const Type* in, uint32_t scaled) \
    { \
        if (!g_active || g_coordinates) return g_from##Tag##Value(view, edx, out, in, scaled); \
        Coordinates scope; \
        Type logical = *in; \
        if (scope.outer && scaled) For(view).FromScreen(logical); \
        return g_from##Tag##Value(view, edx, out, &logical, scaled); \
    } \
    void __fastcall From##Tag##Pointer(void* view, void* edx, Type* in, uint32_t scaled) \
    { \
        if (!g_active || g_coordinates) { g_from##Tag##Pointer(view, edx, in, scaled); return; } \
        Coordinates scope; \
        if (scope.outer && scaled) For(view).FromScreen(*in); \
        g_from##Tag##Pointer(view, edx, in, scaled); \
    }
SCREEN_HOOKS(Point, Point)
SCREEN_HOOKS(Rect, Rect)
#undef SCREEN_HOOKS

Rect* __fastcall InputBoundsHook(void* view, void* edx, Rect* out)
{
    g_bounds(view, edx, out);
    if (g_active && !g_coordinates) {
        auto clip = g_clips.find(view);
        if (clip != g_clips.end()) {
            Rect logical = clip->second.screen;
            For(view).FromScreen(logical);
            Coordinates scope;
            g_fromRectValue(view, edx, out, &logical, 1);
        }
    }
    return out;
}

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
        if (!g_active || g_coordinates) return g_parent##Tag##Value(view, edx, out, in); \
        Coordinates scope; \
        Type logical = *in; \
        if (scope.outer) ParentSpace(view, logical, false); \
        return g_parent##Tag##Value(view, edx, out, &logical); \
    } \
    void __fastcall Parent##Tag##Pointer(void* view, void* edx, Type* in) \
    { \
        if (!g_active || g_coordinates) { g_parent##Tag##Pointer(view, edx, in); return; } \
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
    {"TimerBarBase_c::TimerBarBase_c", 0x512AE, "", 10,
      reinterpret_cast<void*>(&TimerCtorHook), reinterpret_cast<void**>(&g_timerCtor)},
    {"TimerBarBase_c::Reposition", 0x5128A, "\x55\x8B\xEC\x8B\x49\x04", 6,
      reinterpret_cast<void*>(&TimerMoveHook), reinterpret_cast<void**>(&g_timerMove)},
    {"?SetScale@RenderWindow_t@@QAEXABM0@Z", 0x20B0C, "\x55\x8B\xEC\x51\x8B\x45\x08", 7,
      reinterpret_cast<void*>(&SpriteScaleHook), reinterpret_cast<void**>(&g_spriteScale)},
    {"??1RenderWindow_t@@QAE@XZ", 0x21241, "\x56\x8B\xF1\x80\x7E\x4C\x00", 7,
      reinterpret_cast<void*>(&SpriteDestroyHook), reinterpret_cast<void**>(&g_spriteDestroy)},
    {"?Go@PopupMenu_c@@QAEXPAVWindow@@ABVPoint@@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", 0x13BF93, "", 10,
      reinterpret_cast<void*>(&PopupGoHook), reinterpret_cast<void**>(&g_popupGo)},
    {"?Reposition@PopupMenu_c@@QAEXABVPoint@@@Z", 0x13B61E, "\x55\x8B\xEC\x56\x8B\xF1", 6,
      reinterpret_cast<void*>(&PopupMoveHook), reinterpret_cast<void**>(&g_popupMove)},
    {"PFMapRenderer_c::Render", 0xE9F73, "", 10,
      reinterpret_cast<void*>(&MapRenderHook), reinterpret_cast<void**>(&g_mapRender)},
    {"PFMapGroundRenderer_c::Render", 0x478ED, "", 10,
      reinterpret_cast<void*>(&MapGroundRenderHook), reinterpret_cast<void**>(&g_mapGroundRender)},
    {"?DoSetFrame@WndBorder@@AAEXABVRect@@@Z", 0x159888, "", 10,
      reinterpret_cast<void*>(&BorderFrameHook), reinterpret_cast<void**>(&g_borderFrame)},
    {"?MouseDown@WndBorder@@UAEXABVPoint@@HH@Z", 0x1595F7, "\x55\x8B\xEC\x83\xEC\x20", 6,
      reinterpret_cast<void*>(&BorderDownHook), reinterpret_cast<void**>(&g_borderDown)},
    {"?MouseUp@WndBorder@@UAEXABVPoint@@HPAVMessage@@@Z", 0x1596A7, "\x8B\x01\x83\xA1\xD4\x01\x00\x00\x00", 9,
      reinterpret_cast<void*>(&BorderUpHook), reinterpret_cast<void**>(&g_borderUp)},
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

struct Import { const char* name; uint32_t rva; void* target; void** original; };
Import g_imports[] = {
    {"?RenderTriangleFan@render_t@@QAEXKPAXKK@Z", 0x1A8618,
      reinterpret_cast<void*>(&MapFanHook), reinterpret_cast<void**>(&g_fan)},
    {"?RenderLineList@render_t@@QAEXKPAXKK@Z", 0x1A861C,
      reinterpret_cast<void*>(&MapLinesHook), reinterpret_cast<void**>(&g_lines)},
    {"?RenderTriangleStrip@render_t@@QAEXKPAXKK@Z", 0x1A8680,
      reinterpret_cast<void*>(&MapStripHook), reinterpret_cast<void**>(&g_strip)},
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
    if (GetMode("UiScaling", Mode::On) == Mode::Off) {
        Log("interface scaling: disabled ([Native] UiScaling=off); coordinate and font hooks not installed");
        return;
    }
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
    g_windowFrame = reinterpret_cast<WindowFrameFn>(address("?GetFrame@Window@@QBE?AVRect@@_N@Z"));
    g_screen = reinterpret_cast<SizeFn>(address("?GetScreenSize@WindowController_c@@QBE?AVPoint@@XZ"));
    g_spriteMove = reinterpret_cast<TimerMoveFn>(address("?Reposition@RenderWindow_t@@QAEXABVIPoint@@@Z"));
    HMODULE utils = GetModuleHandleA("Utils.dll");
    g_looper = utils ? reinterpret_cast<ViewFn>(GetProcAddress(utils, "?GetLooper@Handler@@QBEPAVLooper@@XZ")) : nullptr;
    g_name = utils ? reinterpret_cast<NameFn>(GetProcAddress(utils, "?GetName@Looper@@QBE?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ")) : nullptr;
    if (!g_top || !g_parent || !g_child || !g_sibling || !g_windowClient || !g_dragObject || !g_dragSource || !g_frame ||
        !g_bounds || !g_borders || !g_windowFrame || !g_screen || !g_looper || !g_name || !g_spriteMove) {
        Log("interface scaling: required GUI exports unavailable, left at 100%%");
        return;
    }
    auto* base = reinterpret_cast<uint8_t*>(gui);
    if (*reinterpret_cast<const void* const*>(base + 0x1C0724 + 13 * 4) != base + 0xE9F73 ||
        *reinterpret_cast<const void* const*>(base + 0x1B0A8C + 13 * 4) != base + 0x478ED) {
        Log("interface scaling: map render entries changed, left at 100%%");
        return;
    }
    struct Query { uint32_t rva; void* original; void* replacement; };
    const Query queries[] = {
        {0x15731B, reinterpret_cast<void*>(g_frame), reinterpret_cast<void*>(&ScreenFrameHook)},
        {0x1578F0, reinterpret_cast<void*>(g_frame), reinterpret_cast<void*>(&ScreenFrameHook)},
        {0x154798, reinterpret_cast<void*>(g_bounds), reinterpret_cast<void*>(&InputBoundsHook)},
        {0x13C990, reinterpret_cast<void*>(g_windowFrame), reinterpret_cast<void*>(&PopupFrameHook)},
    };
    for (const Query& query : queries) {
        int32_t relative;
        std::memcpy(&relative, base + query.rva + 1, 4);
        if (base[query.rva] != 0xE8 || base + query.rva + 5 + relative != query.original) {
            Log("interface scaling: screen hit-test query changed, left at 100%%");
            return;
        }
    }
    // Check the entire group before patching; a partial group never enables scaling.
    for (const Hook& h : g_hooks) {
        const bool internal = h.name[0] != '?';
        auto* at = internal ? base + h.rva : reinterpret_cast<uint8_t*>(address(h.name));
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
    HMODULE renderer = GetModuleHandleA("randy31.dll");
    for (const Import& entry : g_imports) {
        void* original = renderer ? reinterpret_cast<void*>(GetProcAddress(renderer, entry.name)) : nullptr;
        if (!original || *reinterpret_cast<void**>(base + entry.rva) != original) {
            Log("interface scaling: map drawing import changed, left at 100%%");
            return;
        }
        *entry.original = original;
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
    for (const Import& entry : g_imports) {
        DWORD protect;
        if (!VirtualProtect(base + entry.rva, sizeof(void*), PAGE_READWRITE, &protect)) return;
        *reinterpret_cast<void**>(base + entry.rva) = entry.target;
        VirtualProtect(base + entry.rva, sizeof(void*), protect, &protect);
    }
    // Input queries include scaled descendants; native layout queries retain their original units.
    for (const Query& query : queries) {
        DWORD protect;
        if (!VirtualProtect(base + query.rva, 5, PAGE_EXECUTE_READWRITE, &protect)) return;
        const int32_t relative = int32_t(reinterpret_cast<uintptr_t>(query.replacement) - reinterpret_cast<uintptr_t>(base + query.rva + 5));
        std::memcpy(base + query.rva + 1, &relative, 4);
        VirtualProtect(base + query.rva, 5, protect, &protect);
        FlushInstructionCache(GetCurrentProcess(), base + query.rva, 5);
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
    if (g_hudScale != g_wantedHud) {
        g_hudScale = g_wantedHud;
        for (const auto& timer : g_timers) ApplyTimer(timer.first, timer.second);
        g_redraw = true;
    }
    const bool active = g_windowScale != 1.0f || g_hudScale != 1.0f;
    if (!active) {
        if (g_active) {
            g_windows.clear(); g_views.clear(); g_clips.clear();
            g_redraw = true;
        }
        g_active = false;
        return g_redraw;
    }
    g_active = true;
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
        const bool rollup = !std::strcmp(TypeOf(view), ".?AVRollupController_c@@");
        const bool scaffold = split && !rollup && (width >= screen.x * 0.9f || height >= screen.y * 0.9f);
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
                // Keep full-height rollups anchored at the top.
                if (rollup && height >= screen.y * 0.9f) map.offset.y = 0;
            }
            current.emplace(view, Window{owner, map});
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
        const float factor = NativeScreen(owner) ? 1.0f :
                             owner == dragWindow && sourceOwner ? (Hud(sourceOwner) ? g_hudScale : g_windowScale) :
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
        current.emplace(root, Window{owner, map});
        assign(assign, root, owner, map, full);
    }
    g_windows.swap(current);
    g_views.swap(views);
    g_clips.swap(clips);
    return g_redraw;
}

float ScaleForView(void* view) { return For(view).value; }
bool ScalingActive() { return g_active; }

}  // namespace rnative::gui
