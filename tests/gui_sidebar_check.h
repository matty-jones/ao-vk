// Sidebar grouping and custom map drawing through the supported client's GUI.dll.
#pragma once

struct GuiMapDraw {
    uint32_t fvf, count, flags;
    std::vector<uint32_t> vertices;
};
inline std::vector<GuiMapDraw> g_guiMapDraws;
inline void __fastcall GuiMapCapture(void*, void*, uint32_t fvf, void* vertices, uint32_t count, uint32_t flags)
{
    const size_t words = size_t(count) * (fvf == 0x144 ? 7 : 5);
    const auto* data = static_cast<const uint32_t*>(vertices);
    g_guiMapDraws.push_back({fvf, count, flags, {data, data + words}});
}
inline bool __fastcall GuiMapTexture(void*, void*, void*, uint32_t, uint32_t) { return true; }
inline void __fastcall GuiMapStates(void*, void*) {}
using GuiMapTile = void(__thiscall*)(void*, void*, void*, const rnative::gui::Rect*, const rnative::gui::Rect*, uint32_t);
using GuiMapLine = void(__thiscall*)(void*, void*, const rnative::gui::Rect*, uint32_t);
using GuiMapConvert = rnative::gui::Point*(__thiscall*)(void*, rnative::gui::Point*, const rnative::gui::Point*, uint32_t);
using GuiMapPrimitive = void(__thiscall*)(void*, uint32_t, void*, uint32_t, uint32_t);
inline GuiMapTile g_guiMapTile;
inline GuiMapLine g_guiMapLine;
inline GuiMapConvert g_guiMapTo, g_guiMapFrom;
inline GuiMapPrimitive g_guiMapStrip;
inline rnative::gui::Point g_guiMapMouse, g_guiMapLocal, g_guiMapOrigin;
inline bool g_guiMapSourceIntact;

inline void __fastcall GuiMapTiles(void* view, void*, void* viewport)
{
    using namespace rnative::gui;
    const Point zero{};
    g_guiMapTo(view, &g_guiMapOrigin, &zero, 1);
    g_guiMapFrom(view, &g_guiMapLocal, &g_guiMapMouse, 1);
    const Rect uv{0.125f, 0.25f, 0.75f, 1.0f}, tile{10, 20, 99, 149};
    g_guiMapTile(view, viewport, nullptr, &uv, &tile, 0xFF407080);
    const Rect line{g_guiMapOrigin.x + 10, g_guiMapOrigin.y + 20, g_guiMapOrigin.x + 99, g_guiMapOrigin.y + 149};
    g_guiMapLine(view, viewport, &line, 0xFF806040);
    struct Vertex { float x, y, z, rhw; uint32_t colour; float u, v; };
    Vertex strip[4]{};
    for (int i = 0; i < 4; ++i)
        strip[i] = {g_guiMapOrigin.x + i * 10.0f, g_guiMapOrigin.y + i * 20.0f,
                    0.01f, 100, 0xFF408060, i * 0.125f, i * 0.25f};
    Vertex original[4]; std::memcpy(original, strip, sizeof(strip));
    g_guiMapStrip(nullptr, 0x144, strip, 4, 3);
    g_guiMapSourceIntact = std::memcmp(original, strip, sizeof(strip)) == 0;
}

inline int CheckGuiSidebar(HMODULE module)
{
    using namespace rnative::gui;
    auto* bytes = reinterpret_cast<uint8_t*>(module);
    struct Object { alignas(16) uint8_t bytes[0x800]{}; };
    Object controller, top, topData, root, rootData, owner, ownerData;
    Object dock, dockData, first, firstData, second, secondData, third, thirdData, map, mapData, terrain;
    auto put = [](Object& o, size_t offset, const auto& value) { std::memcpy(o.bytes + offset, &value, sizeof(value)); };
    auto link = [&](Object& o, size_t offset, Object& target) { void* p = target.bytes; put(o, offset, p); };
    auto view = [&](Object& object, Object& data, Object& parent, Rect frame) {
        link(object, 0xAC, data); link(object, 0x40, owner); link(data, 0x10, parent);
        put(data, 0x20, frame); put(data, 0x100, Point{1, 1});
    };
    rnative::vc10::String name; name.init();
    void* text = &name; put(owner, 0x60, text);
    put(owner, 0, static_cast<void*>(bytes + 0x1B549C));
    link(owner, 0x70, ownerData); link(ownerData, 0x80, root);
    link(top, 0xAC, topData); put(topData, 0x100, Point{1, 1});
    view(root, rootData, top, {0, 0, 3839, 2159});
    view(dock, dockData, root, {3640, 0, 3839, 2159});
    put(dock, 0, static_cast<void*>(bytes + 0x1B0DD4));
    view(first, firstData, dock, {0, 0, 199, 299});
    view(second, secondData, dock, {0, 300, 199, 599});
    view(third, thirdData, dock, {0, 600, 199, 899});
    view(map, mapData, first, {10, 30, 189, 249});
    link(topData, 0, root); link(rootData, 0, dock); link(root, 0x1B8, dock);
    link(dockData, 0, first); link(dock, 0x1B8, first);
    link(firstData, 8, second); link(secondData, 8, third);
    link(firstData, 0, map); link(first, 0x1B8, map);
    link(controller, 0x94, top); put(controller, 0x80, Point{3840, 2160});
    auto** instance = reinterpret_cast<void**>(GetProcAddress(module, "?s_pcInstance@WindowController_c@@0PAV1@A"));
    void* saved = *instance; *instance = controller.bytes;
    using RectConvert = Rect*(__thiscall*)(void*, Rect*, const Rect*, uint32_t);
    const auto toRect = reinterpret_cast<RectConvert>(GetProcAddress(module, "?ConvertToScreen@View@@QBE?AVRect@@ABV2@_N@Z"));
    g_guiMapTo = reinterpret_cast<GuiMapConvert>(GetProcAddress(module, "?ConvertToScreen@View@@QBE?AVPoint@@ABV2@_N@Z"));
    g_guiMapFrom = reinterpret_cast<GuiMapConvert>(GetProcAddress(module, "?ConvertFromScreen@View@@QBE?AVPoint@@ABV2@_N@Z"));
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("GUI sidebar: %s\n", what); } };
    const Rect bounds{0, 0, 199, 299};
    for (float percent : {150.0f, 200.0f, 75.0f, 176.0f, 100.0f}) {
        SetScale(125, percent); BeginScaleFrame(controller.bytes);
        Rect a, b, c;
        toRect(first.bytes, &a, &bounds, 1); toRect(second.bytes, &b, &bounds, 1); toRect(third.bytes, &c, &bounds, 1);
        check(std::fabs(b.top - a.bottom - 1) < 0.001f && std::fabs(c.top - b.bottom - 1) < 0.001f,
              "tall rollup panels share one transform without overlap or gaps");
        check(a.right == 3839 && b.right == a.right && c.right == a.right,
              "the entire rollup stays aligned to the right screen edge");
        check(a.top == 0, "a full-height rollup keeps its top anchor when shrinking");
        put(thirdData, 0x20, Rect{0, 300, 199, 599}); link(firstData, 8, third);
        BeginScaleFrame(controller.bytes);
        toRect(third.bytes, &c, &bounds, 1);
        check(std::fabs(c.top - a.bottom - 1) < 0.001f, "closing a panel retains native stacking at the scaled height");
        link(firstData, 8, second); put(thirdData, 0x20, Rect{0, 600, 199, 899});
        put(firstData, 0x20, Rect{0, 0, 199, 399});
        put(secondData, 0x20, Rect{0, 400, 199, 699}); put(thirdData, 0x20, Rect{0, 700, 199, 999});
        BeginScaleFrame(controller.bytes);
        const Rect expanded{0, 0, 199, 399};
        toRect(first.bytes, &a, &expanded, 1); toRect(second.bytes, &b, &bounds, 1);
        check(std::fabs(b.top - a.bottom - 1) < 0.001f, "expanding or resizing a panel moves following panels together");
        put(firstData, 0x20, bounds); put(secondData, 0x20, Rect{0, 300, 199, 599});
        put(thirdData, 0x20, Rect{0, 600, 199, 899});
        Point input{17, 21}, hit, local;
        g_guiMapTo(third.bytes, &hit, &input, 1); g_guiMapFrom(third.bytes, &local, &hit, 1);
        check(std::fabs(local.x - input.x) < 0.001f && std::fabs(local.y - input.y) < 0.001f,
              "panel input inverts the shared transform after toggling");
        using Parent = Point*(__thiscall*)(void*, Point*, const Point*);
        const auto parent = reinterpret_cast<Parent>(GetProcAddress(module, "?ConvertFromParent@View@@QBE?AVPoint@@ABV2@@Z"));
        Point dockHit, panelHit;
        parent(dock.bytes, &dockHit, &hit); parent(third.bytes, &panelHit, &dockHit);
        check(std::fabs(panelHit.x - input.x) < 0.001f && std::fabs(panelHit.y - input.y) < 0.001f,
              "native input dispatch crosses the rollup boundary exactly once");
        Rect model; std::memcpy(&model, dockData.bytes + 0x20, sizeof(model));
        check(model.left == 3640 && model.bottom == 2159, "sidebar scaling leaves its saved native frame untouched");
    }
    std::printf("GUI.dll: checking custom PF map drawing\n");
    g_guiMapTile = reinterpret_cast<GuiMapTile>(bytes + 0x46871);
    g_guiMapLine = reinterpret_cast<GuiMapLine>(bytes + 0x465B4);
    g_guiMapStrip = *reinterpret_cast<GuiMapPrimitive*>(bytes + 0x1A8680);
    HMODULE renderer = GetModuleHandleA("randy31.dll");
    GuiCheckPatch fan(reinterpret_cast<void*>(GetProcAddress(renderer, "?RenderTriangleFan@render_t@@QAEXKPAXKK@Z")),
                      reinterpret_cast<void*>(&GuiMapCapture));
    GuiCheckPatch lines(reinterpret_cast<void*>(GetProcAddress(renderer, "?RenderLineList@render_t@@QAEXKPAXKK@Z")),
                        reinterpret_cast<void*>(&GuiMapCapture));
    GuiCheckPatch strip(reinterpret_cast<void*>(GetProcAddress(renderer, "?RenderTriangleStrip@render_t@@QAEXKPAXKK@Z")),
                        reinterpret_cast<void*>(&GuiMapCapture));
    GuiCheckImport states(reinterpret_cast<void**>(bytes + 0x1A8620), reinterpret_cast<void*>(&GuiMapStates));
    GuiCheckImport texture(reinterpret_cast<void**>(bytes + 0x1A8648), reinterpret_cast<void*>(&GuiMapTexture));
    void* dummy = nullptr;
    GuiCheckImport renderInstance(reinterpret_cast<void**>(bytes + 0x1A8640), &dummy);
    GuiCheckPatch tiles(bytes + 0x472D5, reinterpret_cast<void*>(&GuiMapTiles));
    using Render = void(__thiscall*)(void*, void*, float, uint32_t);
    auto groundRender = reinterpret_cast<Render>(bytes + 0x478ED);
    auto mapRender = reinterpret_cast<Render>(bytes + 0xE9F73);
    link(map, 0x134, terrain); put(map, 0x130, uint8_t(1)); put(terrain, 8, uint32_t(1));
    Object viewport;
    std::vector<GuiMapDraw> reference;
    for (float percent : {100.0f, 150.0f, 200.0f, 75.0f, 176.0f, 100.0f}) {
        SetScale(125, percent); BeginScaleFrame(controller.bytes);
        const Point zero{}, input{17, 21}; Point origin;
        g_guiMapTo(map.bytes, &origin, &zero, 1); g_guiMapTo(map.bytes, &g_guiMapMouse, &input, 1);
        g_guiMapDraws.clear(); groundRender(map.bytes, viewport.bytes, 0, 0);
        check(g_guiMapDraws.size() == 3, "native map submits its tile, outline and marker strip");
        check(g_guiMapSourceIntact, "custom drawing never modifies the game's vertex arrays");
        check(g_guiMapOrigin == Point{3650, 30}, "map tile arithmetic retains the native screen origin");
        check(std::fabs(g_guiMapLocal.x - input.x) < 0.001f && std::fabs(g_guiMapLocal.y - input.y) < 0.001f,
              "map input remains inverse-scaled during custom drawing");
        if (reference.empty()) reference = g_guiMapDraws;
        const float factor = percent / 100;
        for (size_t d = 0; d < g_guiMapDraws.size() && d < reference.size(); ++d) {
            const auto& actual = g_guiMapDraws[d]; const auto& native = reference[d];
            check(actual.fvf == native.fvf && actual.count == native.count && actual.flags == native.flags,
                  "map submission preserves primitive format, count and flags");
            const size_t strideWords = actual.fvf == 0x144 ? 7 : 5;
            for (size_t i = 0; i < actual.vertices.size(); i += strideWords) {
                Point a, b; std::memcpy(&a, actual.vertices.data() + i, 8); std::memcpy(&b, native.vertices.data() + i, 8);
                check(std::fabs(a.x - (origin.x + (b.x - 3650) * factor)) < 0.001f &&
                      std::fabs(a.y - (origin.y + (b.y - 30) * factor)) < 0.001f,
                      "PF map tiles, outlines and markers scale exactly once with the containing panel");
                check(std::memcmp(actual.vertices.data() + i + 2, native.vertices.data() + i + 2, (strideWords - 2) * 4) == 0,
                      "map scaling preserves depth, reciprocal depth, colour and texture coordinates");
            }
        }
        Point after; g_guiMapTo(map.bytes, &after, &zero, 1);
        check(after == origin, "native custom drawing restores normal GUI coordinate conversion");
        mapRender(map.bytes, viewport.bytes, 0, 0); // Inactive main map, without engine state.
        g_guiMapDraws.clear(); GuiMapTiles(map.bytes, nullptr, viewport.bytes);
        const auto& outside = g_guiMapDraws.back();
        for (size_t i = 0; i < 4; ++i) {
            Point point; std::memcpy(&point, outside.vertices.data() + i * 7, sizeof(point));
            check(point == Point{origin.x + i * 10.0f, origin.y + i * 20.0f},
                  "unscoped drawing passes through without scaling or an extra submission copy");
        }
    }
    *instance = saved;
    std::printf("GUI sidebar: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures;
}
