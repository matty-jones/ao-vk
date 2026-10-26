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

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr float kNear = 30.0f;             // a doorway this near the camera is recorded from
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
    if (m_sc.on && m_sc.path == path) {
        m_sc.lastNear = Seconds();
        return;
    }
    if (m_sc.on && (m_sc.unsaved || !m_sc.pending.empty()))
        m_scLeaving = std::move(m_sc);           // (another doorway's: saved at the next frame's start)
    m_sc = PortalCaptureState{};
    m_sc.on = true;
    m_sc.street = true;
    m_sc.path = path;
    m_sc.lastNear = Seconds();
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
