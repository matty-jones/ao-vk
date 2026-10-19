// Footprints in the sand (docs/desert-plan.md phase 5, RVK_SandPrints; sand_prints.frag). The characters the grass
// already follows (UpdatePushTrail) leave a print a step apart, left and right by turns; the prints are pressed into a
// map round the eye - 2048 texels a side at 2 cm (41 world units), laid toroidally so it follows the eye without being
// copied - and relax over a few minutes. The native ground reads it on its sand (parallax.glsl SandPrintAt): drawn in
// depth as its ripples are, the ripples pressed flat inside a print and a low rim round it catching the sun.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {
constexpr int kPrintN = 2048;                    // texels a side (sand_prints.frag, parallax.glsl)
constexpr float kPrintCell = 0.02f;              // world units a texel (at 4 cm a print's 13 cm width was 3 texels: its
                                                 // walls filtered nearly flat)
constexpr uint32_t kPrintMax = 64;               // prints stamped a frame (the rest wait for the next)
constexpr double kPrintRelax = 150.0;            // seconds for a print to soften to a third
}

void Device::UpdateSandPrints(VkCommandBuffer cmd)
{
    if (!SandPrintsOn() || !m_printPipeline) {
        m_printValid = false;
        m_printsNew.clear();
        return;
    }
    for (Texture*& t : m_printMap)
        if (!t && !(t = CreateImage(kPrintN, kPrintN, Format::RG16F, 1, true))) {
            m_printsNew.clear();
            return;
        }
    const int origin[2] = {int(std::floor(m_frameEye[0] / kPrintCell)) - kPrintN / 2,
                           int(std::floor(m_frameEye[2] / kPrintCell)) - kPrintN / 2};
    const double now = SwayClock();
    const double dt = m_printValid ? std::clamp(now - m_printTime, 0.0, 1.0) : 0.0;
    m_printTime = now;
    // The prints in the map, the nearest the frame can take (a crowd's more wait, as long as they stay in it).
    const float corner[2] = {float(origin[0]) * kPrintCell, float(origin[1]) * kPrintCell};
    const float size = float(kPrintN) * kPrintCell;
    std::vector<SandPrint> keep;
    struct Gpu { float at[kPrintMax][4]; } prints{};
    uint32_t count = 0;
    for (const SandPrint& p : m_printsNew) {
        const float x = p.x - corner[0], z = p.z - corner[1];
        if (x < 0.3f || z < 0.3f || x > size - 0.3f || z > size - 0.3f)
            continue;
        if (count == kPrintMax) { keep.push_back(p); continue; }
        prints.at[count][0] = x; prints.at[count][1] = z;
        prints.at[count][2] = p.dx; prints.at[count][3] = p.dz;
        ++count;
    }
    m_printsNew.swap(keep);
    m_printsStamped += count;
    if (m_frameNumber % 600 == 50)
        Log("sand prints: %llu stamped so far, %zu waiting; the map's corner %.1f %.1f",
            static_cast<unsigned long long>(m_printsStamped), m_printsNew.size(), double(corner[0]), double(corner[1]));
    if (m_printsNew.size() > 4 * kPrintMax)
        m_printsNew.erase(m_printsNew.begin(), m_printsNew.end() - 4 * kPrintMax);

    EnsureRingSpace(sizeof(Gpu) + 512);
    void* cpu = nullptr;
    const VkDeviceSize offset = Allocate(sizeof(Gpu), std::max<VkDeviceSize>(256, m_props.limits.minStorageBufferOffsetAlignment), &cpu);
    std::memcpy(cpu, &prints, sizeof(Gpu));
    Frame& f = m_frames[m_frameIndex];
    const uint32_t next = m_printCur ^ 1u;
    Texture* last = m_printMap[m_printCur];
    Texture* into = m_printMap[next];
    Transition(cmd, last, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // (the first time: its contents unused, P.p.w)
    Transition(cmd, into, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = into->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {uint32_t(kPrintN), uint32_t(kPrintN)}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport viewport{0.0f, 0.0f, float(kPrintN), float(kPrintN), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {uint32_t(kPrintN), uint32_t(kPrintN)}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_printPipeline);
    VkDescriptorImageInfo lastInfo{m_pointSampler, last->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo printsInfo{f.ring, offset, sizeof(Gpu)};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &lastInfo;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &printsInfo;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_printLayout, 0, 2, w);
    struct Push { int32_t origin[4]; float p[4]; } push{};
    push.origin[0] = origin[0]; push.origin[1] = origin[1];
    push.origin[2] = m_printOrigin[0]; push.origin[3] = m_printOrigin[1];
    push.p[0] = float(std::exp(-dt / kPrintRelax));
    push.p[1] = float(count);
    push.p[2] = std::clamp(m_terrainSettings.prints, 0.0f, 1.0f);
    push.p[3] = m_printValid ? 0.0f : 1.0f;
    vkCmdPushConstants(cmd, m_printLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    Transition(cmd, into, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_printCur = next;
    m_printOrigin[0] = origin[0];
    m_printOrigin[1] = origin[1];
    m_printValid = true;
}

// The frame lights' part (draw.cpp; FL.sand2.yzw): the map's bindless slot (0: none), its corner (world x, z).
void Device::SandPrintLights(float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    Texture* map = m_printMap[m_printCur];
    if (!m_printValid || !map || map->m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return;
    const uint32_t image = BindlessImage(map), sampler = FixedSamplerSlot(0, m_bumpSampler);   // (repeating: toroidal)
    if (image == ~0u || image >= (1u << 20) - 1 || sampler >= 256)
        return;
    const uint32_t slot = (image + 1) | sampler << 20;
    std::memcpy(&out[0], &slot, 4);
    out[1] = float(m_printOrigin[0]) * kPrintCell;
    out[2] = float(m_printOrigin[1]) * kPrintCell;
}

} // namespace rvk
