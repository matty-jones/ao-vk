// RVK_FrameGen: AMD's FSR 3.1 frame generation (the FidelityFX SDK 1.1.4, third_party/ffx-sdk, MIT;
// docs/fsr3-plan.md Phase B) - a frame interpolated between each two the game renders, from optical flow over the
// presented pictures and our depth and motion vectors.
//
// - The swapchain is AMD's frame interpolation swapchain (its replacement functions for create / images / acquire /
//   present / destroy), which presents the generated frame and then the real one, paced, from a thread of its own on
//   a queue the engine never uses (m_fgPresentQueue; device.cpp makes it).
// - An `fsr3` context in its interpolation-only mode - whichever upscaler made the frame (ours, FSR 2, FSR 3) - is
//   handed the scene's depth and its motion vectors each frame (FgPrepare, from EndScene); at present the swapchain
//   calls back into it (ffxFsr3DispatchFrameGeneration) for the optical flow and the interpolation.
// - The interface isn't interpolated: its layer (interface.cpp) isn't laid over our frame but registered with the
//   swapchain, which lays it over the real and the generated frame alike (premultiplied, as ui_composite.frag).
#include "internal.h"

#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

// The frame interpolation swapchain's lock on its queues (third_party/ffx-sdk, FrameInterpolationSwapchainVK_Helpers).
std::mutex& ffxQueueMutex();

namespace rvk {

using namespace vk;
using namespace detail;

struct Device::FrameGenState {
    FfxSwapchainReplacementFunctions fns{};
    VkFrameInterpolationInfoFFX info{};
    VkFormat format = VK_FORMAT_UNDEFINED;       // the swapchain's
    // The interpolation-only context, made at the first prepared frame, again when a size changes.
    bool made = false;
    FfxFsr3Context context{};
    VkDeviceContext deviceContext{};             // (ffxGetDeviceVK keeps a pointer to it)
    std::vector<uint8_t> scratch;
    uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0;
    uint64_t frame = 0;                          // this frame's id (prepare and configure share it)
    bool prepared = false;                       // FgPrepare ran this frame: generate a frame before it
    double lastTime = 0.0;
    uint32_t rendered = 0, generatedAsked = 0;   // (the log's)
    uint64_t shownBefore = 0;
};

namespace {

FfxResource Resource(VkImage image, VkFormat format, uint32_t w, uint32_t h, FfxResourceStates state, const wchar_t* name)
{
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    return ffxGetResourceVK(uint64_t(image), ffxGetImageResourceDescriptionVK(image, ci), name, state);
}

// The swapchain's frame generation callback: FSR 3's (which finds its context itself - the one configured).
FfxErrorCode GenerateFrame(const FfxFrameGenerationDispatchDescription* desc, void*)
{
    return ffxFsr3DispatchFrameGeneration(desc);
}

void DestroyContext(FfxFsr3Context& context, bool& made)
{
    if (!made)
        return;
    ffxFsr3ContextDestroy(&context);
    made = false;
}

}  // namespace

// The frame interpolation swapchain in place of a plain one (CreateSwapchain's create info). False: it couldn't be
// made - CreateSwapchain makes a plain one.
bool Device::FgCreateSwapchain(const VkSwapchainCreateInfoKHR& ci, std::string* error)
{
    if (!m_fgPresentQueue || !m_timelineSemaphores)   // (its pacing waits on timeline semaphores)
        return false;
    auto* s = new FrameGenState;
    // (Its device argument is the VkDevice itself, unlike the rest of the SDK's.)
    if (ffxGetSwapchainReplacementFunctionsVK(reinterpret_cast<FfxDevice>(m_device), &s->fns) != FFX_OK ||
        !s->fns.createSwapchainFFX) {
        delete s;
        if (error) *error = "frame generation: no swapchain replacement functions";
        return false;
    }
    s->info.physicalDevice = m_physical;
    s->info.device = m_device;
    s->info.gameQueue = {m_queue, m_queueFamily, nullptr};
    s->info.asyncComputeQueue = {VK_NULL_HANDLE, m_queueFamily, nullptr};
    s->info.presentQueue = {m_fgPresentQueue, m_queueFamily, nullptr};
    s->info.imageAcquireQueue = {m_fgAcquireQueue ? m_fgAcquireQueue : m_queue, m_queueFamily, nullptr};
    s->info.compositionMode = VK_COMPOSITION_MODE_NOT_FORCED_FFX;
    s->format = ci.imageFormat;
    const VkResult r = s->fns.createSwapchainFFX(m_device, &ci, nullptr, &m_swapchain, &s->info);
    if (r != VK_SUCCESS) {
        m_swapchain = VK_NULL_HANDLE;
        delete s;
        if (error) *error = "frame generation: the frame interpolation swapchain failed (" + std::to_string(int(r)) + ")";
        return false;
    }
    m_fg = s;
    Log("frame generation: on (the frame interpolation swapchain, %ux%u)", ci.imageExtent.width, ci.imageExtent.height);
    return true;
}

// The device idle (vkDeviceWaitIdle, for everything here that waits for it). With frame generation a wait on the device
// uses the swapchain's queues too, which its presenter thread submits to: the frames handed to it shown first, then the
// wait under its lock on them (its last present may come after).
void Device::WaitIdle()
{
    if (!m_fg || !m_swapchain) {
        vkDeviceWaitIdle(m_device);
        return;
    }
    ffxWaitForPresents(FfxSwapchain(m_swapchain));
    std::lock_guard<std::mutex> lock(ffxQueueMutex());
    vkDeviceWaitIdle(m_device);
}

void Device::FgDestroySwapchain()
{
    if (!m_fg)
        return;
    // The swapchain first (its presenter thread ends), then the context its frame generation called into.
    if (m_swapchain) m_fg->fns.destroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
    m_liveShown.store(0, std::memory_order_relaxed);
    WaitIdle();
    DestroyContext(m_fg->context, m_fg->made);
    delete m_fg;
    m_fg = nullptr;
}

VkResult Device::FgGetSwapchainImages(uint32_t* count, VkImage* images)
{
    return m_fg->fns.getSwapchainImagesKHR(m_device, m_swapchain, count, images);
}

VkResult Device::FgAcquire(uint64_t timeout, VkSemaphore semaphore, uint32_t* index)
{
    return m_fg->fns.acquireNextImageKHR(m_device, m_swapchain, timeout, semaphore, VK_NULL_HANDLE, index);
}

VkResult Device::FgPresent(const VkPresentInfoKHR* info)
{
    return m_fg->fns.queuePresentKHR(m_queue, info);
}

uint64_t Device::FgShownFrames() const
{
    return m_fg && m_fg->fns.getLastPresentCountFFX ? m_fg->fns.getLastPresentCountFFX(m_swapchain) : 0;
}

// The scene's depth and motion vectors for the next generated frame (EndScene, after the upscaling; params:
// TaaParams'), at the scene's size, before the depth is brought up to the output's.
void Device::FgPrepare(VkCommandBuffer cmd, const float params[28])
{
    FrameGenState& s = *m_fg;
    s.prepared = false;
    if (!m_depth || !m_scene || !m_swapExtent.width)
        return;
    const uint32_t rw = m_scene->m_width, rh = m_scene->m_height, dw = m_swapExtent.width, dh = m_swapExtent.height;
    if (s.made && (s.renderW != rw || s.renderH != rh || s.displayW != dw || s.displayH != dh)) {
        WaitIdle();
        DestroyContext(s.context, s.made);
    }
    if (!s.made) {
        s.deviceContext = {m_device, m_physical, vkGetDeviceProcAddr};
        s.scratch.assign(ffxGetScratchMemorySizeVK(m_physical, FFX_FSR3_CONTEXT_COUNT), 0);
        FfxFsr3ContextDescription desc{};
        FfxErrorCode e = ffxGetInterfaceVK(&desc.backendInterfaceFrameInterpolation, ffxGetDeviceVK(&s.deviceContext),
                                           s.scratch.data(), s.scratch.size(), FFX_FSR3_CONTEXT_COUNT);
        desc.backendInterfaceSharedResources = desc.backendInterfaceFrameInterpolation;
        desc.backendInterfaceUpscaling = desc.backendInterfaceFrameInterpolation;
        // LDR, motion vectors at the scene's size without the jitter, depth 0 near; interpolation alone (the
        // upscaling is RVK_Upscaler's).
        desc.flags = FFX_FSR3_ENABLE_INTERPOLATION_ONLY;
        desc.maxRenderSize = {rw, rh};
        desc.maxUpscaleSize = {dw, dh};
        desc.displaySize = {dw, dh};
        desc.backBufferFormat = ffxGetSurfaceFormatVK(s.format);
        if (e == FFX_OK)
            e = ffxFsr3ContextCreate(&s.context, &desc);
        if (e != FFX_OK) {
            static bool logged;
            if (!logged) { logged = true; Log("frame generation: the context could not be made (error 0x%x)", unsigned(e)); }
            return;
        }
        s.made = true;
        s.renderW = rw;
        s.renderH = rh;
        s.displayW = dw;
        s.displayH = dh;
        Log("frame generation: %ux%u scene, %ux%u frames", rw, rh, dw, dh);
    }
    if (!FsrInputs(cmd, params))                 // (the motion vectors whole - done already if FSR upscaled)
        return;
    FfxFsr3DispatchFrameGenerationPrepareDescription d{};
    d.commandList = ffxGetCommandListVK(cmd);
    d.depth = Resource(m_depth, kDepthFormat, rw, rh, FFX_RESOURCE_STATE_COMPUTE_READ, L"depth");
    d.motionVectors = Resource(m_fsrMotion->m_image, GetFormatInfo(Format::RG16F).vk, rw, rh,
                               FFX_RESOURCE_STATE_COMPUTE_READ, L"motion");
    d.jitterOffset = {params[26] + 0.5f * (1.0f - SceneScaleX()), params[27] + 0.5f * (1.0f - SceneScaleY())};
    d.motionVectorScale = {-1.0f, -1.0f};        // (ours now - then: fsr2.cpp)
    d.renderSize = {rw, rh};
    const double now = SwayClock();
    d.frameTimeDelta = s.lastTime > 0.0 ? float(std::clamp((now - s.lastTime) * 1000.0, 1.0, 100.0)) : 16.7f;
    s.lastTime = now;
    const auto& p = m_aoProj.m;                  // (the world's camera: fsr2.cpp)
    const float n = p[2][2] != 0.0f ? -p[3][2] / p[2][2] : 0.5f;
    d.cameraNear = n > 0.0f ? n : 0.5f;
    d.cameraFar = p[2][2] > 1.0f ? d.cameraNear * p[2][2] / (p[2][2] - 1.0f) : 10000.0f;
    d.cameraFovAngleVertical = p[1][1] > 0.0f ? 2.0f * std::atan(1.0f / p[1][1]) : 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.frameID = ++s.frame;
    if (ffxFsr3ContextDispatchFrameGenerationPrepare(&s.context, &d) == FFX_OK)
        s.prepared = true;
}

// Before the frame is presented (EndFrame, its commands still open): whether a frame is generated before it (only
// after a prepared one - menus and loading screens without 3D have none), and the interface's layer for the
// swapchain to lay over both.
void Device::FgBeforePresent(VkCommandBuffer cmd)
{
    FrameGenState& s = *m_fg;
    if (s.made) {
        FfxFrameGenerationConfig config{};
        config.swapChain = FfxSwapchain(m_swapchain);
        config.frameGenerationCallback = GenerateFrame;
        config.frameGenerationCallbackContext = &s.context;
        config.frameGenerationEnabled = s.prepared;
        config.allowAsyncWorkloads = false;
        config.frameID = s.frame;
        ffxFsr3ConfigureFrameGeneration(&s.context, &config);
    }
    FfxResource ui{};
    if (m_fgUiLayer && m_uiLayer && m_uiLayerReady.load(std::memory_order_relaxed) && !m_hideInterface &&
        m_uiLayer->m_width == m_ldrMain->m_width && m_uiLayer->m_height == m_ldrMain->m_height) {
        Transition(cmd, m_uiLayer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ui = Resource(m_uiLayer->m_image, GetFormatInfo(m_uiLayer->m_format).vk, m_uiLayer->m_width, m_uiLayer->m_height,
                      FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ, L"interface");
    }
    ffxRegisterFrameinterpolationUiResourceVK(FfxSwapchain(m_swapchain), ui, FFX_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA);
    if (s.prepared) ++s.generatedAsked;
    s.prepared = false;
    m_liveShown.store(FgShownFrames(), std::memory_order_relaxed);   // (the readout's shown rate)
    if (++s.rendered == 600) {                   // (the log: frames shown for frames rendered - about 2 when it works)
        const uint64_t shown = FgShownFrames();
        Log("frame generation: %llu frames shown for %u rendered (%u with a generated one asked for)",
            (unsigned long long)(shown - s.shownBefore), s.rendered, s.generatedAsked);
        s.shownBefore = shown;
        s.rendered = s.generatedAsked = 0;
    }
}

}  // namespace rvk
