// RVK_GiMode 1: AMD's indirect light (FidelityFX Brixelizer 1.0 and Brixelizer GI 1.0.1; the SDK 1.1.4,
// third_party/ffx-sdk, MIT; docs/amd-fx-plan.md phase 5) in place of ours (gi.frag).
//
// Brixelizer keeps a sparse distance field of the static world in cascades round the camera (each twice the last's
// voxel; 64 voxels across), built on the GPU from triangle meshes it is handed as instances - here every opaque draw
// whose vertices and indices the static arena keeps on the GPU (static_gpu.cpp: the statics' retained meshes, the
// native ground's patches), at the place it is drawn. One cascade is brought up to date a frame. Brixelizer GI then
// traces rays through the field from probes on the screen's surfaces, lighting what they hit from a cache of the
// last frame's lit picture (so light bounces off walls and ground that are off screen or hidden too), and denoises
// the result. Its diffuse light goes to the tone mapping as ours does (occlusion.glsl Indirect: x the surface colour,
// x RVK_Gi) - packed by brix_pack.frag at full resolution with the view depth.
//
// The field lives round an origin near the camera (the world's positions are thousands of units; its matrices are
// inverted in float): instances and matrices are moved by it (in double), and it follows the camera once that is
// kRebase away - the field is then made again. An instance goes when its arena slot does (ArenaFree: the slot is reused
// only after this frame's work, which no longer reads it), and a mesh drawn at a new place each frame (a door, a lift)
// is left out after a few moves (placements.h).
//
// RVK_GiDebug (ini): 1 the distance field (Brixelizer's own view: its surfaces' gradient), 2 AMD's light alone.
#include "internal.h"
#include "ffx_common.h"
#include "placements.h"

#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizergi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <unordered_map>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr uint32_t kCascades = 7;
constexpr float kVoxel = 0.25f;                  // cascade 0's voxel (world units): 16 across; cascade 6's 1,024
constexpr double kRebase = 1024.0;               // the origin follows the camera once it is this far

struct RawImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
};

struct RawBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
};

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

// A D3D matrix's memory is the array the SDK asks for (its "row major": row vectors, view x projection).
void ToFfx(const double (&m)[4][4], float out[16])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = float(m[i][j]);
}

// The camera's position from a D3D view matrix (rotation rows orthonormal).
void Eye(const double (&v)[4][4], double eye[3])
{
    for (int k = 0; k < 3; ++k)
        eye[k] = -(v[3][0] * v[k][0] + v[3][1] * v[k][1] + v[3][2] * v[k][2]);
}

}  // namespace

struct Device::BrixState {
    FfxBackend backend, giBackend;
    FfxBrixelizerContext context{};              // (~24 MB, with the baked update ~8 MB more: the state is on the heap)
    FfxBrixelizerBakedUpdateDescription baked{};
    FfxBrixelizerGIContext gi{};
    bool contextMade = false, giMade = false;
    uint32_t w = 0, h = 0;                       // the GI context's size
    double origin[3] = {};
    RawImage atlas;                              // 512^3 R8: the field's bricks
    RawImage historyDepth;                       // last frame's depth (D32, copied)
    RawBuffer brickAabbs, trees[kCascades], maps[kCascades], scratch;
    Texture* historyNormal = nullptr;            // last frame's m_sceneSurface
    Texture* historyLit = nullptr;               // last frame's lit scene
    Texture* diffuse = nullptr;                  // its outputs (storage)
    Texture* specular = nullptr;
    Texture* debug = nullptr;                    // RVK_GiDebug 1
    Texture* noise = nullptr;                    // 128 x 128, two values a texel
    bool historyValid = false;
    uint32_t frameIndex = 0;
    std::unordered_map<uint64_t, uint32_t> buffers;   // registered arena chunks: VkBuffer -> its index
    struct Instance {
        FfxBrixelizerInstanceID id = FFX_BRIXELIZER_INVALID_ID;
        uint64_t vertexSlot = 0, indexSlot = 0;  // SlotKey of the arena slots it reads
    };
    std::unordered_map<uint64_t, Instance> instances;
    std::unordered_multimap<uint64_t, uint64_t> bySlot;   // SlotKey -> instance key
    Placements placements;
    std::vector<FfxBrixelizerInstanceDescription> pending;
    std::vector<uint64_t> pendingKeys;
    uint64_t statsFrame = 0;
    FfxBrixelizerStats stats{};
    bool full = false;                           // (logged: the instance limit)
};

namespace {

bool MakeImage(VmaAllocator allocator, VkDevice device, RawImage* r, VkImageType type, VkFormat format, uint32_t w,
               uint32_t h, uint32_t d, VkImageUsageFlags usage)
{
    r->info.imageType = type;
    r->info.format = format;
    r->info.extent = {w, h, d};
    r->info.mipLevels = 1;
    r->info.arrayLayers = 1;
    r->info.samples = VK_SAMPLE_COUNT_1_BIT;
    r->info.tiling = VK_IMAGE_TILING_OPTIMAL;
    r->info.usage = usage;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    (void)device;
    return vmaCreateImage(allocator, &r->info, &ac, &r->image, &r->allocation, nullptr) == VK_SUCCESS;
}

bool MakeBuffer(VmaAllocator allocator, RawBuffer* r, VkDeviceSize size)
{
    r->info.size = size;
    r->info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;   // (its readbacks copy from them)
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    return vmaCreateBuffer(allocator, &r->info, &ac, &r->buffer, &r->allocation, nullptr) == VK_SUCCESS;
}

FfxResource BufferResource(const RawBuffer& b, const wchar_t* name)
{
    FfxResource r = ffxGetResourceVK(uint64_t(b.buffer), ffxGetBufferResourceDescriptionVK(b.buffer, b.info), name,
                                     FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    r.description.stride = sizeof(uint32_t);
    return r;
}

FfxResource ImageResource(const RawImage& i, const wchar_t* name, FfxResourceStates state)
{
    return ffxGetResourceVK(uint64_t(i.image), ffxGetImageResourceDescriptionVK(i.image, i.info), name, state);
}

}  // namespace

void Device::DestroyBrix()
{
    if (!m_brix)
        return;
    WaitIdle();                                  // (its images, buffers and pipelines go at once)
    BrixState* s = m_brix;
    if (s->giMade) ffxBrixelizerGIContextDestroy(&s->gi);
    if (s->contextMade) ffxBrixelizerContextDestroy(&s->context);
    for (RawImage* i : {&s->atlas, &s->historyDepth})
        if (i->image) vmaDestroyImage(m_allocator, i->image, i->allocation);
    for (RawBuffer* b : {&s->brickAabbs, &s->scratch})
        if (b->buffer) vmaDestroyBuffer(m_allocator, b->buffer, b->allocation);
    for (uint32_t i = 0; i < kCascades; ++i) {
        if (s->trees[i].buffer) vmaDestroyBuffer(m_allocator, s->trees[i].buffer, s->trees[i].allocation);
        if (s->maps[i].buffer) vmaDestroyBuffer(m_allocator, s->maps[i].buffer, s->maps[i].allocation);
    }
    for (Texture* t : {s->historyNormal, s->historyLit, s->diffuse, s->specular, s->debug, s->noise})
        if (t) DestroyTextureNow(t);
    delete s;
    m_brix = nullptr;
}

// The field's context and resources, round the camera. False: it couldn't be made (logged once).
static bool g_brixFailed = false;

// An opaque draw of retained static geometry (Draw, its arena buffers in m_drawStaticBuffer / ib): an instance of the
// field where it is drawn, once. Called only while BrixWanted.
void Device::BrixNoteDraw(uint32_t primitive, uint32_t fvf, const void* vertices, uint32_t stride, uint32_t vertexCount,
                          VkBuffer ib, uint32_t indexCount)
{
    if (!m_brix || !m_brix->contextMade || primitive != d3d::TriangleList || indexCount < 3 || !vertices ||
        stride < 12 || !ib)
        return;
    BrixState* s = m_brix;
    // The mesh (its slots and counts), and it where it is drawn.
    const uint64_t mesh = MeshKey(m_drawStaticBuffer, m_drawStaticOffset, ib, m_drawStaticIndexOffset, vertexCount,
                                  indexCount, stride);
    const uint64_t key = PlaceKey(mesh, m_world);
    uint64_t moved = 0;
    const bool keep = s->placements.Note(mesh, key, m_frameNumber, &moved);
    if (moved) {                                 // it moved: its old instance goes
        auto old = s->instances.find(moved);
        if (old != s->instances.end()) {
            if (old->second.id != FFX_BRIXELIZER_INVALID_ID)
                ffxBrixelizerDeleteInstances(&s->context, &old->second.id, 1);
            s->instances.erase(old);             // (its bySlot entries find nothing later)
        }
    }
    if (!keep)
        return;
    if (s->instances.count(key))
        return;
    if (s->instances.size() + s->pending.size() >= FFX_BRIXELIZER_MAX_INSTANCES - 1) {
        if (!s->full) { s->full = true; Log("Brixelizer: the instance limit (%u) reached", FFX_BRIXELIZER_MAX_INSTANCES); }
        return;
    }
    // Its buffers registered (an arena chunk once).
    uint32_t index[2];
    VkBuffer bufs[2] = {m_drawStaticBuffer, ib};
    for (int k = 0; k < 2; ++k) {
        auto it = s->buffers.find(uint64_t(bufs[k]));
        if (it != s->buffers.end()) { index[k] = it->second; continue; }
        const ArenaChunk* chunk = nullptr;
        for (auto& c : m_staticArena)
            if (c->buffer == bufs[k]) chunk = c.get();
        if (!chunk)
            return;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = chunk->size;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        FfxBrixelizerBufferDescription bd{};
        bd.buffer = ffxGetResourceVK(uint64_t(bufs[k]), ffxGetBufferResourceDescriptionVK(bufs[k], bi), L"arena",
                                     FFX_RESOURCE_STATE_COMPUTE_READ);
        bd.buffer.description.stride = sizeof(uint32_t);
        bd.outIndex = &index[k];
        if (ffxBrixelizerRegisterBuffers(&s->context, &bd, 1) != FFX_OK)
            return;
        s->buffers[uint64_t(bufs[k])] = index[k];
    }
    // Its bounds from the vertices (model space), placed: the 8 corners through the world matrix, less the origin.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        std::memcpy(p, base + size_t(i) * stride, 12);
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
    }
    if (!(lo[0] <= hi[0]))
        return;
    FfxBrixelizerInstanceDescription d{};
    const auto& m = m_world.m;
    double t[3];
    for (int k = 0; k < 3; ++k) t[k] = double(m[3][k]) - s->origin[k];
    for (int k = 0; k < 3; ++k) { d.aabb.min[k] = 1e30f; d.aabb.max[k] = -1e30f; }
    for (int c = 0; c < 8; ++c) {
        const float p[3] = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2]};
        for (int k = 0; k < 3; ++k) {
            const float v = float(p[0] * m[0][k] + p[1] * m[1][k] + p[2] * m[2][k] + t[k]);
            d.aabb.min[k] = std::min(d.aabb.min[k], v);
            d.aabb.max[k] = std::max(d.aabb.max[k], v);
        }
    }
    // Its transform (3 x 4, column vectors: the D3D matrix transposed), moved by the origin.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) d.transform[r * 4 + c] = m[c][r];
        d.transform[r * 4 + 3] = float(t[r]);
    }
    d.maxCascade = kCascades - 1;
    d.indexFormat = FFX_INDEX_TYPE_UINT16;
    d.indexBuffer = index[1];
    d.indexBufferOffset = uint32_t(m_drawStaticIndexOffset);
    d.triangleCount = indexCount / 3;
    d.vertexBuffer = index[0];
    d.vertexStride = stride;
    d.vertexBufferOffset = uint32_t(m_drawStaticOffset);
    d.vertexCount = vertexCount;
    d.vertexFormat = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;
    d.flags = FFX_BRIXELIZER_INSTANCE_FLAG_NONE;
    BrixState::Instance inst;
    inst.vertexSlot = SlotKey(m_drawStaticBuffer, m_drawStaticSlot);
    inst.indexSlot = SlotKey(ib, m_drawStaticIndexOffset);
    s->instances[key] = inst;
    s->bySlot.emplace(inst.vertexSlot, key);
    s->bySlot.emplace(inst.indexSlot, key);
    s->pending.push_back(d);
    s->pendingKeys.push_back(key);
}

// An arena slot freed (ArenaFree): the instances reading it go.
void Device::BrixForgetSlot(VkBuffer buffer, VkDeviceSize offset)
{
    BrixState* s = m_brix;
    auto range = s->bySlot.equal_range(SlotKey(buffer, offset));
    for (auto it = range.first; it != range.second; ++it) {
        auto inst = s->instances.find(it->second);
        if (inst == s->instances.end())
            continue;
        if (inst->second.id != FFX_BRIXELIZER_INVALID_ID) {
            ffxBrixelizerDeleteInstances(&s->context, &inst->second.id, 1);
        } else {                                 // not made yet: out of the pending list
            for (size_t i = 0; i < s->pendingKeys.size(); ++i)
                if (s->pendingKeys[i] == it->second) {
                    s->pendingKeys.erase(s->pendingKeys.begin() + ptrdiff_t(i));
                    s->pending.erase(s->pending.begin() + ptrdiff_t(i));
                    break;
                }
        }
        s->instances.erase(inst);
    }
    s->bySlot.erase(range.first, range.second);
}

// AMD's indirect light into m_brixGi, as RenderGi leaves ours in m_giTex[1]. False: it couldn't run (ours then).
bool Device::RenderBrixGi(VkCommandBuffer cmd)
{
    const bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (!BrixWanted() || !perspective || !m_depth || !m_brixPackPipeline || g_brixFailed)
        return false;
    double v[4][4], p[4][4], eye[3];
    ToDouble(m_aoView, v);
    ToDouble(m_aoProj, p);
    Eye(v, eye);
    // The origin: kept while the camera is near it, else the field made again round it.
    if (m_brix) {
        const double dx = eye[0] - m_brix->origin[0], dy = eye[1] - m_brix->origin[1], dz = eye[2] - m_brix->origin[2];
        if (dx * dx + dy * dy + dz * dz > kRebase * kRebase) {
            Log("Brixelizer: the camera moved %.0f units: the field made again round it", std::sqrt(dx * dx + dy * dy + dz * dz));
            DestroyBrix();
        }
    }
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    if (!m_brix) {
        BrixState* s = new (std::nothrow) BrixState;
        if (!s) {
            g_brixFailed = true;
            Log("Brixelizer: no memory for its state (%u MB): our indirect light", unsigned(sizeof(BrixState) >> 20));
            return false;
        }
        m_brix = s;
        for (int k = 0; k < 3; ++k) s->origin[k] = std::floor(eye[k] / 64.0) * 64.0;
        FfxErrorCode e = s->backend.Init(m_device, m_physical, FFX_BRIXELIZER_CONTEXT_COUNT);
        FfxBrixelizerContextDescription desc{};
        desc.numCascades = kCascades;
        desc.flags = FfxBrixelizerContextFlags(FFX_BRIXELIZER_CONTEXT_FLAG_DEBUG_CONTEXT_READBACK_BUFFERS |
                                               FFX_BRIXELIZER_CONTEXT_FLAG_DEBUG_CASCADE_READBACK_BUFFERS);
        float voxel = kVoxel;
        for (uint32_t i = 0; i < kCascades; ++i, voxel *= 2.0f) {
            desc.cascadeDescs[i].flags = FFX_BRIXELIZER_CASCADE_STATIC;
            desc.cascadeDescs[i].voxelSize = voxel;
        }
        desc.backendInterface = s->backend.iface;
        if (e == FFX_OK) e = ffxBrixelizerContextCreate(&desc, &s->context);
        s->contextMade = e == FFX_OK;
        bool ok = s->contextMade;
        constexpr uint32_t kAtlas = FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE;
        ok = ok && MakeImage(m_allocator, m_device, &s->atlas, VK_IMAGE_TYPE_3D, VK_FORMAT_R8_UNORM, kAtlas, kAtlas, kAtlas,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        ok = ok && MakeBuffer(m_allocator, &s->brickAabbs, FFX_BRIXELIZER_BRICK_AABBS_SIZE);
        for (uint32_t i = 0; ok && i < kCascades; ++i)
            ok = MakeBuffer(m_allocator, &s->trees[i], FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE) &&
                 MakeBuffer(m_allocator, &s->maps[i], FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE);
        if (ok) {
            // The noise its rays start from: two values a texel (R2 sequences, Roberts 2018 - evenly spread).
            std::vector<uint32_t> px(128 * 128);
            for (uint32_t y = 0; y < 128; ++y)
                for (uint32_t x = 0; x < 128; ++x) {
                    const double a = std::fmod(0.5 + x * 0.7548776662466927 + y * 0.5698402909980532, 1.0);
                    const double b = std::fmod(0.25 + x * 0.5698402909980532 + y * 0.7548776662466927 + 0.31 * ((x ^ y) & 7), 1.0);
                    px[y * 128 + x] = 0xFF000000u | uint32_t(a * 255.0) << 16 | uint32_t(b * 255.0) << 8;
                }
            s->noise = CreateTexture(128, 128, px.data());
            ok = s->noise != nullptr;
        }
        if (!ok) {
            Log("Brixelizer: could not be set up (error 0x%x): our indirect light", unsigned(e));
            DestroyBrix();
            g_brixFailed = true;
            return false;
        }
        Log("Brixelizer: %u cascades of voxels %.2f..%.0f units round (%.0f, %.0f, %.0f); state %u MB", kCascades,
            kVoxel, kVoxel * float(1u << (kCascades - 1)), s->origin[0], s->origin[1], s->origin[2],
            unsigned(sizeof(BrixState) >> 20));
        return false;                            // (instances come with the next frame's draws)
    }
    BrixState* s = m_brix;
    if (s->giMade && (s->w != w || s->h != h)) {
        WaitIdle();
        ffxBrixelizerGIContextDestroy(&s->gi);
        s->giMade = false;
        for (Texture** t : {&s->historyNormal, &s->historyLit, &s->diffuse, &s->specular, &s->debug})
            if (*t) { DestroyTextureNow(*t); *t = nullptr; }
        if (s->historyDepth.image) { vmaDestroyImage(m_allocator, s->historyDepth.image, s->historyDepth.allocation); s->historyDepth = {}; }
        s->historyValid = false;
    }
    if (!s->giMade) {
        FfxErrorCode e = s->giBackend.iface.fpGetDeviceCapabilities ? FFX_OK
                         : s->giBackend.Init(m_device, m_physical, FFX_BRIXELIZER_GI_CONTEXT_COUNT);
        FfxBrixelizerGIContextDescription desc{};
        desc.flags = FFX_BRIXELIZER_GI_FLAG_DISABLE_SPECULAR;   // (diffuse only: ours reflects)
        desc.internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT;
        desc.displaySize = {w, h};
        desc.backendInterface = s->giBackend.iface;
        if (e == FFX_OK) e = ffxBrixelizerGIContextCreate(&s->gi, &desc);
        s->giMade = e == FFX_OK;
        bool ok = s->giMade;
        ok = ok && MakeImage(m_allocator, m_device, &s->historyDepth, VK_IMAGE_TYPE_2D, kDepthFormat, w, h, 1,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        for (Texture** t : {&s->historyNormal, &s->historyLit})
            ok = ok && (*t = CreateImage(w, h, Format::RGBA16F, 1, true)) != nullptr;
        for (Texture** t : {&s->diffuse, &s->specular, &s->debug})
            ok = ok && (*t = CreateImage(w, h, Format::RGBA16F, 1, true, true)) != nullptr;
        if (!ok) {
            Log("Brixelizer GI: could not be set up (error 0x%x): our indirect light", unsigned(e));
            DestroyBrix();
            g_brixFailed = true;
            return false;
        }
        s->w = w;
        s->h = h;
        s->historyValid = false;
        Log("Brixelizer GI: at %ux%u (traced at half)", w, h);
    }
    if (!m_brixGi || m_brixGi->m_width != w || m_brixGi->m_height != h) {
        if (m_brixGi) DestroyTexture(m_brixGi);
        m_brixGi = CreateImage(w, h, Format::RGBA16F, 1, true);
        if (!m_brixGi)
            return false;
    }
    if (!RenderSceneSurface(cmd) || !RenderSceneMotion(cmd) || !RenderSceneEnv(cmd))
        return false;

    // The instances noted since the last frame made.
    if (!s->pending.empty()) {
        std::vector<FfxBrixelizerInstanceID> ids(s->pending.size(), FFX_BRIXELIZER_INVALID_ID);
        for (size_t i = 0; i < s->pending.size(); ++i) s->pending[i].outInstanceID = &ids[i];
        const FfxErrorCode e = ffxBrixelizerCreateInstances(&s->context, s->pending.data(), uint32_t(s->pending.size()));
        for (size_t i = 0; i < s->pending.size(); ++i) {
            auto it = s->instances.find(s->pendingKeys[i]);
            if (it == s->instances.end()) continue;
            if (e == FFX_OK && ids[i] != FFX_BRIXELIZER_INVALID_ID) it->second.id = ids[i];
            else s->instances.erase(it);
        }
        s->pending.clear();
        s->pendingKeys.clear();
    }

    // Everything the scene's rendering and the passes before wrote, for its compute passes.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    if (s->atlas.layout != VK_IMAGE_LAYOUT_GENERAL) {
        ImageBarrier(cmd, s->atlas.image, VK_IMAGE_ASPECT_COLOR_BIT, s->atlas.layout, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        s->atlas.layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // The matrices round the origin (in double).
    double shift[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {s->origin[0], s->origin[1], s->origin[2], 1}};
    double view[4][4], prevView[4][4], prevProj[4][4];
    Mul(shift, v, view);
    if (m_prevViewProjValid) {
        double pv[4][4];
        ToDouble(m_prevView, pv);
        Mul(shift, pv, prevView);
        ToDouble(m_prevProj, prevProj);
    } else {
        std::memcpy(prevView, view, sizeof(view));
        std::memcpy(prevProj, p, sizeof(p));
    }

    // The field brought up to date (one cascade).
    const bool debugField = m_giDebug > 0.5f && m_giDebug < 1.5f;
    FfxBrixelizerUpdateDescription u{};
    u.resources.sdfAtlas = ImageResource(s->atlas, L"sdf atlas", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    u.resources.brickAABBs = BufferResource(s->brickAabbs, L"brick aabbs");
    // (Every slot of its arrays bound - the shaders' arrays are 24 long: the ones past our cascades the last's again.)
    for (uint32_t i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; ++i) {
        const uint32_t c = std::min(i, kCascades - 1);
        u.resources.cascadeResources[i].aabbTree = BufferResource(s->trees[c], L"aabb tree");
        u.resources.cascadeResources[i].brickMap = BufferResource(s->maps[c], L"brick map");
    }
    u.frameIndex = s->frameIndex;
    for (int k = 0; k < 3; ++k) u.sdfCenter[k] = float(eye[k] - s->origin[k]);
    u.populateDebugAABBsFlags = FFX_BRIXELIZER_POPULATE_AABBS_NONE;
    u.maxReferences = 8u << 20;
    u.triangleSwapSize = 64u << 20;
    u.maxBricksPerBake = 1u << 13;
    FfxBrixelizerDebugVisualizationDescription dv{};
    if (debugField) {
        double invView[4][4] = {}, invProj[4][4];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) invView[i][j] = view[j][i];
        for (int j = 0; j < 3; ++j)
            invView[3][j] = -(view[3][0] * view[j][0] + view[3][1] * view[j][1] + view[3][2] * view[j][2]);
        invView[3][3] = 1.0;
        d3d::Matrix ip;
        if (InvertMatrix(m_aoProj, &ip)) {
            ToDouble(ip, invProj);
            ToFfx(invView, dv.inverseViewMatrix);
            ToFfx(invProj, dv.inverseProjectionMatrix);
            dv.debugState = FFX_BRIXELIZER_TRACE_DEBUG_MODE_GRAD;
            dv.startCascadeIndex = 0;
            dv.endCascadeIndex = kCascades - 1;
            dv.sdfSolveEps = 0.5f;
            dv.tMin = 0.0f;
            dv.tMax = 10000.0f;
            dv.renderWidth = w;
            dv.renderHeight = h;
            Transition(cmd, s->debug, VK_IMAGE_LAYOUT_GENERAL);
            dv.output = FfxImage(s->debug->m_image, GetFormatInfo(Format::RGBA16F).vk, w, h, true, L"debug");
            dv.commandList = ffxGetCommandListVK(cmd);
            u.debugVisualizationDesc = &dv;
        }
    }
    size_t scratchSize = 0;
    u.outScratchBufferSize = &scratchSize;
    const bool stats = m_frameNumber >= s->statsFrame + 600;
    if (stats) u.outStats = &s->stats;
    FfxErrorCode e = ffxBrixelizerBakeUpdate(&s->context, &u, &s->baked);
    if (e == FFX_OK && scratchSize > s->scratch.info.size) {
        if (s->scratch.buffer) {
            WaitIdle();                          // (rare: its first frames, a busier place)
            vmaDestroyBuffer(m_allocator, s->scratch.buffer, s->scratch.allocation);
            s->scratch = {};
        }
        const VkDeviceSize size = VkDeviceSize(scratchSize) * 3 / 2;
        if (!MakeBuffer(m_allocator, &s->scratch, size))
            e = FFX_ERROR_OUT_OF_MEMORY;
        else
            Log("Brixelizer: %.0f MB of scratch", double(size) / 1048576.0);
    }
    if (e == FFX_OK)
        e = ffxBrixelizerUpdate(&s->context, &s->baked, BufferResource(s->scratch, L"scratch"), ffxGetCommandListVK(cmd));
    if (e != FFX_OK) {
        static bool logged;
        if (!logged) { logged = true; Log("Brixelizer: update failed (error 0x%x): our indirect light", unsigned(e)); }
        return false;
    }
    if (debugField) s->debug->m_layout = VK_IMAGE_LAYOUT_GENERAL;
    if (stats) {
        s->statsFrame = m_frameNumber;
        const FfxBrixelizerStats& st = s->stats;
        Log("Brixelizer: %zu instances, %zu buffers; cascade %u: %u triangles, %u references, %u bricks; %u free bricks",
            s->instances.size(), s->buffers.size(), st.cascadeIndex, st.staticCascadeStats.trianglesAllocated,
            st.staticCascadeStats.referencesAllocated, st.staticCascadeStats.bricksAllocated, st.contextStats.freeBricks);
    }
    ++s->frameIndex;

    // History (the first frame: this one's).
    auto copyHistory = [&]() {
        Transition(cmd, m_scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(cmd, m_sceneSurface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(cmd, s->historyLit, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        Transition(cmd, s->historyNormal, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        ImageBarrier(cmd, s->historyDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, s->historyDepth.layout,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy c{};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.dstSubresource = c.srcSubresource;
        c.extent = {w, h, 1};
        vkCmdCopyImage(cmd, m_scene->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->historyLit->m_image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        vkCmdCopyImage(cmd, m_sceneSurface->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->historyNormal->m_image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyImage(cmd, m_depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->historyDepth.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        m_depthLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ImageBarrier(cmd, s->historyDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        s->historyDepth.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for (Texture* t : {m_scene, m_sceneSurface, s->historyLit, s->historyNormal})
            Transition(cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // (Transition's read stage is the fragment shader's; its passes are compute.)
        VkMemoryBarrier2 cb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        cb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        cb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        cb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        cb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkDependencyInfo cd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        cd.memoryBarrierCount = 1;
        cd.pMemoryBarriers = &cb;
        vkCmdPipelineBarrier2(cmd, &cd);
    };
    if (!s->historyValid) {
        copyHistory();
        s->historyValid = true;
    }

    // The light.
    FfxBrixelizerRawContext* raw = nullptr;
    ffxBrixelizerGetRawContext(&s->context, &raw);
    FfxBrixelizerGIDispatchDescription d{};
    ToFfx(view, d.view);
    ToFfx(p, d.projection);
    ToFfx(prevView, d.prevView);
    ToFfx(prevProj, d.prevProjection);
    for (int k = 0; k < 3; ++k) d.cameraPosition[k] = float(eye[k] - s->origin[k]);
    d.startCascade = 0;
    d.endCascade = kCascades - 1;
    d.rayPushoff = 0.25f;
    d.sdfSolveEps = 0.5f;
    d.specularRayPushoff = 0.25f;
    d.specularSDFSolveEps = 0.5f;
    d.tMin = 0.0f;
    d.tMax = 10000.0f;
    Texture* env = m_sceneEnv;
    const VkFormat hdr = GetFormatInfo(Format::RGBA16F).vk;
    d.environmentMap = FfxImage(env->m_image, GetFormatInfo(env->m_format).vk, env->m_width, env->m_height, false, L"environment");
    d.prevLitOutput = FfxImage(s->historyLit->m_image, hdr, w, h, false, L"prev lit");
    d.depth = FfxImage(m_depth, kDepthFormat, w, h, false, L"depth");
    d.historyDepth = ImageResource(s->historyDepth, L"history depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    d.normal = FfxImage(m_sceneSurface->m_image, hdr, w, h, false, L"normal");
    d.historyNormal = FfxImage(s->historyNormal->m_image, hdr, w, h, false, L"history normal");
    d.roughness = d.normal;
    d.motionVectors = FfxImage(m_sceneMotion->m_image, GetFormatInfo(Format::RG16F).vk, w, h, false, L"motion");
    d.noiseTexture = FfxImage(s->noise->m_image, GetFormatInfo(s->noise->m_format).vk, 128, 128, false, L"noise");
    Transition(cmd, s->noise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    d.normalsUnpackMul = 1.0f;
    d.normalsUnpackAdd = 0.0f;
    d.isRoughnessPerceptual = true;
    d.roughnessChannel = 3;
    d.roughnessThreshold = 0.9f;
    d.environmentMapIntensity = m_giSky;
    d.motionVectorScale = {-1.0f / float(w), -1.0f / float(h)};   // ours: pixels, now - then; its: uv, then - now
    d.sdfAtlas = ImageResource(s->atlas, L"sdf atlas", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.bricksAABBs = BufferResource(s->brickAabbs, L"brick aabbs");
    for (uint32_t i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; ++i) {
        const uint32_t c = std::min(i, kCascades - 1);
        d.cascadeAABBTrees[i] = BufferResource(s->trees[c], L"aabb tree");
        d.cascadeBrickMaps[i] = BufferResource(s->maps[c], L"brick map");
    }
    Transition(cmd, s->diffuse, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, s->specular, VK_IMAGE_LAYOUT_GENERAL);
    d.outputDiffuseGI = FfxImage(s->diffuse->m_image, hdr, w, h, true, L"diffuse");
    d.outputSpecularGI = FfxImage(s->specular->m_image, hdr, w, h, true, L"specular");
    d.brixelizerContext = raw;
    e = ffxBrixelizerGIContextDispatch(&s->gi, &d, ffxGetCommandListVK(cmd));
    if (e != FFX_OK) {
        static bool logged;
        if (!logged) { logged = true; Log("Brixelizer GI: dispatch failed (error 0x%x): our indirect light", unsigned(e)); }
        return false;
    }
    Transition(cmd, s->diffuse, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // For the tone mapping: its light and the view depth (brix_pack.frag), as ours leaves them.
    MakeDepthReadable(cmd);
    float params[4] = {m_aoProj.m[2][2], m_aoProj.m[3][2], 0.0f, 0.0f};
    FullscreenPass(cmd, m_brixGi, m_brixPackPipeline, s->diffuse->m_view, m_depthView, m_pointSampler, params,
                   sizeof(params), false);
    Transition(cmd, m_brixGi, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // This frame's lit scene, depth and normals for the next (before a debug view goes over the scene).
    copyHistory();

    // RVK_GiDebug: the field (1) or its light alone (2) over the scene.
    Texture* show = debugField ? s->debug : m_giDebug > 1.5f ? s->diffuse : nullptr;
    if (show) {
        Transition(cmd, show, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(cmd, m_scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.dstSubresource = c.srcSubresource;
        c.extent = {w, h, 1};
        vkCmdCopyImage(cmd, show->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_scene->m_image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        Transition(cmd, m_scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmd, show, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return true;
}

}  // namespace rvk
