// Shop portals (docs/shop-portal-plan.md phase 3, RVK_Portals): the inside of a shop seen through its door.
//
// Once a doorway a link knows (portal_links.cpp) has been drawn, before the first draw after it that writes no depth -
// the game's effects and particles, which then go over the room; solid things come before those, and drawn after the
// room, those behind the doorway's plane (the building's own insides) showed in it - or at the scene's end (EndScene),
// each such doorway near enough and on the screen (the nearest three a frame) gets its room drawn behind it:
//   1. the scene's depth is copied, and where the doorway's rectangle is the surface seen (nothing in front of its
//      plane: not the door, the recess's walls or a character) the depth is pushed to the far plane (portal_open.frag);
//   2. the room's capture (portal_capture.cpp) is replayed through the draw path - each piece as the game drew it, moved
//      so the room's entrance stands on the doorway's sill facing in, lit by its own lights only (no sun, frame lights
//      or shadows), kept to the doorway's box on the screen; the scene's depth does the rest;
//   3. a shade over the opening dims it by day (RVK_PortalDim), as a room looks from a sunny street.
// Its pixels are marked lit by their own lamps, not the sun (ffp_main: D.clip), so the post passes' traced sun shadows
// and ambient occlusion leave them be; otherwise the post passes treat it as part of the scene. Captures are loaded when first needed and let go when unused.
#include "internal.h"
#include "portal_file.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr uint32_t kMostPortals = 3;       // drawn a frame (the nearest of those on the screen)
constexpr double kKeepFor = 30.0;          // a capture unused this long is let go
constexpr double kRetryAfter = 5.0;        // a capture that couldn't be loaded: tried again after this long
// The room is set this far nearer the street than its entrance's door: its entrance's wall and door frame stand just
// behind that door (a Fair Trade's 0.03 - 0.13; through a wider doorway its door hole's sides showed as lines, cut at
// 0.15 its frame still did), so they come out in front of the doorway's plane and are cut away there, while the floor
// still reaches the plane (cut deeper instead, a strip of black opened at the sill).
constexpr float kEntranceShift = 0.5f;

double Seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

d3d::Matrix Translation(float x, float y, float z)
{
    d3d::Matrix m{};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    m.m[3][0] = x;
    m.m[3][1] = y;
    m.m[3][2] = z;
    return m;
}

// About the vertical, row vectors: (0, 0, 1) turns to (sin a, 0, cos a).
d3d::Matrix TurnY(float a)
{
    d3d::Matrix m{};
    const float c = std::cos(a), s = std::sin(a);
    m.m[0][0] = c;  m.m[0][2] = -s;
    m.m[1][1] = 1.0f;
    m.m[2][0] = s;  m.m[2][2] = c;
    m.m[3][3] = 1.0f;
    return m;
}

d3d::Vector Apply(const d3d::Matrix& m, const d3d::Vector& p, float w)
{
    return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + w * m.m[3][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + w * m.m[3][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + w * m.m[3][2]};
}

}  // namespace

Device::LoadedPortal* Device::PortalLoad(uint32_t playfield)
{
    auto it = m_portals.find(playfield);
    if (it != m_portals.end()) {
        if (it->second.ok || Seconds() - it->second.tried < kRetryAfter) {
            it->second.lastUsed = Seconds();
            return it->second.ok ? &it->second : nullptr;
        }
        m_portals.erase(it);                     // (not loaded a while ago: it may have been captured since)
    }
    LoadedPortal& lp = m_portals[playfield];
    lp.lastUsed = lp.tried = Seconds();
    lp.capture = std::make_shared<portal::Capture>();
    const std::string path = PortalDir() + std::to_string(playfield) + ".rvkp";
    if (!portal::Load(path.c_str(), *lp.capture) || !lp.capture->entranceKnown) {
        if (m_portalMissingLogged.insert(playfield).second)
            Log("portal: no capture of playfield %u to draw yet (%s) - going in once records it", playfield, path.c_str());
        lp.capture.reset();
        return nullptr;
    }
    for (const portal::CapturedTexture& t : lp.capture->textures) {
        Texture* tex = t.level0.empty() ? nullptr : CreateTexture(t.width, t.height, t.format, 1);
        if (tex) {
            UpdateTexture(tex, 0, 0, 0, t.width, t.height, t.level0.data(), FormatRowBytes(t.format, t.width));
            tex->m_rdbId = t.rdbId;              // (its material maps and night glow as the game's own)
        }
        lp.textures.push_back(tex);
    }
    lp.ok = true;
    Log("portal: playfield %u's room loaded to draw through its doors (%zu pieces, %zu textures)", playfield,
        lp.capture->pieces.size(), lp.capture->textures.size());
    return &lp;
}

// A room just captured again (PortalCaptureSave): what was loaded of it goes, the new one is loaded when next drawn.
void Device::PortalForget(uint32_t playfield)
{
    auto it = m_portals.find(playfield);
    if (it == m_portals.end())
        return;
    for (Texture* t : it->second.textures)
        if (t) DestroyTexture(t);
    m_portals.erase(it);
    m_portalMissingLogged.erase(playfield);
}

void Device::PortalForgetUnused(bool all)
{
    const double now = Seconds();
    for (auto it = m_portals.begin(); it != m_portals.end();) {
        if (all || now - it->second.lastUsed > kKeepFor) {
            for (Texture* t : it->second.textures)
                if (t) DestroyTexture(t);
            it = m_portals.erase(it);
        } else {
            ++it;
        }
    }
}

void Device::DestroyPortals()
{
    PortalForgetUnused(true);
    for (Texture* t : m_street.lp.textures)
        if (t) DestroyTexture(t);
    m_street = StreetLoaded{};
    if (m_portalDepthCopy) {
        vkDestroyImageView(m_device, m_portalDepthCopyView, nullptr);
        vmaDestroyImage(m_allocator, m_portalDepthCopy, m_portalDepthCopyAllocation);
        m_portalDepthCopy = VK_NULL_HANDLE;
        m_portalDepthCopyView = VK_NULL_HANDLE;
        m_portalDepthCopyAllocation = nullptr;
    }
}

// The scene's depth into the portal's copy (the open pass reads it while writing the depth).
bool Device::PortalCopyDepth(VkCommandBuffer cmd)
{
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    if (!m_portalDepthCopy || m_portalDepthW != w || m_portalDepthH != h) {
        if (m_portalDepthCopy)
            m_deadImages.push_back({DeathTag(), {m_portalDepthCopy, m_portalDepthCopyView, m_portalDepthCopyAllocation}});
        m_portalDepthCopy = VK_NULL_HANDLE;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kDepthFormat;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(m_allocator, &ci, &ac, &m_portalDepthCopy, &m_portalDepthCopyAllocation, nullptr) != VK_SUCCESS) {
            m_portalDepthCopy = VK_NULL_HANDLE;
            return false;
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_portalDepthCopy;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kDepthFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vi, nullptr, &m_portalDepthCopyView);
        m_portalDepthW = w;
        m_portalDepthH = h;
        m_portalDepthCopyLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    m_depthLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ImageBarrier(cmd, m_portalDepthCopy, VK_IMAGE_ASPECT_DEPTH_BIT, m_portalDepthCopyLayout,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {w, h, 1};
    vkCmdCopyImage(cmd, m_depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_portalDepthCopy,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(cmd, m_portalDepthCopy, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_portalDepthCopyLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    m_depthLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    return true;
}

// The opening (portal_open.frag): the scene's depth copied (once a frame), then pushed to the far plane where the
// rectangle in `push` is the surface seen, inside `box`. The scene's rendering is ended and begun again.
bool Device::PortalOpenPass(VkCommandBuffer cmd, const float push[32], const VkRect2D& box, bool& copied)
{
    const float sw = float(m_scene->m_width), sh = float(m_scene->m_height);
    m_renderEndCause = kEndCopy;
    EndRendering();
    if (!copied && !PortalCopyDepth(cmd)) {
        BeginRenderingOn(m_scene);
        return false;
    }
    copied = true;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {m_scene->m_width, m_scene->m_height}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport viewport{0.0f, 0.0f, sw, sh, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &box);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_portalOpenPipeline);
    VkDescriptorImageInfo image{m_pointSampler, m_portalDepthCopyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &image;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomLayout, 0, 1, &wr);
    vkCmdPushConstants(cmd, m_bloomLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 * sizeof(float), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    m_cache = StateCache{};                      // (the pass bound its own pipeline and set 0)
    m_arenaBound = false;
    m_bindlessBound = false;
    m_constantsDirty = true;
    BeginRenderingOn(m_scene);
    return true;
}

// A capture's pieces replayed through the draw path, each as the game drew it, moved by `place` (m_drawingPortal,
// m_portalScissor and m_portalClip set by the caller, the device's state kept by it too). Returns how many were drawn.
uint32_t Device::PortalReplay(const portal::Capture& cap, const std::vector<Texture*>& textures, const d3d::Matrix& place)
{
    uint32_t drawn = 0;
    for (const portal::CapturedPiece& p : cap.pieces) {
        if (p.vertices.empty()) continue;
        m_rs = p.rs;
        m_tss = p.tss;
        std::fill(m_textures.begin(), m_textures.end(), nullptr);
        for (int s = 0; s < 2; ++s)
            m_textures[s] = p.texture[s] >= 0 ? textures[size_t(p.texture[s])] : nullptr;
        m_world = MulMatrix(p.world, place);
        m_material = p.material;
        m_lights.assign(p.lights.size(), LightSlot{});
        for (size_t i = 0; i < p.lights.size(); ++i) {
            LightSlot& slot = m_lights[i];
            slot.light = p.lights[i];
            slot.light.position = Apply(place, p.lights[i].position, 1.0f);
            slot.light.direction = Apply(place, p.lights[i].direction, 0.0f);
            slot.enabled = true;
            slot.cosHalfTheta = std::cos(slot.light.theta * 0.5f);
            slot.cosHalfPhi = std::cos(slot.light.phi * 0.5f);
        }
        ++m_lightsGen;
        m_drawVisualKind = p.kind;
        m_constantsDirty = true;
        Draw(p.primitive, p.fvf, p.vertices.data(), p.vertexCount, p.indices.empty() ? nullptr : p.indices.data(),
             uint32_t(p.indices.size()));
        ++drawn;
    }
    return drawn;
}

// The opening's pixels (as PortalOpenPass found them) marked in the local-light target as lit by their own lamps
// (portal_mark.frag): the passes after the scene - the street's sun shadows, ambient occlusion, reflections - leave
// them be (the traced occlusion darkened a patch of a room's floor: its own draws' mark didn't hold there).
void Device::PortalMarkPass(VkCommandBuffer cmd, const float push[32], const VkRect2D& box)
{
    if (!m_portalMarkPipeline || !m_localFraction)
        return;
    m_renderEndCause = kEndCopy;
    EndRendering();
    FullscreenPass(cmd, m_localFraction, m_portalMarkPipeline, m_portalDepthCopyView, m_portalDepthCopyView,
                   m_pointSampler, push, 32 * sizeof(float), true, VK_NULL_HANDLE, VK_NULL_HANDLE, &box);
    m_cache = StateCache{};
    m_arenaBound = false;
    m_bindlessBound = false;
    m_constantsDirty = true;
    BeginRenderingOn(m_scene);
}

// While the scene is being drawn (Draw: the first draw writing no depth after a linked doorway; EndScene): the rooms
// behind the linked doorways drawn so far this frame, not yet drawn behind.
void Device::DrawPortals()
{
    m_portalArmed = false;
    if (m_portalsFrame != m_frameNumber) {
        m_portalsFrame = m_frameNumber;
        m_portalsDrawn = 0;
    }
    if (!m_portalsOn || PortalDir().empty() || !m_scenePhase || m_target != m_scene || !m_aoProjValid ||
        !m_portalOpenPipeline || m_indoors) {
        if (!m_portals.empty()) PortalForgetUnused(false);
        return;
    }
    const auto& vm = m_aoView.m;
    float eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
    // The doorways drawn this frame that lead to a room, nearest first.
    const d3d::Matrix viewProj = MulMatrix(m_aoView, m_aoProj);
    const float sw = float(m_scene->m_width), sh = float(m_scene->m_height);
    struct Candidate {
        const BlackQuad* quad;
        uint32_t interior;
        float distance;
        float out[3];                            // its outward facing (horizontal, towards the camera's side)
        d3d::Vector origin, edgeU, edgeV;        // its rectangle: bottom-left corner, bottom and left edges
        VkRect2D box;                            // ... on the scene
    };
    // The doorway's rectangle and its box on the scene; false when none of it is on the screen (behind the camera -
    // zoomed out, the player's own doorway behind it took the frame's rooms - or outside the view).
    auto onScreen = [&](Candidate& c) {
        const BlackQuad& q = *c.quad;
        float out[3] = {q.normal[0], 0.0f, q.normal[2]};
        const float ol = std::sqrt(out[0] * out[0] + out[2] * out[2]);
        if (ol < 1e-4f) return false;
        out[0] /= ol;
        out[2] /= ol;
        if (out[0] * (eye[0] - q.centre[0]) + out[2] * (eye[2] - q.centre[2]) < 0.0f) {
            out[0] = -out[0];
            out[2] = -out[2];
        }
        std::memcpy(c.out, out, sizeof(out));
        const float across[3] = {-out[2], 0.0f, out[0]};
        const float half = 0.5f * q.width;
        c.origin = {q.centre[0] - across[0] * half, q.sill, q.centre[2] - across[2] * half};
        c.edgeU = {across[0] * q.width, 0.0f, across[2] * q.width};
        c.edgeV = {0.0f, q.height, 0.0f};
        float x0 = sw, y0 = sh, x1 = 0.0f, y1 = 0.0f;
        int behind = 0;
        for (int k = 0; k < 4; ++k) {
            const d3d::Vector p{c.origin.x + (k & 1 ? c.edgeU.x : 0.0f), c.origin.y + (k & 2 ? c.edgeV.y : 0.0f),
                                c.origin.z + (k & 1 ? c.edgeU.z : 0.0f)};
            const float* m = &viewProj.m[0][0];
            const float cx = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
            const float cy = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
            const float cw = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
            if (cw <= 0.05f) { ++behind; continue; }
            const float px = (cx / cw * 0.5f + 0.5f) * sw, py = (0.5f - cy / cw * 0.5f) * sh;
            x0 = std::min(x0, px); x1 = std::max(x1, px);
            y0 = std::min(y0, py); y1 = std::max(y1, py);
        }
        if (behind == 4)
            return false;                        // (all of it behind the camera)
        if (behind) { x0 = 0.0f; y0 = 0.0f; x1 = sw; y1 = sh; }   // (across the camera's plane: anywhere)
        x0 = std::clamp(std::floor(x0) - 2.0f, 0.0f, sw);
        y0 = std::clamp(std::floor(y0) - 2.0f, 0.0f, sh);
        x1 = std::clamp(std::ceil(x1) + 2.0f, 0.0f, sw);
        y1 = std::clamp(std::ceil(y1) + 2.0f, 0.0f, sh);
        if (x1 - x0 < 1.0f || y1 - y0 < 1.0f)
            return false;                        // (outside the view)
        c.box = {{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        return true;
    };
    std::vector<Candidate> candidates;
    for (const auto& [key, draw] : m_blackDraws) {
        if (draw.lastFrame != m_frameNumber) continue;
        for (const BlackQuad& q : draw.quads) {
            if (q.height < 1.5f || q.width < 0.7f || q.height > 6.0f || q.width > 6.0f) continue;
            if (q.drawnFrame == m_frameNumber) continue;   // (its room drawn already this frame)
            const PortalLink* link = PortalLinkFor(q, m_skyPlayfield);
            if (!link || !link->interior) continue;
            const float dx = q.centre[0] - eye[0], dy = q.centre[1] - eye[1], dz = q.centre[2] - eye[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            Candidate c{&q, link->interior, d, {}, {}, {}, {}, {}};
            if (d <= m_portalDist && onScreen(c)) {
                candidates.push_back(c);
                // (The reverse's phase 0: what this doorway sees of the street, if walked through - portal_diag.cpp.)
                if (d < 30.0f && link->outdoor == m_skyPlayfield && std::fabs(link->centre[0] - q.centre[0]) < 1.0f &&
                    std::fabs(link->centre[2] - q.centre[2]) < 1.0f) {
                    PortalStreetDiagStart(q, c.out, link->interior);
                    StreetCaptureNear(*link, q, c.out, d);   // (and recorded: portal_street.cpp)
                }
            }
        }
    }
    PortalForgetUnused(false);
    if (candidates.empty())
        return;
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    // (The nearest on the screen first: those drawn earlier in the frame count against kMostPortals.)
    if (candidates.size() > kMostPortals - std::min(m_portalsDrawn, kMostPortals))
        candidates.resize(kMostPortals - std::min(m_portalsDrawn, kMostPortals));
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    d3d::Matrix inverse;
    if (!InvertMatrix(viewProj, &inverse))
        return;
    bool copied = false;
    for (const Candidate& c : candidates) {
        LoadedPortal* lp = PortalLoad(c.interior);
        if (!lp)
            continue;
        c.quad->drawnFrame = m_frameNumber;
        ++m_portalsDrawn;
        const portal::Capture& cap = *lp->capture;
        const BlackQuad& q = *c.quad;
        const float* out = c.out;
        const d3d::Vector& origin = c.origin;
        const d3d::Vector& edgeU = c.edgeU;
        const d3d::Vector& edgeV = c.edgeV;
        const VkRect2D box = c.box;
        // The room's entrance onto the doorway's sill, its way in along the doorway's.
        const float turn = std::atan2(-out[0], -out[2]) - std::atan2(cap.inward[0], cap.inward[2]);
        const d3d::Matrix place = MulMatrix(MulMatrix(Translation(-cap.entrance[0], -cap.entrance[1], -cap.entrance[2]),
                                                      TurnY(turn)),
                                            Translation(q.centre[0] + out[0] * kEntranceShift, q.sill,
                                                        q.centre[2] + out[2] * kEntranceShift));
        // The doorway's rectangle for the opening and the mark (portal_open.frag).
        float push[32];
        std::memcpy(push, &inverse.m[0][0], 64);
        const float rest[16] = {origin.x, origin.y, origin.z, 0.0f, edgeU.x, edgeU.y, edgeU.z, 0.0f,
                                edgeV.x, edgeV.y, edgeV.z, 0.0f, sw, sh, m_aoProj.m[2][2], m_aoProj.m[3][2]};
        std::memcpy(push + 16, rest, sizeof(rest));
        // 1. The opening.
        if (!PortalOpenPass(cmd, push, box, copied))
            return;
        // 2. The room, as the game drew it, moved into the doorway.
        const auto rs = m_rs;
        const auto tss = m_tss;
        const auto textures = m_textures;
        const auto lights = m_lights;
        const d3d::Matrix world = m_world;
        const d3d::Material material = m_material;
        const uint32_t kind = m_drawVisualKind, visual = m_drawVisual, owner = m_drawOwner;
        m_drawVisual = 0;                        // (no game object's: not the last one the game drew)
        m_drawOwner = 0;
        m_drawingPortal = true;
        m_portalScissor = box;
        // Behind the doorway's plane only (the room's entrance, moved out in front of it, cut away: kEntranceShift).
        m_portalClip[0] = -out[0];
        m_portalClip[1] = 0.0f;
        m_portalClip[2] = -out[2];
        m_portalClip[3] = out[0] * q.centre[0] + out[2] * q.centre[2] - 0.01f;
        const uint32_t drawn = PortalReplay(cap, lp->textures, place);
        // 3. The shade over the opening: by day the room is dimmer than the street.
        const float day = std::clamp((m_cond.sunUp + 0.05f) / 0.2f, 0.0f, 1.0f);
        const float shade = (1.0f - std::clamp(m_portalDim, 0.0f, 1.0f)) * day;
        if (shade > 0.003f) {
            struct V { float x, y, z; uint32_t argb; };
            const float lift = 0.02f;            // (in front of the plane, towards the camera)
            const uint32_t argb = uint32_t(std::lround(shade * 255.0f)) << 24;
            V quad[4];                           // (a strip: bottom left, bottom right, top left, top right)
            for (int k = 0; k < 4; ++k)
                quad[k] = {origin.x + (k & 1 ? edgeU.x : 0.0f) + out[0] * lift, origin.y + (k & 2 ? edgeV.y : 0.0f),
                           origin.z + (k & 1 ? edgeU.z : 0.0f) + out[2] * lift, argb};
            m_rs[d3d::RS_LIGHTING] = 0;
            m_rs[d3d::RS_ALPHABLENDENABLE] = 1;
            m_rs[d3d::RS_SRCBLEND] = d3d::BLEND_SRCALPHA;
            m_rs[d3d::RS_DESTBLEND] = d3d::BLEND_INVSRCALPHA;
            m_rs[d3d::RS_ALPHATESTENABLE] = 0;
            m_rs[d3d::RS_ZENABLE] = 1;
            m_rs[d3d::RS_ZWRITEENABLE] = 0;
            m_rs[d3d::RS_ZFUNC] = d3d::CMP_LESSEQUAL;
            m_rs[d3d::RS_CULLMODE] = d3d::CULL_NONE;
            m_rs[d3d::RS_FOGENABLE] = 0;
            m_tss[0][d3d::TSS_COLOROP] = d3d::TOP_SELECTARG2;
            m_tss[0][d3d::TSS_COLORARG2] = d3d::TA_DIFFUSE;
            m_tss[0][d3d::TSS_ALPHAOP] = d3d::TOP_SELECTARG2;
            m_tss[0][d3d::TSS_ALPHAARG2] = d3d::TA_DIFFUSE;
            m_tss[1][d3d::TSS_COLOROP] = d3d::TOP_DISABLE;
            std::fill(m_textures.begin(), m_textures.end(), nullptr);
            m_world = Translation(0.0f, 0.0f, 0.0f);
            m_lights.clear();
            ++m_lightsGen;
            m_drawVisualKind = uint32_t(VisualKind::Effect);
            m_constantsDirty = true;
            std::fill(std::begin(m_portalClip), std::end(m_portalClip), 0.0f);   // (it stands in front of the plane)
            Draw(d3d::TriangleStrip, d3d::FVF_XYZ | d3d::FVF_DIFFUSE, quad, 4, nullptr, 0);
        }
        m_drawingPortal = false;
        // 4. The opening's pixels marked lit by the room's own lamps in the local-light target (portal_mark.frag): the
        // passes after the scene leave them be - not the street's sun shadows, ambient occlusion or reflections (the
        // traced occlusion darkened a patch of the floor: the room's own draws' mark didn't hold there).
        PortalMarkPass(cmd, push, box);
        m_rs = rs;
        m_tss = tss;
        m_textures = textures;
        m_lights = lights;
        ++m_lightsGen;
        m_world = world;
        m_material = material;
        m_drawVisualKind = kind;
        m_drawVisual = visual;
        m_drawOwner = owner;
        m_constantsDirty = true;
        const uint64_t logKey = uint64_t(c.interior) << 32 ^
                                uint32_t(std::lround(q.centre[0] * 10.0f) * 73856093 ^ std::lround(q.centre[2] * 10.0f));
        if (m_portalLogged.insert(logKey).second) {   // (once a doorway, each arrival in the playfield)
            Log("portal: the room of playfield %u drawn through the doorway of %s at (%.2f %.2f %.2f), %.1f away (%u "
                "pieces, the screen box %d,%d %ux%u)",
                c.interior, q.mesh[0] ? q.mesh : "an unnamed mesh", double(q.centre[0]), double(q.centre[1]),
                double(q.centre[2]), double(c.distance), drawn, box.offset.x, box.offset.y, box.extent.width,
                box.extent.height);
        }
    }
}

}  // namespace rvk

// The reverse: the street seen from inside a shop through its entrance door (docs/street-portal-plan.md phase 3,
// RVK_StreetView). At the scene's end indoors, in the room of a doorway walked in through (m_enteredFrom), with its
// street recorded (portal_street.cpp) and its entrance known (the room's capture): the door's opening at the entrance -
// its size the door's (the room capture's doors this visit), else 2.4 x 3.2 - is opened as a doorway is from the street
// (a closed door stands in front of it and keeps it shut); ao-vk's sky is drawn into it at the far plane; the street's
// pieces are replayed, moved so its doorway stands on the entrance facing out, kept on the street's side of the
// entrance's plane. Lit as recorded (the hour it was recorded at, for now), no ground or clouds yet (phases 2 and 4).
namespace rvk {

namespace {
constexpr float kStreetEntranceMargin = 1.05f;   // the opening a little wider and taller than the door's leaf
}  // namespace

void Device::DrawStreetPortal()
{
    if (!m_streetViewOn || !m_portalsOn || !m_indoors || !m_enteredValid || m_enteredFrom.interior != m_skyPlayfield ||
        !m_scenePhase || m_target != m_scene || !m_aoProjValid || !m_portalOpenPipeline || PortalDir().empty())
        return;
    // The room's entrance (its capture's header: read again each arrival and when the room's capture is saved - on the
    // first visit it is only written on leaving).
    if (m_streetRoom.playfield != m_skyPlayfield) {
        m_streetRoom = StreetRoom{};
        m_streetRoom.playfield = m_skyPlayfield;
        portal::Capture header;
        const std::string path = PortalDir() + std::to_string(m_skyPlayfield) + ".rvkp";
        m_streetRoom.known = portal::LoadHeader(path.c_str(), header) && header.entranceKnown;
        std::memcpy(m_streetRoom.entrance, header.entrance, sizeof(m_streetRoom.entrance));
        std::memcpy(m_streetRoom.inward, header.inward, sizeof(m_streetRoom.inward));
    }
    if (!m_streetRoom.known)
        return;
    // The street (loaded once a doorway; tried again a while after a failure).
    char name[96];
    std::snprintf(name, sizeof(name), "street-%u-%ld-%ld.rvkp", m_enteredFrom.outdoor, std::lround(m_enteredFrom.centre[0]),
                  std::lround(m_enteredFrom.centre[2]));
    const std::string path = PortalDir() + name;
    if (m_street.path != path || (!m_street.lp.ok && Seconds() - m_street.lp.tried > kRetryAfter)) {
        for (Texture* t : m_street.lp.textures)
            if (t) DestroyTexture(t);
        m_street = StreetLoaded{};
        m_street.path = path;
        m_street.lp.tried = Seconds();
        auto cap = std::make_shared<portal::Capture>();
        if (portal::Load(path.c_str(), *cap) && cap->entranceKnown) {
            for (const portal::CapturedTexture& t : cap->textures) {
                Texture* tex = t.level0.empty() ? nullptr : CreateTexture(t.width, t.height, t.format, 1);
                if (tex) {
                    UpdateTexture(tex, 0, 0, 0, t.width, t.height, t.level0.data(), FormatRowBytes(t.format, t.width));
                    tex->m_rdbId = t.rdbId;
                }
                m_street.lp.textures.push_back(tex);
            }
            m_street.lp.capture = cap;
            m_street.lp.ok = true;
            Log("street: %s loaded to draw through playfield %u's entrance (%zu pieces, %zu textures)", name,
                m_skyPlayfield, cap->pieces.size(), cap->textures.size());
        } else {
            Log("street: no %s yet - standing at that doorway outside records it", name);
        }
    }
    if (!m_street.lp.ok)
        return;
    const portal::Capture& street = *m_street.lp.capture;
    // The opening: the door's (the one at the entrance this visit), out of the room.
    const float* e = m_streetRoom.entrance;
    float in[3] = {m_streetRoom.inward[0], 0.0f, m_streetRoom.inward[2]};
    float width = 2.4f, height = 3.2f;
    for (const PortalDoor& d : m_pc.doors) {
        const float dx = d.base[0] - e[0], dz = d.base[2] - e[2];
        if (dx * dx + dz * dz < 1.5f * 1.5f && d.width > 0.5f && d.height > 1.0f) {
            width = d.width;
            height = d.height;
        }
    }
    width *= kStreetEntranceMargin;
    height *= kStreetEntranceMargin;
    const float out[3] = {-in[0], 0.0f, -in[2]};
    const auto& vm = m_aoView.m;
    float eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
    const float toEye[3] = {eye[0] - e[0], eye[1] - e[1], eye[2] - e[2]};
    if (toEye[0] * in[0] + toEye[2] * in[2] <= 0.0f ||
        toEye[0] * toEye[0] + toEye[1] * toEye[1] + toEye[2] * toEye[2] > m_portalDist * m_portalDist)
        return;                                  // (the camera outside the room's side of it, or too far)
    const float across[3] = {-out[2], 0.0f, out[0]};
    const d3d::Vector origin{e[0] - across[0] * 0.5f * width, e[1], e[2] - across[2] * 0.5f * width};
    const d3d::Vector edgeU{across[0] * width, 0.0f, across[2] * width};
    const d3d::Vector edgeV{0.0f, height, 0.0f};
    const d3d::Matrix viewProj = MulMatrix(m_aoView, m_aoProj);
    const float sw = float(m_scene->m_width), sh = float(m_scene->m_height);
    float x0 = sw, y0 = sh, x1 = 0.0f, y1 = 0.0f;
    int behind = 0;
    for (int k = 0; k < 4; ++k) {
        const d3d::Vector p{origin.x + (k & 1 ? edgeU.x : 0.0f), origin.y + (k & 2 ? edgeV.y : 0.0f),
                            origin.z + (k & 1 ? edgeU.z : 0.0f)};
        const float* m = &viewProj.m[0][0];
        const float cx = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
        const float cy = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
        const float cw = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
        if (cw <= 0.05f) { ++behind; continue; }
        const float px = (cx / cw * 0.5f + 0.5f) * sw, py = (0.5f - cy / cw * 0.5f) * sh;
        x0 = std::min(x0, px); x1 = std::max(x1, px);
        y0 = std::min(y0, py); y1 = std::max(y1, py);
    }
    if (behind == 4)
        return;
    if (behind) { x0 = 0.0f; y0 = 0.0f; x1 = sw; y1 = sh; }
    x0 = std::clamp(std::floor(x0) - 2.0f, 0.0f, sw);
    y0 = std::clamp(std::floor(y0) - 2.0f, 0.0f, sh);
    x1 = std::clamp(std::ceil(x1) + 2.0f, 0.0f, sw);
    y1 = std::clamp(std::ceil(y1) + 2.0f, 0.0f, sh);
    if (x1 - x0 < 1.0f || y1 - y0 < 1.0f)
        return;
    const VkRect2D box{{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    d3d::Matrix inverse;
    if (!InvertMatrix(viewProj, &inverse))
        return;
    float push[32];
    std::memcpy(push, &inverse.m[0][0], 64);
    const float rest[16] = {origin.x, origin.y, origin.z, 0.0f, edgeU.x, edgeU.y, edgeU.z, 0.0f,
                            edgeV.x, edgeV.y, edgeV.z, 0.0f, sw, sh, m_aoProj.m[2][2], m_aoProj.m[3][2]};
    std::memcpy(push + 16, rest, sizeof(rest));
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    // 1. The opening; 2. the sky into it, at the far plane.
    bool copied = false;
    if (!PortalOpenPass(cmd, push, box, copied))
        return;
    DrawSkyGlow(&box);
    // 3. The street: its doorway (the capture's entrance, facing its "inward" - out into the street) on the room's
    // entrance, facing out of the room.
    const float turn = std::atan2(out[0], out[2]) - std::atan2(street.inward[0], street.inward[2]);
    const d3d::Matrix place = MulMatrix(MulMatrix(Translation(-street.entrance[0], -street.entrance[1],
                                                              -street.entrance[2]),
                                                  TurnY(turn)),
                                        Translation(e[0], e[1], e[2]));
    const auto rs = m_rs;
    const auto tss = m_tss;
    const auto textures = m_textures;
    const auto lights = m_lights;
    const d3d::Matrix world = m_world;
    const d3d::Material material = m_material;
    const uint32_t kind = m_drawVisualKind, visual = m_drawVisual, owner = m_drawOwner;
    m_drawVisual = 0;
    m_drawOwner = 0;
    m_drawingPortal = true;
    m_portalScissor = box;
    // On the street's side of the entrance's plane only, from 5 cm out (the room behind it is the room's; the street's
    // facade round its doorway stands at the doorway's plane or behind it).
    m_portalClip[0] = out[0];
    m_portalClip[1] = 0.0f;
    m_portalClip[2] = out[2];
    m_portalClip[3] = -(out[0] * e[0] + out[2] * e[2]) - 0.05f;
    const uint32_t drawn = PortalReplay(street, m_street.lp.textures, place);
    m_drawingPortal = false;
    m_rs = rs;
    m_tss = tss;
    m_textures = textures;
    m_lights = lights;
    ++m_lightsGen;
    m_world = world;
    m_material = material;
    m_drawVisualKind = kind;
    m_drawVisual = visual;
    m_drawOwner = owner;
    m_constantsDirty = true;
    if (!m_street.logged) {
        m_street.logged = true;
        Log("street: drawn through playfield %u's entrance (%u pieces, the opening %.2f x %.2f, the screen box %d,%d %ux%u)",
            m_skyPlayfield, drawn, double(width), double(height), box.offset.x, box.offset.y, box.extent.width,
            box.extent.height);
    }
}

}  // namespace rvk
