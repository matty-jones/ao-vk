// Saved window geometry across fresh client layouts at different resolutions.
#pragma once
#include <unordered_map>

struct GuiResolutionMessage {
    std::unordered_map<std::string, rnative::gui::Rect> rects;
    std::unordered_map<std::string, rnative::gui::Point> points;
    std::unordered_map<std::string, float> floats;
};
inline rnative::gui::Point g_guiResolutionMinimum{};
inline void __fastcall GuiResolutionFrame(void* view, void*, const rnative::gui::Rect* requested)
{
    auto frame = *requested;
    frame.right = std::max(frame.right, frame.left + g_guiResolutionMinimum.x - 1);
    frame.bottom = std::max(frame.bottom, frame.top + g_guiResolutionMinimum.y - 1);
    GuiCheckMoveFrame(view, nullptr, &frame);
}
inline bool __fastcall GuiResolutionPinned(void*, void*) { return true; }
inline int __fastcall GuiResolutionFindRect(GuiResolutionMessage* message, void*, const char* key,
                                           rnative::gui::Rect* out, int)
{
    const auto found = message->rects.find(key);
    if (found == message->rects.end()) return -1;
    *out = found->second;
    return 0;
}
inline int __fastcall GuiResolutionAddRect(GuiResolutionMessage* message, void*, const char* key,
                                          const rnative::gui::Rect* value)
{
    message->rects[key] = *value;
    return 0;
}
inline int __fastcall GuiResolutionFindPoint(GuiResolutionMessage* message, void*, const char* key,
                                             rnative::gui::Point* out, int)
{
    const auto found = message->points.find(key);
    if (found == message->points.end()) return -1;
    *out = found->second; return 0;
}
inline int __fastcall GuiResolutionAddPoint(GuiResolutionMessage* message, void*, const char* key,
                                            const rnative::gui::Point* value)
{
    message->points[key] = *value; return 0;
}
inline int __fastcall GuiResolutionFindFloat(GuiResolutionMessage* message, void*, const char* key, float* out, int)
{
    const auto found = message->floats.find(key);
    if (found == message->floats.end()) return -1;
    *out = found->second; return 0;
}
inline int __fastcall GuiResolutionAddFloat(GuiResolutionMessage* message, void*, const char* key, float value)
{
    message->floats[key] = value; return 0;
}

inline int CheckGuiResolution(HMODULE module, void* controller)
{
    using namespace rnative::gui;
    auto symbol = [&](const char* name) { return GetProcAddress(module, name); };
    auto* bytes = reinterpret_cast<uint8_t*>(module);
    GuiCheckImport find(reinterpret_cast<void**>(bytes + 0x1A8004), reinterpret_cast<void*>(&GuiResolutionFindRect));
    GuiCheckImport add(reinterpret_cast<void**>(bytes + 0x1A800C), reinterpret_cast<void*>(&GuiResolutionAddRect));
    GuiCheckImport findPoint(reinterpret_cast<void**>(bytes + 0x1A8104), reinterpret_cast<void*>(&GuiResolutionFindPoint));
    GuiCheckImport addPoint(reinterpret_cast<void**>(bytes + 0x1A810C), reinterpret_cast<void*>(&GuiResolutionAddPoint));
    GuiCheckImport findFloat(reinterpret_cast<void**>(bytes + 0x1A8364), reinterpret_cast<void*>(&GuiResolutionFindFloat));
    GuiCheckImport addFloat(reinterpret_cast<void**>(bytes + 0x1A8360), reinterpret_cast<void*>(&GuiResolutionAddFloat));
    using Config = void(__thiscall*)(void*, void*);
    using Bounds = Rect*(__thiscall*)(void*, Rect*);
    using ToScreen = Rect*(__thiscall*)(void*, Rect*, const Rect*, uint32_t);
    auto load = reinterpret_cast<Config>(symbol("?LoadWndConfig@Window@@QAEXABVMessage@@@Z"));
    auto save = reinterpret_cast<Config>(symbol("?SaveWndConfig@Window@@QAEXPAVMessage@@@Z"));
    using Inside = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
    auto inside = reinterpret_cast<Inside>(symbol("?MoveInsideScreen@Window@@QAEX_N00@Z"));
    auto bounds = reinterpret_cast<Bounds>(symbol("?GetBounds@View@@QBE?AVRect@@XZ"));
    auto to = reinterpret_cast<ToScreen>(symbol("?ConvertToScreen@View@@QBE?AVRect@@ABV2@_N@Z"));
    auto field = [](void* object, size_t offset) { return static_cast<uint8_t*>(object) + offset; };
    auto put = [&](void* object, size_t offset, const auto& value) { std::memcpy(field(object, offset), &value, sizeof(value)); };
    auto pointer = [&](void* object, size_t offset) { void* value; std::memcpy(&value, field(object, offset), 4); return value; };
    void* top = pointer(controller, 0x94);
    void* topData = pointer(top, 0xAC);
    void* previous = pointer(topData, 0);
    Point previousScreen; std::memcpy(&previousScreen, field(controller, 0x80), sizeof(previousScreen));
    alignas(16) uint8_t owner[0x400]{}, ownerData[0x400]{}, root[0x400]{}, rootData[0x400]{};
    void* table[65]{};
    table[1 + 0xAC / 4] = reinterpret_cast<void*>(&GuiCheckCaptureMouse);
    table[1 + 0xB8 / 4] = reinterpret_cast<void*>(&GuiCheckSetFrame);
    table[1 + 0xC8 / 4] = reinterpret_cast<void*>(&GuiCheckMovePosition);
    table[1 + 0xF0 / 4] = reinterpret_cast<void*>(&GuiResolutionFrame);
    rnative::vc10::String name; name.init(); std::memcpy(name.buffer, "roundtrip", 10); name.size = 9;
    put(owner, 0, static_cast<void*>(bytes + 0x1C8644)); put(owner, 0x60, static_cast<void*>(&name));
    put(owner, 0x70, static_cast<void*>(ownerData)); put(ownerData, 0x80, static_cast<void*>(root));
    put(ownerData, 0x3C, uint32_t(0x800)); put(root, 0, static_cast<void*>(table + 1));
    put(root, 0x40, static_cast<void*>(owner)); put(root, 0xAC, static_cast<void*>(rootData));
    put(rootData, 0x10, top); put(rootData, 0x100, Point{1, 1});
    auto open = [&](Point screen, float percent, GuiResolutionMessage& message) {
        put(topData, 0, static_cast<void*>(nullptr)); put(controller, 0x80, screen);
        SetUiScale(percent); BeginScaleFrame(controller);
        put(rootData, 0x20, Rect{});
        load(owner, &message);
        put(topData, 0, static_cast<void*>(root)); BeginScaleFrame(controller);
        Rect local, displayed; bounds(root, &local); to(root, &displayed, &local, 1);
        return displayed;
    };
    int failures = 0;
    struct Case { Rect initial; size_t type; };
    auto equal = [](Rect a, Rect b) {
        return std::fabs(a.left - b.left) <= 1 && std::fabs(a.top - b.top) <= 1 &&
               std::fabs(a.right - b.right) <= 1 && std::fabs(a.bottom - b.bottom) <= 1;
    };
    auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("GUI resolution: %s\n", what); } };
    for (Case test : {Case{{100, 80, 399, 279}, 0x1C8644}, {{1500, 900, 1899, 1199}, 0x1C8644},
                      {{3500, 1900, 3799, 2099}, 0x1C8644}, {{2400, 20, 2499, 39}, 0x1B4664}}) {
        put(owner, 0, static_cast<void*>(bytes + test.type));
        for (bool reverse : {false, true}) {
            Rect initial = test.initial;
            if (reverse) Scale{0.5f}.ToScreen(initial);
            const Point startScreen = reverse ? Point{1920, 1080} : Point{3840, 2160};
            const Point otherScreen = reverse ? Point{3840, 2160} : Point{1920, 1080};
            const float startPercent = reverse ? 75 : 150, otherPercent = reverse ? 150 : 75;
            GuiResolutionMessage saved; saved.rects["WindowFrame"] = initial;
            const Rect first = open(startScreen, startPercent, saved);
            save(owner, &saved);
            bool stable = true;
            for (int cycle = 0; cycle < 20; ++cycle) {
                const Rect other = open(otherScreen, otherPercent, saved);
                Rect expected = first; Scale{reverse ? 2.0f : 0.5f}.ToScreen(expected);
                stable = stable && equal(other, expected);
                Rect before, after;
                std::memcpy(&before, rootData + 0x20, sizeof(before));
                inside(owner, 0, 1, 1);
                std::memcpy(&after, rootData + 0x20, sizeof(after));
                stable = stable && equal(before, after);
                save(owner, &saved);
                const Rect returned = open(startScreen, startPercent, saved);
                stable = stable && equal(returned, first);
                save(owner, &saved);
            }
            if (!stable) { ++failures; std::printf("GUI resolution: saved window/HUD bounds drift across 20 restart cycles\n"); }
        }
    }
    put(owner, 0, static_cast<void*>(bytes + 0x1C8644));
    GuiResolutionMessage saved; saved.rects["WindowFrame"] = {100, 80, 399, 279};
    const Rect reference = open({3840, 2160}, 150, saved);
    save(owner, &saved);
    open({1920, 1080}, 75, saved);
    Rect automatic; std::memcpy(&automatic, rootData + 0x20, sizeof(automatic));
    automatic.left += 20; automatic.right += 100;
    GuiCheckMoveFrame(root, nullptr, &automatic);
    save(owner, &saved);
    check(equal(open({3840, 2160}, 150, saved), reference), "automatic layout changes cannot overwrite the reference");
    GuiResolutionMessage minimum;
    minimum.rects["WindowFrame"] = {3500, 1900, 3799, 2099};
    const Rect minimumReference = open({3840, 2160}, 150, minimum);
    save(owner, &minimum);
    for (Point mode : {Point{1920, 1080}, Point{3840, 2160}}) {
        g_guiResolutionMinimum = {800, 600};
        const Rect constrained = open(mode, mode.x == 1920 ? 75 : 100, minimum);
        check(constrained.left >= 0 && constrained.top >= 0 && constrained.right < mode.x && constrained.bottom < mode.y,
              "minimum-size windows remain visible, including 100% UI");
        save(owner, &minimum);
        g_guiResolutionMinimum = {};
        check(equal(open({3840, 2160}, 150, minimum), minimumReference), "native minimum sizes cannot overwrite the reference");
        save(owner, &minimum);
    }
    GuiResolutionMessage malformed;
    malformed.rects["WindowFrame"] = {100, 80, 399, 279};
    malformed.rects["RVK_Layout"] = {0, 0, 0.2f, 0.2f};
    malformed.points["RVK_LayoutScreen"] = {3840, 2160};
    malformed.points["RVK_LayoutAnchor"] = {0.1f, 0};
    malformed.floats["RVK_LayoutScale"] = 1.5f;
    const Rect fallback = open({3840, 2160}, 150, malformed);
    malformed = {}; malformed.rects["WindowFrame"] = {100, 80, 399, 279};
    check(equal(open({3840, 2160}, 150, malformed), fallback), "invalid metadata retains the native preference fallback");
    GuiCheckPatch pointerPatch(reinterpret_cast<void*>(symbol("?UpdateMousePointer@WndBorder@@AAEXW4hit_item@1@@Z")),
                               reinterpret_cast<void*>(&GuiCheckPointer));
    using Down = void(__thiscall*)(void*, const Point*, int, int);
    using Up = void(__thiscall*)(void*, const Point*, int, void*);
    auto down = reinterpret_cast<Down>(symbol("?MouseDown@WndBorder@@UAEXABVPoint@@HH@Z"));
    auto up = reinterpret_cast<Up>(symbol("?MouseUp@WndBorder@@UAEXABVPoint@@HPAVMessage@@@Z"));
    put(ownerData, 0x3C, uint32_t(0x830)); put(root, 0x23C, Rect{0, 0, 299, 15});
    for (Point mode : {Point{1920, 1080}, Point{3840, 2160}}) {
        const float percent = mode.x == 1920 ? 75 : 100;
        open(mode, percent, saved);
        Point grip{30, 5}; down(root, &grip, 1, 1);
        Rect moved; std::memcpy(&moved, rootData + 0x20, sizeof(moved));
        moved.left += 30; moved.right += 50; moved.top += 20; moved.bottom += 30;
        GuiCheckMoveFrame(root, nullptr, &moved); up(root, &grip, 1, nullptr);
        Rect local, edited; bounds(root, &local); to(root, &edited, &local, 1);
        save(owner, &saved);
        check(equal(open(mode, percent, saved), edited), "explicit moves and resizes survive reload, including 100% UI");
        const Point otherScreen = mode.x == 1920 ? Point{3840, 2160} : Point{1920, 1080};
        open(otherScreen, mode.x == 1920 ? 150 : 75, saved); save(owner, &saved);
        check(equal(open(mode, percent, saved), edited), "edited layouts survive a resolution round trip");
    }
    put(rootData, 0x10, static_cast<void*>(nullptr));
    GuiResolutionMessage detached; save(owner, &detached);
    check(detached.rects.count("RVK_Layout") == 0, "container-owned panels do not get independent screen placement");
    put(rootData, 0x10, top);
    {
        GuiCheckPatch pinned(reinterpret_cast<void*>(symbol("?GetPinButtonState@WndBorder@@QBE_NXZ")),
                              reinterpret_cast<void*>(&GuiResolutionPinned));
        GuiResolutionMessage panel; save(owner, &panel);
        check(panel.rects.count("RVK_Layout") == 0, "pinned panels retain their rollup's geometry");
    }
    saved = {};
    saved.rects["WindowFrame"] = {400, 100, 699, 299};
    const Rect scaleReference = open({3840, 2160}, 150, saved);
    save(owner, &saved);
    for (int cycle = 0; cycle < 20; ++cycle) {
        for (float percent : {200.0f, 75.0f, 150.0f}) {
            const Rect shown = open({3840, 2160}, percent, saved);
            if (percent == 150) check(equal(shown, scaleReference), "same-display scale reloads retain their anchor");
            save(owner, &saved);
        }
    }
    GuiResolutionMessage empty;
    load(owner, &empty);
    put(topData, 0, previous); put(controller, 0x80, previousScreen);
    SetScale(100, 100); BeginScaleFrame(controller);
    std::printf("GUI resolution: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures;
}
