// How dark the game gets (docs/darkness-plan.md): RVK_Darkness, the one setting every darkening reads - the tone
// mapping's night darkening, the sky and the clouds at night, the moon's light, a storm's darkening - and a floor
// under the land's brightness. Each darkening was tuned on its own screenshot, and together they multiplied: a night
// thunderstorm or a dim room went near black. The floor catches what they still leave too dark: once a frame the land's
// brightness as it will show is measured (scene_luma.comp) and, below the floor, the exposure raised towards it over a
// couple of seconds, as eyes adapt to the dark.
#include "internal.h"

#include <algorithm>
#include <cmath>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kSceneLumaSpirv[] = {
#include "scene_luma.comp.inc"
};


constexpr float kFloor = 0.12f;                  // the floor at RVK_Darkness 0 (the land's log-average, as shown)
constexpr float kMaxGain = 2.5f;                 // the most the floor raises the exposure

}  // namespace

bool Device::CreateDarknessResources(std::string* error)
{
    const VkShaderStageFlags comp = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
                                               {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, comp, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 3;
    li.pBindings = b;
    VkPushConstantRange push{comp, 0, 32};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_lumaSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_lumaSetLayout), "scene brightness set", error) ||
        !Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_lumaLayout), "scene brightness layout", error))
        return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(kSceneLumaSpirv);
    mi.pCode = kSceneLumaSpirv;
    VkShaderModule module;
    if (!Check(vkCreateShaderModule(m_device, &mi, nullptr, &module), "scene brightness module", error))
        return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main",
                nullptr};
    ci.layout = m_lumaLayout;
    const bool ok = Check(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_lumaPipeline),
                          "scene brightness pipeline", error);
    vkDestroyShaderModule(m_device, module, nullptr);
    if (!ok)
        return false;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    m_lumaStride = std::max<VkDeviceSize>(256, m_props.limits.minStorageBufferOffsetAlignment);
    bi.size = m_lumaStride * kFramesInFlight;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO;
    ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &m_lumaBuffer, &m_lumaAllocation, &info), "scene brightness buffer",
               error))
        return false;
    m_lumaCpu = static_cast<uint8_t*>(info.pMappedData);
    return true;
}

void Device::DestroyDarknessResources()
{
    if (m_lumaPipeline) { vkDestroyPipeline(m_device, m_lumaPipeline, nullptr); m_lumaPipeline = VK_NULL_HANDLE; }
    if (m_lumaLayout) { vkDestroyPipelineLayout(m_device, m_lumaLayout, nullptr); m_lumaLayout = VK_NULL_HANDLE; }
    if (m_lumaSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_lumaSetLayout, nullptr); m_lumaSetLayout = VK_NULL_HANDLE; }
    if (m_lumaBuffer) { vmaDestroyBuffer(m_allocator, m_lumaBuffer, m_lumaAllocation); m_lumaBuffer = VK_NULL_HANDLE; }
    m_lumaCpu = nullptr;
    for (bool& m : m_lumaMeasured) m = false;
}

// Once a frame, after the sun's clock (its height) and before the moon (which takes the moon's share): what each
// darkening takes of RVK_Darkness, and the floor's exposure gain eased towards what the last measure asks.
void Device::UpdateDarkness()
{
    const float d = std::clamp(m_darkness, 0.0f, 2.0f);
    DarkFactors& f = m_dark;
    // The tone mapping's night darkening: the setting itself (1: the old RVK_NightDark 1). Without a sun (a room's
    // playfield) half a step less - none at the default: a dim room is not a night, and the game lit it dim already.
    m_darkIndoors = !SunKnown();
    f.tone = m_darkIndoors ? std::max(d - 0.5f, 0.0f) : d;
    // The sky and clouds at night: lighter below 1, darker above (log10 of the sky's factor) - coming in with the night
    // itself, the sun from ~8 to ~14 degrees down: the blue hour before it is the same at every setting.
    const float lift = d <= 1.0f ? 0.3f * (1.0f - d) : -0.25f * (d - 1.0f);
    const float deep = m_cond.deepNight;
    f.skyLog = lift * deep;
    f.clouds = std::pow(10.0f, 0.8f * f.skyLog);
    // The moon's light: stronger on a lighter night, weaker on a darker one.
    f.moon = d <= 1.0f ? 1.0f + 0.6f * (1.0f - d) : 1.0f - 0.3f * (d - 1.0f);
    // A storm's darkening (StormOvercast): 0.6 .. 1.4 of it.
    f.storm = 0.6f + 0.4f * d;
    // The floor: none at 2.
    const float open = 1.0f - 0.5f * d;
    f.floor = kFloor * open * open * std::clamp(m_darkFloorScale, 0.0f, 2.0f);
    // The gain: the floor over the land's brightness as last measured (a frame or two old; open loop - measured
    // without the gain), at most kMaxGain; eased up over ~2.5 s, down over ~1 s. Without a measure lately (no world
    // drawn: login, loading; the sky filling the screen) back to 1.
    const bool fresh = m_landLumaFrame != ~0ull && m_landLumaFrame + 30 >= m_frameNumber;
    const float target = fresh && f.floor > 0.0f && m_landLuma > 0.0f
                             ? std::clamp(f.floor / m_landLuma, 1.0f, kMaxGain)
                             : 1.0f;
    const float tau = target > m_darkGain ? 2.5f : 1.0f;
    m_darkGain += (target - m_darkGain) * (1.0f - std::exp(-m_frameDt / tau));
    if (std::fabs(m_darkGain - 1.0f) < 1e-3f && target == 1.0f) m_darkGain = 1.0f;
    if (m_frameNumber % 600 == 37)
        Log("darkness: setting %.2f, night %.2f (%s); tone %.2f, sky x%.2f, clouds x%.2f, moon x%.2f, storm x%.2f; "
            "land %.3f (floor %.3f) -> gain %.2f", double(d), double(NightAmount()), m_darkIndoors ? "no sun: indoors" : "outdoors",
            double(f.tone), double(std::pow(10.0f, f.skyLog)), double(f.clouds), double(f.moon), double(f.storm),
            double(fresh ? m_landLuma : -1.0f), double(f.floor), double(m_darkGain));
}

// The land's brightness as it will show (scene_luma.comp), before the tone mapping: the scene's post passes done, the
// depth readable (else the sky counted with it). This frame slot's last measure read first - its commands are done.
void Device::MeasureSceneLuma(VkCommandBuffer cmd)
{
    if (m_lumaMeasured[m_frameIndex] && m_lumaCpu) {
        vmaInvalidateAllocation(m_allocator, m_lumaAllocation, m_lumaStride * m_frameIndex, 16);
        const float* out = reinterpret_cast<const float*>(m_lumaCpu + m_lumaStride * m_frameIndex);
        // (A few land samples only - the sky filling the screen - say little: kept as known.)
        if (out[1] >= 64.0f && std::isfinite(out[0])) {
            m_landLuma = std::exp2(out[0] / out[1]);
            m_landLumaFrame = m_frameNumber;
        }
        m_lumaMeasured[m_frameIndex] = false;
    }
    if (!m_lumaPipeline || !m_lumaCpu || m_dark.floor <= 0.0f || !m_aoProjValid)
        return;
    const bool depth = m_depthLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    struct Push { float info[4], tone[4]; } push{};
    push.info[0] = float(m_scene->m_width);
    push.info[1] = float(m_scene->m_height);
    push.info[2] = m_prepassClearZ;
    push.info[3] = depth ? 1.0f : 0.0f;
    push.tone[0] = m_exposure;
    push.tone[1] = m_dark.tone * NightAmount();
    // (The scene's writes and read layout were made for fragment shaders - this reads them in a compute one.)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_lumaPipeline);
    VkDescriptorImageInfo sceneInfo{m_pointSampler, m_scene->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo depthInfo{m_pointSampler, depth ? m_depthView : m_scene->m_view,   // (unread without)
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo outInfo{m_lumaBuffer, m_lumaStride * m_frameIndex, 16};
    VkWriteDescriptorSet wr[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    wr[0].pImageInfo = &sceneInfo;
    wr[1].pImageInfo = &depthInfo;
    wr[2].pBufferInfo = &outInfo;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_lumaLayout, 0, 3, wr);
    vkCmdPushConstants(cmd, m_lumaLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, 1, 1, 1);
    // (For the CPU, once the frame's commands are done; and the scene read again by the tone mapping's fragments.)
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
    m_lumaMeasured[m_frameIndex] = true;
}

}  // namespace rvk
