// The water's surface from the game's own triangles (water_surface.h, docs/water-mesh-plan.md).
#include "water_surface.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rvk::water {

namespace {

// Where two of the game's corners are the same point: it gives every triangle its own corners, a 64th of a unit apart
// at most (as water.cpp's lips weld them).
uint64_t WeldKey(const float* p)
{
    const auto q = [&](int k) { return uint64_t(int64_t(std::lround(p[k] * 64.0f)) & 0x1FFFFF); };
    return q(0) | q(1) << 21 | q(2) << 42;
}

uint64_t EdgeKey(uint32_t a, uint32_t b) { return a < b ? uint64_t(a) << 32 | b : uint64_t(b) << 32 | a; }

float Distance(const float* a, const float* b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

float Area(const SurfaceVertex& a, const SurfaceVertex& b, const SurfaceVertex& c)
{
    const float e1[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
    const float e2[3] = {c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
    const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    return 0.5f * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
}

// A point between vertices (a midpoint, a centroid): their position and alpha, weighted.
SurfaceVertex Blend(const std::vector<SurfaceVertex>& v, const uint32_t* ids, const float* w, int n)
{
    SurfaceVertex out{};
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) out.pos[k] += v[ids[i]].pos[k] * w[i];
        out.alpha += v[ids[i]].alpha * w[i];
    }
    return out;
}

// Coarse triangles meeting fine ones: a vertex of the fine side lying inside the coarse triangle's edge (a T-junction)
// would move with the waves while that edge stays straight - a crack. Each triangle with such points on its edges is
// fanned from its centroid through them, so both sides share every vertex. Only edges of one triangle can have them
// (an edge two triangles share has no vertex inside it).
uint32_t Stitch(std::vector<SurfaceVertex>& v, std::vector<uint32_t>& tris)
{
    std::unordered_map<uint64_t, uint32_t> uses;
    for (size_t t = 0; t < tris.size(); t += 3)
        for (int k = 0; k < 3; ++k) ++uses[EdgeKey(tris[t + k], tris[t + (k + 1) % 3])];
    // The vertices on a grid (xz), to find those near an edge.
    constexpr float kCell = 8.0f, kOn = 0.02f;
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    const auto cellKey = [](int x, int z) { return uint64_t(uint32_t(x)) << 32 | uint32_t(z); };
    for (uint32_t i = 0; i < v.size(); ++i)
        grid[cellKey(int(std::floor(v[i].pos[0] / kCell)), int(std::floor(v[i].pos[2] / kCell)))].push_back(i);
    std::vector<uint32_t> out;
    out.reserve(tris.size());
    uint32_t stitched = 0;
    std::vector<std::pair<float, uint32_t>> on;
    std::vector<uint32_t> ring;
    for (size_t t = 0; t < tris.size(); t += 3) {
        ring.clear();
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = tris[t + k], b = tris[t + (k + 1) % 3];
            ring.push_back(a);
            if (uses[EdgeKey(a, b)] != 1)
                continue;
            const float* pa = v[a].pos;
            const float* pb = v[b].pos;
            const float d[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
            const float len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (len2 < 1e-6f)
                continue;
            on.clear();
            const int x0 = int(std::floor((std::min(pa[0], pb[0]) - kOn) / kCell));
            const int x1 = int(std::floor((std::max(pa[0], pb[0]) + kOn) / kCell));
            const int z0 = int(std::floor((std::min(pa[2], pb[2]) - kOn) / kCell));
            const int z1 = int(std::floor((std::max(pa[2], pb[2]) + kOn) / kCell));
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z) {
                    auto it = grid.find(cellKey(x, z));
                    if (it == grid.end())
                        continue;
                    for (uint32_t i : it->second) {
                        if (i == a || i == b)
                            continue;
                        const float* p = v[i].pos;
                        const float s = ((p[0] - pa[0]) * d[0] + (p[1] - pa[1]) * d[1] + (p[2] - pa[2]) * d[2]) / len2;
                        if (s <= 1e-4f || s >= 1.0f - 1e-4f)
                            continue;
                        const float q[3] = {pa[0] + d[0] * s, pa[1] + d[1] * s, pa[2] + d[2] * s};
                        if (Distance(p, q) < kOn)
                            on.emplace_back(s, i);
                    }
                }
            std::sort(on.begin(), on.end());
            for (const auto& [s, i] : on) ring.push_back(i);
        }
        if (ring.size() == 3) {
            out.insert(out.end(), {tris[t], tris[t + 1], tris[t + 2]});
            continue;
        }
        ++stitched;
        const uint32_t corners[3] = {tris[t], tris[t + 1], tris[t + 2]};
        const float third[3] = {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f};
        const uint32_t centre = uint32_t(v.size());
        v.push_back(Blend(v, corners, third, 3));
        for (size_t k = 0; k < ring.size(); ++k)
            out.insert(out.end(), {centre, ring[k], ring[(k + 1) % ring.size()]});
    }
    tris.swap(out);
    return stitched;
}

// One round of splitting: every edge longer than kMaxEdge at its midpoint (an edge's split decided by the edge alone,
// so both triangles beside it agree), each triangle into 2, 3 or 4. False: nothing was long.
bool SplitRound(std::vector<SurfaceVertex>& v, std::vector<uint32_t>& tris)
{
    std::unordered_map<uint64_t, uint32_t> mid;
    const auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
        if (Distance(v[a].pos, v[b].pos) <= kMaxEdge)
            return ~0u;
        auto [it, fresh] = mid.emplace(EdgeKey(a, b), uint32_t(v.size()));
        if (fresh) {
            const uint32_t ids[2] = {a, b};
            const float half[2] = {0.5f, 0.5f};
            v.push_back(Blend(v, ids, half, 2));
        }
        return it->second;
    };
    std::vector<uint32_t> out;
    out.reserve(tris.size() * 2);
    for (size_t t = 0; t < tris.size(); t += 3) {
        const uint32_t c[3] = {tris[t], tris[t + 1], tris[t + 2]};
        const uint32_t m[3] = {midpoint(c[0], c[1]), midpoint(c[1], c[2]), midpoint(c[2], c[0])};   // m[k]: edge k, k+1
        const int splits = (m[0] != ~0u) + (m[1] != ~0u) + (m[2] != ~0u);
        if (splits == 0) {
            out.insert(out.end(), {c[0], c[1], c[2]});
        } else if (splits == 3) {
            out.insert(out.end(), {c[0], m[0], m[2], m[0], c[1], m[1], m[2], m[1], c[2], m[0], m[1], m[2]});
        } else {
            // Turned so that the split edges come first (the triangle's winding kept): one split - edge a b; two -
            // edges a b and b c.
            int r = 0;
            if (splits == 1) r = m[0] != ~0u ? 0 : m[1] != ~0u ? 1 : 2;
            else r = m[2] == ~0u ? 0 : m[0] == ~0u ? 1 : 2;
            const uint32_t a = c[r], b = c[(r + 1) % 3], cc = c[(r + 2) % 3];
            const uint32_t ab = m[r], bc = m[(r + 1) % 3];
            if (splits == 1) {
                out.insert(out.end(), {a, ab, cc, ab, b, cc});
            } else {
                out.insert(out.end(), {ab, b, bc});
                // The quad a, ab, bc, c across its shorter diagonal.
                if (Distance(v[a].pos, v[bc].pos) < Distance(v[ab].pos, v[cc].pos))
                    out.insert(out.end(), {a, ab, bc, a, bc, cc});
                else
                    out.insert(out.end(), {a, ab, cc, ab, bc, cc});
            }
        }
    }
    tris.swap(out);
    return !mid.empty();
}

}  // namespace

void BuildSurface(const InVertex* in, size_t count, const std::vector<uint32_t>& flat, const std::vector<uint32_t>& steep,
                  Surface& out)
{
    out = Surface{};
    out.inTriangles = uint32_t(flat.size() / 3);
    if (flat.size() < 3)
        return;
    out.colour = in[flat[0]].colour;
    // Welded.
    std::vector<SurfaceVertex>& v = out.vertices;
    std::unordered_map<uint64_t, uint32_t> weld;
    std::vector<uint32_t> tris;
    tris.reserve(flat.size());
    for (size_t t = 0; t + 2 < flat.size(); t += 3) {
        uint32_t ids[3];
        for (int k = 0; k < 3; ++k) {
            const InVertex& p = in[flat[t + k]];
            auto [it, fresh] = weld.emplace(WeldKey(p.pos), uint32_t(v.size()));
            if (fresh) {
                SurfaceVertex s{};
                for (int j = 0; j < 3; ++j) s.pos[j] = p.pos[j];
                s.alpha = float(p.colour >> 24) / 255.0f;
                v.push_back(s);
            }
            ids[k] = it->second;
        }
        if (ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0] || Area(v[ids[0]], v[ids[1]], v[ids[2]]) < 1e-6f)
            continue;
        tris.insert(tris.end(), {ids[0], ids[1], ids[2]});
    }
    out.welded = uint32_t(v.size());
    // Where the surface meets a fall or a shore step: the welded vertices at the steep triangles' corners.
    std::unordered_set<uint32_t> seam;
    for (uint32_t i : steep) {
        auto it = weld.find(WeldKey(in[i].pos));
        if (it != weld.end()) seam.insert(it->second);
    }
    out.stitched = Stitch(v, tris);
    while (out.splitRounds < 16 && tris.size() < 3 * 400000 && SplitRound(v, tris))
        ++out.splitRounds;

    // Calm: the distance over the mesh from an open edge (a shore) or a seam, out to kCalmDistance.
    std::unordered_map<uint64_t, uint32_t> uses;
    std::vector<std::vector<std::pair<uint32_t, float>>> next(v.size());
    for (size_t t = 0; t < tris.size(); t += 3)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = tris[t + k], b = tris[t + (k + 1) % 3];
            if (uses[EdgeKey(a, b)]++ == 0) {
                const float d = Distance(v[a].pos, v[b].pos);
                next[a].emplace_back(b, d);
                next[b].emplace_back(a, d);
            }
        }
    std::vector<float> dist(v.size(), kCalmDistance);
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    for (const auto& [e, n] : uses)
        if (n == 1)
            for (uint32_t i : {uint32_t(e >> 32), uint32_t(e & 0xFFFFFFFFu)})
                if (dist[i] > 0.0f) { dist[i] = 0.0f; open.emplace(0.0f, i); }
    for (uint32_t i : seam)
        if (dist[i] > 0.0f) { dist[i] = 0.0f; open.emplace(0.0f, i); }
    while (!open.empty()) {
        const auto [d, i] = open.top();
        open.pop();
        if (d > dist[i])
            continue;
        for (const auto& [j, len] : next[i])
            if (d + len < dist[j]) {
                dist[j] = d + len;
                open.emplace(dist[j], j);
            }
    }
    for (size_t i = 0; i < v.size(); ++i) {
        const float s = std::clamp(dist[i] / kCalmDistance, 0.0f, 1.0f);
        v[i].calm = s * s * (3.0f - 2.0f * s);
    }

    // Bodies: the connected pieces, each one's size across (xz) - a pond stays calm, the open sea has a swell.
    std::vector<uint32_t> parent(v.size());
    for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
    const auto find = [&](uint32_t a) { while (parent[a] != a) a = parent[a] = parent[parent[a]]; return a; };
    for (size_t t = 0; t < tris.size(); t += 3) {
        parent[find(tris[t + 1])] = find(tris[t]);
        parent[find(tris[t + 2])] = find(tris[t]);
    }
    std::unordered_map<uint32_t, std::pair<std::pair<float, float>, std::pair<float, float>>> box;   // x lo hi, z lo hi
    for (uint32_t i = 0; i < v.size(); ++i) {
        auto [it, fresh] = box.emplace(find(i), std::make_pair(std::make_pair(v[i].pos[0], v[i].pos[0]),
                                                               std::make_pair(v[i].pos[2], v[i].pos[2])));
        if (!fresh) {
            auto& [x, z] = it->second;
            x.first = std::min(x.first, v[i].pos[0]);
            x.second = std::max(x.second, v[i].pos[0]);
            z.first = std::min(z.first, v[i].pos[2]);
            z.second = std::max(z.second, v[i].pos[2]);
        }
    }
    std::unordered_map<uint32_t, float> size;
    for (const auto& [root, b] : box) {
        const float extent = std::max(b.first.second - b.first.first, b.second.second - b.second.first);
        size[root] = std::clamp((extent - 60.0f) / 700.0f, 0.0f, 1.0f);
        out.largestBody = std::max(out.largestBody, extent);
    }
    out.bodies = uint32_t(size.size());
    for (uint32_t i = 0; i < v.size(); ++i) v[i].body = size[find(i)];

    // (Vertices no triangle uses - a stitched triangle's lone corner can't be one, but keep the buffer tight anyway.)
    out.indices = std::move(tris);
    for (int k = 0; k < 3; ++k) { out.lo[k] = 1e30f; out.hi[k] = -1e30f; }
    for (const SurfaceVertex& s : v)
        for (int k = 0; k < 3; ++k) {
            out.lo[k] = std::min(out.lo[k], s.pos[k]);
            out.hi[k] = std::max(out.hi[k], s.pos[k]);
        }
}

}  // namespace rvk::water
