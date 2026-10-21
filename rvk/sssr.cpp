// RVK_SsrMode 1: AMD's stochastic screen-space reflections (FidelityFX SSSR 1.5 with its reflection denoiser; the SDK
// 1.1.4, third_party/ffx-sdk, MIT; docs/amd-fx-plan.md) in place of ours (ssr.frag). Per pixel that reflects, rays
// sampled from the surface's roughness lobe are traced through a hierarchy of the depth buffer (a mirror's straight
// along the reflection, a rough surface's scattered); a pixel whose surface is rougher than a threshold, and a ray
// leaving the screen, take the environment probe instead (env_common.glsl: the patched callback reads its atlas); the
// denoiser then averages them over neighbours and frames. Our roughness: none is stored, so gbuffer.frag makes one from
// the reflectivity the scene keeps (the more a surface reflects, the smoother) - the normals are the scene's own
// (the motion vectors' zw, while SceneNormalsWanted). sssr_resolve.frag turns its result into what ours hands the tone
// mapping, so the reflections' strength settings keep working.
//
// Its matrices are camera-relative (the world moved so the camera is at the origin): the game's world positions are
// thousands of units, and an inverse in float of the whole view x projection is off by more than the depth buffer's
// hits need. The previous frame's camera is moved by the same amount (in double).
#include "internal.h"
#include "ffx_common.h"

#include <FidelityFX/host/ffx_sssr.h>

#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

struct Device::SssrState {
    FfxBackend backend;
    FfxSssrContext context{};
    uint32_t w = 0, h = 0;
};

namespace {

// a x b (D3D row vectors), in double.
void Mul(const double (&a)[4][4], const double (&b)[4][4], double (&r)[4][4])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
}

void ToDouble(const d3d::Matrix& m, double (&r)[4][4])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r[i][j] = m.m[i][j];
}

// A D3D matrix's memory is the column-major array the SDK asks for of the same transform (its shaders multiply
// matrix x column vector).
void ToFfx(const double (&m)[4][4], float out[16])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = float(m[i][j]);
}

}  // namespace

void Device::DestroySssr()
{
    if (m_sssr) {
        WaitIdle();                              // (its images and pipelines go at once)
        ffxSssrContextDestroy(&m_sssr->context);
        delete m_sssr;
        m_sssr = nullptr;
    }
    if (m_sssrOut) { DestroyTextureNow(m_sssrOut); m_sssrOut = nullptr; }
}

// (At teardown: the scene's surfaces, motion and environment that AMD's effects share.)
void Device::DestroySceneSurfaces()
{
    for (Texture** t : {&m_sceneSurface, &m_sceneMotion, &m_sceneEnv})
        if (*t) { DestroyTextureNow(*t); *t = nullptr; }
}

// gbuffer.frag into m_sceneSurface: each pixel's world normal and roughness. Once a frame (SSSR and Brixelizer GI
// both read it).
bool Device::RenderSceneSurface(VkCommandBuffer cmd)
{
    if (m_sceneSurfaceFrame == m_frameNumber && m_sceneSurface)
        return true;
    if (!m_gbufferPipeline || !m_depth)
        return false;
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    if (!m_sceneSurface || m_sceneSurface->m_width != w || m_sceneSurface->m_height != h) {
        if (m_sceneSurface) DestroyTexture(m_sceneSurface);
        m_sceneSurface = CreateImage(w, h, Format::RGBA16F, 1, true);
        if (!m_sceneSurface)
            return false;
    }
    MakeDepthReadable(cmd);
    Transition(cmd, m_motionVectors, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, m_localFraction, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const auto& v = m_aoView.m;
    // Roughness: 0.6 for a surface that reflects a little, 0.03 for a mirror (the puddles, still water).
    float params[20] = {m_aoProj.m[2][2], m_aoProj.m[3][2], m_aoProj.m[0][0], m_aoProj.m[1][1],
                        float(w), float(h), 0.6f, 0.03f,
                        v[0][0], v[0][1], v[0][2], 0.0f,
                        v[1][0], v[1][1], v[1][2], 0.0f,
                        v[2][0], v[2][1], v[2][2], 0.0f};
    FullscreenPass(cmd, m_sceneSurface, m_gbufferPipeline, m_depthView, m_motionVectors->m_view, m_pointSampler, params,
                   sizeof(params), false, m_localFraction->m_view);
    Transition(cmd, m_sceneSurface, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_sceneSurfaceFrame = m_frameNumber;
    return true;
}

// The motion vectors whole into m_sceneMotion (motion_fill.frag, as FSR has them: the camera's where no draw wrote
// one), RG16F in pixels, now - then. Once a frame.
bool Device::RenderSceneMotion(VkCommandBuffer cmd)
{
    if (m_sceneMotionFrame == m_frameNumber && m_sceneMotion)
        return true;
    if (!m_motionFillPipeline || !m_depth || !m_motionVectors)
        return false;
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    if (!m_sceneMotion || m_sceneMotion->m_width != w || m_sceneMotion->m_height != h) {
        if (m_sceneMotion) DestroyTexture(m_sceneMotion);
        m_sceneMotion = CreateImage(w, h, Format::RG16F, 1, true);
        if (!m_sceneMotion)
            return false;
    }
    MakeDepthReadable(cmd);
    Transition(cmd, m_motionVectors, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const bool continuous = m_prevViewProjValid;
    d3d::Matrix reproject = Identity();
    if (continuous)
        CameraReprojection(m_aoView, m_aoProj, m_prevView, m_prevProj, &reproject);
    float fill[20];
    std::memcpy(fill, &reproject, 64);
    fill[16] = float(w);
    fill[17] = float(h);
    fill[18] = continuous ? 1.0f : 0.0f;         // object motion written this frame (MotionVectorDraw)
    fill[19] = continuous ? 1.0f : 0.0f;         // the camera's reprojection known
    FullscreenPass(cmd, m_sceneMotion, m_motionFillPipeline, m_motionVectors->m_view, m_depthView, m_pointSampler,
                   fill, sizeof(fill), false);
    Transition(cmd, m_sceneMotion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_sceneMotionFrame = m_frameNumber;
    return true;
}

// The environment AMD's effects fall back to (sssr_env.frag) into m_sceneEnv: the probe's atlas where it is sure,
// else the frame's own sky where that way is on screen, else the sky guessed as the scene shader's wet ground has it
// (ffp_main.glsl WetReflection). Once a frame; the HDR scene must be readable.
bool Device::RenderSceneEnv(VkCommandBuffer cmd)
{
    if (m_sceneEnvFrame == m_frameNumber && m_sceneEnv)
        return true;
    if (!m_sssrEnvPipeline || !m_depth)
        return false;
    if (!m_sceneEnv) {
        m_sceneEnv = CreateImage(192, 128, Format::RGBA16F, 1, true);
        if (!m_sceneEnv)
            return false;
    }
    MakeDepthReadable(cmd);
    const bool probe = m_envReady && m_envAtlas;
    Texture* atlas = probe ? m_envAtlas : m_blackTexture;
    Transition(cmd, atlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const uint32_t a = m_rs[d3d::RS_AMBIENT], f = m_rs[d3d::RS_FOGCOLOR];
    const float storm = 1.0f - 0.75f * StormOvercast();
    float push[28];
    for (int i = 0; i < 3; ++i) {
        const float sun = m_frameSunColor[i];
        const float amb = float((a >> (16 - 8 * i)) & 0xFFu) / 255.0f * storm + sun * 0.15f;
        const float fog = float((f >> (16 - 8 * i)) & 0xFFu) / 255.0f;
        push[i] = amb;
        push[4 + i] = (m_rs[d3d::RS_FOGENABLE] ? 0.5f * (amb + fog) : amb) + sun * 0.25f;
        push[8 + i] = m_sheetSky[i];
    }
    push[3] = probe ? 1.0f : 0.0f;
    push[7] = push[11] = 0.0f;
    const d3d::Matrix viewProj = MulMatrix(m_aoView, m_aoProj);
    std::memcpy(push + 12, &viewProj, 64);
    FullscreenPass(cmd, m_sceneEnv, m_sssrEnvPipeline, atlas->m_view, m_scene->m_view, m_pointSampler, push,
                   sizeof(push), false, m_depthView);
    Transition(cmd, m_sceneEnv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_sceneEnvFrame = m_frameNumber;
    return true;
}

// AMD's reflections into m_ssrTex, as RenderSsr leaves ours. False: they couldn't run (ours then).
bool Device::RenderSssr(VkCommandBuffer cmd)
{
    bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (m_ssr <= 0.0f || !perspective || !m_depth || !m_motionFillPipeline || !m_sssrResolvePipeline || !m_sssrEnvPipeline)
        return false;
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    if (m_sssr && (m_sssr->w != w || m_sssr->h != h))
        DestroySssr();
    if (!m_sssr) {
        auto* s = new SssrState;
        FfxSssrContextDescription desc{};
        FfxErrorCode e = s->backend.Init(m_device, m_physical, FFX_SSSR_CONTEXT_COUNT);
        desc.backendInterface = s->backend.iface;
        desc.flags = 0;                          // (depth 0 near)
        desc.renderSize = {w, h};
        desc.normalsHistoryBufferFormat = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        if (e == FFX_OK)
            e = ffxSssrContextCreate(&s->context, &desc);
        if (e != FFX_OK) {
            static bool logged;
            if (!logged) { logged = true; Log("SSSR: the context could not be made (error 0x%x): our reflections", unsigned(e)); }
            delete s;
            return false;
        }
        s->w = w;
        s->h = h;
        m_sssr = s;
        Log("SSSR: AMD's reflections at %ux%u", w, h);
    }
    for (Texture** t : {&m_sssrOut, &m_ssrTex}) {
        if (*t && (*t)->m_width == w && (*t)->m_height == h) continue;
        if (*t) DestroyTexture(*t);
        *t = CreateImage(w, h, Format::RGBA16F, 1, true, t == &m_sssrOut);
        if (!*t)
            return false;
    }
    if (!RenderSceneSurface(cmd) || !RenderSceneMotion(cmd) || !RenderSceneEnv(cmd))
        return false;
    double v[4][4], p[4][4];
    ToDouble(m_aoView, v);
    ToDouble(m_aoProj, p);
    const double eye[3] = {-(v[3][0] * v[0][0] + v[3][1] * v[0][1] + v[3][2] * v[0][2]),
                           -(v[3][0] * v[1][0] + v[3][1] * v[1][1] + v[3][2] * v[1][2]),
                           -(v[3][0] * v[2][0] + v[3][1] * v[2][1] + v[3][2] * v[2][2])};
    const bool continuous = m_prevViewProjValid;
    Texture* env = m_sceneEnv;
    Transition(cmd, m_sssrOut, VK_IMAGE_LAYOUT_GENERAL);
    // (What the fragment passes wrote, for its compute passes: its barriers only move the layouts.)
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

    // The camera-relative matrices (see the top).
    double view[4][4], invView[4][4] = {}, viewProj[4][4];
    std::memcpy(view, v, sizeof(view));
    view[3][0] = view[3][1] = view[3][2] = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) invView[i][j] = v[j][i];
    invView[3][3] = 1.0;
    Mul(view, p, viewProj);
    d3d::Matrix invProjF;
    if (!InvertMatrix(m_aoProj, &invProjF))
        return false;
    double invProj[4][4], invViewProj[4][4], prevViewProj[4][4];
    ToDouble(invProjF, invProj);
    Mul(invProj, invView, invViewProj);
    if (continuous) {
        double pv[4][4], pp[4][4], shift[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {eye[0], eye[1], eye[2], 1}}, t[4][4];
        ToDouble(m_prevView, pv);
        ToDouble(m_prevProj, pp);
        Mul(shift, pv, t);
        Mul(t, pp, prevViewProj);
    } else {
        std::memcpy(prevViewProj, viewProj, sizeof(viewProj));
    }

    FfxSssrDispatchDescription d{};
    d.commandList = ffxGetCommandListVK(cmd);
    d.color = FfxImage(m_scene->m_image, GetFormatInfo(Format::RGBA16F).vk, w, h, false, L"color");
    d.depth = FfxImage(m_depth, kDepthFormat, w, h, false, L"depth");
    d.motionVectors = FfxImage(m_sceneMotion->m_image, GetFormatInfo(Format::RG16F).vk, w, h, false, L"motion");
    d.normal = FfxImage(m_sceneSurface->m_image, GetFormatInfo(Format::RGBA16F).vk, w, h, false, L"normal");
    d.materialParameters = FfxImage(m_sceneSurface->m_image, GetFormatInfo(Format::RGBA16F).vk, w, h, false, L"roughness");
    d.environmentMap = FfxImage(env->m_image, GetFormatInfo(env->m_format).vk, env->m_width, env->m_height, false, L"environment");
    d.brdfTexture = d.environmentMap;            // (registered, read by no pass)
    d.output = FfxImage(m_sssrOut->m_image, GetFormatInfo(Format::RGBA16F).vk, w, h, true, L"output");
    ToFfx(invViewProj, d.invViewProjection);
    ToFfx(p, d.projection);
    ToFfx(invProj, d.invProjection);
    ToFfx(view, d.view);
    ToFfx(invView, d.invView);
    ToFfx(prevViewProj, d.prevViewProjection);
    d.renderSize = {w, h};
    d.motionVectorScale = {-1.0f / float(w), -1.0f / float(h)};   // ours: pixels, now - then; its: uv, then - now
    d.iblFactor = 1.0f;
    d.normalUnPackMul = 1.0f;
    d.normalUnPackAdd = 0.0f;
    d.roughnessChannel = 3;
    d.isRoughnessPerceptual = true;
    d.temporalStabilityFactor = 0.7f;
    d.depthBufferThickness = m_sssrThick;
    d.roughnessThreshold = 0.5f;
    d.varianceThreshold = 0.0f;
    d.maxTraversalIntersections = 128;
    d.minTraversalOccupancy = 4;
    d.mostDetailedMip = 0;
    d.samplesPerQuad = 1;
    d.temporalVarianceGuidedTracingEnabled = 1;
    const FfxErrorCode e = ffxSssrContextDispatch(&m_sssr->context, &d);
    m_sssrOut->m_layout = VK_IMAGE_LAYOUT_GENERAL;   // (its barriers left it so)
    if (e != FFX_OK) {
        static bool logged;
        if (!logged) { logged = true; Log("SSSR: dispatch failed (error 0x%x): our reflections", unsigned(e)); }
        return false;
    }
    Transition(cmd, m_sssrOut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Into what the tone mapping takes (sssr_resolve.frag).
    float params[20] = {m_aoProj.m[2][2], m_aoProj.m[3][2], m_aoProj.m[0][0], m_aoProj.m[1][1],
                        float(w), float(h), m_ssr, 0.0f,
                        float(v[0][0]), float(v[0][1]), float(v[0][2]), 0.0f,
                        float(v[1][0]), float(v[1][1]), float(v[1][2]), 0.0f,
                        float(v[2][0]), float(v[2][1]), float(v[2][2]), 0.0f};
    FullscreenPass(cmd, m_ssrTex, m_sssrResolvePipeline, m_sssrOut->m_view, m_sceneSurface->m_view, m_pointSampler,
                   params, sizeof(params), false, m_localFraction->m_view, m_depthView);
    Transition(cmd, m_ssrTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

}  // namespace rvk
