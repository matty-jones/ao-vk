// Shared by native_check and host-side coordinate checks.
#pragma once
#include "native/gui_scale.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

inline int CheckGuiScale()
{
    using namespace rnative::gui;
    static_assert(sizeof(Point) == 8 && sizeof(Rect) == 16);
    int failures = 0;
    auto check = [&](bool good, const char* what) {
        if (!good) { ++failures; std::printf("GUI scale: %s\n", what); }
    };
    Rect workspace{0, 0, 1919, 1079};
    Scale{2.0f}.ToScreen(workspace);
    check(workspace.left == 0 && workspace.top == 0 && workspace.right == 3839 && workspace.bottom == 2159,
          "200% maps a 1920x1080 layout onto 4K");
    Rect clip{100, 100, 299, 199};
    Scale{2.0f}.ToScreen(clip);
    check(clip.left == 200 && clip.top == 200 && clip.right == 599 && clip.bottom == 399,
          "clip includes its last pixel");
    for (float factor : {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f}) {
        for (Point p : {Point{-7.5f, 0.25f}, Point{0, 0}, Point{1919, 1079}, Point{3839, 2159}}) {
            Point input = p;
            Scale{factor}.ToScreen(input);
            Scale{factor}.FromScreen(input);
            check(std::abs(input.x - p.x) < 0.001f && std::abs(input.y - p.y) < 0.001f,
                  "input agrees with drawing at window edges");
        }
        Rect bounds{-10, 30, 319, 199};
        Scale{factor}.ToScreen(bounds);
        Scale{factor}.FromScreen(bounds);
        check(bounds.left == -10 && bounds.top == 30 && bounds.right == 319 && bounds.bottom == 199,
              "clipped bounds survive the inverse transform");
    }
    Rect unchanged{0.3f, -0.7f, 293.25f, 1079};
    Rect same = unchanged;
    Scale{}.ToScreen(same); Scale{}.FromScreen(same);
    check(std::memcmp(&same, &unchanged, sizeof(same)) == 0, "100% retains exact coordinates");
    Point window{320, 120}, hud = window;
    Scale{2.0f}.ToScreen(window); Scale{1.5f}.ToScreen(hud);
    check(window.x == 640 && window.y == 240 && hud.x == 480 && hud.y == 180, "window and HUD factors are independent");
    const Rect model{3500, 1900, 3799, 2099};
    for (float factor : {1.0f, 2.0f, 0.75f, 1.5f, 1.0f}) {
        Rect display = model;
        const Transform map = Place(model, {3840, 2160}, factor, false);
        map.ToScreen(display);
        if (factor == 2.0f) check(display.left == 3240 && display.right == 3839, "edge placement clamps only display coordinates");
        map.FromScreen(display);
        check(std::memcmp(&display, &model, sizeof(model)) == 0, "clamped placement retains its inverse");
    }
    Rect restored = model;
    Place(model, {3840, 2160}, 1.0f, false).ToScreen(restored);
    check(std::memcmp(&restored, &model, sizeof(model)) == 0, "100% uses the untouched canonical rectangle");
    std::printf("GUI scale: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures;
}
