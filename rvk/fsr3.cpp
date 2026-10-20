// RVK_Upscaler 2: AMD's FSR 3.1 upscaler (the FidelityFX SDK 1.1.4, third_party/ffx-sdk, MIT; docs/fsr3-plan.md)
// brings the scaled scene up to the output's size, through the SDK's `fsr3` context in its upscaling-only mode - the
// context its frame generation will use. It is fed as FSR 2 is (fsr2.cpp): the tone mapped scene (LDR), the scene's
// depth, the motion vectors filled in (FsrInputs), the jitter plus the half-pixel shift our scene keeps, and the
// reactive mask - the tone mapping's alpha as red, an image of its own here (reactive_mask.frag: the SDK makes its own
// views of what it is given). Its output goes through our sharpening into the frame, as the TAA's does.
#include "internal.h"

#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

struct Device::Fsr3State {
    FfxFsr3Context context{};
    VkDeviceContext deviceContext{};             // (ffxGetDeviceVK keeps a pointer to it)
    std::vector<uint8_t> scratch;                // the backend's (ffxGetScratchMemorySizeVK)
    uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0;
    double lastTime = 0.0;
    uint64_t frame = 0;
};

namespace {

// An image of ours as the SDK's resource, in its read state - or, `uav`, for its writes.
FfxResource Resource(VkImage image, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, bool uav,
                     const wchar_t* name)
{
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.usage = usage;
    return ffxGetResourceVK(uint64_t(image), ffxGetImageResourceDescriptionVK(image, ci), name,
                            uav ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ);
}

}  // namespace

void Device::DestroyFsr3()
{
    if (m_fsr3) {
        WaitIdle();                              // (its images and pipelines go at once)
        ffxFsr3ContextDestroy(&m_fsr3->context);
        delete m_fsr3;
        m_fsr3 = nullptr;
    }
    if (m_fsr3Reactive) { DestroyTextureNow(m_fsr3Reactive); m_fsr3Reactive = nullptr; }
}

// params: TaaParams'. False: FSR 3 couldn't run (no context) - the TAA does it.
bool Device::RenderFsr3(VkCommandBuffer cmd, Texture* dst, const float params[28])
{
    if (!m_motionFillPipeline || !m_reactiveMaskPipeline || !m_tonemapped || !m_depth)
        return false;
    const uint32_t rw = m_scene->m_width, rh = m_scene->m_height, dw = m_ldrMain->m_width, dh = m_ldrMain->m_height;
    // The context, for these sizes: made again when one changes (render scale, window), the GPU idle first.
    if (m_fsr3 && (m_fsr3->renderW != rw || m_fsr3->renderH != rh || m_fsr3->displayW != dw || m_fsr3->displayH != dh))
        DestroyFsr3();
    if (!m_fsr3) {
        auto* s = new Fsr3State;
        s->deviceContext = {m_device, m_physical, vkGetDeviceProcAddr};
        s->scratch.resize(ffxGetScratchMemorySizeVK(m_physical, FFX_FSR3_CONTEXT_COUNT));
        FfxFsr3ContextDescription desc{};
        FfxErrorCode e = ffxGetInterfaceVK(&desc.backendInterfaceUpscaling, ffxGetDeviceVK(&s->deviceContext),
                                           s->scratch.data(), s->scratch.size(), FFX_FSR3_CONTEXT_COUNT);
        desc.backendInterfaceSharedResources = desc.backendInterfaceUpscaling;
        desc.backendInterfaceFrameInterpolation = desc.backendInterfaceUpscaling;
        // LDR colour, motion vectors at the scene's size without the jitter, depth 0 near; upscaling alone.
        desc.flags = FFX_FSR3_ENABLE_UPSCALING_ONLY;
        desc.maxRenderSize = {rw, rh};
        desc.maxUpscaleSize = {dw, dh};
        desc.displaySize = {dw, dh};
        desc.backBufferFormat = FFX_SURFACE_FORMAT_B8G8R8A8_UNORM;
        if (e == FFX_OK)
            e = ffxFsr3ContextCreate(&s->context, &desc);
        if (e != FFX_OK) {
            static bool logged;
            if (!logged) { logged = true; Log("FSR 3: the context could not be made (error 0x%x): our TAA upscales", unsigned(e)); }
            delete s;
            return false;
        }
        s->renderW = rw;
        s->renderH = rh;
        s->displayW = dw;
        s->displayH = dh;
        m_fsr3 = s;
        Log("FSR 3.1: %ux%u -> %ux%u", rw, rh, dw, dh);
    }
    if (!m_fsr3Reactive || m_fsr3Reactive->m_width != rw || m_fsr3Reactive->m_height != rh) {
        if (m_fsr3Reactive) DestroyTexture(m_fsr3Reactive);
        m_fsr3Reactive = CreateImage(rw, rh, Format::RGBA8, 1, true);
        if (!m_fsr3Reactive)
            return false;
    }
    // The reactive mask (reactive_mask.frag), then the motion vectors and the barrier for FSR's compute passes.
    Transition(cmd, m_tonemapped, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const float none[4] = {};
    FullscreenPass(cmd, m_fsr3Reactive, m_reactiveMaskPipeline, m_tonemapped->m_view, m_tonemapped->m_view, m_pointSampler,
                   none, sizeof(none), false);
    Transition(cmd, m_fsr3Reactive, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!FsrInputs(cmd, params))
        return false;
    const bool continuous = params[23] > 0.5f;   // (TaaParams: the camera's last frame known, no teleport)

    const VkImageUsageFlags read = VK_IMAGE_USAGE_SAMPLED_BIT;
    FfxFsr3DispatchUpscaleDescription d{};
    d.commandList = ffxGetCommandListVK(cmd);
    d.color = Resource(m_tonemapped->m_image, GetFormatInfo(m_tonemapped->m_format).vk, rw, rh, read, false, L"color");
    d.depth = Resource(m_depth, kDepthFormat, rw, rh, read, false, L"depth");
    d.motionVectors = Resource(m_fsrMotion->m_image, GetFormatInfo(Format::RG16F).vk, rw, rh, read, false, L"motion");
    d.reactive = Resource(m_fsr3Reactive->m_image, GetFormatInfo(Format::RGBA8).vk, rw, rh, read, false, L"reactive");
    d.upscaleOutput = Resource(m_fsrOut->m_image, GetFormatInfo(Format::RGBA16F).vk, dw, dh,
                               read | VK_IMAGE_USAGE_STORAGE_BIT, true, L"output");
    // The scene's jitter in its pixels (TaaParams), and D3D's half-pixel shift our scene keeps (fsr2.cpp).
    d.jitterOffset = {params[26] + 0.5f * (1.0f - SceneScaleX()), params[27] + 0.5f * (1.0f - SceneScaleY())};
    d.motionVectorScale = {-1.0f, -1.0f};        // ours are now - then; FSR's then - now
    d.renderSize = {rw, rh};
    d.upscaleSize = {dw, dh};
    d.enableSharpening = false;                  // (our sharpening after, as the TAA's)
    const double now = SwayClock();
    d.frameTimeDelta = m_fsr3->lastTime > 0.0 ? float(std::clamp((now - m_fsr3->lastTime) * 1000.0, 1.0, 100.0)) : 16.7f;
    m_fsr3->lastTime = now;
    d.preExposure = 1.0f;
    d.reset = !continuous;
    const auto& p = m_aoProj.m;                  // (the world's camera: D3D's projection, depth 0 near)
    const float n = p[2][2] != 0.0f ? -p[3][2] / p[2][2] : 0.5f;
    d.cameraNear = n > 0.0f ? n : 0.5f;
    d.cameraFar = p[2][2] > 1.0f ? d.cameraNear * p[2][2] / (p[2][2] - 1.0f) : 10000.0f;
    d.cameraFovAngleVertical = p[1][1] > 0.0f ? 2.0f * std::atan(1.0f / p[1][1]) : 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.frameID = ++m_fsr3->frame;
    const FfxErrorCode e = ffxFsr3ContextDispatchUpscale(&m_fsr3->context, &d);
    // Its barriers left the images in these: our books say so.
    m_fsrOut->m_layout = VK_IMAGE_LAYOUT_GENERAL;
    if (e != FFX_OK) {
        static bool logged;
        if (!logged) { logged = true; Log("FSR 3: dispatch failed (error 0x%x)", unsigned(e)); }
        return false;
    }
    Transition(cmd, m_fsrOut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    float sharpen[4] = {params[21], 0.0f, 0.0f, 0.0f};
    FullscreenPass(cmd, dst, m_sharpenPipeline, m_fsrOut->m_view, m_fsrOut->m_view, m_pointSampler, sharpen,
                   sizeof(sharpen), false);
    return true;
}

}  // namespace rvk
