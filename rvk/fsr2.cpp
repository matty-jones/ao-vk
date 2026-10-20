// RVK_Upscaler 1: AMD's FidelityFX Super Resolution 2.2 (third_party/fsr2, MIT) brings the scaled scene up to the
// output's size in place of taa.frag's upscaling (docs/render-perf-plan.md Phase 1). It gets what the TAA gets: the
// tone mapped scene (LDR) at the scene's size, the scene's depth, the motion vectors - filled in where no draw wrote
// one (motion_fill.frag), as taa.frag does for itself - the jitter, and as its reactive mask the tone mapping's alpha
// (how much of each pixel effects made: sparks and spells are new each frame). Its output then goes through our
// sharpening into the frame, as the TAA's does. Its shaders are compiled for these choices by
// tools/gen_fsr2_shaders.py; its Vulkan backend records into the frame's command buffer with its own pipelines,
// descriptors and memory.
#include "internal.h"

#include "fsr2_rename.h"                         // (its names apart from the FidelityFX SDK's: FSR 3.1)
#include "ffx_fsr2.h"
#include "vk/ffx_fsr2_vk.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

struct Device::Fsr2State {
    FfxFsr2Context context{};
    std::vector<uint8_t> scratch;                // the backend's (ffxFsr2GetScratchMemorySizeVK)
    uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0;
    double lastTime = 0.0;
};

void Device::DestroyFsr2()
{
    if (m_fsr2) {
        WaitIdle();                              // (its images and pipelines go at once)
        ffxFsr2ContextDestroy(&m_fsr2->context);
        delete m_fsr2;
        m_fsr2 = nullptr;
    }
    if (m_fsr2Reactive) {
        vkDestroyImageView(m_device, m_fsr2Reactive, nullptr);
        m_fsr2Reactive = VK_NULL_HANDLE;
        m_fsr2ReactiveOf = VK_NULL_HANDLE;
    }
    if (m_fsrMotion) { DestroyTextureNow(m_fsrMotion); m_fsrMotion = nullptr; }
    if (m_fsrOut) { DestroyTextureNow(m_fsrOut); m_fsrOut = nullptr; }
}

// FSR 2's and FSR 3's inputs and output (params: TaaParams'): the images at these sizes, the motion vectors whole
// (motion_fill.frag: the camera's where none was written), what the fragment passes wrote made visible to their
// compute passes, the output ready for their writes.
bool Device::FsrInputs(VkCommandBuffer cmd, const float params[28])
{
    if (m_fsrInputsFrame == m_frameNumber && m_fsrMotion && m_fsrOut)
        return true;
    const uint32_t rw = m_scene->m_width, rh = m_scene->m_height, dw = m_ldrMain->m_width, dh = m_ldrMain->m_height;
    if (!m_fsrMotion || m_fsrMotion->m_width != rw || m_fsrMotion->m_height != rh) {
        if (m_fsrMotion) DestroyTexture(m_fsrMotion);
        m_fsrMotion = CreateImage(rw, rh, Format::RG16F, 1, true);
    }
    if (!m_fsrOut || m_fsrOut->m_width != dw || m_fsrOut->m_height != dh) {
        if (m_fsrOut) DestroyTexture(m_fsrOut);
        m_fsrOut = CreateImage(dw, dh, Format::RGBA16F, 1, true, true);
    }
    if (!m_fsrMotion || !m_fsrOut)
        return false;
    const bool continuous = params[23] > 0.5f;

    // The motion vectors whole (motion_fill.frag): the camera's where none was written.
    Transition(cmd, m_motionVectors, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    MakeDepthReadable(cmd);
    float fill[20];
    std::memcpy(fill, params, 64);
    fill[16] = float(rw);
    fill[17] = float(rh);
    fill[18] = params[19];                       // object motion written this frame
    fill[19] = continuous ? 1.0f : 0.0f;         // the camera's reprojection known
    FullscreenPass(cmd, m_fsrMotion, m_motionFillPipeline, m_motionVectors->m_view, m_depthView, m_pointSampler, fill,
                   sizeof(fill), false);
    Transition(cmd, m_fsrMotion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, m_tonemapped, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // (What the fragment passes wrote - the reactive mask's too - for FSR's compute passes: its barriers only move the
    // layouts.)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    Transition(cmd, m_fsrOut, VK_IMAGE_LAYOUT_GENERAL);
    m_fsrInputsFrame = m_frameNumber;
    return true;
}

// params: TaaParams'. False: FSR 2 couldn't run (no context) - the TAA does it.
bool Device::RenderFsr2(VkCommandBuffer cmd, Texture* dst, const float params[28])
{
    if (!m_motionFillPipeline || !m_tonemapped || !m_depth)
        return false;
    const uint32_t rw = m_scene->m_width, rh = m_scene->m_height, dw = m_ldrMain->m_width, dh = m_ldrMain->m_height;
    // The context, for these sizes: made again when one changes (render scale, window), the GPU idle first.
    if (m_fsr2 && (m_fsr2->renderW != rw || m_fsr2->renderH != rh || m_fsr2->displayW != dw || m_fsr2->displayH != dh)) {
        WaitIdle();
        ffxFsr2ContextDestroy(&m_fsr2->context);
        delete m_fsr2;
        m_fsr2 = nullptr;
    }
    if (!m_fsr2) {
        auto* s = new Fsr2State;
        s->scratch.resize(ffxFsr2GetScratchMemorySizeVK(m_physical));
        FfxFsr2ContextDescription desc{};
        FfxErrorCode e = ffxFsr2GetInterfaceVK(&desc.callbacks, s->scratch.data(), s->scratch.size(), m_physical,
                                              vkGetDeviceProcAddr);
        desc.flags = 0;                          // LDR colour, motion vectors at the scene's size without the jitter, depth 0 near
        desc.maxRenderSize = {rw, rh};
        desc.displaySize = {dw, dh};
        desc.device = ffxGetDeviceVK(m_device);
        if (e == FFX_OK)
            e = ffxFsr2ContextCreate(&s->context, &desc);
        if (e != FFX_OK) {
            static bool logged;
            if (!logged) { logged = true; Log("FSR 2: the context could not be made (error 0x%x): our TAA upscales", unsigned(e)); }
            delete s;
            return false;
        }
        s->renderW = rw;
        s->renderH = rh;
        s->displayW = dw;
        s->displayH = dh;
        m_fsr2 = s;
        Log("FSR 2: %ux%u -> %ux%u", rw, rh, dw, dh);
    }
    // The reactive mask: m_tonemapped's alpha as red (a view of it - remade when the image is).
    if (m_fsr2ReactiveOf != m_tonemapped->m_image) {
        if (m_fsr2Reactive)                      // (in use by frames in flight: freed with this frame slot)
            m_deadImages.push_back({DeathTag(), {VK_NULL_HANDLE, m_fsr2Reactive, nullptr}});
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_tonemapped->m_image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = GetFormatInfo(m_tonemapped->m_format).vk;
        vi.components = {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A};
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        m_fsr2Reactive = VK_NULL_HANDLE;
        vkCreateImageView(m_device, &vi, nullptr, &m_fsr2Reactive);
        m_fsr2ReactiveOf = m_tonemapped->m_image;
    }
    const bool continuous = params[23] > 0.5f;   // (TaaParams: the camera's last frame known, no teleport)
    if (!FsrInputs(cmd, params))
        return false;

    FfxFsr2Context* c = &m_fsr2->context;
    FfxFsr2DispatchDescription d{};
    d.commandList = ffxGetCommandListVK(cmd);
    d.color = ffxGetTextureResourceVK(c, m_tonemapped->m_image, m_tonemapped->m_view, rw, rh,
                                      GetFormatInfo(m_tonemapped->m_format).vk, L"color", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxGetTextureResourceVK(c, m_depth, m_depthView, rw, rh, kDepthFormat, L"depth",
                                      FFX_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxGetTextureResourceVK(c, m_fsrMotion->m_image, m_fsrMotion->m_view, rw, rh,
                                              GetFormatInfo(Format::RG16F).vk, L"motion", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.reactive = ffxGetTextureResourceVK(c, m_tonemapped->m_image, m_fsr2Reactive, rw, rh,
                                         GetFormatInfo(m_tonemapped->m_format).vk, L"reactive",
                                         FFX_RESOURCE_STATE_COMPUTE_READ);
    d.output = ffxGetTextureResourceVK(c, m_fsrOut->m_image, m_fsrOut->m_view, dw, dh, GetFormatInfo(Format::RGBA16F).vk,
                                       L"output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    // The scene's jitter in its pixels (x right, y down: TaaParams), and D3D's half-pixel shift the scene keeps in its
    // own pixels (rvk ViewportState): FSR 2 takes output pixel i's centre to be scene position (i + 0.5) * scale, ours
    // is i * scale + 0.5 - the picture stood 0.5 * (1 - scale) scene pixels off (half an output pixel at 50%).
    const float shift = 0.5f * (1.0f - SceneScaleX()), shiftY = 0.5f * (1.0f - SceneScaleY());
    d.jitterOffset = {params[26] + shift, params[27] + shiftY};
    d.motionVectorScale = {-1.0f, -1.0f};        // ours are now - then; FSR 2's then - now
    d.renderSize = {rw, rh};
    d.enableSharpening = false;                  // (our sharpening after, as the TAA's)
    const double now = SwayClock();
    d.frameTimeDelta = m_fsr2->lastTime > 0.0 ? float(std::clamp((now - m_fsr2->lastTime) * 1000.0, 1.0, 100.0)) : 16.7f;
    m_fsr2->lastTime = now;
    d.preExposure = 1.0f;
    d.reset = !continuous;
    // The camera (the world's: D3D's projection, depth 0 near, 1 far).
    const auto& p = m_aoProj.m;
    const float n = p[2][2] != 0.0f ? -p[3][2] / p[2][2] : 0.5f;
    d.cameraNear = n > 0.0f ? n : 0.5f;
    d.cameraFar = p[2][2] > 1.0f ? d.cameraNear * p[2][2] / (p[2][2] - 1.0f) : 10000.0f;
    d.cameraFovAngleVertical = p[1][1] > 0.0f ? 2.0f * std::atan(1.0f / p[1][1]) : 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;            // (Anarchy Online's units are about a metre)
    const FfxErrorCode e = ffxFsr2ContextDispatch(c, &d);
    // Its barriers left the images in these: our books say so.
    m_fsrOut->m_layout = VK_IMAGE_LAYOUT_GENERAL;
    if (e != FFX_OK) {
        static bool logged;
        if (!logged) { logged = true; Log("FSR 2: dispatch failed (error 0x%x)", unsigned(e)); }
        return false;
    }
    Transition(cmd, m_fsrOut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    float sharpen[4] = {params[21], 0.0f, 0.0f, 0.0f};
    FullscreenPass(cmd, dst, m_sharpenPipeline, m_fsrOut->m_view, m_fsrOut->m_view, m_pointSampler, sharpen,
                   sizeof(sharpen), false);
    return true;
}

}  // namespace rvk
