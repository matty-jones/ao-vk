// Cursor placement through the client's MousePointer_t and native sprite-size implementation.
#pragma once
#include <array>
#include <memory>

namespace rnative::gui { void InstallCursorScaling(HMODULE gui); }

struct GuiCursorPoint { int x, y; };
struct GuiCursorSprite {
    alignas(16) uint8_t bytes[0x15C]{};
    GuiCursorPoint position{};
    int moves = 0;
    bool visible = false;
};
inline void __fastcall GuiCursorShow(GuiCursorSprite* sprite, void*) { sprite->visible = true; }
inline void __fastcall GuiCursorHide(GuiCursorSprite* sprite, void*) { sprite->visible = false; }
inline void __fastcall GuiCursorMove(GuiCursorSprite* sprite, void*, const GuiCursorPoint* point) {
    sprite->position = *point; ++sprite->moves;
}

inline int CheckGuiCursor(HMODULE module, void* controller)
{
    using namespace rnative::gui;
    InstallCursorScaling(module);
    SetScale(200, 175); BeginScaleFrame(controller);
    auto* bytes = reinterpret_cast<uint8_t*>(module);
    HMODULE display = GetModuleHandleA("DisplaySystem.dll");
    auto* reposition = reinterpret_cast<void*>(GetProcAddress(display, "?Reposition@RenderSprite_t@@QAEXABVIPoint@@@Z"));
    GuiCheckPatch capture(reposition, reinterpret_cast<void*>(&GuiCursorMove));
    using Move = void(__thiscall*)(void*, const GuiCursorPoint*, uint32_t);
    using SpriteMove = void(__thiscall*)(void*, const GuiCursorPoint*);
    auto move = reinterpret_cast<Move>(bytes + 0x1FDDD);
    static void* table[]{nullptr, nullptr, reinterpret_cast<void*>(&GuiCursorShow), reinterpret_cast<void*>(&GuiCursorHide)};
    std::array<GuiCursorSprite, 25> sprites;
    std::vector<std::unique_ptr<GuiCheckImport>> slots;
    for (int i = 0; i < 25; ++i) {
        const uint32_t rva = i < 15 ? 0x263178 + i * 20 : i < 23 ? 0x2632A8 + (i - 15) * 20 :
                             i == 23 ? 0x26313C : 0x263150;
        slots.push_back(std::make_unique<GuiCheckImport>(reinterpret_cast<void**>(bytes + rva), &sprites[i]));
        const int size = i < 15 ? 32 : 16;
        GuiTimerPut(sprites[i].bytes, 0, static_cast<void*>(table));
        GuiTimerPut(sprites[i].bytes, 0x34, size); GuiTimerPut(sprites[i].bytes, 0x38, size);
    }
    alignas(16) uint8_t pointer[0x24]{};
    pointer[8] = 1;
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("GUI cursor: %s\n", what); } };
    auto scalar = [](const GuiCursorSprite& sprite, size_t offset) {
        float value; std::memcpy(&value, sprite.bytes + offset, 4); return value;
    };
    for (int shape = 0; shape < 15; ++shape) {
        GuiTimerPut(pointer, 0xC, shape);
        for (int detail : {0, 1, 2}) {
            GuiTimerPut(pointer, 0x14, detail ? 1 : -1);
            GuiTimerPut(pointer, 0x1C, detail == 1 ? 1 : detail == 2 ? 3 : -1);
            for (GuiCursorPoint mouse : {GuiCursorPoint{0, 0}, {3839, 0}, {0, 2159}, {3839, 2159}, {1920, 1080}}) {
                const GuiCursorPoint physical = mouse;
                SetCursorScale(100);
                for (auto& sprite : sprites) sprite.moves = 0;
                move(pointer, &mouse, 1);
                std::array<GuiCursorPoint, 25> native;
                std::array<int, 25> moved;
                for (int i = 0; i < 25; ++i) { native[i] = sprites[i].position; moved[i] = sprites[i].moves; }
                for (float percent : {125.0f, 150.0f, 200.0f, 300.0f, 75.0f, 100.0f}) {
                    SetCursorScale(percent);
                    for (auto& sprite : sprites) sprite.moves = 0;
                    move(pointer, &mouse, 1);
                    GuiCursorPoint stored; std::memcpy(&stored, pointer, sizeof(stored));
                    check(stored.x == physical.x && stored.y == physical.y && mouse.x == physical.x && mouse.y == physical.y,
                          "physical click position stays unchanged");
                    const float factor = percent / 100;
                    for (int i = 0; i < 25; ++i) {
                        check(sprites[i].moves == moved[i], "native shape and detail selection stay unchanged");
                        if (!moved[i]) continue;
                        const auto actual = sprites[i].position;
                        check(std::fabs(actual.x + (mouse.x - native[i].x) * factor - mouse.x) <= 0.5001f &&
                              std::fabs(actual.y + (mouse.y - native[i].y) * factor - mouse.y) <= 0.5001f,
                              "every cursor and detail sprite scales around its physical hotspot");
                        check(scalar(sprites[i], 0x44) == factor && scalar(sprites[i], 0x48) == factor,
                              "native sprite scale follows cursor size");
                        const int size = i < 15 ? 32 : 16;
                        check(scalar(sprites[i], 0x3C) == size * factor && scalar(sprites[i], 0x40) == size * factor,
                              "native drawing dimensions match cursor size");
                        if (percent == 100) check(actual.x == native[i].x && actual.y == native[i].y,
                                                 "returning to 100% exactly restores cursor placement");
                    }
                }
            }
        }
    }
    SetCursorScale(200);
    for (GuiCursorPoint mouse : {GuiCursorPoint{-1, 50}, {50, -1}}) {
        for (auto& sprite : sprites) { sprite.moves = 0; sprite.visible = true; }
        move(pointer, &mouse, 1);
        check(sprites[14].moves == 0 && !sprites[14].visible, "hidden and off-window cursors remain hidden");
    }
    pointer[8] = 0;
    for (auto& sprite : sprites) sprite.visible = false;
    const GuiCursorPoint centre{1920, 1080};
    move(pointer, &centre, 0);
    check(std::none_of(sprites.begin(), sprites.end(), [](const auto& sprite) { return sprite.visible; }),
          "scaling does not reactivate the cursor during mouse look");
    GuiCursorSprite other;
    GuiTimerPut(other.bytes, 0x44, 1.0f); GuiTimerPut(other.bytes, 0x48, 1.0f);
    GuiCursorPoint point{123, 456};
    reinterpret_cast<SpriteMove>(*reinterpret_cast<void**>(bytes + 0x1A73E4))(&other, &point);
    check(other.position.x == point.x && other.position.y == point.y && scalar(other, 0x44) == 1,
          "other sprites retain their native position and size");
    SetCursorScale(100);
    SetScale(100, 100); BeginScaleFrame(controller);
    std::printf("GUI cursor: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures;
}
