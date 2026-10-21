// RVK_Vrs 1: variable rate shading (AMD's FidelityFX VRS 1.2 image generator; the SDK 1.1.4, third_party/ffx-sdk, MIT;
// docs/amd-fx-plan.md). Before the scene, a shading rate image - one texel per tile of the scene - says where the
// scene's own shader (ffp_main.glsl, the costly one) may shade a 2 x 2 (or 2 x 1, 1 x 2) block of pixels once: where
// last frame's picture there was flat (its luminance varied less than a cutoff) or moved fast (motion blurs it anyway).
// Vulkan's fragment shading rate attachment (VK_KHR_fragment_shading_rate) takes it as it is - AMD's encoding is
// Vulkan's (log2 width << 2 | log2 height). Only the scene's shader takes the attachment's rate (its pipelines' combiner
// REPLACE); the sky, water, grass, rain, smoke and clouds keep theirs (KEEP: 1 x 1). Every pipeline is made for the
// attachment when the GPU has it (m_sceneRateFlag), so the setting switches without making any.
#include "internal.h"
#include "ffx_common.h"

#include <FidelityFX/host/ffx_vrs.h>

namespace rvk {

using namespace vk;
using namespace detail;

struct Device::VrsState {
    FfxBackend backend;
    FfxVrsContext context{};
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t renderW = 0, renderH = 0, w = 0, h = 0;
    uint64_t frame = ~0ull;                      // the frame its image was made for
};

void Device::DestroyVrs()
{
    if (!m_vrs)
        return;
    WaitIdle();                                  // (its image and pipelines go at once)
    ffxVrsContextDestroy(&m_vrs->context);
    if (m_vrs->view) vkDestroyImageView(m_device, m_vrs->view, nullptr);
    if (m_vrs->image) vmaDestroyImage(m_allocator, m_vrs->image, m_vrs->allocation);
    delete m_vrs;
    m_vrs = nullptr;
}

// The shading rate image for this frame's scene, from last frame's picture and motion. Called before the scene's
// first rendering of the frame (BeginRenderingOn), outside any. False: none (the scene shades every pixel).
bool Device::RenderVrsImage(VkCommandBuffer cmd)
{
    if (!m_sceneRateFlag || m_vrsMode < 0.5f || !m_scene || !m_motionVectors)
        return false;
    const uint32_t rw = m_scene->m_width, rh = m_scene->m_height, tile = m_vrsTexel;
    if (m_vrs && (m_vrs->renderW != rw || m_vrs->renderH != rh))
        DestroyVrs();
    if (!m_vrs) {
        auto* s = new VrsState;
        FfxVrsContextDescription desc{};
        FfxErrorCode e = s->backend.Init(m_device, m_physical, FFX_VRS_CONTEXT_COUNT);
        desc.backendInterface = s->backend.iface;
        desc.flags = 0;                          // (2 x 2 at most: the additional rates are 4 x 4, too coarse here)
        desc.shadingRateImageTileSize = tile;
        if (e == FFX_OK)
            e = ffxVrsContextCreate(&s->context, &desc);
        if (e == FFX_OK)
            e = ffxVrsGetImageSizeFromeRenderResolution(&s->w, &s->h, rw, rh, tile);
        bool ok = e == FFX_OK;
        if (ok) {
            VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ic.imageType = VK_IMAGE_TYPE_2D;
            ic.format = VK_FORMAT_R8_UINT;
            ic.extent = {s->w, s->h, 1};
            ic.mipLevels = 1;
            ic.arrayLayers = 1;
            ic.samples = VK_SAMPLE_COUNT_1_BIT;
            ic.tiling = VK_IMAGE_TILING_OPTIMAL;
            ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
            VmaAllocationCreateInfo ac{};
            ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            ok = vmaCreateImage(m_allocator, &ic, &ac, &s->image, &s->allocation, nullptr) == VK_SUCCESS;
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = s->image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = ic.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ok = ok && vkCreateImageView(m_device, &vi, nullptr, &s->view) == VK_SUCCESS;
            if (!ok) ffxVrsContextDestroy(&s->context);
        }
        if (!ok) {
            static bool logged;
            if (!logged) { logged = true; Log("VRS: could not be set up (error 0x%x): every pixel shaded", unsigned(e)); }
            if (s->view) vkDestroyImageView(m_device, s->view, nullptr);
            if (s->image) vmaDestroyImage(m_allocator, s->image, s->allocation);
            delete s;
            return false;
        }
        s->renderW = rw;
        s->renderH = rh;
        m_vrs = s;
        Log("VRS: a %ux%u shading rate image (tiles of %u) for %ux%u", s->w, s->h, tile, rw, rh);
    }
    // Last frame's picture: the tone mapped scene where there is one at the scene's size (what its cutoff is made
    // for: 0..1), else the HDR scene; both still last frame's here.
    Texture* history = m_tonemapped && m_tonemapped->m_width == rw && m_tonemapped->m_height == rh ? m_tonemapped : m_scene;
    Transition(cmd, history, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, m_motionVectors, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImageBarrier(cmd, m_vrs->image, VK_IMAGE_ASPECT_COLOR_BIT, m_vrs->layout, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_vrs->layout = VK_IMAGE_LAYOUT_GENERAL;
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);

    FfxVrsDispatchDescription d{};
    d.commandList = ffxGetCommandListVK(cmd);
    d.historyColor = FfxImage(history->m_image, GetFormatInfo(history->m_format).vk, rw, rh, false, L"history");
    d.motionVectors = FfxImage(m_motionVectors->m_image, GetFormatInfo(Format::RGBA16F).vk, rw, rh, false, L"motion");
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8_UINT;
    ci.extent = {m_vrs->w, m_vrs->h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    d.output = ffxGetResourceVK(uint64_t(m_vrs->image), ffxGetImageResourceDescriptionVK(m_vrs->image, ci), L"rate",
                                FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.renderSize = {rw, rh};
    d.varianceCutoff = m_vrsCutoff;
    d.motionFactor = 0.01f;
    d.tileSize = tile;
    d.motionVectorScale = {1.0f / float(rw), 1.0f / float(rh)};   // ours in pixels; it takes uv (x its size again)
    const FfxErrorCode e = ffxVrsContextDispatch(&m_vrs->context, &d);
    if (e != FFX_OK) {
        static bool logged;
        if (!logged) { logged = true; Log("VRS: dispatch failed (error 0x%x): every pixel shaded", unsigned(e)); }
        return false;
    }
    ImageBarrier(cmd, m_vrs->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
                 VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR);
    m_vrs->layout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    m_vrs->frame = m_frameNumber;
    return true;
}

// The shading rate attachment for the scene's rendering this frame (VK_NULL_HANDLE: none).
VkImageView Device::VrsAttachment() const
{
    return m_vrs && m_vrs->frame == m_frameNumber && m_vrsMode >= 0.5f ? m_vrs->view : VK_NULL_HANDLE;
}

}  // namespace rvk
