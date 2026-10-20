// Pebbles on the native ground (RVK_Pebbles; docs/pebbles-plan.md): small loose stones on the flat ground that is
// dirt, gravel or sand, near the eye - the rocks (ground_clutter.cpp) lie only where the heights say rock fell.
//
// Which ground gets them is read from the tiles' surface types (NativeGround::types, tile_type_data): each type's base
// tile is classified by its look - green (grass), straight joints or edges (slabs, plates, flagstones), smooth grey
// (asphalt, concrete) get none; the rest some, more the coarser its grain (gravel many, smooth dirt few, sand few and
// small). randy-vk\terrain\pebbles.tsv can set a playfield's type by hand. Within a cell, a pebble is kept only where
// its tile's texel under it is that ground's (a transition tile is part grass, part sand): not green, and near the base
// tile's colour.
//
// Placed in blocks of 8 x 8 cells near the eye, made on worker threads as it moves, dropped behind it. Each pebble is a
// lumped, flattened icosahedron (12 corners, indexed), lying with the ground, a little more than half showing. Drawn in
// the ground's two passes as the rocks are (terrain.glsl's clutter path; D.terrainAt.y 3): the rock tinted to the
// ground's colour where it lies, each stone a little lighter or darker (a hash of its set-0 coordinate, the same at all
// its corners), lit by the ground's light. Over the last units of their range they sink into the ground (ffp.vert), so
// they grow out of it as the eye comes near rather than appear. Only the bigger ones cast shadows.
#include "internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace rvk {

using namespace detail;

namespace {

constexpr uint32_t kFvf = d3d::FVF_XYZ | d3d::FVF_NORMAL | (2u << 8);
struct Vertex { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };

constexpr uint32_t kBlock = 8;                 // cells a block side
constexpr float kRange = kPebbleRange;         // world units: pebbles drawn within this of the eye
constexpr float kMake = 44.0f;                 // ... blocks made within this (a block's box)
constexpr float kDrop = 52.0f;                 // ... and dropped beyond this
constexpr int kMakeAtOnce = 4;                 // blocks made at a time (on workers) at most
constexpr float kPerCell = 70.0f;              // pebbles a cell at density 1 (the coarsest gravel), RVK_Pebbles 1
// Three sizes, each drawn only as far as it shows (and sunk into the ground over the last quarter of that): grit to
// 16 units, small stones to 28, the bigger ones to kRange. Drawn out to kRange all, the far grit was a pixel or two of
// the scene's most expensive shading a stone.
// None casts into the sun's shadow map: the contact shadows (RVK_ContactOn) give each a short soft shadow against it,
// as a stone's is; the map's were sharp and black to their end - at a low sun streaks of several units.
constexpr float kGrit = 0.08f, kBig = 0.15f;    // across: under kGrit grit, from kBig the bigger ones
constexpr float kClassRange[3] = {16.0f, 28.0f, kRange};

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


// A pebble's shape: the icosahedron's 12 corners pushed in and out (a lump or two), its bottom cut flat; unit radius
// about, smooth normals (each corner's faces by their areas - a pebble is worn round).
struct PebbleShape {
    std::array<std::array<float, 3>, 12> p, n;
};
constexpr int kFaces[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                               {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                               {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};

PebbleShape MakePebble(uint32_t seed)
{
    const float t = 1.618034f;
    const float v[12][3] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                            {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    Rng rng{seed * 2654435761u + 7};
    float lump[2][4];
    for (auto& l : lump) {
        float d[3] = {rng() - 0.5f, rng() - 0.5f, rng() - 0.5f};
        const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
        l[0] = d[0] / n; l[1] = d[1] / n; l[2] = d[2] / n;
        l[3] = 0.15f + 0.25f * rng();
    }
    PebbleShape s;
    for (int i = 0; i < 12; ++i) {
        const float l = std::sqrt(v[i][0] * v[i][0] + v[i][1] * v[i][1] + v[i][2] * v[i][2]);
        float p[3] = {v[i][0] / l, v[i][1] / l, v[i][2] / l};
        float r = 0.85f + 0.25f * rng();
        for (const auto& k : lump) r += k[3] * std::max(p[0] * k[0] + p[1] * k[1] + p[2] * k[2] - 0.3f, 0.0f);
        s.p[i] = {p[0] * r, std::max(p[1] * r, -0.55f), p[2] * r};
        s.n[i] = {0.0f, 0.0f, 0.0f};
    }
    for (const auto& f : kFaces) {
        const auto &a = s.p[f[0]], &b = s.p[f[1]], &c = s.p[f[2]];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        // (Outward: away from the middle.)
        if (n[0] * (a[0] + b[0] + c[0]) + n[1] * (a[1] + b[1] + c[1]) + n[2] * (a[2] + b[2] + c[2]) < 0.0f)
            for (float& q : n) q = -q;
        for (int k = 0; k < 3; ++k)
            for (int q = 0; q < 3; ++q) s.n[f[k]][q] += n[q];
    }
    for (auto& n : s.n) {
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (float& q : n) q /= std::max(l, 1e-12f);
    }
    return s;
}

const std::array<PebbleShape, 8>& PebbleShapes()
{
    static const std::array<PebbleShape, 8> shapes = [] {
        std::array<PebbleShape, 8> out;
        for (uint32_t i = 0; i < out.size(); ++i) out[i] = MakePebble(i);
        return out;
    }();
    return shapes;
}

// How many pebbles a ground gets by its base tile's look (density 0..1, their size x), and what it was taken for.
struct Verdict {
    float density = 0.0f, size = 1.0f;
    const char* what = "none";
    bool sand = false;                         // loose sand (the ripples, terrain.glsl)
};
}  // namespace

// Which tiles get pebbles, how many, how big, and where in them (render thread, once a playfield): null while a tile it
// needs isn't uploaded yet.
std::shared_ptr<const Device::PebbleTable> Device::MakePebbleTable()
{
    const NativeGround& g = *m_native.ground;
    const size_t n = g.tiles.size();
    if (g.types.size() != n || !n) return nullptr;
    uint32_t types = 0;
    for (uint16_t t : g.types) types = std::max<uint32_t>(types, t + 1u);
    // Each type's base tile: the tile of its own number when that tile is of it (each playfield's first tiles are its
    // base grounds), else the type's most used tile.
    std::vector<int> base(types, -1);
    std::vector<uint32_t> used;
    for (uint32_t t = 0; t < types; ++t) {
        if (t < n && g.types[t] == t) { base[t] = int(t); continue; }
        if (used.empty()) {
            used.assign(n, 0);
            for (uint16_t code : g.codes)
                if ((code & 0x3FFFu) < n) ++used[code & 0x3FFFu];
        }
        uint32_t most = 0;
        for (size_t i = 0; i < n; ++i)
            if (g.types[i] == t && (base[t] < 0 || used[i] > most)) { base[t] = int(i); most = used[i]; }
    }
    // The hand table: randy-vk\terrain\pebbles.tsv - playfield (or *), type, density (0 none .. 1 gravel's), and an
    // optional size and sand (1: loose sand, rippled; 0 not); '#' starts a comment.
    struct Override { uint32_t playfield; bool any; uint32_t type; float density, size, sand; };
    std::vector<Override> overrides;
    if (!m_pebbleTablePath.empty())
        if (FILE* f = std::fopen(m_pebbleTablePath.c_str(), "r")) {
            char line[256];
            while (std::fgets(line, sizeof(line), f)) {
                char pf[32] = {};
                Override o{0, false, 0, 0.0f, 1.0f, -1.0f};
                if (line[0] == '#' ||
                    std::sscanf(line, "%31s %u %f %f %f", pf, &o.type, &o.density, &o.size, &o.sand) < 3)
                    continue;
                o.any = std::strcmp(pf, "*") == 0;
                o.playfield = o.any ? 0 : uint32_t(std::strtoul(pf, nullptr, 10));
                overrides.push_back(o);
            }
            std::fclose(f);
        }
    // (A texture's look is Device's to read.)
    auto classify = [](const Texture* t) {
        Verdict v;
        if (!t || !t->m_thumbValid || t->m_mean[0] < 0.0f) { v.what = "unknown"; return v; }
        const float r = t->m_mean[0] * 255.0f, g = t->m_mean[1] * 255.0f, b = t->m_mean[2] * 255.0f;
        const float hi = std::max({r, g, b}), lo = std::min({r, g, b});
        const float lum = 0.3f * r + 0.59f * g + 0.11f * b, sat = (hi - lo) / std::max(hi, 1.0f);
        if (g > r * 1.08f && g > b * 1.08f) { v.what = "grass"; return v; }
        if (t->m_line < 0.0f) { v.what = "unmeasured"; return v; }
        if (t->m_line > 0.3f || t->m_edge > 1.2f) { v.what = "slabs"; return v; }
        if (sat < 0.12f && (t->m_grain < 15.0f || (sat < 0.08f && lum > 150.0f))) { v.what = "smooth grey"; return v; }
        // Paved: a cool grey (bluer than it is red) - asphalt, a city square. Natural ground is warm (earth, gravel, sand);
        // Borealis's blue-grey square was dirt and strewn with stones (the user, 2026-10-19).
        if (b > r * 1.05f && sat < 0.25f) { v.what = "paved"; return v; }
        if (t->m_grain > 50.0f) { v.density = 0.3f; v.what = "straw"; return v; }
        // Sand: warm, saturated, bright, fine - the orange dune sand. The deserts' pale ground (Newland Desert's paths,
        // the Wastelands' flats) is packed hard, not loose: it gets no ripples and keeps the dirt's pebbles.
        if (r >= g && g >= b && t->m_grain < 32.0f && sat > 0.4f && lum > 128.0f) {
            // (Few stones lie on loose sand: the wind buries them. 0.25 strewed a dune field with them.)
            v.density = 0.05f; v.size = 0.6f; v.what = "sand"; v.sand = true;
            return v;
        }
        v.density = std::clamp(0.2f + (t->m_grain - 8.0f) / 30.0f, 0.2f, 1.0f) * (sat < 0.2f ? 1.0f : 0.7f);
        v.what = sat < 0.2f && t->m_grain > 25.0f ? "gravel" : "dirt";
        return v;
    };
    auto table = std::make_shared<PebbleTable>();
    std::vector<Verdict> verdicts(types);
    std::string said;
    for (uint32_t t = 0; t < types; ++t) {
        const Texture* tex = base[t] >= 0 ? g.tiles[size_t(base[t])] : nullptr;
        if (tex && !tex->m_thumbValid) return nullptr;   // (not uploaded yet: again later)
        verdicts[t] = classify(tex);
        for (const Override& o : overrides)
            if ((o.any || o.playfield == m_skyPlayfield) && o.type == t) {
                verdicts[t].density = std::clamp(o.density, 0.0f, 2.0f);
                verdicts[t].size = std::clamp(o.size, 0.2f, 3.0f);
                if (o.sand >= 0.0f) verdicts[t].sand = o.sand > 0.5f;
                verdicts[t].what = "by hand";
            }
        char one[64];
        std::snprintf(one, sizeof(one), "%s%u %s %.2f%s", t ? ", " : "", t, verdicts[t].what, verdicts[t].density,
                      verdicts[t].sand && verdicts[t].what != std::string("sand") ? " sand" : "");
        said += one;
    }
    Log("native ground: pebbles by surface type (playfield %u) - %s", m_skyPlayfield, said.c_str());
    table->density.assign(n, 0.0f);
    table->size.assign(n, 1.0f);
    table->sand.assign(n, 0.0f);
    table->mask.assign(n * 256, 0);
    uint32_t kept = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t type = g.types[i];
        const Verdict& v = verdicts[type];
        const Texture* tex = g.tiles[i];
        const Texture* bt = g.tiles[size_t(base[type])];
        if ((v.density <= 0.0f && !v.sand) || !tex || !bt || !tex->m_thumbValid || tex->m_thumbW != 16 || tex->m_thumbH != 16 ||
            bt->m_thumbW != 16 || bt->m_thumbH != 16)
            continue;
        // A tile of this ground with slabs in it (a plate set in gravel), or - type 0, the catch-all of roads and decals
        // - not the colour of its base ground: none.
        if (tex->m_line > 0.3f || tex->m_edge > 1.2f) continue;
        float apart = 0.0f;
        for (int c = 0; c < 3; ++c) apart += (tex->m_mean[c] - bt->m_mean[c]) * (tex->m_mean[c] - bt->m_mean[c]);
        if (type == 0 && v.what != std::string("by hand") && std::sqrt(apart) > 0.08f) continue;
        // Where in it: each texel of its 16 x 16 grid (smoothed over its neighbours) not green and near the base
        // tile's colour - its spread on the same grid.
        float mean[3] = {}, spread = 0.0f;
        for (int k = 0; k < 256; ++k)
            for (int c = 0; c < 3; ++c) mean[c] += bt->m_thumb[size_t(k) * 3 + c] / 256.0f;
        for (int c = 0; c < 3; ++c) {
            float s2 = 0.0f;
            for (int k = 0; k < 256; ++k) s2 += (bt->m_thumb[size_t(k) * 3 + c] - mean[c]) * (bt->m_thumb[size_t(k) * 3 + c] - mean[c]);
            spread += std::sqrt(s2 / 256.0f) / 3.0f;
        }
        const float unlike = std::max(40.0f, 2.0f * spread);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                float s[3] = {};
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = std::clamp(x + dx, 0, 15), sy = std::clamp(y + dy, 0, 15);
                        for (int c = 0; c < 3; ++c) s[c] += tex->m_thumb[size_t(sy * 16 + sx) * 3 + c] / 9.0f;
                    }
                const bool green = s[1] > s[0] * 1.04f && s[1] > s[2] * 1.08f;
                const float d = std::sqrt((s[0] - mean[0]) * (s[0] - mean[0]) + (s[1] - mean[1]) * (s[1] - mean[1]) +
                                          (s[2] - mean[2]) * (s[2] - mean[2]));
                table->mask[i * 256 + size_t(y) * 16 + x] = !green && d <= unlike;
            }
        if (v.sand) {
            uint32_t of = 0;
            for (int k = 0; k < 256; ++k) of += table->mask[i * 256 + size_t(k)];
            table->sand[i] = float(of) / 256.0f;
        }
        table->density[i] = v.density;
        table->size[i] = v.size;
        ++kept;
    }
    Log("native ground: pebbles on %u of %zu tiles", kept, n);
    return table;
}

// One block's pebbles (block (bx, bz) of kBlock cells a side), as static meshes of at most 65535 corners: those that
// cast shadows and those that don't. On a worker thread.
Device::ClutterBuild Device::BuildNativePebbles(std::shared_ptr<const NativeGround> ground, uint32_t bx, uint32_t bz,
                                                float density, std::shared_ptr<const PebbleTable> table)
{
    ClutterBuild out;
    const NativeGround& g = *ground;
    const PebbleTable& pt = *table;
    const uint32_t W = g.cellsX + 1;
    auto H = [&](int c, int r) {
        c = std::clamp(c, 0, int(g.cellsX));
        r = std::clamp(r, 0, int(g.cellsZ));
        return g.heights[size_t(r) * W + c];
    };
    const auto& shapes = PebbleShapes();
    std::vector<Vertex> verts[3];               // grit, small stones, the bigger ones (which cast)
    std::vector<uint16_t> idx[3];
    auto flush = [&](int cls) {
        if (verts[cls].empty()) return;
        auto data = std::make_shared<std::vector<uint8_t>>(verts[cls].size() * sizeof(Vertex));
        std::memcpy(data->data(), verts[cls].data(), data->size());
        auto ix = std::make_shared<std::vector<uint8_t>>(idx[cls].size() * 2);
        std::memcpy(ix->data(), idx[cls].data(), ix->size());
        NativeClutter piece;
        piece.vertices = data;
        piece.indices = ix;
        piece.count = uint32_t(verts[cls].size());
        piece.indexCount = uint32_t(idx[cls].size());
        piece.pebbles = true;
        piece.size = uint8_t(cls);
        out.pieces.push_back(piece);
        verts[cls].clear();
        idx[cls].clear();
    };
    // A trail (kept clear: walked, maintained): a strip of its own surface type at most 5 cells across - its
    // narrowest run of that type through the cell, along x, z or a diagonal - that covers at most half the 9 x 9 cells
    // round it (a strip, wandering or not, not the edge of a wide stretch). A desert's trails and the sandy paths
    // through Stret West Bank's grass; a broad stretch of the same sand keeps its pebbles.
    auto typeAt = [&](int c, int r) {
        c = std::clamp(c, 0, int(g.cellsX) - 1);
        r = std::clamp(r, 0, int(g.cellsZ) - 1);
        const uint32_t tile = g.codes[size_t(r) * g.cellsX + c] & 0x3FFFu;
        return tile < g.types.size() ? uint32_t(g.types[tile]) : 0xFFFFFFFFu;
    };
    auto trail = [&](int c, int r) {
        const uint32_t t = typeAt(c, r);
        const int dirs[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
        int narrowest = 99;
        for (const auto& d : dirs) {
            int run = 1;
            for (int s = -1; s <= 1; s += 2)
                for (int k = 1; k <= 5 && typeAt(c + s * k * d[0], r + s * k * d[1]) == t; ++k) ++run;
            narrowest = std::min(narrowest, run);
        }
        if (narrowest > 5) return false;
        int same = 0;
        for (int dz = -4; dz <= 4; ++dz)
            for (int dx = -4; dx <= 4; ++dx) same += typeAt(c + dx, r + dz) == t ? 1 : 0;
        return same <= 40;
    };
    density = std::clamp(density, 0.0f, 2.0f);
    for (uint32_t j = 0; j < kBlock && bz * kBlock + j < g.cellsZ; ++j)
        for (uint32_t i = 0; i < kBlock && bx * kBlock + i < g.cellsX; ++i) {
            const int c = int(bx * kBlock + i), r = int(bz * kBlock + j);
            const uint16_t code = g.codes[size_t(r) * g.cellsX + c];
            const uint32_t tile = code & 0x3FFFu, turn = code >> 14;
            if (tile >= pt.density.size() || pt.density[tile] <= 0.0f || trail(c, r)) continue;
            const float h00 = H(c, r), h10 = H(c + 1, r), h01 = H(c, r + 1), h11 = H(c + 1, r + 1);
            // (Not on slopes: a slope's stones roll off - the rocks' scree is there.)
            const float gx0 = (h10 + h11 - h00 - h01) / (2.0f * g.cellX), gz0 = (h01 + h11 - h00 - h10) / (2.0f * g.cellZ);
            const float keep = 1.0f - Smoothstep(0.3f, 0.6f, std::sqrt(gx0 * gx0 + gz0 * gz0));
            Rng rng{Hash(uint32_t(c), uint32_t(r), g.id ^ 0x5EB61E5u)};
            const float want = pt.density[tile] * density * kPerCell * keep;
            const int count = int(want) + (rng() < want - std::floor(want) ? 1 : 0);
            for (int k = 0; k < count; ++k) {
                const float fx = rng(), fz = rng(), u = rng(), s1 = rng(), s2 = rng(), s3 = rng(), yaw = rng() * 6.2831853f;
                const float big = rng(), shapeAt = rng();
                // Only where the tile's texel under it is this ground (turned as the cell's tile is: terrain.glsl
                // NativeTurn).
                const float tu[4] = {fx, 1.0f - fz, 1.0f - fx, fz}, tv[4] = {1.0f - fz, 1.0f - fx, fz, fx};
                const int mx = std::min(int(tu[turn] * 16.0f), 15), my = std::min(int(tv[turn] * 16.0f), 15);
                if (!pt.mask[size_t(tile) * 256 + size_t(my) * 16 + mx]) continue;
                // Sizes: mostly small, a few grit specks, now and then a bigger stone on gravel.
                float size = (0.035f + 0.2f * u * std::sqrt(u)) * pt.size[tile];
                if (pt.density[tile] >= 0.6f && big < 0.012f) size = 0.28f + 0.17f * s3;
                const float sx = 0.5f * size * (0.75f + 0.5f * s1), sz = 0.5f * size * (0.75f + 0.5f * s2),
                            sy = 0.5f * size * (0.55f + 0.35f * s3);
                const float y0 = NativeCellHeight(h00, h10, h01, h11, std::min(g.cellX, g.cellZ), fx, fz);
                // Lying with the ground under it.
                const float gx = ((h10 - h00) * (1 - fz) + (h11 - h01) * fz) / g.cellX;
                const float gz = ((h01 - h00) * (1 - fx) + (h11 - h10) * fx) / g.cellZ;
                float up[3] = {-gx, 1.0f, -gz};
                const float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
                for (float& q : up) q /= ul;
                float ax[3] = {std::cos(yaw), 0.0f, std::sin(yaw)};
                const float d = ax[0] * up[0] + ax[2] * up[2];
                for (int q = 0; q < 3; ++q) ax[q] -= d * up[q];
                const float al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                for (float& q : ax) q /= al;
                const float az[3] = {ax[1] * up[2] - ax[2] * up[1], ax[2] * up[0] - ax[0] * up[2], ax[0] * up[1] - ax[1] * up[0]};
                const float x = (float(c) + fx) * g.cellX, z = (float(r) + fz) * g.cellZ;
                const float lift = 0.3f * sy;   // (most of it out of the ground: its cut bottom just under)
                const float across = std::max(sx, sz) * 2.0f;
                const int cls = across >= kBig ? 2 : across >= kGrit ? 1 : 0;
                if (verts[cls].size() + 12 > 65535) flush(cls);
                const PebbleShape& shape = shapes[std::min(int(shapeAt * 8.0f), 7)];
                // (Set 0: where it lies in cell coordinates, at the middle of a 1/256 step - the same at all its corners
                // as the GPU interpolates it: its colour's hash and the ground's colour under it, terrain.glsl.)
                const float s0u = (std::floor((float(c) + fx) * 256.0f) + 0.5f) / 256.0f;
                const float s0v = (std::floor((float(r) + fz) * 256.0f) + 0.5f) / 256.0f;
                const uint16_t first = uint16_t(verts[cls].size());
                for (int e = 0; e < 12; ++e) {
                    const auto& o = shape.p[e];
                    const float lx = o[0] * sx, ly = o[1] * sy + lift, lz = o[2] * sz;
                    Vertex vx;
                    vx.x = x + ax[0] * lx + up[0] * ly + az[0] * lz;
                    vx.y = y0 + ax[1] * lx + up[1] * ly + az[1] * lz;
                    vx.z = z + ax[2] * lx + up[2] * ly + az[2] * lz;
                    const auto& on = shape.n[e];
                    const float nx = on[0] / sx, ny = on[1] / sy, nz = on[2] / sz;
                    float nn[3];
                    for (int q = 0; q < 3; ++q) nn[q] = ax[q] * nx + up[q] * ny + az[q] * nz;
                    const float nl = std::max(std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]), 1e-12f);
                    vx.nx = nn[0] / nl; vx.ny = nn[1] / nl; vx.nz = nn[2] / nl;
                    vx.u0 = s0u;
                    vx.v0 = s0v;
                    vx.u1 = vx.x / g.cellX / float(g.cellsX);
                    vx.v1 = vx.z / g.cellZ / float(g.cellsZ);
                    verts[cls].push_back(vx);
                }
                for (const auto& f : kFaces)
                    for (int e = 0; e < 3; ++e) idx[cls].push_back(uint16_t(first + f[e]));
                ++out.rocks;
            }
        }
    flush(0);
    flush(1);
    flush(2);
    return out;
}

// The table of which tiles get pebbles and which are sand, made once the playfield's tiles are in (tried every 30 calls
// till then). Also wanted without pebbles: the cells' sand (MakeNativeCodes) comes from it.
bool Device::EnsurePebbleTable()
{
    if (m_native.pebbleTable) return true;
    if (!m_native.ground || m_native.ground->types.empty() || m_native.pebbleTries++ % 30) return false;
    m_native.pebbleTable = MakePebbleTable();
    return m_native.pebbleTable != nullptr;
}

// The pebbles of the blocks near the eye and in view, for a pass (from DrawNativeGround, after the rocks); blocks made
// as the eye comes near, dropped behind it.
void Device::DrawNativePebbles(const d3d::Matrix& vp)
{
    if (m_native.pebbles <= 0.0f || !TerrainRockOn() || !m_native.ground || m_native.ground->types.empty()) return;
    if (!EnsurePebbleTable()) return;
    UpdateFrameEye();
    const NativeGround& g = *m_native.ground;
    const float bw = float(kBlock) * g.cellX, bh = float(kBlock) * g.cellZ;
    const uint32_t blocksX = (g.cellsX + kBlock - 1) / kBlock, blocksZ = (g.cellsZ + kBlock - 1) / kBlock;
    auto distance2 = [&](const float lo[3], const float hi[3]) {
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float d = std::max({lo[k] - m_frameEye[k], m_frameEye[k] - hi[k], 0.0f});
            d2 += d * d;
        }
        return d2;
    };
    // Dropped: far behind (not while one is being made - its future would wait here).
    int pending = 0;
    for (auto it = m_native.pebbleBlocks.begin(); it != m_native.pebbleBlocks.end();) {
        PebbleBlock& b = it->second;
        if (b.pending.valid() && b.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++pending;
            ++it;
        } else if (distance2(b.lo, b.hi) > kDrop * kDrop) {
            it = m_native.pebbleBlocks.erase(it);
        } else {
            ++it;
        }
    }
    const int x0 = std::max(int(std::floor((m_frameEye[0] - kMake) / bw)), 0);
    const int x1 = std::min(int(std::floor((m_frameEye[0] + kMake) / bw)), int(blocksX) - 1);
    const int z0 = std::max(int(std::floor((m_frameEye[2] - kMake) / bh)), 0);
    const int z1 = std::min(int(std::floor((m_frameEye[2] + kMake) / bh)), int(blocksZ) - 1);
    // Nearest first (made in that order).
    struct Near { float d2; uint32_t key; };
    std::vector<Near> close;
    for (int bz = z0; bz <= z1; ++bz)
        for (int bx = x0; bx <= x1; ++bx) {
            const uint32_t key = uint32_t(bz) * blocksX + uint32_t(bx);
            auto it = m_native.pebbleBlocks.find(key);
            if (it == m_native.pebbleBlocks.end()) {
                PebbleBlock b;
                b.lo[0] = float(bx) * bw;
                b.hi[0] = b.lo[0] + bw;
                b.lo[2] = float(bz) * bh;
                b.hi[2] = b.lo[2] + bh;
                b.lo[1] = 1e30f;
                b.hi[1] = -1e30f;
                for (uint32_t j = 0; j <= kBlock; ++j)
                    for (uint32_t i = 0; i <= kBlock; ++i) {
                        const uint32_t c = std::min(uint32_t(bx) * kBlock + i, g.cellsX), r = std::min(uint32_t(bz) * kBlock + j, g.cellsZ);
                        const float h = g.heights[size_t(r) * (g.cellsX + 1) + c];
                        b.lo[1] = std::min(b.lo[1], h);
                        b.hi[1] = std::max(b.hi[1], h);
                    }
                b.lo[1] -= 0.5f;
                b.hi[1] += 0.6f;
                if (distance2(b.lo, b.hi) > kMake * kMake) continue;
                it = m_native.pebbleBlocks.emplace(key, std::move(b)).first;
            }
            const float d2 = distance2(it->second.lo, it->second.hi);
            if (d2 <= kMake * kMake) close.push_back({d2, key});
        }
    std::sort(close.begin(), close.end(), [](const Near& a, const Near& b) { return a.d2 < b.d2; });
    m_native.clutter = true;
    for (const Near& nb : close) {
        PebbleBlock& b = m_native.pebbleBlocks[nb.key];
        if (!b.made) {
            if (!b.pending.valid()) {
                if (pending >= kMakeAtOnce) continue;
                b.pending = std::async(std::launch::async, BuildNativePebbles, m_native.ground, nb.key % blocksX,
                                       nb.key / blocksX, m_native.pebbles, m_native.pebbleTable);
                ++pending;
            }
            if (b.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            ClutterBuild built = b.pending.get();
            --pending;
            b.pieces = std::move(built.pieces);
            b.made = true;
            m_native.pebblesMade += built.rocks;
        }
        if (nb.d2 > kRange * kRange || b.pieces.empty() || BoxInClip(b.lo, b.hi, vp, true) < 0) continue;
        for (const NativeClutter& piece : b.pieces) {
            if (nb.d2 > kClassRange[piece.size] * kClassRange[piece.size]) continue;
            m_native.pebbleDraw = true;
            m_native.pebbleRange = kClassRange[piece.size];
            DrawSharedIndexed(d3d::TriangleList, kFvf, piece.vertices, 0, piece.count, piece.indices, piece.indexCount);
        }
    }
    m_native.pebbleDraw = false;
    m_native.clutter = false;
}

// RVK_Pebbles: how many (0 none, 1 as classified, 2 twice); every block made again.
void Device::SetNativeGroundPebbles(float density)
{
    if (density == m_native.pebbles) return;
    m_native.pebbles = density;
    DropNativePebbles(false);
}

// The pebbles' blocks dropped (made again as needed); and the table of which tiles get them, with `table` (a new
// playfield, or the hand table read again).
void Device::DropNativePebbles(bool table)
{
    for (auto& [key, b] : m_native.pebbleBlocks)
        if (b.pending.valid()) b.pending.wait();
    m_native.pebbleBlocks.clear();
    if (table) {
        m_native.pebbleTable.reset();
        m_native.pebbleTries = 0;
    }
}

// randy-vk\terrain\pebbles.tsv (or empty: none) - read when the table is made: at a playfield's ground, or now.
void Device::SetPebbleTablePath(const char* path)
{
    m_pebbleTablePath = path ? path : "";
    DropNativePebbles(true);
}

}  // namespace rvk
