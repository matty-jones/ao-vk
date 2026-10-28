// Native popup placement, global dismissal and mouse activation at independent UI scales.
#pragma once

inline int g_guiPopupCancelled, g_guiPopupActivated;
inline void __fastcall GuiPopupCancel(void*, void*, const void*) { ++g_guiPopupCancelled; }
inline void __fastcall GuiPopupActivate(void*, void*) { ++g_guiPopupActivated; }

inline int CheckGuiPopups(HMODULE module)
{
    using namespace rnative::gui;
    auto* bytes = reinterpret_cast<uint8_t*>(module);
    struct Object { alignas(16) uint8_t bytes[0x400]{}; };
    Object controller, top, topData, root, rootData, tab, tabData, window, windowData, menu, menuData, item;
    auto put = [](Object& o, size_t offset, const auto& value) { std::memcpy(o.bytes + offset, &value, sizeof(value)); };
    auto link = [&](Object& o, size_t offset, Object& target) { void* p = target.bytes; put(o, offset, p); };
    auto view = [&](Object& o, Object& data, Object& parent, Rect frame) {
        link(o, 0xAC, data); link(o, 0x40, window); link(data, 0x10, parent);
        put(data, 0x20, frame); put(data, 0x100, Point{1, 1});
    };
    rnative::vc10::String name; name.init();
    void* text = &name; put(window, 0x60, text);
    link(window, 0x70, windowData); link(windowData, 0x80, root);
    link(top, 0xAC, topData); put(topData, 0x100, Point{1, 1});
    view(root, rootData, top, {0, 0, 99, 59});
    view(tab, tabData, root, {0, 0, 99, 59});
    view(menu, menuData, tab, {0, 0, 99, 59});
    link(topData, 0, root); link(root, 0x1B8, tab); link(rootData, 0, tab);
    link(tab, 0x1B8, menu); link(tabData, 0, menu);
    link(menu, 0x158, window);
    link(controller, 0x94, top); put(controller, 0x80, Point{3840, 2160});
    void* rootTable[66]{}; void** table = rootTable + 1;
    table[0xC8 / 4] = reinterpret_cast<void*>(&GuiCheckMovePosition);
    table[0xF0 / 4] = reinterpret_cast<void*>(&GuiCheckMoveFrame);
    put(root, 0, table);
    void* menuTable[66]{}; void** callbacks = menuTable + 1;
    callbacks[0xAC / 4] = reinterpret_cast<void*>(&GuiCheckCaptureMouse);
    put(menu, 0, callbacks);
    void* items[]{item.bytes};
    put(menu, 0x174, static_cast<void*>(items)); put(menu, 0x178, static_cast<void*>(items + 1));
    put(menu, 0x184, int(0));
    auto** instance = reinterpret_cast<void**>(GetProcAddress(module, "?s_pcInstance@WindowController_c@@0PAV1@A"));
    void* saved = *instance; *instance = controller.bytes;
    GuiCheckPatch cancelled(bytes + 0x13C6A1, reinterpret_cast<void*>(&GuiPopupCancel));
    GuiCheckPatch activated(bytes + 0x13C768, reinterpret_cast<void*>(&GuiPopupActivate));
    using Go = void(__thiscall*)(void*, void*, const Point*, const rnative::vc10::String*);
    using Move = void(__thiscall*)(void*, const Point*);
    using Convert = Point*(__thiscall*)(void*, Point*, const Point*, uint32_t);
    using Down = void(__thiscall*)(void*, const Point*, int, int);
    using Up = void(__thiscall*)(void*, const Point*, int, void*);
    auto go = reinterpret_cast<Go>(bytes + 0x13BF93);
    auto reposition = reinterpret_cast<Move>(bytes + 0x13B61E);
    auto globalDown = reinterpret_cast<Move>(bytes + 0x13C955);
    auto to = reinterpret_cast<Convert>(GetProcAddress(module, "?ConvertToScreen@View@@QBE?AVPoint@@ABV2@_N@Z"));
    auto from = reinterpret_cast<Convert>(GetProcAddress(module, "?ConvertFromScreen@View@@QBE?AVPoint@@ABV2@_N@Z"));
    auto down = reinterpret_cast<Down>(bytes + 0x13CFD2);
    auto up = reinterpret_cast<Up>(bytes + 0x13C88F);
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("GUI popup: %s\n", what); } };
    for (float percent : {150.0f, 200.0f, 75.0f, 100.0f, 176.0f, 150.0f}) {
        SetScale(percent, 200); BeginScaleFrame(controller.bytes);
        for (Point anchor : {Point{900, 600}, Point{2100, 1200}}) {
            go(menu.bytes, window.bytes, &anchor, &name);
            BeginScaleFrame(controller.bytes);
            Point zero{}, actual;
            to(menu.bytes, &actual, &zero, 1);
            check(std::fabs(actual.x - anchor.x) < 0.001f && std::fabs(actual.y - anchor.y) < 0.001f,
                  "popup opens at the physical cursor without multiplying its anchor");
            const Point row{80, 50}; Point hit, local;
            to(menu.bytes, &hit, &row, 1); from(menu.bytes, &local, &hit, 1);
            g_guiPopupCancelled = g_guiPopupActivated = 0;
            globalDown(menu.bytes, &hit);
            check(g_guiPopupCancelled == 0, "global click checking accepts the displayed popup frame");
            down(menu.bytes, &local, 1, 1); up(menu.bytes, &local, 1, nullptr);
            check(g_guiPopupActivated == 1, "native mouse press and release activate the highlighted item action");
            const Point outside{hit.x + 500, hit.y + 500};
            globalDown(menu.bytes, &outside);
            check(g_guiPopupCancelled == 1, "clicking outside still dismisses the popup");
        }
        const Point edge{3830, 2150}; go(menu.bytes, window.bytes, &edge, &name);
        const Point corner{99, 59}; Point clamped;
        to(menu.bytes, &clamped, &corner, 1);
        check(clamped.x <= 3840 && clamped.y <= 2160, "native popup clamping respects screen limits");
        const Point moved{1200, 300}; reposition(menu.bytes, &moved);
        Point zero{}, actual; to(menu.bytes, &actual, &zero, 1);
        check(std::fabs(actual.x - moved.x) < 0.001f && std::fabs(actual.y - moved.y) < 0.001f,
              "repositioned popup menus retain physical anchors");
    }
    put(window, 0, static_cast<const void*>(bytes + 0x1AA07C));
    using Centre = void(__thiscall*)(void*);
    reinterpret_cast<Centre>(bytes + 0x154986)(window.bytes);
    BeginScaleFrame(controller.bytes);
    const Point middle{49.5f, 29.5f}; Point centred;
    to(menu.bytes, &centred, &middle, 1);
    check(std::fabs(centred.x - 1920) <= 1 && std::fabs(centred.y - 1080) <= 1,
          "native login progress centering stays in physical screen coordinates");
    put(window, 0, static_cast<void*>(nullptr));
    SetScale(100, 100); BeginScaleFrame(controller.bytes);
    *instance = saved;
    std::printf("GUI popup: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures;
}
