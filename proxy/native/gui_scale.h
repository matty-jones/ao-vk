// Logical GUI coordinates and display pixels. Rectangles include their last pixel.
#pragma once
#include <algorithm>

namespace rnative::gui {

struct Point { float x, y; bool operator==(const Point&) const = default; };
struct Rect { float left, top, right, bottom; };

struct Scale {
    float value = 1.0f;
    void ToScreen(Point& p) const { if (value != 1.0f) { p.x *= value; p.y *= value; } }
    void FromScreen(Point& p) const { if (value != 1.0f) { p.x /= value; p.y /= value; } }
    void ToScreen(Rect& r) const
    {
        if (value == 1.0f) return;
        r.left *= value; r.top *= value;
        r.right = (r.right + 1.0f) * value - 1.0f;
        r.bottom = (r.bottom + 1.0f) * value - 1.0f;
    }
    void FromScreen(Rect& r) const
    {
        if (value == 1.0f) return;
        r.left /= value; r.top /= value;
        r.right = (r.right + 1.0f) / value - 1.0f;
        r.bottom = (r.bottom + 1.0f) / value - 1.0f;
    }
};

// One user-facing UI percentage; the 1080p client already sizes floating windows
// appropriately at 100% when the HUD needs 75%. Preserve that observed 4:3
// relationship when applying the user's UI percentage.
struct UiFactors { float windows; float hud; };
inline UiFactors EffectiveUiScales(float percent, Point screen)
{
    const float hud = std::clamp(percent, 50.0f, 200.0f);
    const bool native1080 = screen.x == 1920.0f && screen.y == 1080.0f;
    return {native1080 ? std::clamp(hud * (4.0f / 3.0f), 50.0f, 200.0f) : hud, hud};
}

struct Transform {
    float value = 1.0f;
    Point offset{};
    bool operator==(const Transform&) const = default;
    bool Identity() const { return value == 1.0f && offset.x == 0.0f && offset.y == 0.0f; }
    void ToScreen(Point& p) const { if (Identity()) return; Scale{value}.ToScreen(p); p.x += offset.x; p.y += offset.y; }
    void FromScreen(Point& p) const { if (Identity()) return; p.x -= offset.x; p.y -= offset.y; Scale{value}.FromScreen(p); }
    void ToScreen(Rect& r) const
    {
        if (Identity()) return;
        Scale{value}.ToScreen(r);
        r.left += offset.x; r.right += offset.x; r.top += offset.y; r.bottom += offset.y;
    }
    void FromScreen(Rect& r) const
    {
        if (Identity()) return;
        r.left -= offset.x; r.right -= offset.x; r.top -= offset.y; r.bottom -= offset.y;
        Scale{value}.FromScreen(r);
    }
};

inline Transform Place(Rect frame, Point screen, float factor, bool anchored, const Point* fixedAnchor = nullptr)
{
    if (factor == 1.0f) return {};
    Transform result{factor};
    if (anchored) {
        auto anchor = [](float centre, float extent) {
            return centre < extent * 0.25f ? 0.0f : centre > extent * 0.75f ? extent : extent * 0.5f;
        };
        result.offset = {(1 - factor) * (fixedAnchor ? fixedAnchor->x * screen.x : anchor((frame.left + frame.right + 1) * 0.5f, screen.x)),
                         (1 - factor) * (fixedAnchor ? fixedAnchor->y * screen.y : anchor((frame.top + frame.bottom + 1) * 0.5f, screen.y))};
    }
    result.ToScreen(frame);
    result.offset.x += std::clamp(frame.left, 0.0f, std::max(0.0f, screen.x - (frame.right - frame.left + 1))) - frame.left;
    result.offset.y += std::clamp(frame.top, 0.0f, std::max(0.0f, screen.y - (frame.bottom - frame.top + 1))) - frame.top;
    return result;
}

}  // namespace rnative::gui
