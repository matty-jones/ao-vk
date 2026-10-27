// Packed GUI quads retain their UV mapping; filtering clamps only the sampled fragments.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rvk::detail {

// Reserved bit zero tags our extra bounds without changing a valid caller's FVF.
constexpr uint32_t kUiAtlasFvf = 0x80245;
struct UiAtlasVertex { float x, y, z, rhw; uint32_t colour; float u, v, bounds[4]; };
struct UiSourceVertex { float x, y, z, rhw; uint32_t colour; float u, v; };
static_assert(sizeof(UiSourceVertex) == 28 && sizeof(UiAtlasVertex) == 44);

inline bool UiAtlasVertices(const void* input, uint32_t count, uint32_t corners, float width, float height,
                            std::vector<UiAtlasVertex>& output)
{
    if (!input || !count || !corners || count % corners || width <= 0 || height <= 0) return false;
    const auto* source = static_cast<const UiSourceVertex*>(input);
    output.resize(count);
    for (uint32_t first = 0; first < count; first += corners) {
        float left = source[first].x, right = left, top = source[first].y, bottom = top;
        float u0 = source[first].u, u1 = u0, v0 = source[first].v, v1 = v0;
        for (uint32_t i = first; i < first + corners; ++i) {
            const auto& v = source[i];
            if (v.rhw != 1 || !std::isfinite(v.x + v.y + v.u + v.v)) return false;
            left = std::min(left, v.x); right = std::max(right, v.x);
            top = std::min(top, v.y); bottom = std::max(bottom, v.y);
            u0 = std::min(u0, v.u); u1 = std::max(u1, v.u);
            v0 = std::min(v0, v.v); v1 = std::max(v1, v.v);
        }
        if (left == right || top == bottom || u0 < 0 || u1 > 1 || v0 < 0 || v1 > 1) return false;
        const float minU = std::min(u0 + 0.5f / width, (u0 + u1) * 0.5f);
        const float maxU = std::max(u1 - 0.5f / width, minU);
        const float minV = std::min(v0 + 0.5f / height, (v0 + v1) * 0.5f);
        const float maxV = std::max(v1 - 0.5f / height, minV);
        uint32_t covered = 0;
        for (uint32_t i = first; i < first + corners; ++i) {
            const auto& v = source[i];
            if ((v.x != left && v.x != right) || (v.y != top && v.y != bottom) ||
                (v.u != u0 && v.u != u1) || (v.v != v0 && v.v != v1)) return false;
            covered |= 1u << ((v.x == right ? 1 : 0) | (v.y == bottom ? 2 : 0));
            output[i] = {v.x, v.y, v.z, v.rhw, v.colour, v.u, v.v, {minU, minV, maxU, maxV}};
        }
        if (covered != 15) return false;
    }
    return true;
}

}  // namespace rvk::detail
