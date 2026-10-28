// Exercise the checked GUI.dll hooks on synthetic objects in the supported client layout.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native/gui_scale.h"
#include "native/gui.h"
#include "native/gui_font.h"
#include "native/vc10.h"
#include "native/native.h"
#include "../rvk/ui_atlas.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>

namespace rnative::gui {
void InstallScaling(HMODULE gui);
bool BeginScaleFrame(void* controller);
float ScaleForView(void* view);
}

inline int __fastcall CaptureGuiWindowRect(void* message, void*, const char*, const rnative::gui::Rect* frame)
{
    std::memcpy(message, frame, sizeof(*frame));
    return 0;
}

struct GuiCheckPatch {
    void* entry;
    uint8_t saved[5];
    GuiCheckPatch(void* at, void* target) : entry(at)
    {
        std::memcpy(saved, entry, sizeof(saved));
        DWORD protection;
        VirtualProtect(entry, sizeof(saved), PAGE_EXECUTE_READWRITE, &protection);
        uint8_t jump[5]{0xE9};
        const int32_t relative = int32_t(reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(entry) - 5);
        std::memcpy(jump + 1, &relative, 4); std::memcpy(entry, jump, 5);
        VirtualProtect(entry, sizeof(saved), protection, &protection);
        FlushInstructionCache(GetCurrentProcess(), entry, sizeof(saved));
    }
    ~GuiCheckPatch()
    {
        DWORD protection;
        VirtualProtect(entry, sizeof(saved), PAGE_EXECUTE_READWRITE, &protection);
        std::memcpy(entry, saved, sizeof(saved));
        VirtualProtect(entry, sizeof(saved), protection, &protection);
        FlushInstructionCache(GetCurrentProcess(), entry, sizeof(saved));
    }
};

inline void __fastcall GuiCheckCaptureMouse(void*, void*, uint32_t) {}
inline void __fastcall GuiCheckPointer(void*, void*, uint32_t) {}
inline void __fastcall GuiCheckMoveFrame(void* view, void*, const rnative::gui::Rect* frame)
{
    void* data; std::memcpy(&data, static_cast<uint8_t*>(view) + 0xAC, sizeof(data));
    std::memcpy(static_cast<uint8_t*>(data) + 0x20, frame, sizeof(*frame));
}
inline void __fastcall GuiCheckSetFrame(void* view, void*, const rnative::gui::Rect* frame, uint32_t)
{
    GuiCheckMoveFrame(view, nullptr, frame);
}
inline void __fastcall GuiCheckMovePosition(void* view, void*, const rnative::gui::Point* position)
{
    void* data; std::memcpy(&data, static_cast<uint8_t*>(view) + 0xAC, sizeof(data));
    auto& frame = *reinterpret_cast<rnative::gui::Rect*>(static_cast<uint8_t*>(data) + 0x20);
    frame.right += position->x - frame.left; frame.bottom += position->y - frame.top;
    frame.left = position->x; frame.top = position->y;
}
inline bool __fastcall GuiCheckNoCapture(void*, void*) { return false; }
inline rnative::gui::Point g_guiTipPoint;
inline int g_guiTipCalls;
inline void* __cdecl GuiCheckTooltip(void* view, rnative::gui::Point point, rnative::vc10::String* title, rnative::vc10::String*)
{
    g_guiTipPoint = point; ++g_guiTipCalls;
    title->assign("Tooltip probe", 13);
    return view;
}
inline void* __cdecl GuiCheckDisplay() { static uint8_t dummy[16]{}; return dummy; }
inline void __fastcall GuiCheckUpdateArea(void*, void*, const void*) {}
inline int g_guiAllocations;
inline void __fastcall GuiCheckAllocate(void*, void*) { ++g_guiAllocations; }
inline void __fastcall GuiCheckRender(void*, void*, void*, float, uint32_t) {}

struct GuiCheckImport {
    void** slot;
    void* saved;
    GuiCheckImport(void** address, void* target) : slot(address), saved(*address) { Set(target); }
    ~GuiCheckImport() { Set(saved); }
    void Set(void* value) {
        DWORD protection;
        VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection);
        *slot = value;
        VirtualProtect(slot, sizeof(void*), protection, &protection);
    }
};

inline LONG WINAPI GuiCheckException(EXCEPTION_POINTERS* exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_FLT_INVALID_OPERATION &&
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    std::printf("GUI.dll: exception %08lx at %p\n", exception->ExceptionRecord->ExceptionCode,
                exception->ExceptionRecord->ExceptionAddress);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

#include "gui_sidebar_check.h"
#include "gui_popup_check.h"
#include "gui_chat_check.h"
#include "gui_timer_check.h"

inline int CheckGuiClient(const char* client, bool bypass = false, bool benchmark = false)
{
    using namespace rnative::gui;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, &GuiCheckException);
    std::printf("GUI.dll: loading supported client\n");
    SetDllDirectoryA(client);
    const std::string path = std::string(client) + "/GUI.dll";
    HMODULE module = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { std::printf("GUI.dll: load failed (%lu)\n", GetLastError()); return 1; }
    const auto* bytes = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(bytes + dos->e_lfanew);
    if (pe->FileHeader.TimeDateStamp != 0x5CD32BEA || pe->OptionalHeader.SizeOfImage != 0x2B0000) {
        std::printf("GUI.dll: unsupported fixture layout\n"); return 1;
    }
    std::printf("GUI.dll: client loaded\n");
    auto symbol = [&](const char* name) { return GetProcAddress(module, name); };
    using ValuePoint = Point*(__thiscall*)(void*, Point*, const Point*, uint32_t);
    using PointerPoint = void(__thiscall*)(void*, Point*, uint32_t);
    using ValueRect = Rect*(__thiscall*)(void*, Rect*, const Rect*, uint32_t);
    using ParentPoint = Point*(__thiscall*)(void*, Point*, const Point*);
    using Getter = Point*(__thiscall*)(void*, Point*);
    using Screen = Point*(__cdecl*)(Point*);
    auto to = reinterpret_cast<ValuePoint>(symbol("?ConvertToScreen@View@@QBE?AVPoint@@ABV2@_N@Z"));
    auto from = reinterpret_cast<ValuePoint>(symbol("?ConvertFromScreen@View@@QBE?AVPoint@@ABV2@_N@Z"));
    auto toPointer = reinterpret_cast<PointerPoint>(symbol("?ConvertToScreen@View@@QBEXPAVPoint@@_N@Z"));
    auto fromPointer = reinterpret_cast<PointerPoint>(symbol("?ConvertFromScreen@View@@QBEXPAVPoint@@_N@Z"));
    auto toRect = reinterpret_cast<ValueRect>(symbol("?ConvertToScreen@View@@QBE?AVRect@@ABV2@_N@Z"));
    auto fromRect = reinterpret_cast<ValueRect>(symbol("?ConvertFromScreen@View@@QBE?AVRect@@ABV2@_N@Z"));
    auto parent = reinterpret_cast<ParentPoint>(symbol("?ConvertFromParent@View@@QBE?AVPoint@@ABV2@@Z"));
    auto screen = reinterpret_cast<Screen>(symbol("?GetScreenSize@Window@@SA?AVPoint@@XZ"));
    auto physical = reinterpret_cast<Getter>(symbol("?GetScreenSize@WindowController_c@@QBE?AVPoint@@XZ"));
    auto windowMouse = reinterpret_cast<Getter>(symbol("?GetMousePosition@Window@@QBE?AVPoint@@XZ"));
    auto mouse = reinterpret_cast<Getter>(symbol("?GetMousePosition@WindowController_c@@QBE?AVPoint@@XZ"));
    auto** instance = reinterpret_cast<void**>(symbol("?s_pcInstance@WindowController_c@@0PAV1@A"));
    if (!to || !from || !toPointer || !fromPointer || !toRect || !fromRect || !parent || !screen || !physical || !mouse || !windowMouse || !instance)
        return 1;
    struct Object { alignas(16) uint8_t bytes[0x400]{}; };
    Object controller, top, topData, root, rootData, owner, ownerData, child, childData;
    Object hud, hudData, hudOwner, hudOwnerData, hudChild, hudChildData;
    auto put = [](Object& o, size_t offset, const auto& value) { std::memcpy(o.bytes + offset, &value, sizeof(value)); };
    auto pointer = [&](Object& o, size_t offset, Object& target) { void* p = target.bytes; put(o, offset, p); };
    auto view = [&](Object& object, Object& data, Object& window, Rect frame) {
        pointer(object, 0xAC, data); pointer(object, 0x40, window);
        put(data, 0x20, frame); put(data, 0x100, Point{1, 1});
    };
    rnative::vc10::String name, hudName;
    name.init(); hudName.init();
    std::memcpy(name.buffer, "test", 5); name.size = 4;
    std::memcpy(hudName.buffer, "cc_test", 8); hudName.size = 7;
    void* n = &name; put(owner, 0x60, n); n = &hudName; put(hudOwner, 0x60, n);
    pointer(owner, 0x70, ownerData); pointer(ownerData, 0x80, root);
    pointer(hudOwner, 0x70, hudOwnerData); pointer(hudOwnerData, 0x80, hud);
    pointer(top, 0xAC, topData); put(topData, 0x100, Point{1, 1});
    view(root, rootData, owner, {100, 50, 299, 149});
    view(child, childData, owner, {20, 10, 99, 49});
    view(hud, hudData, hudOwner, {500, 50, 699, 149});
    view(hudChild, hudChildData, hudOwner, {10, 10, 89, 49});
    pointer(rootData, 0x10, top); pointer(childData, 0x10, root);
    pointer(hudData, 0x10, top); pointer(hudChildData, 0x10, hud);
    pointer(root, 0x1B8, child); pointer(hud, 0x1B8, hudChild);
    pointer(rootData, 0, child); pointer(hudData, 0, hudChild);
    pointer(topData, 0, root); pointer(rootData, 8, hud);
    pointer(controller, 0x94, top); put(controller, 0x80, Point{3840, 2160});
    put(controller, 0x48, Point{246, 128});
    void* saved = *instance; *instance = controller.bytes;
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("GUI.dll: %s\n", what); } };
    auto equal = [](Point a, Point b) { return a.x == b.x && a.y == b.y; };
    auto bench = [&](const char* label, bool mapping) {
        constexpr int count = 20000;
        LARGE_INTEGER frequency, start, end; QueryPerformanceFrequency(&frequency);
        Point input{3, 4}, result, inverse;
        QueryPerformanceCounter(&start);
        for (int i = 0; i < count; ++i) { to(child.bytes, &result, &input, 1); from(child.bytes, &inverse, &result, 1); }
        QueryPerformanceCounter(&end);
        std::printf("GUI bench: %s coordinate pair %.3f us\n", label, double(end.QuadPart - start.QuadPart) * 1e6 / frequency.QuadPart / count);
        if (mapping) {
            QueryPerformanceCounter(&start);
            for (int i = 0; i < count; ++i) BeginScaleFrame(controller.bytes);
            QueryPerformanceCounter(&end);
            std::printf("GUI bench: %s scale frame %.3f us (two synthetic windows)\n", label, double(end.QuadPart - start.QuadPart) * 1e6 / frequency.QuadPart / count);
        }
    };
    if (benchmark) bench("unhooked", false);
    if (bypass) {
        char directory[MAX_PATH], ini[MAX_PATH];
        GetTempPathA(MAX_PATH, directory); GetTempFileNameA(directory, "gui", 0, ini);
        WritePrivateProfileStringA("Native", "UiScaling", "off", ini);
        rnative::SetIniPath(ini);
        const std::vector<uint8_t> code(bytes, bytes + pe->OptionalHeader.SizeOfImage);
        InstallScaling(module);
        check(!FontScalingReady(), "bypass leaves font hooks uninstalled");
        check(std::memcmp(code.data(), bytes, code.size()) == 0, "bypass leaves GUI code, call sites and imports untouched");
        SetScale(150, 150);
        check(!BeginScaleFrame(nullptr) && !ScalingActive(), "bypass never reads the window hierarchy");
        DeleteFileA(ini); rnative::SetIniPath(""); *instance = saved;
        std::printf("GUI bypass: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
        return failures ? 1 : 0;
    }
    InstallScaling(module);
    std::printf("GUI.dll: scaling installed\n");
    if (benchmark) {
        for (int scale : {100, 150, 200}) {
            SetScale(float(scale), float(scale)); BeginScaleFrame(controller.bytes);
            const std::string label = std::to_string(scale);
            bench(label.c_str(), true);
        }
        *instance = saved; return 0;
    }
    SetScale(200, 150);
    Point pending;
    screen(&pending);
    check(equal(pending, {3840, 2160}), "scale change waits for the next GUI frame");
    BeginScaleFrame(controller.bytes);
    Point input{3, 4}, output, restored;
    to(child.bytes, &output, &input, 1);
    check(equal(output, {246, 128}), "window drawing uses 200%");
    from(child.bytes, &restored, &output, 1);
    check(equal(restored, input), "window hit test inverts the display transform once");
    Point pointerInput = input;
    toPointer(child.bytes, &pointerInput, 1);
    check(equal(pointerInput, output), "pointer and value drawing overloads agree");
    fromPointer(child.bytes, &pointerInput, 1);
    check(equal(pointerInput, input), "pointer input overload inverts drawing");
    to(hudChild.bytes, &output, &input, 1);
    check(equal(output, {769.5f, 96}), "HUD drawing uses its independent 150%");
    from(hudChild.bytes, &restored, &output, 1);
    check(equal(restored, input), "HUD input matches drawing");
    Point hit{246, 128};
    parent(root.bytes, &output, &hit);
    parent(child.bytes, &restored, &output);
    check(equal(restored, input), "root hit testing and nested local coordinates agree");
    Rect rectangle{0, 0, 19, 9}, drawn, inverse;
    toRect(child.bytes, &drawn, &rectangle, 1);
    check(drawn.left == 240 && drawn.top == 120 && drawn.right == 279 && drawn.bottom == 139, "clip bounds map to physical pixels");
    fromRect(child.bytes, &inverse, &drawn, 1);
    check(std::memcmp(&rectangle, &inverse, sizeof(rectangle)) == 0, "clip inverse retains inclusive bounds");
    screen(&output); check(equal(output, {3840, 2160}), "native layout retains the canonical 4K workspace");
    physical(controller.bytes, &output); check(equal(output, {3840, 2160}), "physical output extent stays unchanged");
    mouse(controller.bytes, &output); check(equal(output, {246, 128}), "cursor and world mouse coordinates stay physical");
    SetScale(100, 100);
    to(child.bytes, &output, &input, 1);
    check(equal(output, {246, 128}), "drawing and input keep the displayed scale until the next frame");
    SetScale(200, 150);
    put(child, 0x188, Rect{4, 6, 0, 0});
    windowMouse(owner.bytes, &output);
    check(equal(output, {-1, -2}), "window mouse query accounts for scaled borders");
    // Compare a clipped textured quad against the native 100% reference.
    std::printf("GUI.dll: checking native atlas vertices\n");
    using Vertices = void(__thiscall*)(void*, void*, const Rect*);
    auto vertices = reinterpret_cast<Vertices>(symbol("?_UpdateVertices@ViewArea_c@@AAEXPAVView@@ABVRect@@@Z"));
    Object area, node, slot, texture;
    put(area, 0x80, Rect{10, 20, 109, 69});
    put(area, 0x90, Rect{0, 0, 99, 49});
    pointer(area, 0xA0, node); pointer(node, 0xC, slot); pointer(slot, 0, texture);
    put(texture, 8, uint32_t(256)); put(texture, 12, uint32_t(256));
    auto uv = [&](int i, int coord) {
        float v; std::memcpy(&v, area.bytes + i * 28 + 20 + coord * 4, 4); return v;
    };
    put(rootData, 0x20, Rect{0, 0, 199, 99});
    put(hudData, 0x20, Rect{0, 0, 199, 99});
    put(childData, 0x20, Rect{120, 60, 199, 99});
    // Login and character selection keep their native, fixed-screen geometry.
    SetScale(100, 100); BeginScaleFrame(controller.bytes);
    Point nativeLogin;
    to(child.bytes, &nativeLogin, &input, 1);
    for (size_t table : {size_t(0x1A9CBC), size_t(0x1A9C24), size_t(0x1A9B44), size_t(0x1AA07C)}) {
        put(owner, 0, static_cast<const void*>(bytes + table));
        SetScale(150, 150); BeginScaleFrame(controller.bytes);
        Point original{3, 4};
        to(child.bytes, &output, &original, 1);
        check(equal(output, nativeLogin), "pregame windows retain native screen coordinates at saved 150%");
    }
    for (size_t table : {size_t(0x1B4664), size_t(0x1B45DC), size_t(0x1BF0D4), size_t(0x1A963C),
                         size_t(0x1C258C), size_t(0x1B5784), size_t(0x1B1BA4), size_t(0x1C21EC)}) {
        put(owner, 0, static_cast<const void*>(bytes + table));
        SetScale(100, 150); BeginScaleFrame(controller.bytes);
        check(ScaleForView(child.bytes) == 1.5f, "HUD window classes follow HUD scale independently of window scale");
    }
    for (size_t table : {size_t(0x1AFEFC), size_t(0x1B3D64), size_t(0x1C16AC), size_t(0x1C1F2C), size_t(0x1C240C)}) {
        put(owner, 0, static_cast<const void*>(bytes + table));
        SetScale(125, 200); BeginScaleFrame(controller.bytes);
        check(ScaleForView(child.bytes) == 1.25f, "ordinary window classes follow window scale independently of HUD scale");
    }
    put(owner, 0, static_cast<void*>(nullptr));
    SetScale(100, 100); BeginScaleFrame(controller.bytes);
    check(!ScalingActive(), "100% disables hierarchy mapping");
    put(topData, 0, reinterpret_cast<void*>(1));
    BeginScaleFrame(controller.bytes);
    pointer(topData, 0, root);
    const Rect clip100{150, 60, 199, 109};
    vertices(area.bytes, child.bytes, &clip100);
    float reference[8], positions[8];
    for (int i = 0; i < 4; ++i) for (int c = 0; c < 2; ++c) {
        reference[i * 2 + c] = uv(i, c);
        std::memcpy(&positions[i * 2 + c], area.bytes + i * 28 + c * 4, 4);
    }
    for (float factor : {0.5f, 0.75f, 1.25f, 1.5f, 2.0f, 1.0f}) {
        SetScale(factor * 100, 100); BeginScaleFrame(controller.bytes);
        Rect clip = clip100;
        Scale{factor}.ToScreen(clip);
        vertices(area.bytes, child.bytes, &clip);
        bool sameUv = true, correctPositions = true;
        for (int i = 0; i < 4; ++i) for (int c = 0; c < 2; ++c) {
            sameUv = sameUv && reference[i * 2 + c] == uv(i, c);
            float position;
            std::memcpy(&position, area.bytes + i * 28 + c * 4, 4);
            correctPositions = correctPositions && position == (positions[i * 2 + c] + 0.5f) * factor - 0.5f;
        }
        check(sameUv, "clipped atlas mapping is unchanged by UI scaling");
        check(correctPositions, "clipped vertices scale with their half-pixel convention intact");
        std::vector<rvk::detail::UiAtlasVertex> guarded;
        check(rvk::detail::UiAtlasVertices(area.bytes, 4, 4, 256, 256, guarded),
              "atlas protection accepts actual native GUI vertices, including reciprocal depth 100");
        if (guarded.size() == 4) {
            const auto* original = reinterpret_cast<const rvk::detail::UiSourceVertex*>(area.bytes);
            check(original[0].rhw == 100.0f, "native GUI fixture uses its actual reciprocal depth");
            for (int i = 0; i < 4; ++i)
                check(guarded[i].rhw == original[i].rhw && guarded[i].u == original[i].u && guarded[i].v == original[i].v,
                      "native atlas protection preserves depth and UV mapping");
            rvk::detail::UiSourceVertex perspective[4];
            std::memcpy(perspective, original, sizeof(perspective)); perspective[1].rhw *= 0.5f;
            check(!rvk::detail::UiAtlasVertices(perspective, 4, 4, 256, 256, guarded),
                  "atlas protection rejects quads whose reciprocal depth varies");
        }
    }
    // Slider changes leave the native frames used by preferences untouched.
    const Rect edge{3500, 1900, 3799, 2099};
    put(rootData, 0x20, edge);
    for (float percent : {100.0f, 200.0f, 75.0f, 150.0f, 100.0f}) {
        SetScale(percent, 100); BeginScaleFrame(controller.bytes);
        using WindowFrame = Rect*(__thiscall*)(void*, Rect*, uint32_t);
        auto windowFrame = reinterpret_cast<WindowFrame>(symbol("?GetFrame@Window@@QBE?AVRect@@_N@Z"));
        Rect actual;
        windowFrame(owner.bytes, &actual, 0);
        check(std::memcmp(&actual, &edge, sizeof(actual)) == 0, "native window queries and preference saves use canonical coordinates");
        using SaveWindow = void(__thiscall*)(void*, void*);
        auto saveWindow = reinterpret_cast<SaveWindow>(symbol("?SaveWndConfig@Window@@QAEXPAVMessage@@@Z"));
        auto** rectSink = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(module) + 0x1A800C);
        void* originalSink = *rectSink;
        DWORD protection;
        if (VirtualProtect(rectSink, sizeof(void*), PAGE_READWRITE, &protection)) {
            *rectSink = reinterpret_cast<void*>(&CaptureGuiWindowRect);
            put(ownerData, 0x3C, uint32_t(0x800));
            Rect savedFrame{};
            saveWindow(owner.bytes, &savedFrame);
            check(std::memcmp(&savedFrame, &edge, sizeof(savedFrame)) == 0, "actual native preference serialization excludes display transforms");
            put(ownerData, 0x3C, uint32_t(0));
            *rectSink = originalSink;
            VirtualProtect(rectSink, sizeof(void*), protection, &protection);
        } else check(false, "preference serialization fixture could not install its sink");
        std::memcpy(&actual, rootData.bytes + 0x20, sizeof(actual));
        check(std::memcmp(&actual, &edge, sizeof(actual)) == 0, "scale changes never move or resize the native frame");
        to(child.bytes, &output, &input, 1);
        from(child.bytes, &restored, &output, 1);
        check(equal(restored, input), "edge-clamped drawing and input agree");
    }
    to(child.bytes, &output, &input, 1);
    check(equal(output, {3623, 1964}), "100% restores the original edge placement exactly");
    put(hudData, 0x20, Rect{0, 0, 3839, 2159});
    put(hudChildData, 0x20, Rect{3700, 2000, 3799, 2099});
    SetScale(100, 200); BeginScaleFrame(controller.bytes);
    to(hudChild.bytes, &output, &input, 1);
    check(equal(output, {3566, 1848}), "full-screen HUD children scale around their screen edge");
    from(hudChild.bytes, &restored, &output, 1);
    check(equal(restored, input), "anchored HUD input inverts its display transform");
    Rect full;
    std::memcpy(&full, hudData.bytes + 0x20, sizeof(full));
    check(full.right == 3839 && full.bottom == 2159, "HUD scaling never reflows the native full-screen root");
    Point hudHit{3566, 1848};
    parent(hud.bytes, &output, &hudHit);
    parent(hudChild.bytes, &restored, &output);
    check(equal(restored, input), "HUD hit testing crosses the anchored subtree boundary once");
    SetScale(100, 100); BeginScaleFrame(controller.bytes);
    to(hudChild.bytes, &output, &input, 1);
    check(equal(output, {3703, 2004}), "HUD scale round trip restores original coordinates");
    Object icon, iconData;
    view(icon, iconData, hudOwner, {3700, 0, 3799, 79});
    pointer(iconData, 0x10, hudChild); pointer(hudChildData, 0, icon);
    put(hudChildData, 0x20, Rect{0, 2000, 3839, 2099});
    SetScale(100, 200); BeginScaleFrame(controller.bytes);
    to(icon.bytes, &output, &input, 1);
    check(equal(output, {3566, 1848}), "wide HUD containers preserve child anchoring");
    Point iconHit = output;
    parent(hud.bytes, &output, &iconHit);
    parent(hudChild.bytes, &restored, &output);
    parent(icon.bytes, &output, &restored);
    check(equal(output, input), "nested HUD input crosses a full-width container correctly");
    const Rect strip{0, 0, 3839, 99};
    toRect(hudChild.bytes, &drawn, &strip, 1);
    check(drawn.top == 1840 && drawn.bottom == 2099, "HUD container clipping includes enlarged children");
    SetScale(100, 100); BeginScaleFrame(controller.bytes);
    to(icon.bytes, &output, &input, 1);
    check(equal(output, {3703, 2004}), "nested HUD layout round trip restores original positions");
    failures += CheckGuiSidebar(module);
    failures += CheckGuiPopups(module);
    failures += CheckGuiTimers(module, controller.bytes);
    BeginScaleFrame(controller.bytes);
    std::printf("GUI.dll: checking native border input\n");
    // Run native border input through dispatch's inverse, isolating only cursor capture and frame application.
    {
        GuiCheckPatch pointerPatch(reinterpret_cast<void*>(symbol("?UpdateMousePointer@WndBorder@@AAEXW4hit_item@1@@Z")),
                                   reinterpret_cast<void*>(&GuiCheckPointer));
        GuiCheckPatch movePatch(reinterpret_cast<void*>(symbol("?DoSetFrame@WndBorder@@AAEXABVRect@@@Z")),
                                reinterpret_cast<void*>(&GuiCheckMoveFrame));
        using Down = void(__thiscall*)(void*, const Point*, int, int);
        using Up = void(__thiscall*)(void*, const Point*, int, void*);
        using Move = void(__thiscall*)(void*, const Point*, uint32_t, uint32_t, void*);
        auto down = reinterpret_cast<Down>(symbol("?MouseDown@WndBorder@@UAEXABVPoint@@HH@Z"));
        auto up = reinterpret_cast<Up>(symbol("?MouseUp@WndBorder@@UAEXABVPoint@@HPAVMessage@@@Z"));
        auto move = reinterpret_cast<Move>(symbol("?MouseMove@WndBorder@@UAEXABVPoint@@W4MouseTransition_e@@IPAVMessage@@@Z"));
        void* table[64]{}; table[0xAC / 4] = reinterpret_cast<void*>(&GuiCheckCaptureMouse);
        table[0xB8 / 4] = reinterpret_cast<void*>(&GuiCheckSetFrame);
        table[0xC8 / 4] = reinterpret_cast<void*>(symbol("?MoveTo@View@@UAEXABVPoint@@@Z"));
        void* vtable = table; put(root, 0, vtable);
        put(ownerData, 0x3C, uint32_t(0x30));
        put(root, 0x23C, Rect{0, 0, 299, 15});
        for (float factor : {0.5f, 1.5f, 1.76f, 2.0f, 1.0f}) {
            put(rootData, 0x20, edge);
            SetScale(factor * 100, 100); BeginScaleFrame(controller.bytes);
            Point grip{30, 5}, cursor, local;
            to(root.bytes, &cursor, &grip, 1);
            from(root.bytes, &local, &cursor, 1);
            down(root.bytes, &local, 1, 1);
            Rect dragFrame;
            std::memcpy(&dragFrame, root.bytes + 0x1C4, sizeof(dragFrame));
            check(std::memcmp(&dragFrame, &edge, sizeof(edge)) == 0, "native drag starts with the canonical window frame");
            move(root.bytes, &local, 0, 1, nullptr);
            std::memcpy(&dragFrame, rootData.bytes + 0x20, sizeof(dragFrame));
            check(std::fabs(dragFrame.left - edge.left) < 0.001f && std::fabs(dragFrame.top - edge.top) < 0.001f,
                  "pressing a scaled title bar does not move the window");
            up(root.bytes, &local, 1, nullptr);
            std::memcpy(&dragFrame, rootData.bytes + 0x20, sizeof(dragFrame));
            check(std::fabs(dragFrame.left - edge.left) < 0.001f && std::fabs(dragFrame.top - edge.top) < 0.001f,
                  "releasing a stationary title-bar click preserves the canonical frame");
            down(root.bytes, &local, 1, 1);
            cursor.x -= 60; cursor.y -= 30;
            from(root.bytes, &local, &cursor, 1);
            move(root.bytes, &local, 0, 1, nullptr);
            Point attached;
            to(root.bytes, &attached, &grip, 1);
            check(std::fabs(attached.x - cursor.x) < 0.001f && std::fabs(attached.y - cursor.y) < 0.001f,
                  "native dragging keeps the same scaled title-bar point attached to the cursor");
            up(root.bytes, &local, 1, nullptr);
            to(root.bytes, &attached, &grip, 1);
            check(std::fabs(attached.x - cursor.x) < 0.001f && std::fabs(attached.y - cursor.y) < 0.001f,
                  "normalizing a moved edge-clamped window preserves its displayed position");
            SetScale(100, 100); BeginScaleFrame(controller.bytes);
            SetScale(factor * 100, 100); BeginScaleFrame(controller.bytes);
            to(root.bytes, &attached, &grip, 1);
            check(std::fabs(attached.x - cursor.x) < 0.001f && std::fabs(attached.y - cursor.y) < 0.001f,
                  "reloading the moved native frame reconstructs the same display position");
        }
        put(root, 0, static_cast<void*>(nullptr)); put(ownerData, 0x3C, uint32_t(0));
        put(rootData, 0x20, edge);
    }
    SetScale(100, 100); BeginScaleFrame(controller.bytes);
    to(root.bytes, &output, &input, 1);
    const Point beforeSlider = output;
    put(controller, 0x58, uint32_t(1));
    SetScale(176, 150); BeginScaleFrame(controller.bytes);
    to(root.bytes, &output, &input, 1);
    check(equal(output, beforeSlider), "window scale waits while the primary mouse button is held");
    SetScale(200, 150); BeginScaleFrame(controller.bytes);
    to(root.bytes, &output, &input, 1);
    check(equal(output, beforeSlider), "dragging the scale slider keeps its window and hit mapping stable");
    put(controller, 0x58, uint32_t(0)); BeginScaleFrame(controller.bytes);
    to(root.bytes, &output, &input, 1);
    check(!equal(output, beforeSlider), "releasing the scale slider applies its final value");
    std::printf("GUI.dll: checking native tab resizing\n");
    {
        using Frame = Rect*(__thiscall*)(void*, Rect*);
        auto frame = reinterpret_cast<Frame>(symbol("?GetFrame@View@@QBE?AVRect@@XZ"));
        Rect canonical;
        frame(root.bytes, &canonical);
        check(std::memcmp(&canonical, &edge, sizeof(edge)) == 0, "view frame queries always retain canonical layout units");
        void* borderTable[64]{};
        borderTable[0xB8 / 4] = reinterpret_cast<void*>(&GuiCheckSetFrame);
        borderTable[0xC8 / 4] = reinterpret_cast<void*>(&GuiCheckMovePosition);
        borderTable[0xF0 / 4] = reinterpret_cast<void*>(symbol("?SetFrame@WndBorder@@UAEXABVRect@@@Z"));
        void* clientTable[64]{}; clientTable[0xB8 / 4] = reinterpret_cast<void*>(&GuiCheckSetFrame);
        put(root, 0, static_cast<void*>(borderTable)); put(child, 0, static_cast<void*>(clientTable));
        put(root, 0x2CC, Rect{}); put(root, 0x2DC, Rect{});
        put(root, 0x2EC, Point{1, 1}); put(root, 0x2F4, Point{10000, 10000});
        put(root, 0x3CC, int(0)); put(root, 0x3D0, int(0));
        put(root, 0x3D4, int(10000)); put(root, 0x3D8, int(10000)); put(root, 0x1D4, uint32_t(0));
        put(root, 0x3DC, int(1)); put(root, 0x3E0, int(1));
        auto* base = reinterpret_cast<uint8_t*>(module);
        GuiCheckPatch setFrame(reinterpret_cast<void*>(symbol("?SetFrame@View@@UAEXABVRect@@_N@Z")),
                               reinterpret_cast<void*>(&GuiCheckSetFrame));
        GuiCheckImport display(reinterpret_cast<void**>(base + 0x1A744C), reinterpret_cast<void*>(&GuiCheckDisplay));
        GuiCheckImport update(reinterpret_cast<void**>(base + 0x1A7398), reinterpret_cast<void*>(&GuiCheckUpdateArea));
        using Resize = void(__thiscall*)(void*, const Point*);
        auto resize = reinterpret_cast<Resize>(symbol("?ResizeClientTo@WndBorder@@QAEXABVPoint@@@Z"));
        for (Point size : {Point{350, 240}, Point{299, 199}, Point{350, 240}, Point{299, 199}}) {
            resize(root.bytes, &size);
            frame(root.bytes, &canonical);
            check(canonical.left == edge.left && canonical.top == edge.top,
                  "native client resizing during tab changes retains the window position");
        }
        put(child, 0x188, Rect{});
        put(controller, 0x48, Point{1800, 1000});
        using Placement = void(__thiscall*)(void*, uint32_t);
        auto placement = reinterpret_cast<Placement>(symbol("?MoveToMouse@Window@@QAEX_N@Z"));
        placement(owner.bytes, 1); BeginScaleFrame(controller.bytes);
        const Point centre{149.5f, 99.5f};
        to(root.bytes, &output, &centre, 1);
        check(std::fabs(output.x - 1800) <= 2 && std::fabs(output.y - 1000) <= 2,
              "native mouse placement discards the previous edge-clamp offset");
        put(root, 0, static_cast<void*>(nullptr)); put(child, 0, static_cast<void*>(nullptr));
        put(rootData, 0x20, edge);
    }
    SetScale(176, 150); BeginScaleFrame(controller.bytes);
    std::printf("GUI.dll: checking native tooltip hit testing\n");
    {
        // The real tooltip controller must find displayed bounds even outside Render.
        auto* base = reinterpret_cast<uint8_t*>(module);
        GuiCheckPatch tooltip(base + 0x156D66, reinterpret_cast<void*>(&GuiCheckTooltip));
        void* table[64]{}; table[0x38 / 4] = reinterpret_cast<void*>(&GuiCheckNoCapture);
        put(owner, 0, static_cast<void*>(table));
        auto* title = reinterpret_cast<rnative::vc10::String*>(controller.bytes + 0xA4);
        auto* body = reinterpret_cast<rnative::vc10::String*>(controller.bytes + 0xC0);
        title->init(); body->init(); pointer(controller, 0xA0, root);
        using Tooltip = void(__thiscall*)(void*, uint32_t);
        auto updateTooltip = reinterpret_cast<Tooltip>(symbol("?UpdateToolTip@WindowController_c@@AAEX_N@Z"));
        const Point hover{10, 10};
        to(root.bytes, &output, &hover, 1); put(controller, 0x48, output);
        g_guiTipCalls = 0;
        updateTooltip(controller.bytes, 0);
        check(g_guiTipCalls == 1 && std::fabs(g_guiTipPoint.x - hover.x) < 0.001f && std::fabs(g_guiTipPoint.y - hover.y) < 0.001f,
              "native tooltip hit testing finds the scaled window and logical hover point");
        check(title->size == 13, "native tooltip timer receives its hovered text");
        title->release(); body->release();
        put(owner, 0, static_cast<void*>(nullptr)); put(controller, 0xA0, static_cast<void*>(nullptr));
    }
    std::printf("GUI.dll: checking native item dragging\n");
    {
        Object drag, image, imageData, imageOwner, imageOwnerData;
        view(image, imageData, imageOwner, {0, 0, 31, 31});
        put(imageOwner, 0x60, static_cast<void*>(&name)); pointer(imageOwner, 0x70, imageOwnerData);
        pointer(imageOwnerData, 0x80, image); pointer(imageData, 0x10, top); pointer(hudData, 8, image);
        pointer(controller, 0x194, drag); pointer(drag, 0x54, imageOwner);
        put(drag, 0x58, Point{-8, -5});
        void* table[64]{}; table[0xC8 / 4] = reinterpret_cast<void*>(&GuiCheckMovePosition);
        put(image, 0, static_cast<void*>(table));
        using DragMove = void(__thiscall*)(void*, const Point*);
        auto dragMove = reinterpret_cast<DragMove>(symbol("?SlotMouseMoved@DragObject_c@@AAEXABVPoint@@@Z"));
        for (bool fromHud : {false, true}) {
            pointer(drag, 0x60, fromHud ? hudChild : child);
            SetScale(176, 150);
            for (Point cursor : {Point{1000, 600}, Point{2400, 1200}, Point{10, 10}}) {
                put(controller, 0x48, cursor);
                dragMove(drag.bytes, &cursor);
                BeginScaleFrame(controller.bytes);
                const Point grip{8, 5};
                to(image.bytes, &output, &grip, 1);
                check(std::fabs(output.x - cursor.x) < 0.001f && std::fabs(output.y - cursor.y) < 0.001f,
                      "native dragged item image keeps its hotspot on the physical cursor");
            }
        }
        put(controller, 0x194, static_cast<void*>(nullptr)); put(hudData, 8, static_cast<void*>(nullptr));
        put(controller, 0x48, Point{246, 128});
    }
    SetScale(100, 100); BeginScaleFrame(controller.bytes);
    failures += CheckGuiChat(module, controller.bytes, root.bytes);
    check(FontScalingReady(), "native text-cache hooks installed");
    using FontCtor = void*(__thiscall*)(void*);
    using FontDtor = void(__thiscall*)(void*);
    struct IPoint { int x, y; };
    using RenderString = void(__thiscall*)(void*, IPoint*, int, int, int, void*, uint32_t, uint32_t, uint32_t);
    auto fontCtor = reinterpret_cast<FontCtor>(symbol("??0FontInfo_t@@QAE@XZ"));
    auto fontDtor = reinterpret_cast<FontDtor>(symbol("??1FontInfo_t@@QAE@XZ"));
    auto renderString = reinterpret_cast<RenderString>(symbol("?_RenderString@TextRenderer_c@@AAEXPAVIPoint@@HHHPAVFontInfo_t@@I_N2@Z"));
    alignas(16) uint8_t fontInfo[0x1418]{};
    fontCtor(fontInfo);
    LOGFONTA description{};
    description.lfHeight = -16; description.lfWeight = FW_NORMAL; description.lfCharSet = DEFAULT_CHARSET;
    std::memcpy(description.lfFaceName, "Verdana", 8);
    HFONT fontHandle = CreateFontIndirectA(&description);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldFont = SelectObject(dc, fontHandle);
    TEXTMETRICA metrics{}; GetTextMetricsA(dc, &metrics);
    char face[LF_FACESIZE]{}; GetTextFaceA(dc, LF_FACESIZE, face);
    std::printf("GUI font: requested Verdana, resolved %s (charset %u)\n", face, unsigned(metrics.tmCharSet));
    SelectObject(dc, oldFont); DeleteDC(dc);
    auto rawPut = [](void* object, size_t offset, const auto& value) { std::memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(value)); };
    rawPut(fontInfo, 0x1018, fontHandle); rawPut(fontInfo, 0xFFC, uint8_t(1)); rawPut(fontInfo, 0xFF0, int(metrics.tmHeight));
    Object text, textData;
    view(text, textData, owner, {0, 0, 383, 39});
    rnative::vc10::String content{}; content.init(); content.assign("Verdana 4K: Agjp 123", 20);
    put(text, 0x14C, content);
    std::vector<uint32_t> baseline;
    IPoint baselinePen{};
    for (float factor : {1.0f, 2.0f, 1.5f, 1.1f, 1.76f, 1.0f}) {
        alignas(16) uint8_t bitmap[0x44]{};
        CreateTextBitmap(bitmap, 384, 40, factor);
        void* cache = bitmap; put(text, 0x248, cache);
        IPoint pen{4, 4};
        renderString(text.bytes, &pen, metrics.tmHeight, 0, int(content.size), fontInfo, 0xFFFFFF, 0, 0);
        uint8_t* data; int width, height, pitch, bytesPerPixel;
        std::memcpy(&data, bitmap, 4); std::memcpy(&width, bitmap + 0xC, 4); std::memcpy(&height, bitmap + 8, 4);
        std::memcpy(&pitch, bitmap + 0x20, 4); std::memcpy(&bytesPerPixel, bitmap + 0x24, 4);
        std::vector<uint32_t> rendered(size_t(width) * height);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            if (bytesPerPixel == 4) std::memcpy(&rendered[size_t(y) * width + x], data + size_t(y) * pitch + x * 4, 4);
            else {
                uint16_t pixel; std::memcpy(&pixel, data + size_t(y) * pitch + x * 2, 2);
                rendered[size_t(y) * width + x] = (pixel & 0x8000 ? 0xFF000000 : 0) | (pixel & 0x7FFF ? 0xFFFFFF : 0);
            }
        }
        const auto* pixels = rendered.data();
        int visible = 0, antialias = 0;
        for (int i = 0; i < width * height; ++i) {
            const uint32_t alpha = pixels[i] >> 24;
            if (alpha) ++visible;
            if (alpha && alpha < 255) ++antialias;
        }
        check(visible > 0, "native text string produces visible glyphs");
        if (factor == 1.0f && baseline.empty()) { baseline.assign(pixels, pixels + width * height); baselinePen = pen; }
        check(pen.x == baselinePen.x && pen.y == baselinePen.y, "font raster size leaves native text advances unchanged");
        if (factor > 1.0f) check(antialias > 0, "larger text uses freshly rasterized antialiased outlines");
        if (factor == 2.0f) {
            int differences = 0;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                if (pixels[size_t(y) * width + x] != baseline[size_t(y / 2) * 384 + x / 2]) ++differences;
            check(differences > 0, "200% text is not a magnified copy of the native bitmap");
        }
        if (factor > 1.0f) {
            IPoint selectedPen{4, 4};
            renderString(text.bytes, &selectedPen, metrics.tmHeight, 0, int(content.size), fontInfo, 0xFFFFFF, 1, 1);
            uint32_t selectedPixel;
            const int corner = int(std::lround(4 * factor));
            std::memcpy(&selectedPixel, data + size_t(corner) * pitch + corner * 4, 4);
            check((selectedPixel >> 24) == 255, "selection and underline drawing use the high-resolution cache");
            check(selectedPen.x == baselinePen.x && selectedPen.y == baselinePen.y, "selected text retains native advances");
        }
        Object surface;
        put(surface, 0x3D, uint8_t(1)); put(surface, 0x3E, uint8_t(1));
        put(surface, 4, Rect{0, 0, float(width - 1), float(height - 1)});
        put(surface, 0x14, Rect{0, 0, float(width - 1), float(height - 1)});
        put(surface, 0x34, float(width)); put(surface, 0x38, float(height));
        void* textureSurface = surface.bytes; put(text, 0x240, textureSurface);
        put(text, 0x28C, 1.0f); put(text, 0x290, 1.0f);
        using LayoutSurface = void(__thiscall*)(void*);
        using SurfaceFrame = Rect*(__thiscall*)(void*, Rect*);
        auto layoutSurface = reinterpret_cast<LayoutSurface>(symbol("?_LayoutRenderSurface@TextRenderer_c@@AAEXXZ"));
        auto sourceFrame = reinterpret_cast<SurfaceFrame>(symbol("?GetSrcFrame@ViewSurface_c@@QBE?AVRect@@XZ"));
        auto destinationFrame = reinterpret_cast<SurfaceFrame>(symbol("?GetDstFrame@ViewSurface_c@@QBE?AVRect@@XZ"));
        layoutSurface(text.bytes);
        Rect textureFrame, logicalFrame;
        sourceFrame(surface.bytes, &textureFrame); destinationFrame(surface.bytes, &logicalFrame);
        check(textureFrame.right - textureFrame.left + 1 == width, "text texture retains its full raster resolution");
        check(logicalFrame.right - logicalFrame.left + 1 == 384 && logicalFrame.bottom - logicalFrame.top + 1 == 40,
              "larger font texture retains its native logical layout size");
        if (factor == 1.0f) check(std::memcmp(pixels, baseline.data(), baseline.size() * 4) == 0, "100% retains the exact native text raster");
        char* prefix = nullptr; size_t prefixSize = 0;
        _dupenv_s(&prefix, &prefixSize, "RANDYVK_GUI_FONT_SHOT");
        if (prefix) {
            const std::string path = std::string(prefix) + "-" + std::to_string(int(factor * 100)) + ".bmp";
            FILE* outputFile = nullptr;
            if (fopen_s(&outputFile, path.c_str(), "wb") == 0 && outputFile) {
                BITMAPFILEHEADER file{}; file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + width * height * 4;
                BITMAPINFOHEADER image{}; image.biSize = sizeof(image); image.biWidth = width; image.biHeight = -height; image.biPlanes = 1; image.biBitCount = 32;
                std::fwrite(&file, sizeof(file), 1, outputFile); std::fwrite(&image, sizeof(image), 1, outputFile);
                for (int i = 0; i < width * height; ++i) {
                    const uint32_t value = (pixels[i] >> 24) * 0x010101;
                    std::fwrite(&value, 4, 1, outputFile);
                }
                std::fclose(outputFile);
            }
        }
        std::free(prefix);
        rnative::vc10::FreeArray(data); ForgetTextBitmap(bitmap);
    }
    fontDtor(fontInfo);
    std::printf("GUI.dll: checking small-font strokes and descenders\n");
    for (int size : {6, 8, 9, 12, 16}) for (int style = 0; style < 4; ++style) {
        std::memset(fontInfo, 0, sizeof(fontInfo)); fontCtor(fontInfo);
        description.lfHeight = style == 0 ? -size : size;
        description.lfWeight = style == 2 ? FW_BOLD : FW_NORMAL; description.lfItalic = style == 3;
        HFONT original = CreateFontIndirectA(&description);
        HDC reference = CreateCompatibleDC(nullptr);
        HGDIOBJ previous = SelectObject(reference, original);
        GetTextMetricsA(reference, &metrics);
        rawPut(fontInfo, 0x1018, original); rawPut(fontInfo, 0xFFC, uint8_t(1)); rawPut(fontInfo, 0xFF0, int(metrics.tmHeight)); rawPut(fontInfo, 0x1014, int(metrics.tmOverhang));
        for (float factor : {1.25f, 1.5f, 1.76f, 2.0f}) {
            LOGFONTA outlineDescription = description;
            outlineDescription.lfHeight = int(std::floor(metrics.tmHeight * factor)); outlineDescription.lfQuality = ANTIALIASED_QUALITY;
            HFONT outline = CreateFontIndirectA(&outlineDescription);
            int nativeAdvance = 0, scaledAdvance = 0;
            for (WCHAR c : std::wstring(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")) {
                SIZE extent{};
                SelectObject(reference, original); GetTextExtentPoint32W(reference, &c, 1, &extent); nativeAdvance += std::max(0, std::min(int(extent.cx), 32) - int(metrics.tmOverhang));
                SelectObject(reference, outline); GetTextExtentPoint32W(reference, &c, 1, &extent); scaledAdvance += extent.cx;
            }
            TEXTMETRICA outlineMetrics{}; GetTextMetricsA(reference, &outlineMetrics);
            for (char character : std::string("ilWMgjpqyNCU0123456789")) {
                content.assign(&character, 1); put(text, 0x14C, content);
                alignas(16) uint8_t bitmap[0x44]{};
                CreateTextBitmap(bitmap, 64, metrics.tmHeight * 2, factor);
                void* cache = bitmap; put(text, 0x248, cache);
                IPoint pen{2, 0};
                renderString(text.bytes, &pen, metrics.tmHeight, 0, 1, fontInfo, 0xFFFFFF, 0, 0);
                IPoint secondLine{2, int(metrics.tmHeight)};
                renderString(text.bytes, &secondLine, metrics.tmHeight, 0, 1, fontInfo, 0xFFFFFF, 0, 0);
                MAT2 identity{};
                const int fixed = int(std::lround(float(nativeAdvance) * factor / scaledAdvance * 65536));
                identity.eM11.value = SHORT(fixed >> 16); identity.eM11.fract = WORD(fixed);
                identity.eM22.value = 1;
                GLYPHMETRICS glyph{};
                const DWORD count = GetGlyphOutlineW(reference, WCHAR(character), GGO_GRAY8_BITMAP, &glyph, 0, nullptr, &identity);
                check(count != GDI_ERROR, "reference font supplies a natural-size glyph");
                std::vector<uint8_t> expected(count == GDI_ERROR ? 0 : count);
                if (count && count != GDI_ERROR) GetGlyphOutlineW(reference, WCHAR(character), GGO_GRAY8_BITMAP, &glyph, count, expected.data(), &identity);
                uint64_t expectedInk = 0, actualInk = 0;
                const size_t stride = (glyph.gmBlackBoxX + 3) & ~size_t(3);
                for (DWORD y = 0; y < glyph.gmBlackBoxY && count != GDI_ERROR; ++y)
                    for (DWORD x = 0; x < glyph.gmBlackBoxX; ++x) expectedInk += std::min(255u, unsigned(expected[y * stride + x]) * 255 / 64);
                uint8_t* data; int width, height, pitch;
                std::memcpy(&data, bitmap, 4); std::memcpy(&width, bitmap + 0xC, 4);
                std::memcpy(&height, bitmap + 8, 4); std::memcpy(&pitch, bitmap + 0x20, 4);
                std::vector<uint32_t> expectedPixels(size_t(width) * height);
                const int left = int(std::lround(2 * factor)) + glyph.gmptGlyphOrigin.x;
                const int baseline = outlineMetrics.tmAscent;
                for (int line = 0; line < 2; ++line) {
                    const int top = int(std::lround(line * metrics.tmHeight * factor)) + baseline - glyph.gmptGlyphOrigin.y;
                    for (DWORD y = 0; y < glyph.gmBlackBoxY; ++y) for (DWORD x = 0; x < glyph.gmBlackBoxX; ++x) {
                        if (top + int(y) >= 0 && top + int(y) < height && left + int(x) >= 0 && left + int(x) < width)
                            expectedPixels[size_t(top + y) * width + left + x] = std::min(255u, unsigned(expected[y * stride + x]) * 255 / 64);
                    }
                }
                bool placed = true;
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                    const uint32_t alpha = reinterpret_cast<uint32_t*>(data + size_t(y) * pitch)[x] >> 24;
                    actualInk += alpha;
                    if (alpha != expectedPixels[size_t(y) * width + x]) placed = false;
                }
                check(placed, "glyph bearings and shared baseline match the font-wide outline transform");
                if (actualInk != expectedInk * 2) std::printf("GUI font: %dpx %c %.2f ink %llu expected %llu\n", size, character, factor, actualInk, expectedInk * 2);
                check(actualInk == expectedInk * 2, "small glyphs retain natural strokes and complete descenders on tight consecutive lines");
                rnative::vc10::FreeArray(data); ForgetTextBitmap(bitmap);
            }
            content.assign("NCU 0123456789 AV To Wa gjpqy", 29); put(text, 0x14C, content);
            alignas(16) uint8_t run[0x44]{};
            CreateTextBitmap(run, 320, metrics.tmHeight, factor);
            void* runCache = run; put(text, 0x248, runCache);
            IPoint runPen{2, 0}; renderString(text.bytes, &runPen, metrics.tmHeight, 0, int(content.size), fontInfo, 0xFFFFFF, 0, 0);
            uint8_t* runData; int runWidth, runHeight, runPitch;
            std::memcpy(&runData, run, 4); std::memcpy(&runWidth, run + 0xC, 4);
            std::memcpy(&runHeight, run + 8, 4); std::memcpy(&runPitch, run + 0x20, 4);
            std::vector<uint32_t> expectedRun(size_t(runWidth) * runHeight);
            int advance = 2;
            MAT2 matrix{}; matrix.eM22.value = 1;
            const int fixed = int(std::lround(float(nativeAdvance) * factor / scaledAdvance * 65536));
            matrix.eM11.value = SHORT(fixed >> 16); matrix.eM11.fract = WORD(fixed);
            for (char c : std::string(content.c_str(), content.size)) {
                GLYPHMETRICS gm{};
                const DWORD length = GetGlyphOutlineW(reference, WCHAR(c), GGO_GRAY8_BITMAP, &gm, 0, nullptr, &matrix);
                std::vector<uint8_t> coverage(length == GDI_ERROR ? 0 : length);
                if (length && length != GDI_ERROR) GetGlyphOutlineW(reference, WCHAR(c), GGO_GRAY8_BITMAP, &gm, length, coverage.data(), &matrix);
                const size_t stride = (gm.gmBlackBoxX + 3) & ~size_t(3);
                const int left = int(std::lround(advance * factor)) + gm.gmptGlyphOrigin.x;
                const int top = outlineMetrics.tmAscent - gm.gmptGlyphOrigin.y;
                for (DWORD y = 0; y < gm.gmBlackBoxY && length && length != GDI_ERROR; ++y) for (DWORD x = 0; x < gm.gmBlackBoxX; ++x) {
                    const uint32_t alpha = std::min(255u, unsigned(coverage[size_t(y) * stride + x]) * 255 / 64);
                    check(!alpha || (top + int(y) >= 0 && top + int(y) < runHeight && left + int(x) >= 0 && left + int(x) < runWidth),
                          "complete run fits the tight line cache without losing glyph coverage");
                    if (top + int(y) >= 0 && top + int(y) < runHeight && left + int(x) >= 0 && left + int(x) < runWidth) {
                        auto& dst = expectedRun[size_t(top + y) * runWidth + left + x];
                        dst = alpha + (dst * (255 - alpha) + 127) / 255;
                    }
                }
                SIZE extent{}; const WCHAR code = WCHAR(c);
                SelectObject(reference, original); GetTextExtentPoint32W(reference, &code, 1, &extent); SelectObject(reference, outline);
                advance += std::max(0, std::min(int(extent.cx), 32) - int(metrics.tmOverhang));
            }
            bool sameRun = runPen.x == advance;
            for (int y = 0; y < runHeight; ++y) for (int x = 0; x < runWidth; ++x)
                sameRun &= (reinterpret_cast<const uint32_t*>(runData + size_t(y) * runPitch)[x] >> 24) == expectedRun[size_t(y) * runWidth + x];
            check(sameRun, "whole-string spacing and baseline match the font-wide reference without changing native advances");
            rnative::vc10::FreeArray(runData); ForgetTextBitmap(run);
            SelectObject(reference, original); DeleteObject(outline);
        }
        SelectObject(reference, previous); DeleteDC(reference); fontDtor(fontInfo);
    }
    description.lfHeight = -16; description.lfWeight = FW_NORMAL; description.lfItalic = 0;
    // Bitmap-font letters use outlines; inline TrueType icons keep their source pixels.
    using AddBitmap = void(__thiscall*)(void*, int, void*);
    auto addBitmap = reinterpret_cast<AddBitmap>(symbol("?AddBitmap@FontInfo_t@@QAEXHPAVSpriteInfo_t@@@Z"));
    {
        std::memset(fontInfo, 0, sizeof(fontInfo)); fontCtor(fontInfo); rawPut(fontInfo, 0xFF0, 9);
        LOGFONTA logical{}; logical.lfHeight = 9; std::memcpy(logical.lfFaceName, "Verdana", 8);
        HFONT logicalFont = CreateFontIndirectA(&logical);
        HDC reference = CreateCompatibleDC(nullptr); HGDIOBJ previous = SelectObject(reference, logicalFont);
        const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        struct Source { alignas(16) uint8_t bytes[0x44]{}; };
        std::vector<Source> sources(alphabet.size());
        int nativeAdvance = 0;
        for (size_t i = 0; i < alphabet.size(); ++i) {
            const WCHAR c = WCHAR(alphabet[i]); SIZE size{}; GetTextExtentPoint32W(reference, &c, 1, &size);
            CreateTextBitmap(sources[i].bytes, std::max(1, int(size.cx)), 9, 1);
            addBitmap(fontInfo, alphabet[i], sources[i].bytes); nativeAdvance += std::max(1, int(size.cx));
        }
        for (float factor : {1.25f, 1.5f, 1.76f, 2.0f}) {
            LOGFONTA physical = logical; physical.lfHeight = int(std::floor(9 * factor)); physical.lfQuality = ANTIALIASED_QUALITY;
            HFONT outline = CreateFontIndirectA(&physical); SelectObject(reference, outline);
            TEXTMETRICA tm{}; GetTextMetricsA(reference, &tm);
            int physicalAdvance = 0;
            for (char c : alphabet) { const WCHAR code = WCHAR(c); SIZE size{}; GetTextExtentPoint32W(reference, &code, 1, &size); physicalAdvance += size.cx; }
            const int fixed = int(std::lround(float(nativeAdvance) * factor / physicalAdvance * 65536));
            MAT2 matrix{}; matrix.eM11.value = SHORT(fixed >> 16); matrix.eM11.fract = WORD(fixed); matrix.eM22.value = 1;
            for (char c : std::string("NCU01gjpqy")) {
                content.assign(&c, 1); put(text, 0x14C, content);
                alignas(16) uint8_t bitmap[0x44]{}; CreateTextBitmap(bitmap, 32, 9, factor);
                void* cache = bitmap; put(text, 0x248, cache);
                IPoint pen{2, 0}; renderString(text.bytes, &pen, 9, 0, 1, fontInfo, 0xFFFFFF, 0, 0);
                GLYPHMETRICS gm{};
                const DWORD length = GetGlyphOutlineW(reference, WCHAR(c), GGO_GRAY8_BITMAP, &gm, 0, nullptr, &matrix);
                std::vector<uint8_t> coverage(length == GDI_ERROR ? 0 : length);
                if (length && length != GDI_ERROR) GetGlyphOutlineW(reference, WCHAR(c), GGO_GRAY8_BITMAP, &gm, length, coverage.data(), &matrix);
                uint8_t* data; int width, height, pitch;
                std::memcpy(&data, bitmap, 4); std::memcpy(&width, bitmap + 0xC, 4);
                std::memcpy(&height, bitmap + 8, 4); std::memcpy(&pitch, bitmap + 0x20, 4);
                const int left = int(std::lround(2 * factor)) + gm.gmptGlyphOrigin.x, top = tm.tmAscent - gm.gmptGlyphOrigin.y;
                std::vector<uint32_t> expected(size_t(width) * height);
                const size_t stride = (gm.gmBlackBoxX + 3) & ~size_t(3);
                bool same = length != GDI_ERROR;
                for (DWORD y = 0; y < gm.gmBlackBoxY && length && length != GDI_ERROR; ++y) for (DWORD x = 0; x < gm.gmBlackBoxX; ++x) {
                    const uint32_t alpha = std::min(255u, unsigned(coverage[size_t(y) * stride + x]) * 255 / 64);
                    const bool fits = top + int(y) >= 0 && top + int(y) < height && left + int(x) >= 0 && left + int(x) < width;
                    same &= !alpha || fits;
                    if (fits) expected[size_t(top + y) * width + left + x] = alpha;
                }
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                    same &= (reinterpret_cast<uint32_t*>(data + size_t(y) * pitch)[x] >> 24) == expected[size_t(y) * width + x];
                check(same, "bitmap-font fallback retains a common baseline and font-wide pitch in tight HUD cells");
                rnative::vc10::FreeArray(data); ForgetTextBitmap(bitmap);
            }
            SelectObject(reference, logicalFont); DeleteObject(outline);
        }
        SelectObject(reference, previous); DeleteDC(reference); DeleteObject(logicalFont); fontDtor(fontInfo);
        for (auto& source : sources) { void* data; std::memcpy(&data, source.bytes, 4); rnative::vc10::FreeArray(data); ForgetTextBitmap(source.bytes); }
    }
    for (bool trueType : {false, true}) {
        std::memset(fontInfo, 0, sizeof(fontInfo)); fontCtor(fontInfo);
        rawPut(fontInfo, 0xFF0, 12); rawPut(fontInfo, 0xFFC, uint8_t(trueType));
        if (trueType) rawPut(fontInfo, 0x1018, CreateFontIndirectA(&description));
        alignas(16) uint8_t source[0x44]{}, bitmap[0x44]{};
        CreateTextBitmap(source, 8, 12, 1);
        uint8_t* sourceData; int sourcePitch;
        std::memcpy(&sourceData, source, 4); std::memcpy(&sourcePitch, source + 0x20, 4);
        for (int y = 0; y < 12; ++y) for (int x = 0; x < 8; ++x)
            reinterpret_cast<uint16_t*>(sourceData + y * sourcePitch)[x] = 0xFFFF;
        addBitmap(fontInfo, 'M', source);
        content.assign("M", 1); put(text, 0x14C, content);
        CreateTextBitmap(bitmap, 64, 24, 2);
        void* cache = bitmap; put(text, 0x248, cache);
        IPoint pen{2, 0}; renderString(text.bytes, &pen, 12, 0, 1, fontInfo, 0xFFFFFF, 0, 0);
        uint8_t* data; int width, height, pitch;
        std::memcpy(&data, bitmap, 4); std::memcpy(&width, bitmap + 0xC, 4);
        std::memcpy(&height, bitmap + 8, 4); std::memcpy(&pitch, bitmap + 0x20, 4);
        int visible = 0, antialias = 0;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const uint32_t alpha = reinterpret_cast<uint32_t*>(data + size_t(y) * pitch)[x] >> 24;
            if (alpha) ++visible;
            if (alpha && alpha < 255) ++antialias;
        }
        check(visible > 0, "bitmap-font and inline-icon fixtures draw");
        check(trueType ? antialias == 0 : antialias > 0, "bitmap letters rerasterize while inline icons retain their pixels");
        fontDtor(fontInfo);
        rnative::vc10::FreeArray(data); ForgetTextBitmap(bitmap);
        rnative::vc10::FreeArray(sourceData); ForgetTextBitmap(source);
    }
    {
        using PreRender = void(__thiscall*)(void*, void*, float, uint32_t);
        auto preRender = reinterpret_cast<PreRender>(symbol("?PreRender@TextRenderer_c@@UAEXPAVRViewPort_t@@MI@Z"));
        GuiCheckPatch allocatePatch(reinterpret_cast<void*>(symbol("?_AllocateBitmap@TextRenderer_c@@AAEXXZ")), reinterpret_cast<void*>(&GuiCheckAllocate));
        GuiCheckPatch renderPatch(reinterpret_cast<void*>(symbol("?Render@View@@UAEXPAVRViewPort_t@@MI@Z")), reinterpret_cast<void*>(&GuiCheckRender));
        pointer(textData, 0x10, root);
        SetScale(150, 150); BeginScaleFrame(controller.bytes);
        put(text, 0x240, static_cast<void*>(nullptr)); put(text, 0x248, static_cast<void*>(nullptr));
        put(text, 0x28C, 1.0f); put(text, 0x290, 1.0f);
        put(text, 0x288, uint8_t(0)); g_guiAllocations = 0;
        preRender(text.bytes, nullptr, 0, 0); preRender(text.bytes, nullptr, 0, 0);
        check(g_guiAllocations == 1, "empty text attempts a density allocation once rather than every frame");
        Object empty[5];
        std::memcpy(empty[0].bytes, text.bytes, sizeof(text.bytes));
        std::memcpy(empty[4].bytes, text.bytes, sizeof(text.bytes));
        g_guiAllocations = 0;
        for (int i = 0; i < 2; ++i) {
            preRender(empty[0].bytes, nullptr, 0, 0); preRender(empty[4].bytes, nullptr, 0, 0);
        }
        check(g_guiAllocations == 2, "empty text retry tracking handles colliding view addresses independently");
        for (auto [native, density] : {std::pair{1.5f, 1.5f}, std::pair{1.5f, 2.25f}, std::pair{0.5f, 1.5f}}) {
            put(text, 0x28C, native); put(text, 0x290, native);
            alignas(16) uint8_t bitmap[0x44]{};
            CreateTextBitmap(bitmap, 64, 24, density);
            void* cache = bitmap; put(text, 0x248, cache);
            put(text, 0x288, uint8_t(0)); g_guiAllocations = 0;
            preRender(text.bytes, nullptr, 0, 0);
            check(g_guiAllocations == (native > 1 && density == 1.5f ? 1 : 0), "text density includes native enlargement and keeps small text antialiased");
            void* data; std::memcpy(&data, bitmap, 4);
            rnative::vc10::FreeArray(data); ForgetTextBitmap(bitmap);
        }
    }
    content.release();
    *instance = saved;
    std::printf("GUI.dll: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
