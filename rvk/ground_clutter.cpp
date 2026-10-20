// Rocks on the native ground (RVK_GroundRocks; docs/terrain-plan.md, Phase 4): scree at the feet of cliffs and a few
// stones on rough slopes, where rock would have fallen - placed from the playfield's heights alone, so flat ground
// (meadows, roads, towns) gets none.
//
// Each rock is a small mesh (an icosahedron, split once for the bigger ones, its corners pushed in and out and
// flattened), turned, sized and half buried where it lies, shaded smooth over its gentle folds and sharp at its edges
// (ShapeNormals). A patch's rocks are one static mesh in the ground's own
// format, drawn in the ground's own two passes right after its patches (DrawNativeGround): the base pass lays the
// cliffs' rock on them from every side (terrain.glsl, TerrainClutter: tinted to the colour of the cliff they fell
// from - set 0 its cell), the lighting
// pass gives them the ground's light where they lie - its lightmap, turned to their faces by the lightmap's fit
// (NativeSunTurn) - so they sit in the land's light and shade. They cast sun shadows (the ground itself doesn't).
// Made on worker threads, a couple of patches at a time, as the camera comes near; dropped when it is far.
#include "internal.h"

#include <algorithm>
#include <array>
#include <chrono>
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
constexpr int kMakeAtOnce = 2;                 // patches' rocks made at a time (on workers) at most
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

// A rock's shape: triangles (three corners each, a position and a normal), unit size, its bottom at about -0.5.
struct Corner { std::array<float, 3> p, n; };
using Shape = std::vector<Corner>;

std::array<float, 3> FaceNormal(const std::array<float, 3>& a, const std::array<float, 3>& b,
                                const std::array<float, 3>& c)
{
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    std::array<float, 3> n = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l < 1e-12f) return {0.0f, 0.0f, 0.0f};
    // (Outward: away from the rock's middle, the origin.)
    if (n[0] * (a[0] + b[0] + c[0]) + n[1] * (a[1] + b[1] + c[1]) + n[2] * (a[2] + b[2] + c[2]) < 0.0f) l = -l;
    for (float& q : n) q /= l;
    return n;
}

// Each corner's normal: the faces round it that fold less than ~50 degrees from its own face, by their areas - the
// lumps shade round, each triangle no longer its own flat facet; a chipped edge and the cut bottom stay sharp.
Shape ShapeNormals(const std::vector<std::array<float, 3>>& v, const std::vector<std::array<int, 3>>& f)
{
    std::vector<std::array<float, 3>> fn(f.size()), area(f.size());
    std::vector<std::vector<int>> around(v.size());
    for (size_t i = 0; i < f.size(); ++i) {
        fn[i] = FaceNormal(v[f[i][0]], v[f[i][1]], v[f[i][2]]);
        const auto &a = v[f[i][0]], &b = v[f[i][1]], &c = v[f[i][2]];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float x = e1[1] * e2[2] - e1[2] * e2[1], y = e1[2] * e2[0] - e1[0] * e2[2], z = e1[0] * e2[1] - e1[1] * e2[0];
        const float ar = 0.5f * std::sqrt(x * x + y * y + z * z);
        area[i] = {fn[i][0] * ar, fn[i][1] * ar, fn[i][2] * ar};
        for (int k = 0; k < 3; ++k) around[f[i][k]].push_back(int(i));
    }
    Shape s;
    for (size_t i = 0; i < f.size(); ++i)
        for (int k = 0; k < 3; ++k) {
            std::array<float, 3> n = {0.0f, 0.0f, 0.0f};
            for (int j : around[f[i][k]])
                if (fn[j][0] * fn[i][0] + fn[j][1] * fn[i][1] + fn[j][2] * fn[i][2] > 0.64f)
                    for (int q = 0; q < 3; ++q) n[q] += area[j][q];
            const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (l > 1e-12f) for (float& q : n) q /= l;
            else n = fn[i];
            s.push_back({v[f[i][k]], n});
        }
    return s;
}

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
    return ShapeNormals(v, f);
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

// One patch's rocks (patch index of the ground's): where the land says they fell, made into static meshes of at most
// 65535 vertices each. On a worker thread (5 to 8 ms a patch): it reads the ground's data only - which tiles are grass
// (grassy, by tile) the render thread worked out from their textures.
Device::ClutterBuild Device::BuildNativeClutter(std::shared_ptr<const NativeGround> ground, uint32_t index,
                                                float density, std::vector<uint8_t> grassy)
{
    ClutterBuild out;
    const NativeGround& g = *ground;
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
    // A grass tile gets only the cliffs' scree, not stones over its slopes. Made ground (paving, slabs, smooth grey:
    // GrassyTiles 2) gets none, nor ground at the foot of it: in a city the heights rise in ramps under the buildings
    // and steps, not cliffs, and boulders lay against Borealis's shop fronts (the user, 2026-10-19).
    auto green = [&](int c, int r) {
        const uint32_t tile = g.codes[size_t(r) * g.cellsX + c] & 0x3FFFu;
        return tile < grassy.size() && grassy[tile] == 1;
    };
    auto made = [&](int c, int r) {
        c = std::clamp(c, 0, int(g.cellsX) - 1);
        r = std::clamp(r, 0, int(g.cellsZ) - 1);
        const uint32_t tile = g.codes[size_t(r) * g.cellsX + c] & 0x3FFFu;
        return tile < grassy.size() && grassy[tile] == 2;
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
        out.pieces.push_back({data, idx, uint32_t(verts.size()), pebbles != 0});
        verts.clear();
    };
    density = std::clamp(density, 0.0f, 2.0f);
    // (Every cell's slope and mean height over the patch and two cells round it, once: each cell looks at 24 others.)
    const int B = int(P) + 4, c0 = int(px * P) - 2, r0 = int(pz * P) - 2;
    std::vector<float> slopes(size_t(B) * B), means(size_t(B) * B);
    for (int r = 0; r < B; ++r)
        for (int c = 0; c < B; ++c) slopes[size_t(r) * B + c] = slope(c0 + c, r0 + r, &means[size_t(r) * B + c]);
    for (uint32_t j = 0; j < P && pz * P + j < g.cellsZ; ++j)
        for (uint32_t i = 0; i < P && px * P + i < g.cellsX; ++i) {
            const int c = int(px * P + i), r = int(pz * P + j);
            const size_t at = size_t(r - r0) * B + size_t(c - c0);
            const float mean = means[at], s = slopes[at];
            // A cliff rising from here: the steepest cell within two whose ground is above this one's - where the
            // rocks came from, and so their colour.
            float cliff = 0.0f;
            int fromC = c, fromR = r;
            for (int dz = -2; dz <= 2; ++dz)
                for (int dx = -2; dx <= 2; ++dx) {
                    if (!dx && !dz) continue;
                    const size_t n = at + size_t(ptrdiff_t(dz) * B + dx);
                    const float m = means[n], t = slopes[n];
                    if (m > mean + 1.0f && t > cliff) {
                        cliff = t;
                        fromC = std::clamp(c + dx, 0, int(g.cellsX) - 1);
                        fromR = std::clamp(r + dz, 0, int(g.cellsZ) - 1);
                    }
                }
            if (made(c, r) || (cliff > 0.0f && made(fromC, fromR))) continue;
            const float foot = (1.0f - Smoothstep(0.7f, 1.2f, s)) * Smoothstep(1.0f, 2.2f, cliff);
            const float rough = Smoothstep(0.35f, 0.75f, s) * (1.0f - Smoothstep(1.1f, 1.7f, s)) * (green(c, r) ? 0.0f : 0.3f);
            Rng rng{Hash(uint32_t(c), uint32_t(r), g.id)};
            const float want = (foot * 4.0f + rough * 2.0f) * density;
            const int count = int(want) + (rng() < want - std::floor(want) ? 1 : 0);
            for (int k = 0; k < count && rocks < kMaxRocksPerPatch; ++k, ++rocks) {
                const float fx = rng(), fz = rng();
                const float x = (float(c) + fx) * g.cellX, z = (float(r) + fz) * g.cellZ;
                const float h00 = H(c, r), h10 = H(c + 1, r), h01 = H(c, r + 1), h11 = H(c + 1, r + 1);
                // (On the triangles the ground is drawn with, not between them: a rock on a cell split the other
                // way floated or sank by up to half its twist.)
                const float y0 = NativeCellHeight(h00, h10, h01, h11, std::min(g.cellX, g.cellZ), fx, fz);
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
                        const auto& o = shape[q + e].p;
                        const float lx = o[0] * sx, ly = o[1] * sy + 0.32f * sy - sink, lz = o[2] * sz;
                        w[e][0] = x + ax[0] * lx + up[0] * ly + az[0] * lz;
                        w[e][1] = y0 + ax[1] * lx + up[1] * ly + az[1] * lz;
                        w[e][2] = z + ax[2] * lx + up[2] * ly + az[2] * lz;
                    }
                    for (int e = 0; e < 3; ++e) {
                        // (The corner's normal through the rock's stretch - the inverse of its sizes - and turn.)
                        const auto& on = shape[q + e].n;
                        const float lx = on[0] / sx, ly = on[1] / sy, lz = on[2] / sz;
                        float n[3];
                        for (int k = 0; k < 3; ++k) n[k] = ax[k] * lx + up[k] * ly + az[k] * lz;
                        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        for (float& q2 : n) q2 /= std::max(nl, 1e-12f);
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
    out.rocks = rocks;
    return out;
}

// The rocks of the patches near the eye and in view, for a pass (from DrawNativeGround, the ground's state set);
// patches' rocks made as they come near, dropped far away.
void Device::DrawNativeClutter(const d3d::Matrix& vp)
{
    if (m_native.rocks <= 0.0f || !TerrainRockOn() || !m_native.ground) return;
    UpdateFrameEye();
    int pending = 0;
    for (const NativePatch& p : m_native.patches) pending += p.clutterPending.valid() ? 1 : 0;
    std::vector<uint8_t> grassy;
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
            continue;                            // (one being made is taken when it comes near again)
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
            // (Made on a worker, kMakeAtOnce patches at a time; drawn once in.)
            if (!p.clutterPending.valid()) {
                if (pending >= kMakeAtOnce) continue;
                if (grassy.empty()) grassy = GrassyTiles();
                if (grassy.empty()) continue;    // (the tiles' looks not measured yet: made ground unknown - later)
                p.clutterPending = std::async(std::launch::async, BuildNativeClutter, m_native.ground, i, m_native.rocks,
                                              grassy);
            }
            if (p.clutterPending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++pending;
                continue;
            }
            ClutterBuild built = p.clutterPending.get();
            p.clutter = std::move(built.pieces);
            p.clutterMade = true;
            m_native.rocksMade += built.rocks;
        }
        for (const NativeClutter& piece : p.clutter)
            if (!piece.pebbles || d2 < kSmallRange * kSmallRange)
                DrawSharedIndexed(d3d::TriangleList, kFvf, piece.vertices, 0, piece.count, piece.indices, piece.count);
    }
    m_native.clutter = false;
}

// Which of the ground's tiles are grass (their mean colour green: 1) or made ground - slabs, smooth grey, cool grey
// paving (2; as ground_pebbles.cpp's verdicts), by tile.
std::vector<uint8_t> Device::GrassyTiles() const
{
    std::vector<uint8_t> out(m_native.ground->tiles.size(), 0);
    for (size_t i = 0; i < out.size(); ++i) {
        const Texture* t = m_native.ground->tiles[i];
        if (t && t->m_image && (!t->m_thumbValid || t->m_line < 0.0f)) return {};   // (not measured yet: again later)
        if (!t || t->m_mean[0] < 0.0f) continue;
        const float r = t->m_mean[0], g = t->m_mean[1], b = t->m_mean[2];
        if (g > r * 1.08f && g > b * 1.08f) { out[i] = 1; continue; }
        const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), sat = (hi - lo) / std::max(hi, 1e-3f);
        const float lum = 255.0f * (0.3f * r + 0.59f * g + 0.11f * b);
        const bool measured = t->m_line >= 0.0f;
        if ((measured && (t->m_line > 0.3f || t->m_edge > 1.2f)) ||
            (measured && sat < 0.12f && (t->m_grain < 15.0f || (sat < 0.08f && lum > 150.0f))) ||
            (b > r * 1.05f && sat < 0.25f))
            out[i] = 2;
    }
    return out;
}

// RVK_GroundRocks: how many (0 none, 1 as placed, 2 twice); every patch's made again.
void Device::SetNativeGroundRocks(float density)
{
    if (density == m_native.rocks) return;
    m_native.rocks = density;
    for (NativePatch& p : m_native.patches) {
        if (p.clutterPending.valid()) p.clutterPending.wait();   // (made with the old count: dropped)
        p.clutterPending = {};
        p.clutter.clear();
        p.clutterMade = false;
    }
}

}  // namespace rvk
