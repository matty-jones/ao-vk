// Street portals' capture (docs/street-portal-plan.md phase 1): what a shop's doorway sees of the street, recorded to
// draw it from inside the shop through its entrance door.
//
// When a doorway walked through (its own link: portal_links.cpp) is near the camera (within 30) and on the screen
// (DrawPortals finds it), the static draws in front of it are recorded the first time each is drawn: those whose box
// lies within 100 of the doorway and inside 60 degrees of its outward facing - buildings and props; not characters,
// effects, the sky, water, plants' canopies (ao-vk draws their leaves) or the ground (ao-vk's native ground: a cut-out
// of its data, phase 2). Each piece as a room's (PortalRecordPiece), into a capture whose entrance is the doorway's sill
// centre and whose "inward" is the doorway's outward facing: the way out into the street. Saved as a room's capture
// (PortalCaptureSave) to <dir>\street-<playfield>-<x>-<z>.rvkp, merged with earlier visits, once quiet, every 5 s while
// pieces come, and once the doorway has been out of reach for 3 s or the playfield changes.
#include "internal.h"
#include "portal_file.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr float kNear = 30.0f;             // a doorway this near the camera is recorded from
constexpr float kSwitchNearer = 3.0f;      // ... another one recorded instead when it is this much nearer
constexpr float kRange = 100.0f, kCos = 0.5f;   // what it sees: this far, inside 60 degrees of its facing
constexpr double kQuiet = 1.5, kSaveEvery = 5.0, kGone = 3.0;
constexpr size_t kMostPieces = 8192;
constexpr size_t kMostBytes = 24u << 20;   // (a street holds more than a room: Phase 0 measures it)

double Seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// DrawPortals: a walked-through doorway near and on the screen - its street recorded (or still).
void Device::StreetCaptureNear(const PortalLink& link, const BlackQuad& q, const float out[3], float distance)
{
    if (distance > kNear || PortalDir().empty() || m_indoors)
        return;
    char name[96];
    std::snprintf(name, sizeof(name), "street-%u-%ld-%ld.rvkp", link.outdoor, std::lround(link.centre[0]),
                  std::lround(link.centre[2]));
    const std::string path = PortalDir() + name;
    const double now = Seconds();
    if (m_sc.on && m_sc.path == path) {
        m_sc.lastNear = now;
        m_sc.nearDistance = distance;
        return;
    }
    // Another walked-through doorway in reach while one is recorded: kept to the one being recorded unless this one is
    // clearly the nearer (two shop doors 35 apart in Borealis: recorded by turns, each restarted every frame).
    if (m_sc.on && now - m_sc.lastNear < 1.0 && distance > m_sc.nearDistance - kSwitchNearer)
        return;
    if (m_sc.on && (m_sc.unsaved || !m_sc.pending.empty()))
        m_scLeaving = std::move(m_sc);           // (another doorway's: saved at the next frame's start)
    m_sc = PortalCaptureState{};
    m_sc.on = true;
    m_sc.street = true;
    m_sc.path = path;
    m_sc.lastNear = now;
    m_sc.nearDistance = distance;
    std::memcpy(m_sc.apex, q.centre, sizeof(m_sc.apex));
    m_sc.dir[0] = out[0];
    m_sc.dir[2] = out[2];
    m_sc.capture = std::make_shared<portal::Capture>();
    portal::Capture& c = *m_sc.capture;
    c.playfield = link.outdoor;
    c.entranceKnown = true;
    c.entrance[0] = q.centre[0];
    c.entrance[1] = q.sill;
    c.entrance[2] = q.centre[2];
    c.inward[0] = out[0];                        // (out into the street: a street's capture is seen from its doorway)
    c.inward[2] = out[2];
    Log("street capture: the doorway of %s at (%.2f %.2f %.2f) into %u - its street recorded to %s",
        q.mesh[0] ? q.mesh : "an unnamed mesh", double(q.centre[0]), double(q.centre[1]), double(q.centre[2]),
        link.interior, path.c_str());
    StreetCaptureGround(q, out);
}

// The native ground in front of the doorway (phase 2): its cells under the cone's box, a few round it - heights, tile
// codes (their tiles renumbered among those used), the tiles' textures (read back with the pieces'), their surface
// types, and the hour's lightmap over them. The ground's passes' states come as the game draws it
// (StreetCaptureGroundPass).
void Device::StreetCaptureGround(const BlackQuad& q, const float out[3])
{
    if (!m_native.on || !m_native.ground)
        return;
    const NativeGround& g = *m_native.ground;
    float lo[2] = {q.centre[0], q.centre[2]}, hi[2] = {q.centre[0], q.centre[2]};
    for (int k = -4; k <= 4; ++k) {                  // (the cone's arc, every 15 degrees)
        const float a = float(k) * 0.2617994f, c = std::cos(a), s = std::sin(a);
        const float d[2] = {out[0] * c - out[2] * s, out[0] * s + out[2] * c};
        for (int j = 0; j < 2; ++j) {
            lo[j] = std::min(lo[j], (j ? q.centre[2] : q.centre[0]) + d[j] * kRange);
            hi[j] = std::max(hi[j], (j ? q.centre[2] : q.centre[0]) + d[j] * kRange);
        }
    }
    constexpr float kMargin = 8.0f;
    const int x0 = std::clamp(int(std::floor((lo[0] - kMargin) / g.cellX)), 0, int(g.cellsX));
    const int x1 = std::clamp(int(std::ceil((hi[0] + kMargin) / g.cellX)), 0, int(g.cellsX));
    const int z0 = std::clamp(int(std::floor((lo[1] - kMargin) / g.cellZ)), 0, int(g.cellsZ));
    const int z1 = std::clamp(int(std::ceil((hi[1] + kMargin) / g.cellZ)), 0, int(g.cellsZ));
    if (x1 - x0 < 2 || z1 - z0 < 2)
        return;
    portal::CapturedGround& cg = m_sc.capture->ground;
    cg = portal::CapturedGround{};
    cg.cellsX = uint32_t(x1 - x0);
    cg.cellsZ = uint32_t(z1 - z0);
    cg.cellX0 = uint32_t(x0);
    cg.cellZ0 = uint32_t(z0);
    cg.cellX = g.cellX;
    cg.cellZ = g.cellZ;
    const uint32_t W = g.cellsX + 1;
    for (int r = z0; r <= z1; ++r)
        for (int c = x0; c <= x1; ++c)
            cg.heights.push_back(g.heights[size_t(r) * W + c]);
    std::unordered_map<uint32_t, uint16_t> local;    // the playfield's tile -> the cut-out's
    for (int r = z0; r < z1; ++r)
        for (int c = x0; c < x1; ++c) {
            const uint16_t code = g.codes[size_t(r) * g.cellsX + c];
            const uint32_t tile = code & 0x3FFFu;
            auto [it, fresh] = local.emplace(tile, uint16_t(cg.tiles.size()));
            if (fresh) {
                Texture* t = tile < g.tiles.size() ? g.tiles[tile] : nullptr;
                cg.tiles.push_back(t && t->m_image && !t->m_renderTarget ? PortalTextureOf(m_sc, t) : -1);
                if (!g.types.empty())
                    cg.types.push_back(tile < g.types.size() ? g.types[tile] : 0);
            }
            cg.codes.push_back(uint16_t(it->second | (code & 0xC000u)));
        }
    StreetCaptureLight();
    m_sc.bytes += cg.heights.size() * 4 + cg.codes.size() * 2 + cg.lightW * cg.lightH * 4;
    Log("street capture: its ground - %u x %u cells from (%u, %u) of playfield %u, %zu tiles, the lightmap %s",
        cg.cellsX, cg.cellsZ, cg.cellX0, cg.cellZ0, g.id, cg.tiles.size(), cg.light.empty() ? "not yet" : "kept");
}

// The hour's lightmap's texels over the cut-out's cells (it lies over the playfield a texel a cell, or scaled to it) -
// when the street's ground is cut, or later (StreetCaptureGroundPass), should the hour's lightmap come after it.
void Device::StreetCaptureLight()
{
    portal::CapturedGround& cg = m_sc.capture->ground;
    if (!cg.light.empty() || !m_native.ground || m_native.lightPixels.empty() || !m_native.lightW || !m_native.lightH)
        return;
    const NativeGround& g = *m_native.ground;
    cg.lightW = cg.cellsX;
    cg.lightH = cg.cellsZ;
    cg.light.reserve(size_t(cg.lightW) * cg.lightH);
    for (uint32_t r = 0; r < cg.lightH; ++r)
        for (uint32_t c = 0; c < cg.lightW; ++c) {
            const uint32_t tx = std::min((cg.cellX0 + c) * m_native.lightW / g.cellsX, m_native.lightW - 1);
            const uint32_t ty = std::min((cg.cellZ0 + r) * m_native.lightH / g.cellsZ, m_native.lightH - 1);
            cg.light.push_back(m_native.lightPixels[size_t(ty) * m_native.lightW + tx]);
        }
}

// NativeGroundReplaces: the game's ground pass about to be replaced - its states, for the street's ground drawn
// again (the base pass's and the lighting pass's, once each).
void Device::StreetCaptureGroundPass(bool light)
{
    if (!m_sc.on || !m_sc.capture || m_indoors || m_external)
        return;
    portal::CapturedGround& cg = m_sc.capture->ground;
    if (cg.cellsX && light && cg.light.empty()) {
        StreetCaptureLight();
        if (!cg.light.empty() && cg.baseKnown && cg.lightKnown && !m_sc.unsaved++)
            m_sc.lastSave = std::max(m_sc.lastSave, m_sc.lastNew = Seconds());   // (saved again with it)
    }
    if (!cg.cellsX || (light ? cg.lightKnown : cg.baseKnown))
        return;
    if (light) {
        cg.lightRs = m_rs;
        cg.lightTss = m_tss;
        cg.material = m_material;
        cg.lights.clear();
        for (const LightSlot& slot : m_lights)
            if (slot.enabled && cg.lights.size() < 8)
                cg.lights.push_back(slot.light);
        cg.lightKnown = true;
    } else {
        cg.baseRs = m_rs;
        cg.baseTss = m_tss;
        cg.baseKnown = true;
    }
    if (cg.baseKnown && cg.lightKnown) {
        m_sc.lastNew = Seconds();
        if (!m_sc.unsaved++)
            m_sc.lastSave = std::max(m_sc.lastSave, m_sc.lastNew);
    }
}

// The street's ground from its capture (DrawStreetPortal, at its loading): a native ground of its own, made in
// m_streetNative - its patches, tile codes from the capture's textures, its lightmap - without rocks, pebbles or the
// sky's visibility.
bool Device::BuildStreetGround(const portal::Capture& cap, const std::vector<Texture*>& textures)
{
    DestroyStreetGround();
    const portal::CapturedGround& cg = cap.ground;
    if (!cg.cellsX || !cg.cellsZ || !cg.baseKnown || !cg.lightKnown)
        return false;
    auto g = std::make_shared<NativeGround>();
    g->id = cap.playfield;
    g->cellsX = cg.cellsX;
    g->cellsZ = cg.cellsZ;
    g->patch = 32;
    g->cellX = cg.cellX;
    g->cellZ = cg.cellZ;
    g->heights = cg.heights;
    g->codes = cg.codes;
    for (int32_t t : cg.tiles)
        g->tiles.push_back(t >= 0 && size_t(t) < textures.size() ? textures[size_t(t)] : nullptr);
    g->types = cg.types;
    std::swap(m_native, m_streetNative);
    m_native.on = true;
    m_native.rocks = 0.0f;
    m_native.pebbles = 0.0f;
    m_native.ao = 0.0f;
    m_native.sun = m_streetNative.sun;
    SetNativeGround(g);
    if (m_native.ground && !cg.light.empty()) {
        m_native.lightmap = CreateTexture(cg.lightW, cg.lightH, cg.light.data());
        m_native.lightPixels = cg.light;
        m_native.lightW = cg.lightW;
        m_native.lightH = cg.lightH;
        FitNativeLight();
    }
    const bool ok = m_native.ground != nullptr;
    std::swap(m_native, m_streetNative);
    return ok;
}

void Device::DestroyStreetGround()
{
    if (!m_streetNative.ground && !m_streetNative.lightmap && !m_streetNative.lightmapGrey)
        return;
    std::swap(m_native, m_streetNative);
    DestroyNativeGround();
    for (Texture** t : {&m_native.lightmap, &m_native.lightmapGrey})
        if (*t) { DestroyTexture(*t); *t = nullptr; }
    m_native.lightPixels.clear();
    m_native.lightW = m_native.lightH = 0;
    std::swap(m_native, m_streetNative);
}

// The street's ground into the opening (DrawStreetPortal, m_drawingPortal and its clip set): its two passes as the
// game drew them outside, its cells where they lay in the street, moved by `place` with the street's pieces.
void Device::DrawStreetGround(const portal::Capture& cap, const d3d::Matrix& place)
{
    if (!m_streetNative.ground)
        return;
    const portal::CapturedGround& cg = cap.ground;
    std::swap(m_native, m_streetNative);
    if (NativeMapsSignature() != m_native.mapsSignature)
        MakeNativeCodes();                       // (the settings or the tiles' maps changed)
    d3d::Matrix offset{};                        // (its cells from where the cut-out began)
    offset.m[0][0] = offset.m[1][1] = offset.m[2][2] = offset.m[3][3] = 1.0f;
    offset.m[3][0] = float(cg.cellX0) * cg.cellX;
    offset.m[3][2] = float(cg.cellZ0) * cg.cellZ;
    m_world = MulMatrix(offset, place);
    m_drawVisualKind = uint32_t(VisualKind::Terrain);
    m_material = cg.material;
    m_streetGround = true;
    for (const bool light : {false, true}) {
        m_rs = light ? cg.lightRs : cg.baseRs;
        m_tss = light ? cg.lightTss : cg.baseTss;
        std::fill(m_textures.begin(), m_textures.end(), nullptr);
        PortalSetLights(light ? cg.lights : std::vector<d3d::Light>{}, place);
        m_constantsDirty = true;
        DrawNativeGround(light);
    }
    m_streetGround = false;
    std::swap(m_native, m_streetNative);
}

// The draw path (not the portals' own draws): a static draw in front of the doorway being recorded from.
void Device::StreetCaptureDraw(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices,
                               uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    if (!m_sc.on || m_indoors || m_external || m_target != m_scene || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ ||
        !vertices || !vertexCount || !stride || IsTerrain(fvf))
        return;
    const auto kind = VisualKind(m_drawVisualKind);
    if ((kind != VisualKind::Static && kind != VisualKind::Unknown) || m_drawSkin || m_drawIsLabel || m_drawLeaves ||
        m_drawOwner || CharacterDraw(fvf, vertexCount) || m_sc.capture->pieces.size() >= kMostPieces ||
        m_sc.bytes >= kMostBytes)
        return;
    // Not the black texture's doorways and windows (from inside, the doorway's own quad would close the opening), nor
    // doors (opened and closed by the server; a room's capture leaves them out too).
    if (m_textures[0] && m_textures[0]->m_rdbId == 6014)
        return;
    if (m_drawMesh && m_drawMesh->geo) {
        const auto ids = MeshIdsOf(m_drawMesh->geo);
        if (ids.first != ids.second && std::strstr(MeshIdName(*ids.first), "door"))
            return;
    }
    // In front of the doorway: its box's centre within the range and the cone, give or take its size.
    const auto& w = m_world.m;
    float c[3] = {w[3][0], w[3][1], w[3][2]}, r = 0.0f;
    if (m_drawMesh) {
        float e2 = 0.0f;
        for (int j = 0; j < 3; ++j) {
            c[j] = w[3][j];
            float e = 0.0f;
            for (int i = 0; i < 3; ++i) {
                c[j] += 0.5f * (m_drawMesh->boundsMin[i] + m_drawMesh->boundsMax[i]) * w[i][j];
                e += 0.5f * (m_drawMesh->boundsMax[i] - m_drawMesh->boundsMin[i]) * std::fabs(w[i][j]);
            }
            e2 += e * e;
        }
        r = std::sqrt(e2);
    }
    const float v[3] = {c[0] - m_sc.apex[0], c[1] - m_sc.apex[1], c[2] - m_sc.apex[2]};
    const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (d - r > kRange || v[0] * m_sc.dir[0] + v[2] * m_sc.dir[2] < kCos * d - r)
        return;
    PortalRecordPiece(m_sc, kind, primitive, fvf, stride, vertices, vertexCount, indices, indexCount);
}

// PortalCaptureFrame (outside a frame): textures read back, saved when quiet, let go once the doorway is out of reach.
void Device::StreetCaptureFrame()
{
    if (m_scLeaving.on) {
        PortalCaptureReadBack(m_scLeaving);
        if (m_scLeaving.unsaved)
            PortalCaptureSave(m_scLeaving);
        m_scLeaving = PortalCaptureState{};
    }
    if (!m_sc.on)
        return;
    PortalCaptureReadBack(m_sc);
    const double now = Seconds();
    const bool gone = now - m_sc.lastNear > kGone;
    if (m_sc.unsaved && (gone || now - m_sc.lastNew >= kQuiet || now - m_sc.lastSave >= kSaveEvery))
        PortalCaptureSave(m_sc);
    if (gone)
        m_sc = PortalCaptureState{};
}

}  // namespace rvk
