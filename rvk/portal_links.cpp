// Shop portals' links (docs/shop-portal-plan.md phase 2): which doorway leads to which room's playfield, learned by
// walking through it.
//
// Behind a shop's door the building has a quad of the black texture (1010004:6014); its windows are more quads of it
// in the same draw. Each such draw is split once into its connected pieces (triangles sharing vertices) - each a quad
// in the world: its centre, facing, sill, width and height, and where it sits on its mesh. When a teleport begins
// (ZoneSignal), the doorway-sized one nearest the player, drawn in the last second, is the one walked through; the
// playfield arrived in, once it turns out to be a room's, is where it leads. The link is kept in <dir>\links.tsv, with
// the building's mesh and the doorway's place on it: the same doorway of the same building elsewhere (every Fair Trade
// shop of a kind) leads to the same room until its own first use says otherwise.
#include "internal.h"
#include "portal_file.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <string>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr uint32_t kBlackTexture = 6014;   // 1010004:6014 blacktexture.png
constexpr double kPendingFor = 30.0;       // a teleport's link waits this long for its room
constexpr float kNearPlayer = 8.0f;        // the doorway walked through: this near the player (horizontally) - a door's
                                           // trigger is ~6 in front of a Fair Trade shop's doorway

double Seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string LinksPath()
{
    const std::string dir = PortalDir();
    return dir.empty() ? std::string() : dir + "links.tsv";
}

}  // namespace

// The draw path: a draw of the black texture, split into its quads the first time it is drawn at its place.
void Device::PortalNoteBlack(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices,
                             uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    if (!m_textures[0] || m_textures[0]->m_rdbId != kBlackTexture || m_external || m_target != m_scene ||
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || primitive != d3d::TriangleList || !vertices || !indices ||
        indexCount < 3 || !vertexCount || PortalDir().empty())
        return;
    const auto& w = m_world.m;
    const int32_t at[3] = {int32_t(std::lround(w[3][0] * 100.0f)), int32_t(std::lround(w[3][1] * 100.0f)),
                           int32_t(std::lround(w[3][2] * 100.0f))};
    uint64_t key = m_drawMesh ? m_drawMesh->indexHash : portal::Hash(indices, size_t(indexCount) * 2);
    key = portal::Hash(at, sizeof(at), key ^ vertexCount);
    auto it = m_blackDraws.find(key);
    if (it != m_blackDraws.end()) {
        it->second.lastFrame = m_frameNumber;
        PortalReportLinks(it->second);
        return;
    }
    if (m_blackDraws.size() > 4096)
        m_blackDraws.clear();                    // (a long session: those near come back as they are drawn)
    BlackDraw& draw = m_blackDraws[key];
    draw.lastFrame = m_frameNumber;
    const char* mesh = "";
    uint16_t piece = 0;
    if (m_drawMesh && m_drawMesh->geo) {
        const auto ids = MeshIdsOf(m_drawMesh->geo);
        if (ids.first != ids.second) {
            mesh = MeshIdName(*ids.first);
            piece = ids.first->piece;
        }
    }
    // Its connected pieces: vertices joined through the triangles (union-find).
    std::vector<uint32_t> parent(vertexCount);
    std::iota(parent.begin(), parent.end(), 0u);
    auto root = [&](uint32_t v) {
        while (parent[v] != v) v = parent[v] = parent[parent[v]];
        return v;
    };
    const uint32_t triangles = indexCount / 3;
    for (uint32_t t = 0; t < triangles; ++t) {
        const uint32_t a = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
        parent[root(b)] = root(a);
        parent[root(c)] = root(a);
    }
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    auto model = [&](uint32_t v, float out[3]) { std::memcpy(out, base + size_t(v) * stride, 12); };
    auto world = [&](const float p[3], float out[3]) {
        for (int j = 0; j < 3; ++j) out[j] = p[0] * w[0][j] + p[1] * w[1][j] + p[2] * w[2][j] + w[3][j];
    };
    std::unordered_map<uint32_t, uint32_t> firstTriangle;   // a piece's root -> its first triangle
    for (uint32_t t = 0; t < triangles; ++t) {
        const uint32_t a = indices[t * 3];
        if (a < vertexCount) firstTriangle.emplace(root(a), t);
    }
    for (const auto& [r, t] : firstTriangle) {
        BlackQuad q;
        std::snprintf(q.mesh, sizeof(q.mesh), "%s", mesh);
        q.piece = piece;
        // Its facing: its first triangle's (world).
        float p0[3], p1[3], p2[3], m[3];
        model(indices[t * 3], m), world(m, p0);
        model(indices[t * 3 + 1], m), world(m, p1);
        model(indices[t * 3 + 2], m), world(m, p2);
        const float u[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const float v[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-8f) continue;
        for (float& x : n) x /= len;
        std::memcpy(q.normal, n, sizeof(n));
        // Across it: horizontal, in its plane.
        float across[3] = {-n[2], 0.0f, n[0]};
        const float al = std::sqrt(across[0] * across[0] + across[2] * across[2]);
        if (al > 1e-4f) { across[0] /= al; across[2] /= al; } else { across[0] = 1.0f; across[2] = 0.0f; }
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f}, mmn[3] = {1e30f, 1e30f, 1e30f},
              mmx[3] = {-1e30f, -1e30f, -1e30f}, amn = 1e30f, amx = -1e30f;
        for (uint32_t vtx = 0; vtx < vertexCount; ++vtx) {
            if (root(vtx) != r) continue;
            float mp[3], wp[3];
            model(vtx, mp);
            world(mp, wp);
            for (int j = 0; j < 3; ++j) {
                mn[j] = std::min(mn[j], wp[j]); mx[j] = std::max(mx[j], wp[j]);
                mmn[j] = std::min(mmn[j], mp[j]); mmx[j] = std::max(mmx[j], mp[j]);
            }
            const float a = wp[0] * across[0] + wp[2] * across[2];
            amn = std::min(amn, a);
            amx = std::max(amx, a);
        }
        for (int j = 0; j < 3; ++j) {
            q.centre[j] = 0.5f * (mn[j] + mx[j]);
            q.modelCentre[j] = 0.5f * (mmn[j] + mmx[j]);
        }
        q.sill = mn[1];
        q.height = mx[1] - mn[1];
        q.width = amx - amn;
        draw.quads.push_back(q);
    }
    PortalReportLinks(draw);
}

// Once each arrival in a playfield: the draw's doorways that links know (this one walked through, or the same one of the same
// building elsewhere), logged.
void Device::PortalReportLinks(BlackDraw& draw)
{
    // (A doorway leading to a room: the room is drawn behind it before the next draw - portal.cpp.)
    if (draw.linksGen != m_portalLinksGen) {
        draw.linksGen = m_portalLinksGen;
        draw.hasPortal = false;
        for (const BlackQuad& q : draw.quads)
            if (const PortalLink* link = PortalLinkFor(q, m_skyPlayfield); link && link->interior)
                draw.hasPortal = true;
    }
    if (draw.hasPortal && m_portalsOn)
        m_portalArmed = true;
    if (draw.reportedIn == m_portalArrivals)
        return;
    draw.reportedIn = m_portalArrivals;
    for (const BlackQuad& q : draw.quads)
        if (const PortalLink* link = PortalLinkFor(q, m_skyPlayfield); link && link->interior)
            Log("portal link: the doorway of %s at (%.2f %.2f %.2f) in playfield %u leads to %u (%s)",
                q.mesh[0] ? q.mesh : "an unnamed mesh", double(q.centre[0]), double(q.centre[1]), double(q.centre[2]),
                m_skyPlayfield, link->interior,
                link->outdoor == m_skyPlayfield && std::fabs(link->centre[0] - q.centre[0]) < 1.0f &&
                        std::fabs(link->centre[2] - q.centre[2]) < 1.0f
                    ? "walked through"
                    : "the same building's elsewhere");
}

// The link for a doorway: its own (this playfield, this place), else the same doorway of the same building (a link to a
// room, not one that led outdoors). Its own link may lead nowhere (interior 0): no portal.
const Device::PortalLink* Device::PortalLinkFor(const BlackQuad& q, uint32_t playfield)
{
    PortalLoadLinks();
    const PortalLink* same = nullptr;
    for (const PortalLink& l : m_portalLinks) {
        if (l.outdoor == playfield && std::fabs(l.centre[0] - q.centre[0]) < 1.0f &&
            std::fabs(l.centre[1] - q.centre[1]) < 1.0f && std::fabs(l.centre[2] - q.centre[2]) < 1.0f)
            return &l;
        if (!same && l.interior && q.mesh[0] && std::strcmp(l.mesh, q.mesh) == 0 && l.piece == q.piece &&
            std::fabs(l.modelCentre[0] - q.modelCentre[0]) < 0.05f &&
            std::fabs(l.modelCentre[1] - q.modelCentre[1]) < 0.05f &&
            std::fabs(l.modelCentre[2] - q.modelCentre[2]) < 0.05f)
            same = &l;
    }
    return same;
}

// A teleport begins (ZoneSignal): the doorway the player stands in, if any - its link waits for the playfield arrived in.
void Device::PortalLinkTeleport()
{
    if (PortalDir().empty() || m_indoors)
        return;                                  // (leaving a room: its own entrance isn't a doorway into anything)
    // The player: the characters' head nearest the camera, else the camera.
    float player[3] = {};
    bool have = false;
    if (m_aoProjValid) {
        const auto& v = m_aoView.m;
        for (int i = 0; i < 3; ++i) player[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
        have = true;
        float best = 1e30f, eye[3];
        std::memcpy(eye, player, sizeof(eye));
        for (const FramePusher& p : m_framePushers) {
            const float d = (p.p[0] - eye[0]) * (p.p[0] - eye[0]) + (p.p[1] - eye[1]) * (p.p[1] - eye[1]) +
                            (p.p[2] - eye[2]) * (p.p[2] - eye[2]);
            if (d < best) { best = d; std::memcpy(player, p.p, sizeof(player)); }
        }
    }
    if (!have)
        return;
    const BlackQuad* best = nullptr;
    float bestD = kNearPlayer * kNearPlayer, nearest = 1e30f;
    for (const auto& [key, draw] : m_blackDraws) {
        if (draw.lastFrame + 60 < m_frameNumber) continue;
        for (const BlackQuad& q : draw.quads) {
            if (q.height < 1.5f || q.width < 0.7f || q.height > 6.0f || q.width > 6.0f) continue;   // (not a window)
            const float dx = q.centre[0] - player[0], dz = q.centre[2] - player[2];
            nearest = std::min(nearest, dx * dx + dz * dz);
            if (dx * dx + dz * dz < bestD) { bestD = dx * dx + dz * dz; best = &q; }
        }
    }
    if (!best) {
        Log("portal link: a teleport from (%.1f %.1f %.1f) in playfield %u, no doorway near (the nearest drawn %s)",
            double(player[0]), double(player[1]), double(player[2]), m_skyPlayfield,
            nearest < 1e29f ? (std::to_string(std::sqrt(nearest)).substr(0, 5) + " away").c_str() : "none");
        return;
    }
    m_portalPending = PortalPending{};
    m_portalPending.active = true;
    m_portalPending.time = Seconds();
    m_portalPending.from = m_skyPlayfield;
    m_portalPending.quad = *best;
    // Its facing: out, towards the player.
    float* n = m_portalPending.quad.normal;
    if (n[0] * (player[0] - best->centre[0]) + n[2] * (player[2] - best->centre[2]) < 0.0f)
        for (int j = 0; j < 3; ++j) n[j] = -n[j];
    Log("portal link: a teleport through the doorway of %s at (%.2f %.2f %.2f) in playfield %u, %.1f from the player",
        best->mesh[0] ? best->mesh : "an unnamed mesh", double(best->centre[0]), double(best->centre[1]),
        double(best->centre[2]), m_skyPlayfield, double(std::sqrt(bestD)));
}

// The start of a frame (PortalCaptureFrame): a teleport's link made once the playfield arrived in is a room's.
void Device::PortalLinkFrame()
{
    PortalPending& p = m_portalPending;
    if (!p.active)
        return;
    if (Seconds() - p.time > kPendingFor) {
        p.active = false;
        return;
    }
    if (!m_pc.on || !m_pc.capture || m_pc.capture->playfield == p.from)
        return;
    // Outdoors (or nowhere a room's): kept as a link to nothing, so this doorway no longer takes another building's
    // room (an inherited link that was wrong).
    const bool outdoors = m_pc.terrainSeen;
    if (!outdoors && !m_pc.roomsSeen)
        return;
    PortalLink link{};
    link.outdoor = p.from;
    link.interior = outdoors ? 0u : m_pc.capture->playfield;
    std::memcpy(link.mesh, p.quad.mesh, sizeof(link.mesh));
    link.piece = p.quad.piece;
    std::memcpy(link.modelCentre, p.quad.modelCentre, sizeof(link.modelCentre));
    std::memcpy(link.centre, p.quad.centre, sizeof(link.centre));
    std::memcpy(link.normal, p.quad.normal, sizeof(link.normal));
    link.sill = p.quad.sill;
    link.width = p.quad.width;
    link.height = p.quad.height;
    p.active = false;
    PortalLoadLinks();
    std::erase_if(m_portalLinks, [&](const PortalLink& l) {
        return l.outdoor == link.outdoor && std::fabs(l.centre[0] - link.centre[0]) < 1.0f &&
               std::fabs(l.centre[2] - link.centre[2]) < 1.0f;
    });
    m_portalLinks.push_back(link);
    ++m_portalLinksGen;
    const bool saved = PortalSaveLinks();
    Log("portal link: the doorway of %s at (%.2f %.2f %.2f) in playfield %u leads to %u%s (learned; %zu links)%s",
        link.mesh[0] ? link.mesh : "an unnamed mesh", double(link.centre[0]), double(link.centre[1]),
        double(link.centre[2]), link.outdoor, m_pc.capture->playfield, outdoors ? ", outdoors: no portal" : "",
        m_portalLinks.size(), saved ? "" : " - NOT SAVED");
}

void Device::PortalLoadLinks()
{
    if (m_portalLinksLoaded)
        return;
    m_portalLinksLoaded = true;
    const std::string path = LinksPath();
    FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "r");
    if (!f)
        return;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        PortalLink l{};
        unsigned piece = 0;
        if (std::sscanf(line, "%u\t%u\t%47[^\t]\t%u\t%f %f %f\t%f %f %f\t%f %f %f\t%f\t%f\t%f", &l.outdoor, &l.interior,
                        l.mesh, &piece, &l.modelCentre[0], &l.modelCentre[1], &l.modelCentre[2], &l.centre[0],
                        &l.centre[1], &l.centre[2], &l.normal[0], &l.normal[1], &l.normal[2], &l.sill, &l.width,
                        &l.height) == 16) {
            l.piece = uint16_t(piece);
            if (std::strcmp(l.mesh, "-") == 0) l.mesh[0] = 0;
            m_portalLinks.push_back(l);
        }
    }
    std::fclose(f);
    ++m_portalLinksGen;
    Log("portal link: %zu links from %s", m_portalLinks.size(), path.c_str());
}

bool Device::PortalSaveLinks()
{
    const std::string path = LinksPath();
    if (path.empty())
        return false;
    std::error_code made;
    std::filesystem::create_directories(PortalDir(), made);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;
    std::fprintf(f, "# shop portals: doorways and the rooms they lead to (rvk portal_links.cpp)\n"
                    "# outdoor\tinterior\tmesh\tpiece\tcentre on the mesh\tcentre\toutward\tsill\twidth\theight\n");
    for (const PortalLink& l : m_portalLinks)
        std::fprintf(f, "%u\t%u\t%s\t%u\t%.3f %.3f %.3f\t%.3f %.3f %.3f\t%.4f %.4f %.4f\t%.3f\t%.3f\t%.3f\n", l.outdoor,
                     l.interior, l.mesh[0] ? l.mesh : "-", unsigned(l.piece), double(l.modelCentre[0]),
                     double(l.modelCentre[1]), double(l.modelCentre[2]), double(l.centre[0]), double(l.centre[1]),
                     double(l.centre[2]), double(l.normal[0]), double(l.normal[1]), double(l.normal[2]),
                     double(l.sill), double(l.width), double(l.height));
    return std::fclose(f) == 0;
}

}  // namespace rvk
