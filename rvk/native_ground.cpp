// The ground drawn from the playfield's own data instead of the game's (docs/native-ground-plan.md, RVK_NativeGround).
//
// The game's ground (DisplaySystem.dll AnarchyGround_t) rebuilds its triangles on the game's thread as the camera
// moves and bakes everything past its near ring into blurry pictures. Here the playfield's heights and tiles (handed
// over by the proxy, rvk_ground.cpp) become static patches once, drawn in the game's own two passes - the base pass
// (each cell's own tile: terrain.glsl's native mode, through a tile-code texture) and the lighting pass (the hour's
// baked lightmap) - with the game's render state for each: the first ground draw of a pass in a frame draws all the
// patches in view for it, and every ground draw of the game's is skipped. Everything that reads the ground's draws
// (cliff faces, shadows, hill shade, mist, wet ground, fog) reads these the same way.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace detail;

namespace {

// The game's ground format: position, normal, two coordinate sets (IsTerrain).
constexpr uint32_t kFvf = d3d::FVF_XYZ | d3d::FVF_NORMAL | (2u << 8);
struct Vertex { float x, y, z, nx, ny, nz, u0, v0, u1, v1; };
static_assert(sizeof(Vertex) == 40, "vertex layout");

// The game's lightmap palette (AnarchyGround_t::SetShadowMap): 16 greys, (i / 15 x 188 + 4) x (1 - shade).
uint32_t PaletteGrey(uint32_t i, float shade)
{
    const float v = (float(i) / 15.0f * 188.0f + 4.0f) * std::clamp(1.0f - shade, 0.0f, 1.0f);
    const uint32_t g = uint32_t(std::clamp(v, 0.0f, 255.0f));
    return 0xFF000000u | g << 16 | g << 8 | g;
}

}  // namespace

void Device::DestroyNativeGround()
{
    for (Texture** t : {&m_native.codes, &m_native.lightmap})
        if (*t) { DestroyTexture(*t); *t = nullptr; }
    m_native.patches.clear();
    m_native.indices.reset();
    m_native.ground.reset();
}

// A playfield's ground: its patches (vertices, boxes), the shared indices, the tile-code texture. Null: the game's own.
void Device::SetNativeGround(std::shared_ptr<const NativeGround> ground)
{
    DestroyNativeGround();
    if (!ground || !ground->cellsX || !ground->cellsZ || !ground->patch ||
        ground->heights.size() != size_t(ground->cellsX + 1) * (ground->cellsZ + 1) ||
        ground->codes.size() != size_t(ground->cellsX) * ground->cellsZ) {
        if (ground) Log("native ground: playfield %u's data doesn't add up - the game's ground", ground->id);
        return;
    }
    const NativeGround& g = *ground;
    const uint32_t P = g.patch, W = g.cellsX + 1;
    const uint32_t pw = (g.cellsX + P - 1) / P, ph = (g.cellsZ + P - 1) / P;
    auto heightAt = [&](int c, int r) {
        c = std::clamp(c, 0, int(g.cellsX));
        r = std::clamp(r, 0, int(g.cellsZ));
        return g.heights[size_t(r) * W + c];
    };
    // Shared indices: (P x P) cells of two triangles; a patch at the playfield's edge has fewer cells, its vertices
    // still (P + 1)^2 (clamped to the edge) so the indices fit every patch.
    auto indices = std::make_shared<std::vector<uint8_t>>(size_t(P) * P * 6 * 2);
    uint16_t* ix = reinterpret_cast<uint16_t*>(indices->data());
    for (uint32_t j = 0; j < P; ++j)
        for (uint32_t i = 0; i < P; ++i) {
            const uint16_t a = uint16_t(j * (P + 1) + i), b = uint16_t(a + 1), c = uint16_t(a + P + 1), d = uint16_t(c + 1);
            const uint16_t q[6] = {a, c, b, b, c, d};   // (the game's winding: culling is off for the ground anyway)
            std::memcpy(ix, q, sizeof(q));
            ix += 6;
        }
    m_native.indices = indices;
    m_native.indexCount = P * P * 6;
    m_native.vertices = (P + 1) * (P + 1);
    for (uint32_t pz = 0; pz < ph; ++pz)
        for (uint32_t px = 0; px < pw; ++px) {
            auto data = std::make_shared<std::vector<uint8_t>>(size_t(m_native.vertices) * sizeof(Vertex));
            Vertex* v = reinterpret_cast<Vertex*>(data->data());
            NativePatch p{};
            p.lo[0] = p.lo[1] = p.lo[2] = 1e30f;
            p.hi[0] = p.hi[1] = p.hi[2] = -1e30f;
            for (uint32_t j = 0; j <= P; ++j)
                for (uint32_t i = 0; i <= P; ++i) {
                    const int c = int(std::min(px * P + i, g.cellsX)), r = int(std::min(pz * P + j, g.cellsZ));
                    Vertex& o = v[j * (P + 1) + i];
                    o.x = float(c) * g.cellX;
                    o.z = float(r) * g.cellZ;
                    o.y = heightAt(c, r);
                    // The normal from the heights round it (central differences).
                    const float dx = (heightAt(c + 1, r) - heightAt(c - 1, r)) / (2.0f * g.cellX);
                    const float dz = (heightAt(c, r + 1) - heightAt(c, r - 1)) / (2.0f * g.cellZ);
                    const float len = std::sqrt(dx * dx + dz * dz + 1.0f);
                    o.nx = -dx / len;
                    o.ny = 1.0f / len;
                    o.nz = -dz / len;
                    o.u0 = float(c);                                  // the cell coordinates (terrain.glsl)
                    o.v0 = float(r);
                    o.u1 = float(c) / float(g.cellsX);                // the playfield's lightmap: a texel a cell
                    o.v1 = float(r) / float(g.cellsZ);
                    const float pos[3] = {o.x, o.y, o.z};
                    for (int k = 0; k < 3; ++k) { p.lo[k] = std::min(p.lo[k], pos[k]); p.hi[k] = std::max(p.hi[k], pos[k]); }
                }
            p.vertices = data;
            m_native.patches.push_back(std::move(p));
        }
    // The tile codes: each cell's tile's bindless image (R, G) and its turn (B) - read by the base pass per pixel.
    std::vector<uint32_t> codes(size_t(g.cellsX) * g.cellsZ);
    uint32_t missing = 0;
    std::vector<uint32_t> image(g.tiles.size(), BindlessImage(nullptr));
    for (size_t t = 0; t < g.tiles.size(); ++t) {
        if (g.tiles[t] && g.tiles[t]->m_image) image[t] = BindlessImage(g.tiles[t]);
        else ++missing;
    }
    for (size_t k = 0; k < codes.size(); ++k) {
        const uint16_t code = g.codes[k];
        const uint32_t tile = code & 0x3FFFu, turn = code >> 14;
        const uint32_t img = tile < image.size() ? image[tile] : image.empty() ? 0u : image[0];
        codes[k] = 0xFF000000u | (img & 0xFFu) << 16 | ((img >> 8) & 0xFFu) << 8 | turn;
    }
    m_native.codes = CreateTexture(g.cellsX, g.cellsZ, codes.data());
    if (!m_native.lightmapGrey) {
        const uint32_t grey = PaletteGrey(15, 0.5f);
        m_native.lightmapGrey = CreateTexture(1, 1, &grey);
    }
    m_native.ground = ground;
    m_native.drawnBase = m_native.drawnLight = ~0ull;
    Log("native ground: playfield %u, %u x %u cells of %.1f x %.1f, %zu patches of %u, %zu tiles (%u without a "
        "texture) - drawn by rvk", g.id, g.cellsX, g.cellsZ, g.cellX, g.cellZ, m_native.patches.size(), P, g.tiles.size(),
        missing);
}

// The hour's baked lightmap, as the game hands it to its ground: unpacked through the game's palette, a texel a cell.
void Device::SetNativeGroundLightmap(std::shared_ptr<const std::vector<uint8_t>> packed, uint32_t w, uint32_t h,
                                     float shade)
{
    if (m_native.lightmap) {
        DestroyTexture(m_native.lightmap);
        m_native.lightmap = nullptr;
    }
    const uint32_t rowBytes = (w + 1) / 2;
    if (!packed || !w || !h || packed->size() < size_t(rowBytes) * h)
        return;
    uint32_t palette[16];
    for (uint32_t i = 0; i < 16; ++i) palette[i] = PaletteGrey(i, shade);
    std::vector<uint32_t> px(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t b = (*packed)[size_t(y) * rowBytes + x / 2];
            px[size_t(y) * w + x] = palette[(x & 1) ? (b & 15u) : (b >> 4)];
        }
    m_native.lightmap = CreateTexture(w, h, px.data());
    Log("native ground: the hour's lightmap, %u x %u, shade %.2f", w, h, shade);
}

// A draw of the game's: is it its ground, replaced (drawn by this first, for its pass, once a frame)? Skipped then.
bool Device::NativeGroundReplaces(uint32_t fvf)
{
    if (!m_native.on || !m_native.ground || !m_native.codes || m_native.drawing)
        return false;
    if (m_drawVisualKind != uint32_t(VisualKind::Terrain) || !IsTerrain(fvf))
        return false;
    const bool base = !m_rs[d3d::RS_LIGHTING] && !m_rs[d3d::RS_ALPHABLENDENABLE];
    const bool light = m_rs[d3d::RS_LIGHTING] && IsMultiplyPass();
    if (base && m_native.drawnBase != m_frameNumber) {
        m_native.drawnBase = m_frameNumber;
        DrawNativeGround(false);
    } else if (light && m_native.drawnLight != m_frameNumber) {
        m_native.drawnLight = m_frameNumber;
        DrawNativeGround(true);
    }
    return true;
}

// The patches in view for one pass, with the game's state for it: the base pass's stage 0 texture a stand-in (the
// shader reads each cell's tile), the lighting pass's the playfield's lightmap.
void Device::DrawNativeGround(bool lightPass)
{
    Texture* saved = m_textures[0];
    m_textures[0] = lightPass ? (m_native.lightmap ? m_native.lightmap : m_native.lightmapGrey) : m_blackTexture;
    m_native.drawing = true;
    // In view: a patch's box against the camera's clip planes (all eight corners outside one: left out).
    const d3d::Matrix vp = MulMatrix(m_view, m_proj);
    uint32_t drawn = 0;
    for (const NativePatch& p : m_native.patches) {
        bool outside[6] = {true, true, true, true, true, true};
        for (int k = 0; k < 8; ++k) {
            const float w[3] = {(k & 1) ? p.hi[0] : p.lo[0], (k & 2) ? p.hi[1] : p.lo[1], (k & 4) ? p.hi[2] : p.lo[2]};
            float c[4];
            for (int j = 0; j < 4; ++j)
                c[j] = w[0] * vp.m[0][j] + w[1] * vp.m[1][j] + w[2] * vp.m[2][j] + vp.m[3][j];
            outside[0] = outside[0] && c[0] < -c[3];
            outside[1] = outside[1] && c[0] > c[3];
            outside[2] = outside[2] && c[1] < -c[3];
            outside[3] = outside[3] && c[1] > c[3];
            outside[4] = outside[4] && c[2] < 0.0f;
            outside[5] = outside[5] && c[2] > c[3];
        }
        if (outside[0] || outside[1] || outside[2] || outside[3] || outside[4] || outside[5])
            continue;
        DrawSharedIndexed(d3d::TriangleList, kFvf, p.vertices, 0, m_native.vertices, m_native.indices, m_native.indexCount);
        ++drawn;
    }
    m_native.drawing = false;
    m_textures[0] = saved;
    if (!lightPass) {
        m_native.patchesDrawn += drawn;
        ++m_native.framesDrawn;
    }
}

}  // namespace rvk
