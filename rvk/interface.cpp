// The interface layer (docs/rvk.md "Interface refresh rate"): the game's interface (GUI.dll's windows) drawn into an
// image of its own, premultiplied by its coverage, and blended over the main target every frame - so frames that
// don't redraw it (the game skips GUI.dll's drawing, rnative gui.cpp) still show it.
//
// In the layer every draw blends, its colour as the game asked and its alpha as coverage: what a sequence of blended
// draws over a transparent image leaves is exactly what they would have left over the frame, once the layer is put
// over the frame with (ONE, INV_SRC_ALPHA) - for the interface's alpha-blended and additive draws. (An unblended
// draw keeps its texture's alpha as coverage; one multiplying the frame - DESTCOLOR - is approximated.)
#include "internal.h"

namespace rvk {

using namespace vk;
using namespace detail;

void Device::InterfaceSceneEnd()
{
    // The interface's first draw ends the 3D scene (Draw): particles the game no longer draws go first, then (HDR) the
    // scene is tone mapped into the 8-bit target the interface goes over. A frame that doesn't redraw the interface
    // has no such draw.
    if (!m_external && m_target == m_main && m_particleSaw3D && !m_particleOrphansDone) {
        m_particleOrphanTrigger = "the interface";
        DrawOrphanParticles();
    }
    if (m_scenePhase && m_target == m_scene) {
        m_sceneEndDraw = m_dumpDraw;
        m_sceneEndFvf = 0;
        EndScene();
    }
}

void Device::OverlayBegin()
{
    if (!m_inFrame || !m_main)
        return;
    InterfaceSceneEnd();
    if (m_target == m_main && !m_overlayLabelsActive && m_main->m_format == Format::A8R8G8B8)
        OverlayLabelsBegin();
}

void Device::InterfaceBegin(bool redraw, bool guardAtlas)
{
    m_uiAtlasGuard = guardAtlas;
    if (!m_inFrame || !m_main)
        return;
    InterfaceSceneEnd();
    // Only over the 8-bit main target (the layer's format; HDR: after the scene's tone mapping). Otherwise - a frame
    // without 3D still in the float scene - the interface is drawn straight into the target, and is redrawn each
    // frame (not ready).
    if (m_main->m_format != Format::A8R8G8B8) {
        m_uiLayerReady.store(false, std::memory_order_relaxed);
        return;
    }
    if (m_overlayLabelsActive)
        OverlayLabelsEnd();
    m_uiLayerOpen = true;
    if (!redraw)
        return;
    if (!m_uiLayer || m_uiLayer->m_width != m_main->m_width || m_uiLayer->m_height != m_main->m_height) {
        if (m_uiLayer) DestroyTexture(m_uiLayer);
        m_uiLayer = CreateImage(m_main->m_width, m_main->m_height, Format::A8R8G8B8, 1, true);
        m_uiLayerReady.store(false, std::memory_order_relaxed);
        if (!m_uiLayer)
            return;
    }
    m_uiLayerReturn = m_target;
    const d3d::Viewport viewport = m_viewport;
    m_uiLayerActive = true;
    SetRenderTarget(m_uiLayer);
    m_viewport = viewport;
    m_cache = StateCache{};                      // the layer's blend equations differ (ApplyDynamicState)
    VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    clear.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkClearRect rect{{{0, 0}, {m_uiLayer->m_width, m_uiLayer->m_height}}, 0, 1};
    vkCmdClearAttachments(m_frames[m_frameIndex].main, 1, &clear, 1, &rect);
}

void Device::InterfaceEnd()
{
    m_uiAtlasGuard = false;
    if (!m_inFrame || !m_uiLayerOpen)
        return;
    m_uiLayerOpen = false;
    m_interfaceEndFrame = m_frameNumber;
    if (m_uiLayerActive) {
        m_uiLayerActive = false;
        m_renderEndCause = kEndTarget;
        EndRendering();
        Texture* back = m_uiLayerReturn && m_uiLayerReturn != m_uiLayer ? m_uiLayerReturn : m_main;
        m_target = back;
        m_uiLayerReady.store(true, std::memory_order_relaxed);
    } else {
        m_renderEndCause = kEndTarget;
        EndRendering();
    }
    // Rebind state after returning from the interface target.
    m_cache = StateCache{};
    m_arenaBound = false;
    m_bindlessBound = false;
    m_constantsDirty = true;
    BeginRenderingOn(m_target);
}

void Device::SetConsoleImage(uint32_t width, uint32_t height, const void* pixels)
{
    m_consoleW = pixels && height ? width : 0;
    m_consoleH = pixels && width ? height : 0;
    if (m_consoleH)
        m_consolePixels.assign(static_cast<const uint8_t*>(pixels),
                               static_cast<const uint8_t*>(pixels) + size_t(width) * height * 4);
    else
        std::vector<uint8_t>().swap(m_consolePixels);
    m_consoleDirty = true;
}

// Draw the console into the composed frame or interpolation overlay. Uploads precede the frame's draws.
// An image made for another size waits for the console's next update.
void Device::DrawConsole(VkCommandBuffer cmd, Texture* dst)
{
    Texture* target = dst ? dst : m_main;
    if (!m_consoleH || !m_uiCompositePipeline || !target || m_main->m_format != Format::A8R8G8B8 ||
        m_consoleW != target->m_width || m_consoleH > target->m_height)
        return;
    if (!m_consoleTex || m_consoleTex->m_width != m_consoleW || m_consoleTex->m_height != m_consoleH) {
        if (m_consoleTex) DestroyTexture(m_consoleTex);
        m_consoleTex = CreateTexture(m_consoleW, m_consoleH, Format::A8R8G8B8, 1);
        m_consoleDirty = true;
        if (!m_consoleTex) return;
    }
    if (m_consoleDirty) {
        UpdateTexture(m_consoleTex, 0, 0, 0, m_consoleW, m_consoleH, m_consolePixels.data(), m_consoleW * 4);
        m_consoleDirty = false;
    }
    // (The interface layer's pass: texel for pixel, blended ONE / INV_SRC_ALPHA - only the console's rows drawn.)
    const VkRect2D area{{0, 0}, {m_consoleW, m_consoleH}};
    const float params[4] = {};
    FullscreenPass(cmd, target, dst ? m_uiOverlayPipeline : m_uiCompositePipeline, m_consoleTex->m_view, m_consoleTex->m_view, m_pointSampler, params,
                   sizeof(params), true, VK_NULL_HANDLE, VK_NULL_HANDLE, &area);
    if (!dst)
        Transition(cmd, m_main, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

// Labels and floating text share a display-sized layer, with the world's depth for occlusion.
bool Device::OverlayLabelsBegin()
{
    const uint32_t w = m_ldrMain->m_width, h = m_ldrMain->m_height;
    if (!m_overlayLabels || m_overlayLabels->m_width != w || m_overlayLabels->m_height != h) {
        if (m_overlayLabels) DestroyTexture(m_overlayLabels);
        m_overlayLabels = CreateImage(w, h, Format::A8R8G8B8, 1, true);
        if (!m_overlayLabels)
            return false;
    }
    m_renderEndCause = kEndTarget;
    EndRendering();
    m_target = m_overlayLabels;
    m_overlayLabelsActive = true;
    m_cache = StateCache{};                      // (the layer's blend equations differ)
    BeginRenderingOn(m_overlayLabels);
    if (m_overlayLabelsFrame == m_frameNumber)
        return true;                            // append floating text to the held name labels
    VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    clear.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkClearRect rect{{{0, 0}, {w, h}}, 0, 1};
    vkCmdClearAttachments(m_frames[m_frameIndex].main, 1, &clear, 1, &rect);
    return true;
}

void Device::OverlayLabelsEnd()
{
    m_renderEndCause = kEndTarget;
    EndRendering();
    m_overlayLabelsActive = false;
    m_overlayLabelsFrame = m_frameNumber;
    m_target = m_ldrMain;
    m_cache = StateCache{};
    BeginRenderingOn(m_ldrMain);
}

// Late draws, including the cursor, stay above the console and interface.
bool Device::OverlayTopBegin()
{
    const uint32_t w = m_main->m_width, h = m_main->m_height;
    if (m_main->m_format != Format::A8R8G8B8)
        return false;
    if (!m_overlayTop || m_overlayTop->m_width != w || m_overlayTop->m_height != h) {
        if (m_overlayTop) DestroyTexture(m_overlayTop);
        m_overlayTop = CreateImage(w, h, Format::A8R8G8B8, 1, true);
        if (!m_overlayTop)
            return false;
    }
    m_renderEndCause = kEndTarget;
    EndRendering();
    m_target = m_overlayTop;
    m_overlayTopActive = true;
    m_cache = StateCache{};                      // (the layer's blend equations differ)
    BeginRenderingOn(m_overlayTop);
    VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    clear.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkClearRect rect{{{0, 0}, {w, h}}, 0, 1};
    vkCmdClearAttachments(m_frames[m_frameIndex].main, 1, &clear, 1, &rect);
    return true;
}

// An image cleared to transparent black (a render target, left as a colour attachment).
void Device::ClearImage(VkCommandBuffer cmd, Texture* t)
{
    Transition(cmd, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = t->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {t->m_width, t->m_height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdEndRendering(cmd);
}

// Compose overlays without changing the world target; leave the result in TRANSFER_SRC. Null: no overlays.
// Call outside rendering and rebind pipeline state afterwards.
Texture* Device::ComposedFrame(VkCommandBuffer cmd, bool console)
{
    if (!m_main || m_main->m_format != Format::A8R8G8B8 || !m_uiCompositePipeline)
        return nullptr;
    const uint32_t w = m_main->m_width, h = m_main->m_height;
    const bool interface = m_uiLayer && (m_uiLayerActive || m_uiLayerReady.load(std::memory_order_relaxed)) && !m_hideInterface &&
                           m_uiLayer->m_width == w && m_uiLayer->m_height == h;
    const bool labels = !m_hideInterface && m_overlayLabels && (m_overlayLabelsActive || m_overlayLabelsFrame == m_frameNumber) && m_overlayLabels->m_width == w &&
                        m_overlayLabels->m_height == h;
    const bool top = m_overlayTop && (m_overlayTopActive || m_overlayTopFrame == m_frameNumber) && m_overlayTop->m_width == w && m_overlayTop->m_height == h;
    console = console && m_consoleH && m_consoleW == w && m_consoleH <= h;
    if (!interface && !labels && !top && !console)
        return nullptr;
    if (!m_composedFrame || m_composedFrame->m_width != w || m_composedFrame->m_height != h) {
        if (m_composedFrame) DestroyTexture(m_composedFrame);
        m_composedFrame = CreateImage(w, h, Format::A8R8G8B8, 1, true);
        if (!m_composedFrame)
            return nullptr;
    }
    Transition(cmd, m_main, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cmd, m_composedFrame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {w, h, 1};
    vkCmdCopyImage(cmd, m_main->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_composedFrame->m_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    const float none[4] = {};
    for (Texture* layer : {labels ? m_overlayLabels : nullptr, interface ? m_uiLayer : nullptr}) {
        if (!layer) continue;
        Transition(cmd, layer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        FullscreenPass(cmd, m_composedFrame, m_uiCompositePipeline, layer->m_view, layer->m_view, m_pointSampler, none,
                       sizeof(none), true);
    }
    if (console) DrawConsole(cmd, m_composedFrame);
    if (top) {
        Transition(cmd, m_overlayTop, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        FullscreenPass(cmd, m_composedFrame, m_uiCompositePipeline, m_overlayTop->m_view, m_overlayTop->m_view,
                       m_pointSampler, none, sizeof(none), true);
    }
    Transition(cmd, m_composedFrame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return m_composedFrame;
}

}  // namespace rvk
