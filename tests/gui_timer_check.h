// Legacy combat bars through native construction, sprite scaling and hotspot refresh.
#pragma once
#include <memory>
#include <unordered_map>

struct GuiTimerPoint { int x, y; };
struct GuiTimerSprite { rnative::gui::Point scale; GuiTimerPoint position; };
struct GuiTimerWindow {
    GuiTimerSprite sprite{};
    void* sprites[1]{&sprite};
    uint8_t tree[12]{}, head[24]{}, node[24]{};
};
inline std::unordered_map<void*, std::unique_ptr<GuiTimerWindow>> g_guiTimerWindows;
inline void GuiTimerPut(void* object, size_t offset, const auto& value) {
    std::memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(value));
}
inline void* __fastcall GuiTimerWindowCtor(void* window, void*, int, int, const GuiTimerPoint* tile,
                                          const GuiTimerPoint* position, int, float x, float y, int)
{
    auto data = std::make_unique<GuiTimerWindow>();
    std::memset(window, 0, 0x50);
    GuiTimerPut(window, 0, *tile); GuiTimerPut(window, 8, int(1)); GuiTimerPut(window, 12, int(1));
    GuiTimerPut(window, 0x18, static_cast<void*>(data->sprites));
    GuiTimerPut(window, 0x2C, *position); GuiTimerPut(window, 0x3C, rnative::gui::Point{x, y});
    GuiTimerPut(data->tree, 4, static_cast<void*>(data->head));
    for (size_t offset : {0u, 4u, 8u}) GuiTimerPut(data->head, offset, static_cast<void*>(data->head));
    data->head[0x11] = 1;
    GuiTimerPut(window, 0x48, static_cast<void*>(data->tree));
    GuiTimerPut(window, 0x4C, uint8_t(1));
    g_guiTimerWindows[window] = std::move(data);
    return window;
}
inline void __fastcall GuiTimerSpriteScale(void* sprite, void*, const float* x, const float* y) {
    static_cast<GuiTimerSprite*>(sprite)->scale = {*x, *y};
}
inline void __fastcall GuiTimerSpriteMove(void* sprite, void*, const GuiTimerPoint* position) {
    static_cast<GuiTimerSprite*>(sprite)->position = *position;
}
inline void __fastcall GuiTimerColour(void*, void*, uint32_t) {}
inline void __fastcall GuiTimerResize(void* window, void*, const GuiTimerPoint* size) { GuiTimerPut(window, 0x34, *size); }
inline void* __fastcall GuiTimerDelete(void* object, void*, uint32_t) { rnative::vc10::Free(object); return nullptr; }
inline void* __fastcall GuiTimerPowerCtor(void* bar, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                         uint32_t, uint32_t, uint32_t, uint32_t, void* window)
{
    static void* table[]{reinterpret_cast<void*>(&GuiTimerDelete)};
    std::memset(bar, 0, 0x74); GuiTimerPut(bar, 0, static_cast<void*>(table)); GuiTimerPut(bar, 0x28, window);
    GuiTimerPut(bar, 0x3C, GuiTimerPoint{5, 1}); GuiTimerPut(bar, 0x44, GuiTimerPoint{100, 10});
    GuiTimerPut(bar, 0x6C, GuiTimerPoint{100, 10});
    auto& data = *g_guiTimerWindows.at(window);
    for (size_t offset : {0u, 4u, 8u}) {
        GuiTimerPut(data.head, offset, static_cast<void*>(data.node));
        GuiTimerPut(data.node, offset, static_cast<void*>(data.head));
    }
    GuiTimerPut(data.node, 0xC, bar);
    return bar;
}

inline int CheckGuiTimers(HMODULE module, void* controller)
{
    using namespace rnative::gui;
    auto* bytes = reinterpret_cast<uint8_t*>(module);
    GuiCheckPatch windowCtor(bytes + 0x2113B, reinterpret_cast<void*>(&GuiTimerWindowCtor));
    GuiCheckPatch powerCtor(bytes + 0x20696, reinterpret_cast<void*>(&GuiTimerPowerCtor));
    GuiCheckPatch resize(bytes + 0x20938, reinterpret_cast<void*>(&GuiTimerResize));
    GuiCheckImport colour(reinterpret_cast<void**>(bytes + 0x1A73E8), reinterpret_cast<void*>(&GuiTimerColour));
    GuiCheckImport spriteScale(reinterpret_cast<void**>(bytes + 0x1A73D8), reinterpret_cast<void*>(&GuiTimerSpriteScale));
    GuiCheckImport spriteMove(reinterpret_cast<void**>(bytes + 0x1A73E4), reinterpret_cast<void*>(&GuiTimerSpriteMove));
    using Ctor = void*(__thiscall*)(void*, int, int, const void*, const char*, uint32_t);
    using Move = void(__thiscall*)(void*, const GuiTimerPoint*);
    using Scale = void(__thiscall*)(void*, const float*, const float*);
    using Destroy = void(__thiscall*)(void*);
    auto construct = reinterpret_cast<Ctor>(bytes + 0x512AE);
    auto move = reinterpret_cast<Move>(bytes + 0x5128A);
    auto scale = reinterpret_cast<Scale>(bytes + 0x20B0C);
    auto destroy = reinterpret_cast<Destroy>(bytes + 0x51458);
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("GUI timer: %s\n", what); } };
    for (int kind : {0, 1, 2}) {
        alignas(16) uint8_t timer[0x28]{};
        const uint32_t identity[2]{};
        SetScale(100, 150); BeginScaleFrame(controller);
        construct(timer, kind, 1, identity, nullptr, 0xFFFFFF);
        void* window; void* bar; std::memcpy(&window, timer + 4, 4); std::memcpy(&bar, timer + 8, 4);
        const GuiTimerPoint position{40, 60}; move(timer, &position);
        const float nativeX = 1.25f, nativeY = 1.0f; scale(window, &nativeX, &nativeY);
        for (float percent : {150.0f, 200.0f, 75.0f, 100.0f, 176.0f, 150.0f}) {
            SetScale(200, percent); BeginScaleFrame(controller);
            const float factor = percent / 100;
            const auto& sprite = g_guiTimerWindows.at(window)->sprite;
            const int x = int(std::lround(position.x * factor)), y = int(std::lround(position.y * factor));
            check(std::fabs(sprite.scale.x - nativeX * factor) < 0.001f && std::fabs(sprite.scale.y - factor) < 0.001f,
                  "combat-bar sprites use HUD scale independently of window scale");
            check(sprite.position.x == x && sprite.position.y == y, "combat-bar positions scale without accumulating movement");
            int hit[4]; std::memcpy(hit, static_cast<uint8_t*>(bar) + 0x2C, sizeof(hit));
            check(hit[0] == x + int(5 * nativeX * factor) && hit[1] == y + int(factor) &&
                  hit[2] == hit[0] + int(100 * nativeX * factor) && hit[3] == hit[1] + int(10 * factor),
                  "native hotspot refresh follows scaled drawing");
        }
        destroy(timer); g_guiTimerWindows.clear();
    }
    alignas(16) uint8_t ordinary[0x50]{};
    const GuiTimerPoint tile{128, 16}, position{40, 60};
    GuiTimerWindowCtor(ordinary, nullptr, 0, 0, &tile, &position, 0, 1, 1, 0);
    const float one = 1; scale(ordinary, &one, &one);
    check(g_guiTimerWindows.at(ordinary)->sprite.scale == Point{1, 1}, "other legacy sprite windows retain their native scale");
    reinterpret_cast<Destroy>(bytes + 0x21241)(ordinary); g_guiTimerWindows.clear();
    SetScale(100, 100); BeginScaleFrame(controller);
    std::printf("GUI timers: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures;
}
