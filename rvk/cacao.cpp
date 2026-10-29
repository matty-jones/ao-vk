// RVK_AoMode 1: AMD's ambient occlusion (FidelityFX CACAO 1.4; the SDK 1.1.4, third_party/ffx-sdk, MIT;
// docs/amd-fx-plan.md) in place of ours (ao.frag) - for comparing the two. From the depth buffer alone (it makes its
// normals from it, as ours does): the depth deinterleaved into four quarter images, each sampled with its own rotated
// pattern, more samples where an importance map says the occlusion varies, an edge-aware blur, the four put back
// together at half resolution and brought up to full with a bilateral filter. RVK_Ao is its strength (its shadow
// multiplier), RVK_AoRadius its radius. cacao_pack.frag puts its result with the view depth, as the tone mapping takes
// ours - at full resolution.
#include "internal.h"
#include "ffx_common.h"

#include <FidelityFX/host/ffx_cacao.h>

#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

struct Device::CacaoState {
    FfxBackend backend;
    FfxCacaoContext context{};
    uint32_t w = 0, h = 0;
};

void Device::DestroyCacao()
{
    if (m_cacao) {
        WaitIdle();                              // (its images and pipelines go at once)
        ffxCacaoContextDestroy(&m_cacao->context);
        delete m_cacao;
        m_cacao = nullptr;
    }
    if (m_aoFullTex == m_cacaoAo) m_aoFullTex = nullptr;
    for (Texture** t : {&m_cacaoOut, &m_cacaoAo})
        if (*t) { DestroyTextureNow(*t); *t = nullptr; }
}

// AMD's AO into m_cacaoAo. False: it couldn't run (ours then).
bool Device::RenderCacao(VkCommandBuffer cmd)
{
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (m_aoStrength <= 0.0f || !perspective || !m_depth || !m_cacaoPackPipeline || !m_gatherExtended)
        return false;
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    if (m_cacao && (m_cacao->w != w || m_cacao->h != h))
        DestroyCacao();
    if (!m_cacao) {
        auto* s = new CacaoState;
        FfxCacaoContextDescription desc{};
        FfxErrorCode e = s->backend.Init(m_device, m_physical, FFX_CACAO_CONTEXT_COUNT);
        desc.backendInterface = s->backend.iface;
        desc.width = w;
        desc.height = h;
        desc.useDownsampledSsao = true;          // (half resolution, brought up by its bilateral filter: as ours)
        if (e == FFX_OK)
            e = ffxCacaoContextCreate(&s->context, &desc);
        if (e != FFX_OK) {
            static bool logged;
            if (!logged) { logged = true; Log("CACAO: the context could not be made (error 0x%x): our AO", unsigned(e)); }
            delete s;
            return false;
        }
        s->w = w;
        s->h = h;
        m_cacao = s;
        Log("CACAO: AMD's ambient occlusion at %ux%u", w, h);
    }
    for (Texture** t : {&m_cacaoOut, &m_cacaoAo}) {
        if (*t && (*t)->m_width == w && (*t)->m_height == h) continue;
        if (*t) DestroyTexture(*t);
        *t = t == &m_cacaoOut ? CreateImage(w, h, Format::RGBA16F, 1, true, true) : CreateImage(w, h, Format::RG16F, 1, true);
        if (!*t)
            return false;
    }
    FfxCacaoSettings settings = FFX_CACAO_DEFAULT_SETTINGS;
    settings.radius = m_aoRadius;
    // (Its defaults - multiplier 1, power 1.5 - darkened the foot of every wall black against ours at the same
    // strength: half, and linear.)
    settings.shadowMultiplier = 0.5f * m_aoStrength;
    settings.shadowPower = 1.0f;
    settings.generateNormals = true;
    settings.fadeOutFrom = 60.0f;                // (our AO fades into the game's fog: occlusion.glsl)
    settings.fadeOutTo = 400.0f;
    // A rotated, rescaled sampling kernel each frame of three: the temporal anti-aliasing / FSR averages them.
    const float k = float(m_frameNumber % 3);
    settings.temporalSupersamplingAngleOffset = k / 3.0f * 3.14159265f;
    settings.temporalSupersamplingRadiusOffset = 1.0f + (k - 1.0f) / 3.0f * 0.1f;
    if (ffxCacaoUpdateSettings(&m_cacao->context, &settings, true) != FFX_OK)
        return false;

    MakeDepthReadable(cmd);
    Transition(cmd, m_cacaoOut, VK_IMAGE_LAYOUT_GENERAL);
    // (The depth the scene wrote, for its compute passes: its barriers only move the layouts.)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);

    // The projection as it is: CACAO takes a D3D one, row-major (its depth unpacking: m[3][2], m[2][2]).
    FfxFloat32x4x4 proj, toView;
    std::memcpy(proj, &m_aoProj, sizeof(proj));
    std::memcpy(toView, &m_aoView, sizeof(toView));   // (unused: it makes its own normals)
    FfxCacaoDispatchDescription d{};
    d.commandList = ffxGetCommandListVK(cmd);
    d.depthBuffer = FfxImage(m_depth, kDepthFormat, w, h, false, L"depth");
    d.normalBuffer = {};
    d.outputBuffer = FfxImage(m_cacaoOut->m_image, GetFormatInfo(Format::RGBA16F).vk, w, h, true, L"output");
    d.proj = &proj;
    d.normalsToView = &toView;
    d.normalUnpackMul = 1.0f;
    d.normalUnpackAdd = 0.0f;
    const FfxErrorCode e = ffxCacaoContextDispatch(&m_cacao->context, &d);
    m_cacaoOut->m_layout = VK_IMAGE_LAYOUT_GENERAL;  // (its barriers left it so)
    if (e != FFX_OK) {
        static bool logged;
        if (!logged) { logged = true; Log("CACAO: dispatch failed (error 0x%x): our AO", unsigned(e)); }
        return false;
    }
    Transition(cmd, m_cacaoOut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    float params[4] = {m_aoProj.m[2][2], m_aoProj.m[3][2], 0.0f, 0.0f};
    FullscreenPass(cmd, m_cacaoAo, m_cacaoPackPipeline, m_cacaoOut->m_view, m_depthView, m_pointSampler, params,
                   sizeof(params), false);
    Transition(cmd, m_cacaoAo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_aoFullTex = m_cacaoAo;
    return true;
}

}  // namespace rvk
