// Logical GUI coordinates and display pixels. Rectangles include their last pixel.
#pragma once

namespace rnative::gui {

struct Point { float x, y; };
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

}  // namespace rnative::gui
