// Rocks on the native ground (RVK_GroundRocks; docs/terrain-plan.md, Phase 4): scree at the feet of cliffs and a few
// stones on rough slopes, where rock would have fallen - placed from the playfield's heights alone, so flat ground
// (meadows, roads, towns) gets none.
//
// Each rock is a small faceted mesh (an icosahedron, split once for the bigger ones, its corners pushed in and out and
// flattened), turned, sized and half buried where it lies. A patch's rocks are one static mesh in the ground's own
// format, drawn in the ground's own two passes right after its patches (DrawNativeGround): the base pass lays the
// cliffs' rock on them from every side (terrain.glsl, TerrainClutter: tinted to the colour of the cliff they fell
// from - set 0 its cell), the lighting
// pass gives them the ground's light where they lie - its lightmap, turned to their faces by the lightmap's fit
// (NativeSunTurn) - so they sit in the land's light and shade. They cast sun shadows (the ground itself doesn't).
// Made a few patches a frame as the camera comes near, dropped when it is far.
#include "internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace detail;

namespace {

constexpr uint32_t kFvf = d3d::FVF_XYZ | d3d::FVF_NORMAL | (2u << 8);
struct Vertex { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };

constexpr float kRange = 200.0f;               // world units: rocks drawn within this of the eye (a patch's box)
constexpr float kKeep = 450.0f;                // ... and kept made within this
constexpr float kSmallRange = 90.0f;           // ... the small ones (under kSmall across) within this
constexpr float kSmall = 0.45f;
constexpr int kMakePerFrame = 2;               // patches' rocks made a frame at most
constexpr uint32_t kMaxRocksPerPatch = 6000;

uint32_t Hash(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x8DA6B343u ^ b * 0xD8163841u ^ c * 0xCB1AB31Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

struct Rng {
    uint32_t s;
    float operator()() { s = s * 1664525u + 1013904223u; return float(s >> 8) / 16777216.0f; }
};

// A rock's shape: flat triangles (three corners each), unit size, its bottom at about -0.5.
using Shape = std::vector<std::array<float, 3>>;

Shape MakeShape(uint32_t seed, bool fine)
{
    // The icosahedron.
    const float t = 1.618034f;
    std::vector<std::array<float, 3>> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    std::vector<std::array<int, 3>> f = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9},
                                         {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2},
                                         {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10},
                                         {8, 6, 7}, {9, 8, 1}};
    for (auto& p : v) {
        const float l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        for (float& c : p) c /= l;
    }
    if (fine) {                                  // (split once: each triangle into four, the new corners on the sphere)
        std::vector<std::array<int, 3>> g;
        auto mid = [&](int a, int b) {
            std::array<float, 3> m = {v[a][0] + v[b][0], v[a][1] + v[b][1], v[a][2] + v[b][2]};
            const float l = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            for (float& c : m) c /= l;
            for (size_t i = 0; i < v.size(); ++i)
                if (std::fabs(v[i][0] - m[0]) + std::fabs(v[i][1] - m[1]) + std::fabs(v[i][2] - m[2]) < 1e-5f)
                    return int(i);
            v.push_back(m);
            return int(v.size() - 1);
        };
        for (const auto& t3 : f) {
            const int a = mid(t3[0], t3[1]), b = mid(t3[1], t3[2]), c = mid(t3[2], t3[0]);
            g.push_back({t3[0], a, c});
            g.push_back({t3[1], b, a});
            g.push_back({t3[2], c, b});
            g.push_back({a, b, c});
        }
        f = std::move(g);
    }
    // Each corner pushed in or out (a few big lumps and a chipped edge or two), the whole squashed and its bottom cut
    // flat: a stone that lies, not a ball.
    Rng rng{seed * 2654435761u + 1};
    float lump[4][4];
    for (auto& l : lump) {
        float d[3] = {rng() - 0.5f, rng() - 0.5f, rng() - 0.5f};
        const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
        l[0] = d[0] / n; l[1] = d[1] / n; l[2] = d[2] / n;
        l[3] = (0.2f + 0.25f * rng()) * (rng() < 0.5f ? 1.0f : -0.7f);   // (a lump or a hollow)
    }
    for (auto& p : v) {
        float r = 1.0f + 0.12f * (rng() - 0.5f);
        for (const auto& l : lump) {
            const float d = p[0] * l[0] + p[1] * l[1] + p[2] * l[2];
            r += l[3] * std::max(d - 0.4f, 0.0f);
        }
        p[0] *= r;
        p[1] = std::max(p[1] * r * 0.62f, -0.32f);
        p[2] *= r;
    }
    Shape s;
    for (const auto& t3 : f)
        for (int k = 0; k < 3; ++k) s.push_back(v[t3[k]]);
    return s;
}

const std::vector<Shape>& Shapes()
{
    static const std::vector<Shape> shapes = [] {
        std::vector<Shape> out;
        for (uint32_t i = 0; i < 6; ++i) out.push_back(MakeShape(i, false));   // 0..5: small (20 faces)
        for (uint32_t i = 0; i < 6; ++i) out.push_back(MakeShape(i + 100, true));   // 6..11: big (80 faces)
        return out;
    }();
    return shapes;
}

}  // namespace

// One patch's rocks (patch i of the ground's): where the land says they fell, made into static meshes of at most 65535
// vertices each.
void Device::MakeNativeClutter(uint32_t index)
{
    NativePatch& p = m_native.patches[index];
    p.clutterMade = true;
    p.clutter.clear();
    const NativeGround& g = *m_native.ground;
    const uint32_t P = g.patch, pw = (g.cellsX + P - 1) / P;
    const uint32_t px = index % pw, pz = index / pw, W = g.cellsX + 1;
    auto H = [&](int c, int r) {
        c = std::clamp(c, 0, int(g.cellsX));
        r = std::clamp(r, 0, int(g.cellsZ));
        return g.heights[size_t(r) * W + c];
    };
    // A cell's slope (rise a unit) and mean height.
    auto slope = [&](int c, int r, float* mean) {
        const float a = H(c, r), b = H(c + 1, r), d = H(c, r + 1), e = H(c + 1, r + 1);
        if (mean) *mean = 0.25f * (a + b + d + e);
        const float gx = (b + e - a - d) / (2.0f * g.cellX), gz = (d + e - a - b) / (2.0f * g.cellZ);
        return std::sqrt(gx * gx + gz * gz);
    };
    // A green tile (grass) gets only the cliffs' scree, not stones over its slopes.
    auto green = [&](int c, int r) {
        const uint16_t code = g.codes[size_t(r) * g.cellsX + c];
        const uint32_t tile = code & 0x3FFFu;
        const Texture* t = tile < g.tiles.size() ? g.tiles[tile] : nullptr;
        return t && t->m_mean[0] >= 0.0f && t->m_mean[1] > t->m_mean[0] * 1.08f && t->m_mean[1] > t->m_mean[2] * 1.08f;
    };
    const auto& shapes = Shapes();
    std::vector<Vertex> lists[2];                // the big rocks, the small
    uint32_t rocks = 0;
    auto flush = [&](int pebbles) {
        std::vector<Vertex>& verts = lists[pebbles];
        if (verts.empty()) return;
        auto data = std::make_shared<std::vector<uint8_t>>(verts.size() * sizeof(Vertex));
        std::memcpy(data->data(), verts.data(), data->size());
        auto idx = std::make_shared<std::vector<uint8_t>>(verts.size() * 2);
        uint16_t* ix = reinterpret_cast<uint16_t*>(idx->data());
        for (size_t i = 0; i < verts.size(); ++i) ix[i] = uint16_t(i);
        p.clutter.push_back({data, idx, uint32_t(verts.size()), pebbles != 0});
        verts.clear();
    };
    const float density = std::clamp(m_native.rocks, 0.0f, 2.0f);
    for (uint32_t j = 0; j < P && pz * P + j < g.cellsZ; ++j)
        for (uint32_t i = 0; i < P && px * P + i < g.cellsX; ++i) {
            const int c = int(px * P + i), r = int(pz * P + j);
            float mean;
            const float s = slope(c, r, &mean);
            // A cliff rising from here: the steepest cell within two whose ground is above this one's - where the
            // rocks came from, and so their colour.
            float cliff = 0.0f;
            int fromC = c, fromR = r;
            for (int dz = -2; dz <= 2; ++dz)
                for (int dx = -2; dx <= 2; ++dx) {
                    if (!dx && !dz) continue;
                    float m;
                    const float t = slope(c + dx, r + dz, &m);
                    if (m > mean + 1.0f && t > cliff) {
                        cliff = t;
                        fromC = std::clamp(c + dx, 0, int(g.cellsX) - 1);
                        fromR = std::clamp(r + dz, 0, int(g.cellsZ) - 1);
                    }
                }
            auto smooth = [](float a, float b, float x) {
                const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };
            const float foot = (1.0f - smooth(0.7f, 1.2f, s)) * smooth(1.0f, 2.2f, cliff);
            const float rough = smooth(0.35f, 0.75f, s) * (1.0f - smooth(1.1f, 1.7f, s)) * (green(c, r) ? 0.0f : 0.3f);
            Rng rng{Hash(uint32_t(c), uint32_t(r), g.id)};
            const float want = (foot * 4.0f + rough * 2.0f) * density;
            const int count = int(want) + (rng() < want - std::floor(want) ? 1 : 0);
            for (int k = 0; k < count && rocks < kMaxRocksPerPatch; ++k, ++rocks) {
                const float fx = rng(), fz = rng();
                const float x = (float(c) + fx) * g.cellX, z = (float(r) + fz) * g.cellZ;
                const float h00 = H(c, r), h10 = H(c + 1, r), h01 = H(c, r + 1), h11 = H(c + 1, r + 1);
                const float y0 = (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz;
                // Sizes: mostly small, now and then a boulder at a cliff's foot.
                const bool fell = foot > rough;
                const int tintC = fell ? fromC : c, tintR = fell ? fromR : r;
                const float u = rng();
                const float size = fell ? 0.3f + 1.1f * u * u * u : 0.2f + 0.45f * u * u;
                const Shape& shape = shapes[(size > 0.7f ? 6 : 0) + (Hash(uint32_t(c), uint32_t(r), uint32_t(k)) % 6)];
                const float sx = size * (0.8f + 0.45f * rng()), sy = size * (0.7f + 0.5f * rng()),
                            sz = size * (0.8f + 0.45f * rng());
                const float yaw = rng() * 6.2831853f, cy = std::cos(yaw), sny = std::sin(yaw);
                // Leaning with the ground under it, half way.
                const float gx = ((h10 - h00) * (1 - fz) + (h11 - h01) * fz) / g.cellX;
                const float gz = ((h01 - h00) * (1 - fx) + (h11 - h10) * fx) / g.cellZ;
                float up[3] = {-gx * 0.5f, 1.0f, -gz * 0.5f};
                const float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
                for (float& q : up) q /= ul;
                // A frame: up, and two across it.
                float ax[3] = {cy, 0.0f, sny};
                const float d = ax[0] * up[0] + ax[2] * up[2];
                for (int q = 0; q < 3; ++q) ax[q] -= d * up[q];
                const float al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                for (float& q : ax) q /= al;
                const float az[3] = {ax[1] * up[2] - ax[2] * up[1], ax[2] * up[0] - ax[0] * up[2], ax[0] * up[1] - ax[1] * up[0]};
                const float sink = 0.12f * sy;  // (its cut bottom at -0.32: a little under the ground)
                const int pebble = std::max(sx, sz) < kSmall ? 1 : 0;
                std::vector<Vertex>& verts = lists[pebble];
                if (verts.size() + shape.size() > 65535) flush(pebble);
                for (size_t q = 0; q < shape.size(); q += 3) {
                    float w[3][3];
                    for (int e = 0; e < 3; ++e) {
                        const auto& o = shape[q + e];
                        const float lx = o[0] * sx, ly = o[1] * sy + 0.32f * sy - sink, lz = o[2] * sz;
                        w[e][0] = x + ax[0] * lx + up[0] * ly + az[0] * lz;
                        w[e][1] = y0 + ax[1] * lx + up[1] * ly + az[1] * lz;
                        w[e][2] = z + ax[2] * lx + up[2] * ly + az[2] * lz;
                    }
                    const float e1[3] = {w[1][0] - w[0][0], w[1][1] - w[0][1], w[1][2] - w[0][2]};
                    const float e2[3] = {w[2][0] - w[0][0], w[2][1] - w[0][1], w[2][2] - w[0][2]};
                    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                    float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (nl < 1e-12f) continue;
                    // (Outward: away from the rock's middle.)
                    const float mx = (w[0][0] + w[1][0] + w[2][0]) / 3 - x, my = (w[0][1] + w[1][1] + w[2][1]) / 3 - y0,
                                mz = (w[0][2] + w[1][2] + w[2][2]) / 3 - z;
                    if (n[0] * mx + n[1] * my + n[2] * mz < 0.0f) nl = -nl;
                    for (float& q2 : n) q2 /= nl;
                    for (int e = 0; e < 3; ++e) {
                        Vertex vx;
                        vx.x = w[e][0]; vx.y = w[e][1]; vx.z = w[e][2];
                        vx.nx = n[0]; vx.ny = n[1]; vx.nz = n[2];
                        // (Set 0: the middle of the cell it came from - its colour, terrain.glsl TerrainRockTint; set 1
                        // the lightmap's coordinates, as the ground's.)
                        vx.u0 = float(tintC) + 0.5f;
                        vx.v0 = float(tintR) + 0.5f;
                        vx.u1 = vx.x / g.cellX / float(g.cellsX);
                        vx.v1 = vx.z / g.cellZ / float(g.cellsZ);
                        verts.push_back(vx);
                    }
                }
            }
        }
    flush(0);
    flush(1);
    m_native.rocksMade += rocks;
}

// The rocks of the patches near the eye and in view, for a pass (from DrawNativeGround, the ground's state set);
// patches' rocks made as they come near, dropped far away.
void Device::DrawNativeClutter(const d3d::Matrix& vp)
{
    if (m_native.rocks <= 0.0f || !TerrainRockOn() || !m_native.ground) return;
    UpdateFrameEye();
    int made = 0;
    m_native.clutter = true;
    for (uint32_t i = 0; i < m_native.patches.size(); ++i) {
        NativePatch& p = m_native.patches[i];
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float d = std::max({p.lo[k] - m_frameEye[k], m_frameEye[k] - p.hi[k], 0.0f});
            d2 += d * d;
        }
        if (d2 > kKeep * kKeep) {
            if (p.clutterMade) { p.clutter.clear(); p.clutterMade = false; }
            continue;
        }
        if (d2 > kRange * kRange) continue;
        bool outside[6] = {true, true, true, true, true, true};
        for (int k = 0; k < 8; ++k) {
            // (Two units round the box: the rocks stand on its ground.)
            const float w[3] = {(k & 1) ? p.hi[0] + 2.0f : p.lo[0] - 2.0f, (k & 2) ? p.hi[1] + 2.0f : p.lo[1] - 2.0f,
                                (k & 4) ? p.hi[2] + 2.0f : p.lo[2] - 2.0f};
            float c[4];
            for (int j = 0; j < 4; ++j) c[j] = w[0] * vp.m[0][j] + w[1] * vp.m[1][j] + w[2] * vp.m[2][j] + vp.m[3][j];
            outside[0] = outside[0] && c[0] < -c[3];
            outside[1] = outside[1] && c[0] > c[3];
            outside[2] = outside[2] && c[1] < -c[3];
            outside[3] = outside[3] && c[1] > c[3];
            outside[4] = outside[4] && c[2] < 0.0f;
            outside[5] = outside[5] && c[2] > c[3];
        }
        if (outside[0] || outside[1] || outside[2] || outside[3] || outside[4] || outside[5]) continue;
        if (!p.clutterMade) {
            if (made >= kMakePerFrame) continue;
            ++made;
            MakeNativeClutter(i);
        }
        for (const NativeClutter& piece : p.clutter)
            if (!piece.pebbles || d2 < kSmallRange * kSmallRange)
                DrawSharedIndexed(d3d::TriangleList, kFvf, piece.vertices, 0, piece.count, piece.indices, piece.count);
    }
    m_native.clutter = false;
}

// RVK_GroundRocks: how many (0 none, 1 as placed, 2 twice); every patch's made again.
void Device::SetNativeGroundRocks(float density)
{
    if (density == m_native.rocks) return;
    m_native.rocks = density;
    for (NativePatch& p : m_native.patches) {
        p.clutter.clear();
        p.clutterMade = false;
    }
}

}  // namespace rvk
