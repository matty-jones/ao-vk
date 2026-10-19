// The ground's own texturing (terrain.glsl): how big each chunk's base texture lies on the world, so the shader can lay
// it at that size on the faces of cliffs (where the game's, laid from above, stretches into streaks) and hide its
// repeats.
#include "internal.h"
#include "material_maps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace rvk {

using namespace detail;

namespace {
#include "cliff_side_diff_1k.jpg.inc"
#include "cliff_side_nor_gl_1k.jpg.inc"
#include "cliff_side_arm_1k.jpg.inc"

// One repeat of the rock (a photo of 1.83 m of cliff) over this many world units: its layers about as thick as a
// canyon's look from the ground, not a hand's breadth - at 9 a canyon wall was rock wallpaper - and few enough repeats
// up a cliff that its bands don't stack into stripes.
constexpr float kRockUnits = 24.0f;
}  // namespace

namespace detail {

// One triangle's mapping: set 0 = a x + b z + o over its three vertices. False where its corners stand in a line
// seen from above (not in a heightmap).
static bool TriangleMapping(const float p[3][3], const float uv[3][2], float a[2], float b[2])
{
    float e1x = p[1][0] - p[0][0], e1z = p[1][2] - p[0][2], e2x = p[2][0] - p[0][0], e2z = p[2][2] - p[0][2];
    float det = e1x * e2z - e1z * e2x;
    if (std::fabs(det) < 1e-4f) return false;
    for (int k = 0; k < 2; ++k) {
        float d1 = uv[1][k] - uv[0][k], d2 = uv[2][k] - uv[0][k];
        a[k] = (d1 * e2z - d2 * e1z) / det;
        b[k] = (d2 * e1x - d1 * e2x) / det;
    }
    return true;
}

TerrainFit FitTerrain(const void* vertices, uint32_t vertexCount, const FvfLayout& layout, const uint16_t* indices,
                      uint32_t indexCount)
{
    TerrainFit fit;
    if (!vertices || layout.offset[0] < 0) return fit;
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    uint32_t count = indices ? indexCount : vertexCount;
    uint32_t step = std::max(1u, count / 4096);
    double ata[4][4] = {}, atb[4][4] = {};               // the normal equations; right sides u0 v0 u1 v1
    double sumSq[4] = {};
    float nyMin = 1e30f, nyMax = -1e30f, lenMin = 1e30f, lenMax = 0.0f;
    uint32_t used = 0;
    for (uint32_t k = 0; k < count; k += step) {
        uint32_t i = indices ? indices[k] : k;
        if (i >= vertexCount) continue;
        const uint8_t* p = v + size_t(i) * layout.stride;
        float pos[3], uv[4] = {}, n[3] = {};
        std::memcpy(pos, p + layout.offset[0], 12);
        if (layout.offset[4] >= 0) std::memcpy(uv, p + layout.offset[4], 8);
        if (layout.offset[5] >= 0) std::memcpy(uv + 2, p + layout.offset[5], 8);
        if (layout.offset[1] >= 0) std::memcpy(n, p + layout.offset[1], 12);
        double row[4] = {pos[0], pos[1], pos[2], 1.0};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) { ata[r][c] += row[r] * row[c]; atb[r][c] += row[r] * uv[c]; }
        for (int c = 0; c < 4; ++c) sumSq[c] += double(uv[c]) * uv[c];
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        lenMin = std::min(lenMin, len);
        lenMax = std::max(lenMax, len);
        if (len > 0.0f) { nyMin = std::min(nyMin, n[1] / len); nyMax = std::max(nyMax, n[1] / len); }
        ++used;
    }
    if (used < 4) return fit;
    // Gauss-Jordan with partial pivoting, the four right sides at once. Flat ground has no spread in y: its column is
    // left out (b = 0) rather than solved from nothing.
    double m[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) { m[r][c] = ata[r][c]; m[r][4 + c] = atb[r][c]; }
    double ySpread = ata[1][1] / used - (ata[1][3] / used) * (ata[1][3] / used);
    bool solveY = ySpread > 1e-4;
    if (!solveY) {
        for (int c = 0; c < 8; ++c) m[1][c] = 0.0;
        for (int r = 0; r < 4; ++r) m[r][1] = 0.0;
        m[1][1] = 1.0;
    }
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
        if (std::fabs(m[piv][c]) < 1e-9) return fit;  // (all the vertices in a line)
        for (int k = 0; k < 8; ++k) std::swap(m[c][k], m[piv][k]);
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            double s = m[r][c] / m[c][c];
            for (int k = 0; k < 8; ++k) m[r][k] -= s * m[c][k];
        }
    }
    for (int j = 0; j < 4; ++j) {
        double x[4];
        for (int i = 0; i < 4; ++i) x[i] = m[i][4 + j] / m[i][i];
        double r = sumSq[j];                             // the error: uv.uv - 2 x.atb + x.ata.x
        for (int i = 0; i < 4; ++i) {
            r -= 2.0 * x[i] * atb[i][j];
            for (int k = 0; k < 4; ++k) r += x[i] * ata[i][k] * x[k];
        }
        for (int i = 0; i < 4; ++i) fit.coef[j][i] = float(x[i]);
        fit.rms[j] = float(std::sqrt(std::max(r, 0.0) / used));
    }
    fit.normalY[0] = nyMin;
    fit.normalY[1] = nyMax;
    fit.normalLength[0] = lenMin;
    fit.normalLength[1] = lenMax;
    fit.vertices = used;

    // The triangles (a triangle list; up to ~2000 of them): each one's repeats per unit, the median, and how many
    // are that many repeats of the tile turned or flipped (both directions the same length, square to each other).
    uint32_t triCount = count / 3, triStep = std::max(1u, triCount / 2048);
    std::vector<float> scales;
    std::vector<float> lengths;                          // |a|, |b|, a.b of each, for the share
    scales.reserve(std::min(triCount, 2048u) + 1);
    for (uint32_t t = 0; t < triCount; t += triStep) {
        float p[3][3], uv[3][2];
        bool ok = true;
        for (int c = 0; c < 3 && ok; ++c) {
            uint32_t i = indices ? indices[3 * t + c] : 3 * t + c;
            if (i >= vertexCount) { ok = false; break; }
            const uint8_t* q = v + size_t(i) * layout.stride;
            std::memcpy(p[c], q + layout.offset[0], 12);
            if (layout.offset[4] >= 0) std::memcpy(uv[c], q + layout.offset[4], 8);
            else ok = false;
        }
        float a[2], b[2];
        if (!ok || !TriangleMapping(p, uv, a, b)) continue;
        float la = std::sqrt(a[0] * a[0] + a[1] * a[1]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1]);
        scales.push_back(std::sqrt(std::fabs(a[0] * b[1] - a[1] * b[0])));
        lengths.push_back(la);
        lengths.push_back(lb);
        lengths.push_back(a[0] * b[0] + a[1] * b[1]);
    }
    fit.triangles = uint32_t(scales.size());
    if (!scales.empty()) {
        std::vector<float> sorted = scales;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const float s = sorted[sorted.size() / 2];
        uint32_t square = 0;
        for (size_t k = 0; k < scales.size(); ++k) {
            float la = lengths[3 * k], lb = lengths[3 * k + 1], dot = lengths[3 * k + 2];
            if (std::fabs(la - s) < 0.02f * s && std::fabs(lb - s) < 0.02f * s && std::fabs(dot) < 0.02f * s * s)
                ++square;
        }
        fit.cellScale = s;
        fit.cellShare = float(square) / float(scales.size());
        fit.tiled = fit.cellShare >= 0.9f && s >= 1.0f / 64.0f;
    }
    return fit;
}

}  // namespace detail

// The chunk's fit, made once (the ground's chunks keep their vertices) and kept by TerrainChunkKey. A zone's worth is
// a few thousand; past that the cache starts over.
const TerrainFit& Device::TerrainFitFor(uint64_t key, const void* vertices, uint32_t vertexCount,
                                        const FvfLayout& layout, const uint16_t* indices, uint32_t indexCount)
{
    auto it = m_terrainFits.find(key);
    if (it != m_terrainFits.end()) return it->second;
    if (m_terrainFits.size() >= 16384) m_terrainFits.clear();
    TerrainFit fit = FitTerrain(vertices, vertexCount, layout, indices, indexCount);
    if (fit.tiled) ++m_terrainFitsAbove;
    else ++m_terrainFitsOther;
    // (The log: the counts now and then, and the first few chunks left as the game lays them.)
    uint32_t total = m_terrainFitsAbove + m_terrainFitsOther;
    if ((total & (total - 1)) == 0 && total >= 16)
        Log("terrain: %u ground chunks tiled from above, %u not (the far map, or not a tile)", m_terrainFitsAbove,
            m_terrainFitsOther);
    if (!fit.tiled && m_terrainFitsOther <= 5)
        Log("terrain: a chunk left as the game lays it (%u triangles): %.5f repeats per unit, %.0f%% of them a tile "
            "turned or flipped", fit.triangles, fit.cellScale, 100.0f * fit.cellShare);
    return m_terrainFits.emplace(key, fit).first->second;
}

// The draw's record: its tile's repeats per world unit (set 0 is drawn in world space - the ground's world matrix is
// the identity), its tile's mean colour (the rock on its cliffs is tinted to it) and which pass this is.
void Device::FillTerrainRecord(DrawTransform& dt, const TerrainFit& fit, uint32_t pass, const Texture* base) const
{
    if (!fit.tiled || !TerrainTexturingOn()) return;
    dt.terrain[0] = fit.cellScale;
    for (int c = 0; c < 3; ++c) dt.terrainAt[c] = base ? base->m_mean[c] : -1.0f;
    dt.terrainAt[3] = float(pass);
}

// ---------------------------------------------------------------- the cliffs' rock
// "Cliff Side" (Poly Haven, CC0; rvk/assets/terrain): albedo, normal map and occlusion / roughness / metallic, embedded
// as JPEGs, decoded and mipmapped off the render thread (rvk/material_maps.h), uploaded when in.

struct Device::RockMaps {
    maps::MipChain albedo, normal, orm;
    std::string error;
};

std::shared_ptr<Device::RockMaps> Device::DecodeRock()
{
    auto r = std::make_shared<RockMaps>();
    maps::Image albedo, normal, orm;
    if (!maps::LoadImageFromMemory(kRockAlbedo, sizeof(kRockAlbedo), &albedo, &r->error) ||
        !maps::LoadImageFromMemory(kRockNormal, sizeof(kRockNormal), &normal, &r->error) ||
        !maps::LoadImageFromMemory(kRockOrm, sizeof(kRockOrm), &orm, &r->error))
        return r;
    maps::NormalSpread spread;
    maps::MakeNormalChain(normal, &r->normal, &spread);
    maps::MakeAlbedoChain(albedo, &r->albedo);
    std::vector<float> packed(size_t(orm.w) * orm.h * 3);
    for (size_t i = 0; i < size_t(orm.w) * orm.h; ++i)
        for (int c = 0; c < 3; ++c) packed[i * 3 + c] = orm.rgba[i * 4 + c] / 255.0f;
    maps::MakeOrmChain(std::move(packed), orm.w, orm.h, spread, &r->orm);
    return r;
}

// At a frame's start (render thread): the decode started once the rock is wanted, its maps uploaded once it is done.
void Device::PollTerrainRock()
{
    if (m_rock.albedo || !m_terrainSettings.rock || m_terrainSettings.cliff <= 0.0f) return;
    if (!m_rock.started) {
        m_rock.started = true;
        m_rock.pending = std::async(std::launch::async, DecodeRock);
        return;
    }
    if (!m_rock.pending.valid() || m_rock.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    std::shared_ptr<RockMaps> r = m_rock.pending.get();
    if (!r->error.empty() || r->albedo.Empty()) {
        Log("terrain: the cliffs' rock couldn't be read: %s", r->error.c_str());
        return;
    }
    m_rock.normal = maps::Upload(*this, r->normal);
    m_rock.orm = maps::Upload(*this, r->orm);
    m_rock.albedo = maps::Upload(*this, r->albedo);
    const uint32_t mean = r->albedo.levels.back()[0];   // (the 1 x 1 level)
    for (int c = 0; c < 3; ++c) m_rock.mean[c] = float(mean >> (16 - 8 * c) & 0xFFu) / 255.0f;
    m_frameLightsDirty = true;
    Log("terrain: the cliffs' rock is in (%ux%u, %.1f MB with its mips; mean colour %.2f %.2f %.2f)", r->albedo.w,
        r->albedo.h, double(r->albedo.Bytes() + r->normal.Bytes() + r->orm.Bytes()) / (1024.0 * 1024.0),
        m_rock.mean[0], m_rock.mean[1], m_rock.mean[2]);
}

// The frame block's rock: its maps (or none), its size, mean colour and relief.
void Device::FillTerrainRockLights(float rock[4], float rock2[4])
{
    std::memset(rock, 0, 4 * sizeof(float));
    std::memset(rock2, 0, 4 * sizeof(float));
    if (!TerrainRockOn()) return;
    const uint32_t slots[3] = {BindlessImage(m_rock.albedo) + 1, BindlessImage(m_rock.normal) + 1,
                               BindlessImage(m_rock.orm) + 1};
    std::memcpy(rock, slots, sizeof(slots));
    rock[3] = kRockUnits;
    for (int c = 0; c < 3; ++c) rock2[c] = m_rock.mean[c];
    rock2[3] = m_normalMaps ? m_normalStrength : 0.0f;
}

}  // namespace rvk
