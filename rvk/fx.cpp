// Our own effects (docs/fx-plan.md): the game's effects replaced by particles of ours. The proxy defines them from
// .rvkfx files (FxSetEffect) and announces each one playing every frame (FxInstance): which effect, a key stable while
// it lives (the game's handle), its points - head, source, target - and the phase it is in.
//
// Each emitter of a playing effect gets a block of particles. At the start of the next frame (SimulateFx) the CPU works
// out each emitter's point, how many particles it spawns (its rate over the frame, a burst when one of its phases
// starts) and its parameters; one compute pass (shaders/fx.comp) spawns, moves and writes every particle's quad in the
// game's sprite vertex format; at the scene's end (DrawFx) each block is drawn through the scene's draw path with its
// emitter's sprite, blending and brightness - so effects take the scene's fog, HDR and glow like the game's sprites. An
// effect no longer announced spawns nothing more; its particles live out their lives, then its blocks are freed.
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kFxSpirv[] = {
#include "fx.comp.inc"
};

constexpr uint32_t kPerBlock = fx::kMaxParticles;   // must match fx.comp's PER_BLOCK
constexpr uint32_t kQuadStride = 24;                // FVF 0x142
constexpr uint32_t kFxFvf = 0x142;                  // XYZ | DIFFUSE | TEX1
constexpr uint32_t kGroupSize = 64;
constexpr uint32_t kAtlasCell = 128, kAtlasColumns = 4, kAtlasRows = 2;

// GPU layouts (std430), must match fx.comp.
struct GpuBlock {
    float point[4], prevPoint[4], shape[4], dir[4], lifeSpeed[4], forces[4], gravity[4], misc[4], vel[4];
    float sizeT[4], sizeV[4], colourT[4], colour[4][4], uv[4];
    uint32_t keys[4];
    float pathStart[4];
};
static_assert(sizeof(GpuBlock) == 19 * 16, "fx.comp Block");
struct GpuParticle {
    float posAge[4], velLife[4];
    uint32_t meta[4];
};

double Clock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float BitsAsFloat(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

const float* PointOf(const Device::FxAnchors& a, uint32_t at)
{
    return at == fx::kAtSource ? a.source : at == fx::kAtTarget ? a.target : a.head;
}

void Normalize(float v[3])
{
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f)
        for (int i = 0; i < 3; ++i) v[i] /= l;
}

}  // namespace

bool Device::CreateFxResources(std::string* error)
{
    VkDescriptorSetLayoutBinding b[3];
    for (uint32_t i = 0; i < 3; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 3;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_fxSetLayout), "fx set layout", error))
        return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_fxSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_fxLayout), "fx pipeline layout", error))
        return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(kFxSpirv);
    mi.pCode = kFxSpirv;
    VkShaderModule module;
    if (!Check(vkCreateShaderModule(m_device, &mi, nullptr, &module), "fx shader module", error))
        return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    ci.layout = m_fxLayout;
    const bool ok = Check(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_fxPipeline),
                          "fx pipeline", error);
    vkDestroyShaderModule(m_device, module, nullptr);
    if (!ok)
        return false;
    auto buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, bool host, VkBuffer* out, VmaAllocation_T** alloc,
                      void** mapped) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        if (host)
            ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info;
        if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, out, alloc, &info), "fx buffer", error))
            return false;
        if (mapped) *mapped = info.pMappedData;
        return true;
    };
    if (!buffer(VkDeviceSize(kFxBlocks) * kPerBlock * sizeof(GpuParticle),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, &m_fxState,
                &m_fxStateAllocation, nullptr))
        return false;
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        if (!buffer(VkDeviceSize(kFxDrawnBlocks) * kPerBlock * 4 * kQuadStride,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false, &m_fxQuads[i],
                    &m_fxQuadsAllocation[i], nullptr))
            return false;
    void* mapped = nullptr;
    if (!buffer(VkDeviceSize(kPerBlock) * 6 * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, &m_fxIndices,
                &m_fxIndicesAllocation, &mapped))
        return false;
    auto* idx = static_cast<uint16_t*>(mapped);
    for (uint32_t q = 0; q < kPerBlock; ++q) {
        const uint16_t v = uint16_t(q * 4);
        const uint16_t tri[6] = {v, uint16_t(v + 1), uint16_t(v + 2), uint16_t(v + 1), uint16_t(v + 2), uint16_t(v + 3)};
        std::memcpy(idx + q * 6, tri, sizeof(tri));
    }
    vmaFlushAllocation(m_allocator, m_fxIndicesAllocation, 0, VK_WHOLE_SIZE);
    m_fxFreeBlocks.clear();
    for (uint32_t i = kFxBlocks; i-- > 0;)
        m_fxFreeBlocks.push_back(i);
    return true;
}

void Device::DestroyFxResources()
{
    if (m_fxPipeline) vkDestroyPipeline(m_device, m_fxPipeline, nullptr);
    if (m_fxLayout) vkDestroyPipelineLayout(m_device, m_fxLayout, nullptr);
    if (m_fxSetLayout) vkDestroyDescriptorSetLayout(m_device, m_fxSetLayout, nullptr);
    if (m_fxState) vmaDestroyBuffer(m_allocator, m_fxState, m_fxStateAllocation);
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        if (m_fxQuads[i]) vmaDestroyBuffer(m_allocator, m_fxQuads[i], m_fxQuadsAllocation[i]);
    if (m_fxIndices) vmaDestroyBuffer(m_allocator, m_fxIndices, m_fxIndicesAllocation);
    if (m_fxAtlas) DestroyTexture(m_fxAtlas);
    m_fxAtlas = nullptr;
}

// The sprites (fx::Sprite), white with their shape in alpha, cells of an atlas: a soft dot, a spark (a streak along
// u), a ring, a four-pointed star, a puff of smoke (soft, broken by noise), a flare (a hot core in a wide halo).
void Device::MakeFxAtlas()
{
    const uint32_t w = kAtlasCell * kAtlasColumns, h = kAtlasCell * kAtlasRows;
    std::vector<uint32_t> px(size_t(w) * h, 0x00FFFFFFu);
    auto hash = [](uint32_t x) {
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return float(x >> 8) * (1.0f / 16777216.0f);
    };
    auto noise = [&](float x, float y) {         // value noise, smooth, a few octaves
        float sum = 0.0f, amp = 0.5f;
        for (int o = 0; o < 4; ++o) {
            const int ix = int(std::floor(x)), iy = int(std::floor(y));
            const float fx = x - float(ix), fy = y - float(iy);
            const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
            auto at = [&](int a, int b) { return hash(uint32_t(a * 73856093) ^ uint32_t(b * 19349663) ^ uint32_t(o * 83492791)); };
            const float v = (at(ix, iy) * (1 - sx) + at(ix + 1, iy) * sx) * (1 - sy) +
                            (at(ix, iy + 1) * (1 - sx) + at(ix + 1, iy + 1) * sx) * sy;
            sum += v * amp;
            amp *= 0.5f;
            x *= 2.0f;
            y *= 2.0f;
        }
        return sum;
    };
    for (uint32_t s = 0; s < fx::kSpriteCount; ++s) {
        const uint32_t cx = (s % kAtlasColumns) * kAtlasCell, cy = (s / kAtlasColumns) * kAtlasCell;
        for (uint32_t y = 0; y < kAtlasCell; ++y)
            for (uint32_t x = 0; x < kAtlasCell; ++x) {
                const float u = (float(x) + 0.5f) / float(kAtlasCell) * 2.0f - 1.0f;
                const float v = (float(y) + 0.5f) / float(kAtlasCell) * 2.0f - 1.0f;
                const float r2 = u * u + v * v, r = std::sqrt(r2);
                float a = 0.0f;
                switch (s) {
                case fx::kDot: a = std::exp(-r2 * 6.0f); break;
                case fx::kSpark: a = std::exp(-u * u * 2.5f - v * v * 40.0f); break;
                case fx::kRingSprite: a = std::exp(-(r - 0.7f) * (r - 0.7f) * 120.0f); break;
                case fx::kStar:
                    a = std::max(std::exp(-u * u * 60.0f - v * v * 3.0f), std::exp(-v * v * 60.0f - u * u * 3.0f)) +
                        std::exp(-r2 * 18.0f);
                    break;
                case fx::kSmoke: a = std::exp(-r2 * 3.0f) * std::clamp(noise(u * 2.5f + 7.0f, v * 2.5f + 3.0f) * 1.8f - 0.25f, 0.0f, 1.0f); break;
                case fx::kFlare: a = std::exp(-r2 * 40.0f) + 0.35f * std::exp(-r2 * 4.0f); break;
                }
                if (r >= 1.0f) a = 0.0f;
                const uint32_t alpha = uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
                px[size_t(cy + y) * w + cx + x] = (alpha << 24) | 0x00FFFFFFu;
            }
    }
    m_fxAtlas = CreateTexture(w, h, px.data());
}

void Device::FxSetEffect(uint32_t id, std::shared_ptr<const fx::Effect> effect)
{
    if (effect)
        m_fxEffects[id] = std::move(effect);
}

void Device::FxForget(uint32_t id)
{
    m_fxEffects.erase(id);
}

void Device::FxInstance(uint64_t key, uint32_t effect, const FxAnchors& anchors)
{
    auto def = m_fxEffects.find(effect);
    if (def == m_fxEffects.end())
        return;
    FxPlaying* p = nullptr;
    for (FxPlaying& q : m_fxPlaying)
        if (q.key == key && !q.gone) {
            p = &q;
            break;
        }
    if (!p) {
        if (m_fxPlaying.size() >= 1024)
            return;
        m_fxPlaying.push_back({});
        p = &m_fxPlaying.back();
        p->key = key;
        p->noise = float(key % 997) * 3.17f;
    }
    if (p->effect != effect || p->def != def->second) {   // (a new effect, or this one redefined: its blocks again)
        for (FxEmitterRun& r : p->runs)
            if (r.block >= 0) m_fxFreeBlocks.push_back(uint32_t(r.block));
        p->runs.assign(def->second->emitters.size(), FxEmitterRun{});
        p->effect = effect;
        p->def = def->second;
    }
    p->now = anchors;
    p->lastSeen = m_frameNumber;
}

void Device::SimulateFx(VkCommandBuffer cmd)
{
    m_fxDrawn.clear();
    m_fxDraws = 0;
    const double now = Clock();
    const float dt = m_fxLastClock > 0.0 ? float(std::clamp(now - m_fxLastClock, 0.0, 0.05)) : 0.0f;
    m_fxLastClock = now;
    m_fxTime += dt;
    // Effects announced last frame play on; the others have ended: their particles live out their lives, then their
    // blocks are given back.
    for (FxPlaying& p : m_fxPlaying) {
        const bool announced = p.lastSeen + 1 == m_frameNumber;
        if (!announced && !p.gone) {
            p.gone = true;
            p.goneAt = now;
        }
    }
    std::erase_if(m_fxPlaying, [&](FxPlaying& p) {
        if (!p.gone || now - p.goneAt <= double(fx::LongestLife(*p.def)) + 0.1)
            return false;
        for (FxEmitterRun& r : p.runs)
            if (r.block >= 0) m_fxFreeBlocks.push_back(uint32_t(r.block));
        return true;
    });
    if (m_fxPlaying.empty() || !m_fxPipeline)
        return;

    // The camera the quads face: the last frame's world camera.
    const auto& v = m_aoView.m;
    float eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
    struct Candidate { uint32_t playing, emitter; float distance; };
    std::vector<Candidate> candidates;
    for (uint32_t i = 0; i < m_fxPlaying.size(); ++i) {
        const FxPlaying& p = m_fxPlaying[i];
        const float d = (p.now.head[0] - eye[0]) * (p.now.head[0] - eye[0]) + (p.now.head[1] - eye[1]) * (p.now.head[1] - eye[1]) +
                        (p.now.head[2] - eye[2]) * (p.now.head[2] - eye[2]);
        for (uint32_t e = 0; e < p.def->emitters.size(); ++e) candidates.push_back({i, e, d});
    }
    if (candidates.size() > kFxDrawnBlocks) {        // the nearest (the others pause)
        std::nth_element(candidates.begin(), candidates.begin() + kFxDrawnBlocks, candidates.end(),
                         [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
        candidates.resize(kFxDrawnBlocks);
    }

    if (!m_fxStateCleared) {                         // all particles start dead (life 0)
        vkCmdFillBuffer(cmd, m_fxState, 0, VK_WHOLE_SIZE, 0);
        m_fxStateCleared = true;
    }
    const VkDeviceSize align = m_props.limits.minStorageBufferOffsetAlignment;
    const VkDeviceSize blockBytes = VkDeviceSize(std::max<size_t>(candidates.size(), 1)) * sizeof(GpuBlock);
    EnsureRingSpace(blockBytes + align);
    void* cpu;
    const VkDeviceSize blockOffset = Allocate(blockBytes, align, &cpu);
    auto* gb = static_cast<GpuBlock*>(cpu);
    uint32_t n = 0;
    for (const Candidate& c : candidates) {
        FxPlaying& p = m_fxPlaying[c.playing];
        const fx::Emitter& e = p.def->emitters[c.emitter];
        FxEmitterRun& run = p.runs[c.emitter];
        if (run.block < 0) {
            if (m_fxFreeBlocks.empty())
                continue;
            run.block = int32_t(m_fxFreeBlocks.back());
            m_fxFreeBlocks.pop_back();
            run.reset = true;
            run.cursor = 0;
        }
        const FxAnchors& a = p.now;
        const FxAnchors& b = p.havePrev ? p.prev : p.now;
        // How many it spawns: its rate over the frame while playing in one of its phases, and its burst as one starts.
        uint32_t spawn = 0;
        const bool playing = !p.gone && (e.during & a.phase);
        if (playing) {
            run.carry += e.rate * dt;
            spawn = uint32_t(run.carry);
            run.carry -= float(spawn);
            if (a.phase != p.lastPhase)
                spawn += uint32_t(e.burst);
        } else {
            run.carry = 0.0f;
        }
        spawn = std::min(spawn, e.count);
        const uint32_t count = std::min(e.count, kPerBlock);
        GpuBlock& g = gb[n];
        std::memset(&g, 0, sizeof(g));
        const float* pt = PointOf(a, e.at);
        const float* prevPt = PointOf(b, e.at);
        std::memcpy(g.point, pt, 12);
        g.point[3] = float(spawn);
        std::memcpy(g.prevPoint, prevPt, 12);
        g.prevPoint[3] = BitsAsFloat(run.cursor);
        g.shape[0] = float(e.shape);
        g.shape[1] = e.radius;
        g.shape[2] = float(count);
        g.shape[3] = BitsAsFloat(uint32_t(run.block) * kPerBlock);
        // Its launch direction: in the world, or (forward) relative to the head's travel - x right, y up, z ahead.
        float dir[3] = {e.direction[0], e.direction[1], e.direction[2]};
        if (e.forward) {
            float ahead[3] = {a.head[0] - b.head[0], a.head[1] - b.head[1], a.head[2] - b.head[2]};
            if (ahead[0] * ahead[0] + ahead[1] * ahead[1] + ahead[2] * ahead[2] < 1e-8f)
                for (int i = 0; i < 3; ++i) ahead[i] = a.target[i] - a.source[i];
            Normalize(ahead);
            float right[3] = {ahead[2], 0.0f, -ahead[0]};
            Normalize(right);
            const float up[3] = {ahead[1] * right[2] - ahead[2] * right[1], ahead[2] * right[0] - ahead[0] * right[2],
                                 ahead[0] * right[1] - ahead[1] * right[0]};
            for (int i = 0; i < 3; ++i)
                dir[i] = right[i] * e.direction[0] + up[i] * e.direction[1] + ahead[i] * e.direction[2];
        }
        Normalize(dir);
        std::memcpy(g.dir, dir, 12);
        g.dir[3] = std::cos(e.spread * 3.14159265f / 180.0f);
        g.lifeSpeed[0] = e.life[0];
        g.lifeSpeed[1] = e.life[1];
        g.lifeSpeed[2] = e.speed[0];
        g.lifeSpeed[3] = e.speed[1];
        g.forces[0] = e.drag;
        g.forces[1] = e.curl;
        g.forces[2] = e.curlScale;
        g.forces[3] = e.swirl;
        std::memcpy(g.gravity, e.gravity, 12);
        g.gravity[3] = e.pull;
        g.misc[0] = e.inherit;
        g.misc[1] = e.stretch;
        g.misc[2] = run.reset ? 1.0f : 0.0f;
        g.misc[3] = e.at == fx::kAtPath ? 1.0f : 0.0f;
        for (int i = 0; i < 3; ++i) {
            float vel = dt > 0.0f ? (pt[i] - prevPt[i]) / dt : 0.0f;
            g.vel[i] = std::clamp(vel, -50.0f, 50.0f);
        }
        g.vel[3] = p.noise;
        for (uint32_t k = 0; k < fx::kMaxKeys; ++k) {
            const fx::SizeKey& sk = e.size[std::min(k, e.sizeKeys - 1)];
            g.sizeT[k] = k < e.sizeKeys ? sk.t : 1.0f;
            g.sizeV[k] = sk.size;
            const fx::ColourKey& ck = e.colour[std::min(k, e.colourKeys - 1)];
            g.colourT[k] = k < e.colourKeys ? ck.t : 1.0f;
            std::memcpy(g.colour[k], ck.rgba, 16);
        }
        const uint32_t cell = std::min(e.sprite, uint32_t(fx::kSpriteCount) - 1);
        g.uv[0] = float(cell % kAtlasColumns) / float(kAtlasColumns);
        g.uv[1] = float(cell / kAtlasColumns) / float(kAtlasRows);
        g.uv[2] = 1.0f / float(kAtlasColumns);
        g.uv[3] = 1.0f / float(kAtlasRows);
        g.keys[0] = e.sizeKeys;
        g.keys[1] = e.colourKeys;
        g.keys[2] = n;
        if (e.at == fx::kAtPath) std::memcpy(g.pathStart, a.source, 12);
        run.cursor = (run.cursor + spawn) % count;
        run.reset = false;
        m_fxDrawn.push_back({c.playing, c.emitter, n});
        ++n;
    }
    for (FxPlaying& p : m_fxPlaying) {
        p.prev = p.now;
        p.havePrev = true;
        if (!p.gone) p.lastPhase = p.now.phase;
    }
    if (!n)
        return;

    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);

    Frame& f = m_frames[m_frameIndex];
    VkDescriptorBufferInfo infos[3] = {
        {f.ring, blockOffset, VkDeviceSize(n) * sizeof(GpuBlock)},
        {m_fxState, 0, VK_WHOLE_SIZE},
        {m_fxQuads[m_frameIndex], 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_fxPipeline);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_fxLayout, 0, 3, writes);
    const float push[12] = {
        dt, m_fxTime, float(m_frameNumber & 0xFFFFFF), 0.0f,
        v[0][0], v[1][0], v[2][0], 0.0f,             // camera right: the view matrix's first column
        v[0][1], v[1][1], v[2][1], 0.0f,             // camera up: its second
    };
    vkCmdPushConstants(cmd, m_fxLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(cmd, kPerBlock / kGroupSize, n, 1);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Device::DrawFx()
{
    if (m_fxDrawn.empty() || !m_scenePhase || m_target != m_scene)
        return;
    if (!m_fxAtlas)                              // (made in a frame, as textures are: not at its start)
        MakeFxAtlas();
    if (!m_fxAtlas)
        return;
    const auto rs = m_rs;
    const auto tss = m_tss;
    const auto textures = m_textures;
    const d3d::Matrix world = m_world;
    const uint32_t kind = m_drawVisualKind, visual = m_drawVisual, owner = m_drawOwner;
    m_world = Identity();                        // (the quads are in world space)
    m_drawVisualKind = uint32_t(VisualKind::Effect);
    m_drawVisual = m_drawOwner = 0;
    m_rs[d3d::RS_LIGHTING] = 0;
    m_rs[d3d::RS_ALPHABLENDENABLE] = 1;
    m_rs[d3d::RS_ALPHATESTENABLE] = 0;
    m_rs[d3d::RS_ZENABLE] = 1;
    m_rs[d3d::RS_ZWRITEENABLE] = 0;
    m_rs[d3d::RS_ZFUNC] = d3d::CMP_LESSEQUAL;
    m_rs[d3d::RS_CULLMODE] = d3d::CULL_NONE;
    m_rs[d3d::RS_FOGENABLE] = 0;
    m_rs[d3d::RS_COLORVERTEX] = 1;
    m_tss[0][d3d::TSS_COLOROP] = d3d::TOP_MODULATE;
    m_tss[0][d3d::TSS_COLORARG1] = d3d::TA_TEXTURE;
    m_tss[0][d3d::TSS_COLORARG2] = d3d::TA_DIFFUSE;
    m_tss[0][d3d::TSS_ALPHAOP] = d3d::TOP_MODULATE;
    m_tss[0][d3d::TSS_ALPHAARG1] = d3d::TA_TEXTURE;
    m_tss[0][d3d::TSS_ALPHAARG2] = d3d::TA_DIFFUSE;
    m_tss[0][d3d::TSS_TEXCOORDINDEX] = 0;
    m_tss[0][d3d::TSS_TEXTURETRANSFORMFLAGS] = 0;
    m_tss[0][d3d::TSS_ADDRESSU] = m_tss[0][d3d::TSS_ADDRESSV] = m_tss[0][d3d::TSS_ADDRESS] = d3d::TADDRESS_CLAMP;
    m_tss[0][d3d::TSS_MAGFILTER] = d3d::TFG_LINEAR;
    m_tss[0][d3d::TSS_MINFILTER] = d3d::TFN_LINEAR;
    m_tss[1][d3d::TSS_COLOROP] = d3d::TOP_DISABLE;
    m_tss[1][d3d::TSS_ALPHAOP] = d3d::TOP_DISABLE;
    m_textures = {m_fxAtlas, nullptr};
    for (const FxDrawn& d : m_fxDrawn) {
        const FxPlaying& p = m_fxPlaying[d.playing];
        const fx::Emitter& e = p.def->emitters[d.emitter];
        m_rs[d3d::RS_SRCBLEND] = d3d::BLEND_SRCALPHA;
        m_rs[d3d::RS_DESTBLEND] = e.additive ? d3d::BLEND_ONE : d3d::BLEND_INVSRCALPHA;
        const ExternalGeometry geometry{m_fxQuads[m_frameIndex], m_fxIndices, int32_t(d.region * kPerBlock * 4)};
        m_external = &geometry;
        m_drawColorScale = e.intensity;
        m_constantsDirty = true;
        const uint32_t quads = std::min(e.count, kPerBlock);
        Draw(d3d::TriangleList, kFxFvf, nullptr, quads * 4, nullptr, quads * 6);
        ++m_fxDraws;
    }
    m_external = nullptr;
    m_drawColorScale = 1.0f;
    m_rs = rs;
    m_tss = tss;
    m_textures = textures;
    m_world = world;
    m_drawVisualKind = kind;
    m_drawVisual = visual;
    m_drawOwner = owner;
    m_constantsDirty = true;
}

}  // namespace rvk
