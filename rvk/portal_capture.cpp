// Shop portals' capture (docs/shop-portal-plan.md phase 1): a room's playfield recorded as the game draws it, to draw
// it later through its door from outside.
//
// From each playfield change on, every static draw of the scene - the rooms (VisualRoom_t) and the placed props; not
// characters, effects, the sky or anything carried - is recorded the first time it is drawn at its place: its vertices
// and indices, world matrix, textures, material, render and stage states and the lights the game enabled for it. Its
// textures are read back from the GPU between frames, once each. Doors are left out (a closed one would block the
// view in) but give the entrance: the door nearest the camera on arrival, its bottom centre and the way into the room.
// A playfield turns out to be a room's (rooms drawn, no terrain); after 3 s without a new piece its record is merged
// into <dir>\<playfield>.rvkp (earlier visits' pieces kept) - after 1.5 s without a new piece, every 5 s while new ones
// keep coming (props come into view as the player looks round), and at the next frame after leaving. Outdoors nothing
// is kept.
#include "internal.h"
#include "portal_file.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr double kQuiet = 1.5;             // seconds without a new piece before saving
constexpr double kSaveEvery = 5.0;         // ... or this long since the last save, with new pieces
constexpr size_t kMostPieces = 8192;       // a room's playfield: hundreds (a Fair Trade shop: ~320)
constexpr size_t kMostBytes = 16u << 20;   // ... and a few MB (a Fair Trade shop: 4.5): a dungeon's isn't kept whole

std::string g_portalDir;

double Seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool CaptureOn()
{
    static const bool on = [] { const char* v = std::getenv("RANDYVK_PORTAL_CAPTURE"); return !(v && *v == '0'); }();
    return on && !g_portalDir.empty();
}

}  // namespace

const std::string& PortalDir()
{
    return g_portalDir;
}

void SetPortalDir(const char* dir)
{
    g_portalDir = dir ? dir : "";
    if (!g_portalDir.empty() && g_portalDir.back() != '\\' && g_portalDir.back() != '/')
        g_portalDir += '\\';
}

void Device::PortalCapturePlayfield(uint32_t playfield)
{
    ++m_portalArrivals;
    // Leaving a room's playfield with pieces not yet saved: saved at the next frame's start (outside a frame, its last
    // textures read back first).
    if (m_pc.on && m_pc.roomsSeen && !m_pc.terrainSeen && (m_pc.unsaved || !m_pc.pending.empty()))
        m_pcLeaving = std::move(m_pc);
    m_pc = PortalCaptureState{};
    if (!CaptureOn() || !playfield)
        return;
    // Only a room walked into through a doorway (a link waits for it: portal_links.cpp), or one captured before - not
    // every dungeon and mission (one indoor playfield's capture came to 51 MB).
    const std::string path = g_portalDir + std::to_string(playfield) + ".rvkp";
    if (!m_portalPending.active && !std::filesystem::exists(path))
        return;
    m_pc.on = true;
    m_pc.capture = std::make_shared<portal::Capture>();
    m_pc.capture->playfield = playfield;
}

void Device::PortalForgetTexture(Texture* texture)
{
    // Gone before it was read back: its pieces go without it (PortalCaptureSave drops a texture with no pixels).
    for (PortalCaptureState* s : {&m_pc, &m_pcLeaving}) {
        auto it = s->textureIndex.find(texture);
        if (it == s->textureIndex.end())
            continue;
        s->textureIndex.erase(it);
        std::erase_if(s->pending, [&](const auto& p) { return p.first == texture; });
    }
}

// The draw path, once the draw's mesh info is known (DrawMeshInfo).
void Device::PortalCaptureDraw(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices,
                               uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    if (!m_pc.on || m_external || m_target != m_scene || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ)
        return;
    if (IsTerrain(fvf)) {
        m_pc.terrainSeen = true;                 // (an outdoor playfield: nothing of it is kept)
        return;
    }
    const auto kind = VisualKind(m_drawVisualKind);
    if (kind == VisualKind::Room)
        m_pc.roomsSeen = true;
    if (m_pc.terrainSeen || !vertices || !vertexCount || !stride || m_drawSkin || m_drawIsLabel || m_drawLeaves ||
        (kind != VisualKind::Room && kind != VisualKind::Static && kind != VisualKind::Unknown) ||
        CharacterDraw(fvf, vertexCount) || m_pc.capture->pieces.size() >= kMostPieces || m_pc.bytes >= kMostBytes)
        return;
    const auto& w = m_world.m;
    const char* name = "";
    if (m_drawMesh && m_drawMesh->geo) {
        const auto ids = MeshIdsOf(m_drawMesh->geo);
        if (ids.first != ids.second)
            name = MeshIdName(*ids.first);
    }
    if (std::strstr(name, "door")) {
        // A door: not kept, but a candidate for the entrance - its bottom centre, and the horizontal axis it is
        // thinnest along (in its model, turned by its world matrix): the way through it.
        if (!m_drawMesh || std::strstr(name, "blocker"))
            return;
        const float* mn = m_drawMesh->boundsMin;
        const float* mx = m_drawMesh->boundsMax;
        const int thin = (mx[0] - mn[0]) < (mx[2] - mn[2]) ? 0 : 2;
        float axis[3] = {w[thin][0], 0.0f, w[thin][2]};
        const float len = std::sqrt(axis[0] * axis[0] + axis[2] * axis[2]);
        if (len < 1e-4f)
            return;
        axis[0] /= len;
        axis[2] /= len;
        float centre[3], lowest = 1e30f;
        for (int j = 0; j < 3; ++j) {
            centre[j] = w[3][j];
            for (int i = 0; i < 3; ++i) centre[j] += 0.5f * (mn[i] + mx[i]) * w[i][j];
        }
        for (int corner = 0; corner < 8; ++corner) {
            float y = w[3][1];
            for (int i = 0; i < 3; ++i) y += (corner >> i & 1 ? mx[i] : mn[i]) * w[i][1];
            lowest = std::min(lowest, y);
        }
        PortalDoor door{{centre[0], lowest, centre[2]}, {axis[0], 0.0f, axis[2]}, {}};
        std::snprintf(door.name, sizeof(door.name), "%s", name);
        for (const PortalDoor& d : m_pc.doors) {
            const float dx = d.base[0] - door.base[0], dz = d.base[2] - door.base[2];
            if (dx * dx + dz * dz < 1.0f) return;   // (its other pieces, or the same door opening)
        }
        if (m_pc.doors.size() < 32)
            m_pc.doors.push_back(door);
        return;
    }
    const int32_t at[3] = {int32_t(std::lround(w[3][0] * 100.0f)), int32_t(std::lround(w[3][1] * 100.0f)),
                           int32_t(std::lround(w[3][2] * 100.0f))};
    const uint32_t rdb = m_textures[0] ? m_textures[0]->m_rdbId : 0;
    uint64_t key = m_drawMesh ? m_drawMesh->indexHash : portal::Hash(indices, size_t(indexCount) * 2);
    key = portal::Hash(at, sizeof(at), key ^ (uint64_t(vertexCount) << 32 | indexCount));
    key = portal::Hash(&rdb, sizeof(rdb), key);
    if (!m_pc.keys.insert(key).second)
        return;
    portal::CapturedPiece p;
    p.key = key;
    p.kind = uint32_t(kind);
    p.fvf = fvf;
    p.primitive = primitive;
    p.stride = stride;
    p.vertexCount = vertexCount;
    p.world = m_world;
    p.material = m_material;
    p.rs = m_rs;
    p.tss = m_tss;
    for (const LightSlot& slot : m_lights)
        if (slot.enabled && p.lights.size() < 8)
            p.lights.push_back(slot.light);
    p.vertices.assign(static_cast<const uint8_t*>(vertices),
                      static_cast<const uint8_t*>(vertices) + size_t(vertexCount) * stride);
    if (indices && indexCount)
        p.indices.assign(indices, indices + indexCount);
    for (int s = 0; s < 2; ++s) {
        Texture* t = m_textures[s];
        if (!t || t->m_renderTarget)
            continue;
        auto it = m_pc.textureIndex.find(t);
        if (it == m_pc.textureIndex.end()) {
            portal::CapturedTexture ct;
            ct.rdbId = t->m_rdbId;
            ct.width = t->m_width;
            ct.height = t->m_height;
            ct.format = t->m_format;
            const int32_t index = int32_t(m_pc.capture->textures.size());
            m_pc.capture->textures.push_back(std::move(ct));
            it = m_pc.textureIndex.emplace(t, index).first;
            m_pc.pending.emplace_back(t, index);
            m_pc.bytes += size_t(FormatRowBytes(t->m_format, t->m_width)) * FormatRows(t->m_format, t->m_height);
        }
        p.texture[s] = it->second;
    }
    // (The rooms' box: which side of the entrance the room is.)
    if (kind == VisualKind::Room && m_drawMesh) {
        float lowest = w[3][1];
        for (int j = 0; j < 3; ++j) {
            float c = w[3][j];
            for (int i = 0; i < 3; ++i) c += 0.5f * (m_drawMesh->boundsMin[i] + m_drawMesh->boundsMax[i]) * w[i][j];
            m_pc.roomCentre[j] += c;
        }
        for (int i = 0; i < 3; ++i)
            lowest += std::min(m_drawMesh->boundsMin[i] * w[i][1], m_drawMesh->boundsMax[i] * w[i][1]);
        m_pc.roomFloor = std::min(m_pc.roomFloor, lowest);
    }
    if (kind == VisualKind::Room) ++m_pc.roomPieces;
    m_pc.bytes += p.vertices.size() + p.indices.size() * 2;
    m_pc.capture->pieces.push_back(std::move(p));
    m_pc.lastNew = Seconds();
    if (!m_pc.unsaved++)
        m_pc.lastSave = std::max(m_pc.lastSave, m_pc.lastNew);   // (the 5 s count from the first unsaved piece)
}

// Textures' level 0 as stored (FormatRowBytes x FormatRows), read back now - outside a frame - in one submission
// and one wait.
bool Device::ReadTexturesLevel0(const std::vector<Texture*>& textures, std::vector<std::vector<uint8_t>*>& out)
{
    if (m_inFrame || textures.empty() || textures.size() != out.size())
        return false;
    struct Read { Texture* t; VkBuffer buffer; VmaAllocation allocation; VmaAllocationInfo info; size_t bytes; };
    std::vector<Read> reads;
    for (Texture* t : textures) {
        Read r{t, VK_NULL_HANDLE, nullptr, {}, 0};
        if (t && t->m_image) {
            r.bytes = size_t(FormatRowBytes(t->m_format, t->m_width)) * FormatRows(t->m_format, t->m_height);
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = r.bytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo ac{};
            ac.usage = VMA_MEMORY_USAGE_AUTO;
            ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            if (vmaCreateBuffer(m_allocator, &bi, &ac, &r.buffer, &r.allocation, &r.info) != VK_SUCCESS)
                r.buffer = VK_NULL_HANDLE;
        }
        reads.push_back(r);
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = m_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device, &ai, &cmd);
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &b);
    for (const Read& r : reads) {
        if (!r.buffer)
            continue;
        Texture* t = r.t;
        const VkImageLayout was = t->m_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                             : t->m_layout;
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {t->m_width, t->m_height, 1};
        Transition(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd, t->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.buffer, 1, &region);
        Transition(cmd, t, was);                 // (as the frames' draws expect it)
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_queue);
    m_completed = m_submitted;
    vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
    for (size_t i = 0; i < reads.size(); ++i) {
        const Read& r = reads[i];
        out[i]->clear();
        if (!r.buffer)
            continue;
        const auto* p = static_cast<const uint8_t*>(r.info.pMappedData);
        out[i]->assign(p, p + r.bytes);
        vmaDestroyBuffer(m_allocator, r.buffer, r.allocation);
    }
    return true;
}

// The start of a frame, before anything is recorded: the camera on arrival, textures read back, the capture saved
// once quiet.
void Device::PortalCaptureFrame()
{
    if (m_inFrame)
        return;
    if (m_pcLeaving.on) {
        PortalCaptureReadBack(m_pcLeaving);
        PortalCaptureSave(m_pcLeaving);
        m_pcLeaving = PortalCaptureState{};
    }
    PortalLinkFrame();                           // (a teleport's link, once its room is known: portal_links.cpp)
    if (!m_pc.on)
        return;
    if (!m_pc.arrivalSet && m_sceneSaw3D && m_aoProjValid) {   // (the last frame's: the first with 3D)
        const auto& v = m_aoView.m;
        for (int i = 0; i < 3; ++i) {
            m_pc.arrival[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
            m_pc.arrivalForward[i] = v[i][2];
        }
        m_pc.arrivalSet = true;
    }
    if (m_pc.terrainSeen) {                      // outdoors: let it go
        Log("portal capture: playfield %u has terrain - not a room's, nothing kept", m_pc.capture->playfield);
        m_pc = PortalCaptureState{};
        return;
    }
    PortalCaptureReadBack(m_pc);
    const double now = Seconds();
    if (m_pc.roomsSeen && m_pc.unsaved && (now - m_pc.lastNew >= kQuiet || now - m_pc.lastSave >= kSaveEvery))
        PortalCaptureSave(m_pc);
}

// The textures first used since the last frame, read back (one submission).
void Device::PortalCaptureReadBack(PortalCaptureState& s)
{
    if (s.pending.empty())
        return;
    std::vector<Texture*> textures;
    std::vector<std::vector<uint8_t>*> out;
    for (const auto& [texture, index] : s.pending) {
        textures.push_back(texture);
        out.push_back(&s.capture->textures[size_t(index)].level0);
    }
    if (ReadTexturesLevel0(textures, out))
        for (const auto& [texture, index] : s.pending) {
            portal::CapturedTexture& ct = s.capture->textures[size_t(index)];
            ct.hash = portal::Hash(ct.level0.data(), ct.level0.size(), uint64_t(ct.width) << 32 | ct.height);
        }
    s.pending.clear();
}

void Device::PortalCaptureSave(PortalCaptureState& s)
{
    portal::Capture& c = *s.capture;
    // The entrance: the door nearest the camera on arrival; into the room is towards the rooms' centre.
    if (!c.entranceKnown && s.arrivalSet && !s.doors.empty() && s.roomPieces) {
        const PortalDoor* best = nullptr;
        float bestD = 1e30f;
        for (const PortalDoor& d : s.doors) {
            const float dx = d.base[0] - s.arrival[0], dz = d.base[2] - s.arrival[2];
            if (dx * dx + dz * dz < bestD) { bestD = dx * dx + dz * dz; best = &d; }
        }
        float room[3];
        for (int j = 0; j < 3; ++j) room[j] = s.roomCentre[j] / float(s.roomPieces);
        float in[3] = {best->axis[0], 0.0f, best->axis[2]};
        if (in[0] * (room[0] - best->base[0]) + in[2] * (room[2] - best->base[2]) < 0.0f) {
            in[0] = -in[0];
            in[2] = -in[2];
        }
        std::memcpy(c.entrance, best->base, sizeof(c.entrance));
        std::memcpy(c.inward, in, sizeof(c.inward));
        c.entranceKnown = true;
        Log("portal capture: playfield %u's entrance: %s, %.1f from the camera on arrival, at (%.2f %.2f %.2f), into the "
            "room (%.3f 0 %.3f)",
            c.playfield, best->name, double(std::sqrt(bestD)), double(c.entrance[0]), double(c.entrance[1]),
            double(c.entrance[2]), double(in[0]), double(in[2]));
    } else if (!c.entranceKnown && s.arrivalSet && s.doors.empty() && s.roomPieces && s.roomFloor < 1e29f) {
        // No door named: where the camera arrived, on the rooms' floor, the way it looked.
        float in[3] = {s.arrivalForward[0], 0.0f, s.arrivalForward[2]};
        const float len = std::sqrt(in[0] * in[0] + in[2] * in[2]);
        if (len > 1e-3f) {
            c.entrance[0] = s.arrival[0];
            c.entrance[1] = s.roomFloor;
            c.entrance[2] = s.arrival[2];
            c.inward[0] = in[0] / len;
            c.inward[1] = 0.0f;
            c.inward[2] = in[2] / len;
            c.entranceKnown = true;
            Log("portal capture: playfield %u's entrance: no door seen - where the camera arrived, (%.2f %.2f %.2f), into "
                "the room (%.3f 0 %.3f)",
                c.playfield, double(c.entrance[0]), double(c.entrance[1]), double(c.entrance[2]), double(c.inward[0]),
                double(c.inward[2]));
        }
    }
    // Merged with what earlier visits saved: their pieces and textures kept, this visit's new ones added.
    const std::string path = g_portalDir + std::to_string(c.playfield) + ".rvkp";
    portal::Capture merged;
    const bool had = portal::Load(path.c_str(), merged) && merged.playfield == c.playfield;
    if (!had)
        merged = portal::Capture{c.playfield};
    if (!merged.entranceKnown && c.entranceKnown) {
        merged.entranceKnown = true;
        std::memcpy(merged.entrance, c.entrance, sizeof(merged.entrance));
        std::memcpy(merged.inward, c.inward, sizeof(merged.inward));
    }
    std::unordered_map<uint64_t, int32_t> byHash;
    for (size_t i = 0; i < merged.textures.size(); ++i)
        byHash.emplace(merged.textures[i].hash, int32_t(i));
    std::vector<int32_t> remap(c.textures.size(), -1);
    for (size_t i = 0; i < c.textures.size(); ++i) {
        const portal::CapturedTexture& t = c.textures[i];
        if (t.level0.empty())
            continue;                            // (gone before it was read back)
        auto it = byHash.find(t.hash);
        if (it == byHash.end()) {
            it = byHash.emplace(t.hash, int32_t(merged.textures.size())).first;
            merged.textures.push_back(t);
        }
        remap[i] = it->second;
    }
    std::unordered_set<uint64_t> keys;
    for (const portal::CapturedPiece& p : merged.pieces)
        keys.insert(p.key);
    uint32_t added = 0;
    for (const portal::CapturedPiece& p : c.pieces) {
        if (keys.count(p.key))
            continue;
        portal::CapturedPiece q = p;
        for (int32_t& t : q.texture)
            t = t >= 0 ? remap[size_t(t)] : -1;
        merged.pieces.push_back(std::move(q));
        ++added;
    }
    std::error_code made;
    std::filesystem::create_directories(g_portalDir, made);
    const bool saved = portal::Save(path.c_str(), merged);
    size_t bytes = 0;
    for (const auto& t : merged.textures) bytes += t.level0.size();
    for (const auto& p : merged.pieces) bytes += p.vertices.size() + p.indices.size() * 2;
    Log("portal capture: playfield %u: %u new pieces (%zu in all, %zu textures, %.1f MB)%s %s %s", c.playfield, added,
        merged.pieces.size(), merged.textures.size(), double(bytes) / 1048576.0, had ? " added to" : " written to",
        path.c_str(), saved ? "" : "- FAILED");
    s.unsaved = 0;
    s.lastSave = Seconds();
}

}  // namespace rvk
