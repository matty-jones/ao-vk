// Shop portals, phase 0 (docs/shop-portal-plan.md): what a playfield is drawn as in the first half minute after
// arriving in it - each distinct piece (kind, mesh name, texture, size, place) when it is first drawn, a count each
// second, and the camera and the characters' heads at the first 3D frame. It tells whether a shop's interior comes
// whole from the entrance or only as it comes into view, and where the arrival puts the camera and the player.
// RANDYVK_PORTAL_DIAG=0: none of it.
#include "internal.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr double kWindow = 30.0;           // seconds after arriving
constexpr uint32_t kMaxPieceLines = 600;

double Seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool DiagOn()
{
    static const bool on = [] { const char* v = std::getenv("RANDYVK_PORTAL_DIAG"); return !(v && *v == '0'); }();
    return on;
}

const char* KindName(uint32_t kind)
{
    static const char* const kNames[] = {"unknown", "character", "character part", "static", "terrain", "room",
                                         "water", "sky", "blob shadow", "effect", "other"};
    return kind < sizeof(kNames) / sizeof(kNames[0]) ? kNames[kind] : "?";
}

}  // namespace

void Device::PortalDiagPlayfield(uint32_t playfield)
{
    if (!DiagOn())
        return;
    m_pd = {};
    m_pd.active = true;
    m_pd.playfield = playfield;
    m_pd.start = m_pd.lastSummary = Seconds();
    Log("portal diag: playfield %u - its pieces as they are first drawn, for %.0f s", playfield, kWindow);
}

// The draw path, once the draw's mesh info is known (DrawMeshInfo): a world-space piece of the scene, logged the first
// time it is drawn at its place.
void Device::PortalDiagDraw(uint32_t fvf, uint32_t vertexCount, uint32_t indexCount)
{
    if (!m_pd.active || m_external || m_target != m_scene || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ ||
        !vertexCount)
        return;
    const auto& w = m_world.m;
    const int32_t at[3] = {int32_t(std::lround(w[3][0] * 10.0f)), int32_t(std::lround(w[3][1] * 10.0f)),
                           int32_t(std::lround(w[3][2] * 10.0f))};
    const uint32_t texture = m_textures[0] ? m_textures[0]->m_rdbId : 0;
    uint64_t key = m_drawMesh ? m_drawMesh->indexHash : (uint64_t(vertexCount) << 32 | indexCount);
    for (int32_t c : at)
        key = key * 0x100000001B3ull ^ uint32_t(c);
    key = key * 0x100000001B3ull ^ texture;
    if (!m_pd.seen.insert(key).second)
        return;
    ++m_pd.pieces;
    ++m_pd.newSince;
    uint32_t kind = m_drawVisualKind;
    if (m_drawSkin || CharacterDraw(fvf, vertexCount))
        kind = uint32_t(VisualKind::Character);
    if (kind < 16) ++m_pd.byKind[kind];
    if (m_pd.lines >= kMaxPieceLines)
        return;
    ++m_pd.lines;
    const char* name = "-";
    if (m_drawMesh && m_drawMesh->geo) {
        const auto ids = MeshIdsOf(m_drawMesh->geo);
        if (ids.first != ids.second && *MeshIdName(*ids.first))
            name = MeshIdName(*ids.first);
    }
    // The piece's world box (its model box through the world matrix).
    float c[3] = {w[3][0], w[3][1], w[3][2]}, e[3] = {};
    if (m_drawMesh)
        for (int j = 0; j < 3; ++j) {
            c[j] = w[3][j];
            for (int i = 0; i < 3; ++i) {
                c[j] += 0.5f * (m_drawMesh->boundsMin[i] + m_drawMesh->boundsMax[i]) * w[i][j];
                e[j] += 0.5f * (m_drawMesh->boundsMax[i] - m_drawMesh->boundsMin[i]) * std::fabs(w[i][j]);
            }
        }
    const bool door = std::strstr(name, "door") != nullptr;
    Log("portal diag: +%5.0f ms %s %s, fvf %X, %u vertices, %u indices, texture %u, box (%.2f %.2f %.2f) +- (%.2f %.2f "
        "%.2f), lit %u%s",
        (Seconds() - m_pd.start) * 1000.0, KindName(kind), name, fvf, vertexCount, indexCount, texture, double(c[0]),
        double(c[1]), double(c[2]), double(e[0]), double(e[1]), double(e[2]), m_rs[d3d::RS_LIGHTING],
        door ? " - A DOOR" : "");
}

// EndFrame: the camera and the characters' heads at the first 3D frame, a count each second, the end of the window.
void Device::PortalDiagFrame()
{
    if (!m_pd.active)
        return;
    const double now = Seconds();
    auto camera = [&](const char* when) {
        if (!m_aoProjValid)
            return;
        const auto& v = m_aoView.m;
        float eye[3], fwd[3];
        for (int i = 0; i < 3; ++i) {
            eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
            fwd[i] = v[i][2];
        }
        char heads[256] = "";
        size_t used = 0;
        for (const FramePusher& p : m_framePushers) {
            if (used + 40 >= sizeof(heads)) break;
            used += size_t(std::snprintf(heads + used, sizeof(heads) - used, " (%.2f %.2f %.2f)", double(p.p[0]),
                                         double(p.p[1]), double(p.p[2])));
        }
        Log("portal diag: +%5.0f ms %s: camera at (%.2f %.2f %.2f) looking (%.3f %.3f %.3f); characters' heads%s",
            (now - m_pd.start) * 1000.0, when, double(eye[0]), double(eye[1]), double(eye[2]), double(fwd[0]),
            double(fwd[1]), double(fwd[2]), used ? heads : " none");
    };
    if (m_sceneSaw3D && !m_pd.cameraLogged) {
        m_pd.cameraLogged = true;
        camera("the first 3D frame");
    }
    if (now - m_pd.lastSummary >= 1.0) {
        m_pd.lastSummary = now;
        Log("portal diag: +%5.0f ms: %u pieces (%u new in the last second): %u rooms, %u statics, %u unknown, %u "
            "characters, %u effects, %u other",
            (now - m_pd.start) * 1000.0, m_pd.pieces, m_pd.newSince, m_pd.byKind[uint32_t(VisualKind::Room)],
            m_pd.byKind[uint32_t(VisualKind::Static)], m_pd.byKind[uint32_t(VisualKind::Unknown)],
            m_pd.byKind[uint32_t(VisualKind::Character)] + m_pd.byKind[uint32_t(VisualKind::CharacterPart)],
            m_pd.byKind[uint32_t(VisualKind::Effect)],
            m_pd.pieces - m_pd.byKind[uint32_t(VisualKind::Room)] - m_pd.byKind[uint32_t(VisualKind::Static)] -
                m_pd.byKind[uint32_t(VisualKind::Unknown)] - m_pd.byKind[uint32_t(VisualKind::Character)] -
                m_pd.byKind[uint32_t(VisualKind::CharacterPart)] - m_pd.byKind[uint32_t(VisualKind::Effect)]);
        m_pd.newSince = 0;
        if (m_sceneSaw3D)
            camera("now");
    }
    if (now - m_pd.start > kWindow) {
        Log("portal diag: playfield %u - done (%u pieces)", m_pd.playfield, m_pd.pieces);
        m_pd.active = false;
        m_pd.seen.clear();
    }
}

}  // namespace rvk
