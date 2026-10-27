// Exercise the checked GUI.dll hooks on synthetic objects in the supported client layout.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native/gui_scale.h"
#include "native/gui.h"
#include "native/vc10.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace rnative::gui {
void InstallScaling(HMODULE gui);
bool BeginScaleFrame(void* controller);
void ScaleDrawing(bool drawing);
void EndScaleFrame();
}

inline int __fastcall CaptureGuiWindowRect(void* message, void*, const char*, const rnative::gui::Rect* frame)
{
    std::memcpy(message, frame, sizeof(*frame));
    return 0;
}

inline int CheckGuiClient(const char* client)
{
    using namespace rnative::gui;
    SetDllDirectoryA(client);
    const std::string path = std::string(client) + "/GUI.dll";
    HMODULE module = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { std::printf("GUI.dll: load failed (%lu)\n", GetLastError()); return 1; }
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
    InstallScaling(module);
    SetScale(200, 150);
    Point pending;
    screen(&pending);
    check(equal(pending, {3840, 2160}), "scale change waits for the next GUI frame");
    BeginScaleFrame(controller.bytes);
    ScaleDrawing(true);
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
    SetScale(100, 100); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
    const Rect clip100{150, 60, 199, 109};
    vertices(area.bytes, child.bytes, &clip100);
    float reference[8], positions[8];
    for (int i = 0; i < 4; ++i) for (int c = 0; c < 2; ++c) {
        reference[i * 2 + c] = uv(i, c);
        std::memcpy(&positions[i * 2 + c], area.bytes + i * 28 + c * 4, 4);
    }
    for (float factor : {0.5f, 0.75f, 1.25f, 1.5f, 2.0f, 1.0f}) {
        SetScale(factor * 100, 100); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
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
        check(sameUv, "clipped atlas sampling must be invariant under scaling");
        check(correctPositions, "clipped vertices scale with their half-pixel convention intact");
    }
    // Slider changes leave the native frames used by preferences untouched.
    const Rect edge{3500, 1900, 3799, 2099};
    put(rootData, 0x20, edge);
    for (float percent : {100.0f, 200.0f, 75.0f, 150.0f, 100.0f}) {
        SetScale(percent, 100); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
        using WindowFrame = Rect*(__thiscall*)(void*, Rect*, uint32_t);
        auto windowFrame = reinterpret_cast<WindowFrame>(symbol("?GetFrame@Window@@QBE?AVRect@@_N@Z"));
        Rect actual;
        ScaleDrawing(false);
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
        ScaleDrawing(true);
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
    SetScale(100, 200); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
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
    SetScale(100, 100); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
    to(hudChild.bytes, &output, &input, 1);
    check(equal(output, {3703, 2004}), "HUD scale round trip restores original coordinates");
    Object icon, iconData;
    view(icon, iconData, hudOwner, {3700, 0, 3799, 79});
    pointer(iconData, 0x10, hudChild); pointer(hudChildData, 0, icon);
    put(hudChildData, 0x20, Rect{0, 2000, 3839, 2099});
    SetScale(100, 200); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
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
    SetScale(100, 100); BeginScaleFrame(controller.bytes); ScaleDrawing(true);
    to(icon.bytes, &output, &input, 1);
    check(equal(output, {3703, 2004}), "nested HUD layout round trip restores original positions");
    EndScaleFrame(); *instance = saved;
    std::printf("GUI.dll: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
