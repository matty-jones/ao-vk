// Ray queries (docs/rt-plan.md): acceleration structures of the static world, for rays from any shader - where the GPU
// has them (VK_KHR_acceleration_structure, VK_KHR_ray_query: device.cpp m_rayQuery; under Wine they work from this
// 32-bit DLL as from a 64-bit one).
//
// The meshes are the draws Brixelizer takes (draw.cpp: opaque draws whose vertices and indices the static arena keeps -
// the statics' retained meshes, the native ground's patches). Each mesh gets a bottom-level structure (BLAS), built
// once from its arena slots - a budget of triangles a frame, so a zone's arrival doesn't stall - and freed with them
// (ArenaFree; destroyed once no submission in flight can read it). Each place a mesh is drawn is an instance
// (placements.h), kept while its mesh lives. Each frame the instances near the camera go into a top-level structure
// (TLAS), placed relative to the camera (in double): rays start at the eye's origin, exact at the world's thousands of
// units.
//
// Characters (skinned on the GPU, skin_gpu.cpp): each solid piece a BLAS of its own, refitted every frame it is drawn
// from that frame's skinned vertices (built again when its counts change), an instance where it is drawn - in that
// frame's TLAS only.
//
// The playfield's placed objects (statels.cpp: every static object it places, from its own list) are instances too,
// once their placement is confirmed - their solid pieces, each piece's BLAS built when one of its places first comes
// within range: the static world whole, not only what the game draws (and has drawn since it came near).
//
// RVK_RtDebug 1: a ray through each pixel from the camera, what it hits coloured by instance - over the scene.
// RVK_RtShadow 1: the sun's shadows traced from the depth pre-pass's depth (RenderRtShadow).
#include "internal.h"
#include "placements.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kRtDebugSpirv[] = {
#include "rt_debug.comp.inc"
};
const uint32_t kRtAoSpirv[] = {
#include "rt_ao.comp.inc"
};
const uint32_t kRtShadowSpirv[] = {
#include "rt_shadow.comp.inc"
};
const uint32_t kRtShadowBlurSpirv[] = {
#include "rt_shadow_blur.comp.inc"
};

constexpr uint32_t kBuildBudget = 400000;        // triangles of new BLAS a frame
constexpr double kRange = 1500.0;                // instances this near the camera go into the TLAS (world units)
// Instance masks: what casts the sun's shadows, and the ground (which casts none: its lightmap has the hills' shade, as
// for the shadow map - shadow.cpp IsTerrain).
constexpr uint8_t kMaskCaster = 0x01, kMaskGround = 0x02;
constexpr uint64_t kStatelMeshSeed = 0x5354454C4D455348ull, kStatelPlaceSeed = 0x5354454C504C4143ull,
                   kSkinnedSeed = 0x534B494E4E454400ull;

struct RtBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
    void* mapped = nullptr;
};

bool MakeBuffer(VmaAllocator allocator, VkDevice device, RtBuffer* b, VkDeviceSize size, VkBufferUsageFlags usage,
                bool host)
{
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (host) ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator, &bi, &ac, &b->buffer, &b->allocation, &info) != VK_SUCCESS)
        return false;
    b->size = size;
    b->mapped = info.pMappedData;
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = b->buffer;
    b->address = vkGetBufferDeviceAddress(device, &ai);
    return true;
}

VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

// A TLAS instance's geometry for a ray's hit on it (rt_common.glsl RtGeo, std430): its vertices' and indices' (16-bit)
// addresses, its vertices' stride, a cut-out's texture coordinates' place, texture (bindless image + 1 | sampler << 20;
// 0: solid) and alpha reference.
struct GeoRecord {
    uint32_t vertices[2], indices[2];
    uint32_t stride, uvOffset, texture;
    float alphaRef;
};
static_assert(sizeof(GeoRecord) == 32, "rt_common.glsl RtGeo");

// One global barrier.
void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
             VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage;
    mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

constexpr VkPipelineStageFlags2 kBuild = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
constexpr VkAccessFlags2 kAsRW = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                 VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
constexpr VkPipelineStageFlags2 kRayStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

}  // namespace

struct Device::RtState {
    struct Mesh {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        RtBuffer storage;
        VkDeviceAddress vertices = 0, indices = 0, address = 0;
        uint32_t stride = 0, vertexCount = 0, triangles = 0;
        bool built = false;
        bool queued = true;                      // in toBuild (a placed object's piece: once a place of it is in range)
        std::vector<uint64_t> places;            // its instances (the arena's meshes)
    };
    std::unordered_map<uint64_t, Mesh> meshes;
    struct Instance {
        uint64_t mesh;
        double world[3][4];                      // rows: x' = world[0] . (x, y, z, 1) ...
        double centre[3], radius;
        uint32_t id;                             // (the debug view's colour)
        uint8_t mask;                            // kMaskCaster / kMaskGround
        Texture* texture;                        // a cut-out's: its alpha tested at the rays' hits (null: solid)
        float alphaRef;                          // ... kept where over this (0..1)
        uint32_t uvOffset;                       // ... its texture coordinates' place in a vertex
    };
    std::unordered_map<uint64_t, Instance> instances;
    std::unordered_multimap<uint64_t, uint64_t> bySlot;   // SlotKey -> mesh
    std::unordered_multimap<Texture*, uint64_t> byTexture;   // a cut-out's texture -> its instances
    std::unordered_map<uint64_t, VkDeviceAddress> chunkAddress;
    Placements placements;
    std::vector<uint64_t> toBuild;
    RtBuffer scratch;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    RtBuffer tlasStorage;
    uint32_t tlasCapacity = 0, tlasCount = 0;
    RtBuffer instanceBuf[kFramesInFlight];
    // Per TLAS instance (by its index), what a ray's hit on it needs to test a cut-out's alpha (rt_common.glsl RtGeo).
    RtBuffer geoBuf[kFramesInFlight];
    struct Dead {
        uint64_t tag;
        VkAccelerationStructureKHR as;
        RtBuffer buffer;
    };
    std::vector<Dead> dead;
    // Characters' pieces (RtNoteSkinned): their BLAS (by the piece's key), this frame's draws of them, and the keys in
    // this frame's TLAS (RtTraced).
    struct Skinned {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        RtBuffer storage;
        uint32_t vertexCount = 0, triangles = 0;
        VkDeviceSize buildScratch = 0, updateScratch = 0;
        uint64_t lastFrame = 0;
        bool built = false;
    };
    std::unordered_map<uint64_t, Skinned> skinned;
    struct SkinnedDraw {
        const void* job;                         // (drawn again this frame - another pass: the same instance)
        uint64_t key;
        VkDeviceAddress vertices, indices;
        uint32_t vertexCount, triangles;
        double world[3][4];
        double centre[3];                        // its box's middle, placed (the bones may place it, not the world)
    };
    std::vector<SkinnedDraw> skinnedDraws;
    uint64_t skinnedFrame = ~0ull;
    std::unordered_set<uint64_t> tracedSkinned;
    // The placed objects' (statels.cpp): their pack's load, which kinds were confirmed; the meshes and places made.
    uint32_t statelGen = ~0u;
    bool statelCasting[2] = {};
    std::vector<uint64_t> statelMeshes, statelPlaces;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE, aoSetLayout = VK_NULL_HANDLE, shadowSetLayout = VK_NULL_HANDLE,
                          blurSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE, aoLayout = VK_NULL_HANDLE, shadowLayout = VK_NULL_HANDLE,
                     blurLayout = VK_NULL_HANDLE;
    VkPipeline debugPipeline = VK_NULL_HANDLE, aoPipeline = VK_NULL_HANDLE, shadowPipeline = VK_NULL_HANDLE,
               blurPipeline = VK_NULL_HANDLE;
    // RVK_RtShadow: its history (ping-pong: visibility, view depth, frames, hit distance), the trace's output, the blur's
    // middle step and the mask the scene reads (FL.rtShadow); the trace's parameters (a uniform buffer per frame slot).
    Texture* shadowHistory[2] = {};
    Texture* shadowRaw = nullptr;
    Texture* shadowBlur = nullptr;
    Texture* shadowMask = nullptr;
    uint32_t shadowCurrent = 0;
    uint64_t shadowFrame = ~0ull;                // the frame it last ran
    RtBuffer shadowParams[kFramesInFlight];
    Texture* debug = nullptr;
    // RVK_AoMode 2: its history (ping-pong: occlusion, view depth, frames), its output for the blur, the blur's
    // middle step and its result (m_aoFullTex).
    Texture* aoHistory[2] = {};
    Texture* aoOut = nullptr;
    Texture* aoBlur = nullptr;
    Texture* ao = nullptr;
    uint32_t aoCurrent = 0;
    uint64_t aoFrame = ~0ull;                    // the frame it last ran (its history good for the next)
    uint64_t tlasFrame = ~0ull;                  // the frame the TLAS was last built for
    double eye[3] = {};
    VkDeviceSize meshBytes = 0;
    uint64_t meshTriangles = 0, logFrame = 0;
    uint32_t nextId = 1, builtMeshes = 0;
    bool failed = false;
};

void Device::RtCollect()
{
    auto& dead = m_rt->dead;
    auto done = std::remove_if(dead.begin(), dead.end(), [&](const RtState::Dead& d) {
        if (d.tag > m_completed) return false;
        if (d.as) vkDestroyAccelerationStructureKHR(m_device, d.as, nullptr);
        if (d.buffer.buffer) vmaDestroyBuffer(m_allocator, d.buffer.buffer, d.buffer.allocation);
        return true;
    });
    dead.erase(done, dead.end());
}

void Device::DestroyRt()
{
    if (!m_rt)
        return;
    WaitIdle();                                  // (everything goes at once; nothing recorded since reads the dead)
    RtState* s = m_rt;
    auto kill = [&](VkAccelerationStructureKHR as, RtBuffer& b) {
        if (as) vkDestroyAccelerationStructureKHR(m_device, as, nullptr);
        if (b.buffer) vmaDestroyBuffer(m_allocator, b.buffer, b.allocation);
        b = {};
    };
    for (RtState::Dead& d : s->dead) kill(d.as, d.buffer);
    for (auto& [key, m] : s->meshes) kill(m.as, m.storage);
    for (auto& [key, k] : s->skinned) kill(k.as, k.storage);
    kill(s->tlas, s->tlasStorage);
    kill(VK_NULL_HANDLE, s->scratch);
    for (RtBuffer& b : s->instanceBuf) kill(VK_NULL_HANDLE, b);
    for (RtBuffer& b : s->geoBuf) kill(VK_NULL_HANDLE, b);
    for (RtBuffer& b : s->shadowParams) kill(VK_NULL_HANDLE, b);
    for (VkPipeline p : {s->debugPipeline, s->aoPipeline, s->shadowPipeline, s->blurPipeline})
        if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (VkPipelineLayout l : {s->layout, s->aoLayout, s->shadowLayout, s->blurLayout})
        if (l) vkDestroyPipelineLayout(m_device, l, nullptr);
    for (VkDescriptorSetLayout l : {s->setLayout, s->aoSetLayout, s->shadowSetLayout, s->blurSetLayout})
        if (l) vkDestroyDescriptorSetLayout(m_device, l, nullptr);
    for (Texture* t : {s->debug, s->aoHistory[0], s->aoHistory[1], s->aoOut, s->aoBlur, s->ao, s->shadowHistory[0],
                       s->shadowHistory[1], s->shadowRaw, s->shadowBlur, s->shadowMask})
        if (t) DestroyTextureNow(t);
    if (m_aoFullTex == s->ao) m_aoFullTex = nullptr;
    delete s;
    m_rt = nullptr;
}

// An opaque draw of retained static geometry (draw.cpp, its arena buffers in m_drawStaticBuffer / ib): a mesh, and an
// instance of it where it is drawn. Its instance's key (its shadow caster's: RtTraced), 0 when its place isn't kept.
uint64_t Device::RtNoteDraw(uint32_t primitive, const void* vertices, uint32_t stride, uint32_t vertexCount, VkBuffer ib,
                            uint32_t indexCount, bool ground, Texture* cutout, float alphaRef, uint32_t uvOffset)
{
    RtState* s = m_rt;
    if (s->failed || primitive != d3d::TriangleList || indexCount < 3 || !vertices || stride < 12 || !ib)
        return 0;
    const uint64_t mesh = MeshKey(m_drawStaticBuffer, m_drawStaticOffset, ib, m_drawStaticIndexOffset, vertexCount,
                                  indexCount, stride);
    const uint64_t place = PlaceKey(mesh, m_world);
    uint64_t moved = 0;
    const bool keep = s->placements.Note(mesh, place, m_frameNumber, &moved);
    if (moved)
        s->instances.erase(moved);               // (its mesh's list finds nothing for it later)
    if (!keep)
        return 0;
    if (s->instances.count(place))
        return place;
    auto it = s->meshes.find(mesh);
    if (it == s->meshes.end()) {
        auto address = [&](VkBuffer b) {
            auto a = s->chunkAddress.find(uint64_t(b));
            if (a != s->chunkAddress.end()) return a->second;
            VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            ai.buffer = b;
            const VkDeviceAddress v = vkGetBufferDeviceAddress(m_device, &ai);
            s->chunkAddress[uint64_t(b)] = v;
            return v;
        };
        RtState::Mesh m;
        m.vertices = address(m_drawStaticBuffer) + m_drawStaticOffset;
        m.indices = address(ib) + m_drawStaticIndexOffset;
        m.stride = stride;
        m.vertexCount = vertexCount;
        m.triangles = indexCount / 3;
        it = s->meshes.emplace(mesh, std::move(m)).first;
        s->bySlot.emplace(SlotKey(m_drawStaticBuffer, m_drawStaticSlot), mesh);
        s->bySlot.emplace(SlotKey(ib, m_drawStaticIndexOffset), mesh);
        s->toBuild.push_back(mesh);
    }
    // Its bounds (model space, from the vertices), placed: the 8 corners through the world matrix.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        std::memcpy(p, base + size_t(i) * stride, 12);
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
    }
    if (!(lo[0] <= hi[0]))
        return 0;
    RtState::Instance inst{};
    inst.mesh = mesh;
    const auto& w = m_world.m;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) inst.world[r][c] = w[c][r];
    double wlo[3] = {1e300, 1e300, 1e300}, whi[3] = {-1e300, -1e300, -1e300};
    for (int c = 0; c < 8; ++c) {
        const double p[3] = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2]};
        for (int k = 0; k < 3; ++k) {
            const double v = inst.world[k][0] * p[0] + inst.world[k][1] * p[1] + inst.world[k][2] * p[2] + inst.world[k][3];
            wlo[k] = std::min(wlo[k], v);
            whi[k] = std::max(whi[k], v);
        }
    }
    double r2 = 0.0;
    for (int k = 0; k < 3; ++k) {
        inst.centre[k] = 0.5 * (wlo[k] + whi[k]);
        r2 += 0.25 * (whi[k] - wlo[k]) * (whi[k] - wlo[k]);
    }
    inst.radius = std::sqrt(r2);
    inst.id = s->nextId++ & 0xFFFFFFu;
    inst.mask = ground ? kMaskGround : kMaskCaster;
    inst.texture = cutout;
    inst.alphaRef = alphaRef;
    inst.uvOffset = uvOffset;
    s->instances[place] = inst;
    it->second.places.push_back(place);
    if (cutout)
        s->byTexture.emplace(cutout, place);
    return place;
}

// A texture going (DestroyTexture): the cut-outs tested against it go too.
void Device::RtForgetTexture(Texture* texture)
{
    RtState* s = m_rt;
    auto range = s->byTexture.equal_range(texture);
    for (auto it = range.first; it != range.second; ++it) {
        auto i = s->instances.find(it->second);
        if (i != s->instances.end() && i->second.texture == texture)
            s->instances.erase(i);
    }
    s->byTexture.erase(range.first, range.second);
}

// A character's solid piece skinned on the GPU, drawn: its vertices in this frame's skin arena (32 bytes each, the
// position first), its indices in its mesh's buffer. Taken into this frame's TLAS (RtUpdate) - if it comes before it
// is built. Its key (its shadow caster's: RtTraced).
uint64_t Device::RtNoteSkinned(const void* job, VkBuffer arena, VkDeviceSize vertexOffset, uint32_t vertexCount,
                               VkBuffer meshBuffer, VkDeviceSize indexOffset, uint32_t indexCount)
{
    RtState* s = m_rt;
    if (s->failed || !arena || !meshBuffer || indexCount < 3 || !vertexCount || s->tlasFrame == m_frameNumber)
        return 0;
    if (s->skinnedFrame != m_frameNumber) {
        s->skinnedDraws.clear();
        s->skinnedFrame = m_frameNumber;
    }
    // The piece's key, kept from frame to frame (its BLAS refitted): its mesh and its character - the game makes a new
    // job each frame it skins one - and, for two of one mesh on one character (or of characters it can't tell apart),
    // which of them this is in the frame's order.
    const skin::Job* j = static_cast<const skin::Job*>(job);
    const uint64_t base = MixKey(MixKey(kSkinnedSeed, uint64_t(reinterpret_cast<uintptr_t>(j->source.get()))), m_drawOwner);
    uint64_t key = base;
    for (const RtState::SkinnedDraw& d : s->skinnedDraws) {
        if (d.job == job) return d.key;          // (drawn again this frame: one instance)
        if (d.key == key) key = MixKey(key, 1);  // (the next of that mesh on that character)
    }
    auto address = [&](VkBuffer b) {
        VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai.buffer = b;
        return vkGetBufferDeviceAddress(m_device, &ai);
    };
    RtState::SkinnedDraw d{};
    d.job = job;
    d.key = key;
    d.vertices = address(arena) + vertexOffset;
    d.indices = address(meshBuffer) + indexOffset;
    d.vertexCount = vertexCount;
    d.triangles = indexCount / 3;
    const auto& w = m_world.m;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) d.world[r][c] = w[c][r];
    for (int r = 0; r < 3; ++r) {
        const double m[3] = {0.5 * (double(j->boundsMin[0]) + j->boundsMax[0]), 0.5 * (double(j->boundsMin[1]) + j->boundsMax[1]),
                             0.5 * (double(j->boundsMin[2]) + j->boundsMax[2])};
        d.centre[r] = d.world[r][0] * m[0] + d.world[r][1] * m[1] + d.world[r][2] * m[2] + d.world[r][3];
    }
    s->skinnedDraws.push_back(d);
    return key;
}

// A caster in the rays: its instance is there, casts, and its mesh is built (or being built this frame).
bool Device::RtTraced(uint64_t place) const
{
    const RtState* s = m_rt;
    if (!s || !place)
        return false;
    if (s->tracedSkinned.count(place))
        return true;
    auto i = s->instances.find(place);
    if (i == s->instances.end() || !(i->second.mask & kMaskCaster))
        return false;
    auto m = s->meshes.find(i->second.mesh);
    return m != s->meshes.end() && m->second.built;
}

uint64_t Device::RtStatelPlace(uint32_t item) const
{
    return m_rt ? MixKey(kStatelPlaceSeed ^ m_statelGeneration, item) : 0;
}

// An arena slot freed (ArenaFree): the meshes built from it go (once no submission in flight reads them), and their
// instances.
void Device::RtForgetSlot(VkBuffer buffer, VkDeviceSize offset)
{
    RtState* s = m_rt;
    auto range = s->bySlot.equal_range(SlotKey(buffer, offset));
    for (auto it = range.first; it != range.second; ++it) {
        auto m = s->meshes.find(it->second);
        if (m == s->meshes.end())
            continue;
        for (uint64_t place : m->second.places)
            s->instances.erase(place);
        if (m->second.built) {
            s->meshBytes -= m->second.storage.size;
            s->meshTriangles -= m->second.triangles;
            --s->builtMeshes;
        }
        if (m->second.as || m->second.storage.buffer)
            s->dead.push_back({DeathTag(), m->second.as, m->second.storage});
        s->meshes.erase(m);
    }
    s->bySlot.erase(range.first, range.second);
}

// The placed objects' pieces (statels.cpp) as meshes and instances, made again when their pack is loaded or a kind's
// placement is confirmed (as their shadows wait for it: a decode gone wrong would trace things turned the wrong way).
// The cut-out pieces (fences' wire, leaves) with their alpha masks, tested at the rays' hits.
void Device::RtSyncStatels()
{
    RtState* s = m_rt;
    const bool casting[2] = {StatelKindCasting(0), StatelKindCasting(1)};
    if (s->statelGen == m_statelGeneration && casting[0] == s->statelCasting[0] && casting[1] == s->statelCasting[1])
        return;
    for (uint64_t place : s->statelPlaces)
        s->instances.erase(place);
    for (uint64_t key : s->statelMeshes) {
        auto m = s->meshes.find(key);
        if (m == s->meshes.end())
            continue;
        if (m->second.built) {
            s->meshBytes -= m->second.storage.size;
            s->meshTriangles -= m->second.triangles;
            --s->builtMeshes;
        }
        if (m->second.as || m->second.storage.buffer)
            s->dead.push_back({DeathTag(), m->second.as, m->second.storage});
        s->meshes.erase(m);
    }
    s->statelMeshes.clear();
    s->statelPlaces.clear();
    s->statelGen = m_statelGeneration;
    s->statelCasting[0] = casting[0];
    s->statelCasting[1] = casting[1];
    if (!m_statelBuffer || (!casting[0] && !casting[1]))
        return;
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = m_statelBuffer;
    const VkDeviceAddress base = vkGetBufferDeviceAddress(m_device, &ai);
    for (uint32_t i = 0; i < m_statelItems.size(); ++i) {
        const StatelItem& item = m_statelItems[i];
        const StatelPiece& piece = m_statelPieces[item.piece];
        if (!casting[item.kind] || piece.indexCount < 3 || !piece.vertexCount)
            continue;
        const uint64_t key = MixKey(kStatelMeshSeed ^ m_statelGeneration, item.piece);
        if (!s->meshes.count(key)) {
            RtState::Mesh m;
            m.vertices = base + VkDeviceAddress(piece.firstVertex) * 20;
            m.indices = base + m_statelIndexBase + VkDeviceAddress(piece.firstIndex) * 2;
            m.stride = 20;
            m.vertexCount = piece.vertexCount;
            m.triangles = piece.indexCount / 3;
            m.queued = false;
            s->meshes.emplace(key, std::move(m));
            s->statelMeshes.push_back(key);
        }
        RtState::Instance inst{};
        inst.mesh = key;
        const auto& w = item.world.m;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) inst.world[r][c] = w[c][r];
        double r2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            inst.centre[k] = 0.5 * (double(item.boundsMin[k]) + item.boundsMax[k]);
            const double e = double(item.boundsMax[k]) - item.boundsMin[k];
            r2 += 0.25 * e * e;
        }
        inst.radius = std::sqrt(r2);
        inst.id = s->nextId++ & 0xFFFFFFu;
        inst.mask = kMaskCaster;
        inst.texture = piece.texture;            // (its alpha mask: white, the mask in alpha)
        inst.alphaRef = piece.alphaRef <= 1.0f ? piece.alphaRef : piece.alphaRef / 255.0f;
        inst.uvOffset = 12;
        const uint64_t place = RtStatelPlace(i);
        s->instances[place] = inst;
        if (piece.texture)
            s->byTexture.emplace(piece.texture, place);
        s->statelPlaces.push_back(place);
    }
    Log("ray tracing: %zu placed objects' pieces (%zu meshes) from the playfield's list", s->statelPlaces.size(),
        s->statelMeshes.size());
}

// This frame's new meshes built (within the budget) and the TLAS of the instances near the camera, in `cmd` - before
// any ray query of the frame. False: nothing to trace (no state, no camera, or it couldn't be made).
bool Device::RtUpdate(VkCommandBuffer cmd)
{
    if (!m_rayQuery || !m_aoProjValid)
        return false;
    if (!m_rt) {
        auto* s = new RtState;
        m_rt = s;
        // Its passes: push-descriptor sets, compute pipelines.
        auto pipeline = [&](const VkDescriptorSetLayoutBinding* b, uint32_t count, uint32_t pushBytes, const uint32_t* code,
                            size_t codeBytes, VkDescriptorSetLayout* set, VkPipelineLayout* layout, VkPipeline* out) {
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            li.bindingCount = count;
            li.pBindings = b;
            VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            VkDescriptorSetLayout sets[2] = {VK_NULL_HANDLE, m_bindlessSetLayout};   // (set 1: the cut-outs' textures)
            pl.setLayoutCount = 2;
            pl.pSetLayouts = sets;
            pl.pushConstantRangeCount = 1;
            pl.pPushConstantRanges = &push;
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = codeBytes;
            mi.pCode = code;
            VkShaderModule module = VK_NULL_HANDLE;
            bool made = vkCreateDescriptorSetLayout(m_device, &li, nullptr, set) == VK_SUCCESS;
            sets[0] = *set;
            made = made && vkCreatePipelineLayout(m_device, &pl, nullptr, layout) == VK_SUCCESS &&
                   vkCreateShaderModule(m_device, &mi, nullptr, &module) == VK_SUCCESS;
            if (made) {
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                            module, "main", nullptr};
                ci.layout = *layout;
                made = vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, out) == VK_SUCCESS;
            }
            if (module) vkDestroyShaderModule(m_device, module, nullptr);
            return made;
        };
        const VkShaderStageFlags comp = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkDescriptorSetLayoutBinding debugBindings[3] = {
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, comp, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, comp, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, comp, nullptr}};
        const VkDescriptorSetLayoutBinding aoBindings[8] = {
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, comp, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
            {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, comp, nullptr},
            {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, comp, nullptr},
            {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, comp, nullptr}};
        const bool ok = m_bindlessSetLayout && m_bindlessSet &&
                        pipeline(debugBindings, 3, 80, kRtDebugSpirv, sizeof(kRtDebugSpirv), &s->setLayout, &s->layout,
                                 &s->debugPipeline) &&
                        pipeline(aoBindings, 8, 112, kRtAoSpirv, sizeof(kRtAoSpirv), &s->aoSetLayout, &s->aoLayout,
                                 &s->aoPipeline);
        // RVK_RtShadow's trace (rt_shadow.comp) and blur (rt_shadow_blur.comp).
        const VkDescriptorSetLayoutBinding shadowBindings[7] = {
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, comp, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, comp, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, comp, nullptr},
            {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, comp, nullptr},
            {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, comp, nullptr}};
        const VkDescriptorSetLayoutBinding blurBindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, comp, nullptr}};
        if (ok && !(pipeline(shadowBindings, 7, 16, kRtShadowSpirv, sizeof(kRtShadowSpirv), &s->shadowSetLayout,
                             &s->shadowLayout, &s->shadowPipeline) &&
                    pipeline(blurBindings, 2, 16, kRtShadowBlurSpirv, sizeof(kRtShadowBlurSpirv), &s->blurSetLayout,
                             &s->blurLayout, &s->blurPipeline))) {
            Log("ray tracing: the sun shadows' pipelines could not be made");
            if (s->shadowPipeline) vkDestroyPipeline(m_device, s->shadowPipeline, nullptr);
            s->shadowPipeline = VK_NULL_HANDLE;
        }
        if (!ok) {
            Log("ray tracing: its pipelines could not be made");
            s->failed = true;
        } else {
            Log("ray tracing: on (meshes from the static arena and the placed objects, instances within %.0f units)",
                kRange);
        }
        return false;                            // (meshes come with the next frame's draws)
    }
    RtState* s = m_rt;
    if (s->failed)
        return false;
    if (s->tlasFrame == m_frameNumber)
        return true;                             // (built already this frame: RenderRtShadow, at the pre-pass's end)
    RtSyncStatels();
    {
        const auto& v = m_aoView.m;
        for (int k = 0; k < 3; ++k)
            s->eye[k] = -(double(v[3][0]) * v[k][0] + double(v[3][1]) * v[k][1] + double(v[3][2]) * v[k][2]);
    }
    // (This frame's arena uploads and characters' skinning - the upload commands, submitted first - and the last
    // frame's builds and reads.)
    Barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | kBuild | kRayStages,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            kBuild, kAsRW | VK_ACCESS_2_SHADER_READ_BIT);

    // The new meshes' BLAS, a budget of triangles at a time.
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<VkDeviceSize> scratchAt;
    std::vector<uint64_t> building;
    VkDeviceSize scratchNeed = 0;
    uint32_t triangles = 0;
    size_t taken = 0;
    for (; taken < s->toBuild.size() && triangles < kBuildBudget; ++taken) {
        auto it = s->meshes.find(s->toBuild[taken]);
        if (it == s->meshes.end() || it->second.built)
            continue;
        building.push_back(it->first);
        triangles += it->second.triangles;
    }
    s->toBuild.erase(s->toBuild.begin(), s->toBuild.begin() + ptrdiff_t(taken));
    geoms.resize(building.size());
    infos.resize(building.size());
    ranges.resize(building.size());
    scratchAt.resize(building.size());
    for (size_t i = 0; i < building.size(); ++i) {
        RtState::Mesh& m = s->meshes[building[i]];
        VkAccelerationStructureGeometryKHR& g = geoms[i];
        g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& t = g.geometry.triangles;
        t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t.vertexData.deviceAddress = m.vertices;
        t.vertexStride = m.stride;
        t.maxVertex = m.vertexCount - 1;
        t.indexType = VK_INDEX_TYPE_UINT16;
        t.indexData.deviceAddress = m.indices;
        VkAccelerationStructureBuildGeometryInfoKHR& bi = infos[i];
        bi = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi,
                                                &m.triangles, &sz);
        if (!MakeBuffer(m_allocator, m_device, &m.storage, sz.accelerationStructureSize,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false)) {
            building.resize(i);
            break;
        }
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = m.storage.buffer;
        ci.size = sz.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        vkCreateAccelerationStructureKHR(m_device, &ci, nullptr, &m.as);
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = m.as;
        m.address = vkGetAccelerationStructureDeviceAddressKHR(m_device, &ai);
        bi.dstAccelerationStructure = m.as;
        scratchAt[i] = scratchNeed;
        scratchNeed = AlignUp(scratchNeed + sz.buildScratchSize, m_asScratchAlign);
        ranges[i] = {m.triangles, 0, 0, 0};
    }
    infos.resize(building.size());

    // This frame's characters' pieces: built the first time (and when their counts change), refitted after.
    std::vector<VkAccelerationStructureGeometryKHR> skinGeoms;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> skinInfos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> skinRanges;
    std::vector<VkDeviceSize> skinScratchAt;
    std::vector<const RtState::SkinnedDraw*> skinDrawn;
    if (s->skinnedFrame == m_frameNumber) {
        skinGeoms.resize(s->skinnedDraws.size());
        skinInfos.resize(s->skinnedDraws.size());
        skinRanges.resize(s->skinnedDraws.size());
        skinScratchAt.resize(s->skinnedDraws.size());
        for (const RtState::SkinnedDraw& d : s->skinnedDraws) {
            const double dx = d.centre[0] - s->eye[0], dy = d.centre[1] - s->eye[1], dz = d.centre[2] - s->eye[2];
            if (dx * dx + dy * dy + dz * dz > kRange * kRange)
                continue;
            RtState::Skinned& k = s->skinned[d.key];
            if (k.as && (k.vertexCount != d.vertexCount || k.triangles != d.triangles)) {
                s->dead.push_back({DeathTag(), k.as, k.storage});
                k = {};
            }
            const size_t i = skinDrawn.size();
            VkAccelerationStructureGeometryKHR& g = skinGeoms[i];
            g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            auto& t = g.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexData.deviceAddress = d.vertices;
            t.vertexStride = 32;
            t.maxVertex = d.vertexCount - 1;
            t.indexType = VK_INDEX_TYPE_UINT16;
            t.indexData.deviceAddress = d.indices;
            VkAccelerationStructureBuildGeometryInfoKHR& bi = skinInfos[i];
            bi = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR |
                       VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            bi.geometryCount = 1;
            bi.pGeometries = &g;
            if (!k.as) {
                VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
                vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi,
                                                        &d.triangles, &sz);
                if (!MakeBuffer(m_allocator, m_device, &k.storage, sz.accelerationStructureSize,
                                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false))
                    break;
                VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
                ci.buffer = k.storage.buffer;
                ci.size = sz.accelerationStructureSize;
                ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                vkCreateAccelerationStructureKHR(m_device, &ci, nullptr, &k.as);
                k.vertexCount = d.vertexCount;
                k.triangles = d.triangles;
                k.buildScratch = sz.buildScratchSize;
                k.updateScratch = sz.updateScratchSize;
            }
            bi.mode = k.built ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            bi.srcAccelerationStructure = k.built ? k.as : VK_NULL_HANDLE;
            bi.dstAccelerationStructure = k.as;
            skinScratchAt[i] = scratchNeed;
            scratchNeed = AlignUp(scratchNeed + (k.built ? k.updateScratch : k.buildScratch), m_asScratchAlign);
            skinRanges[i] = {d.triangles, 0, 0, 0};
            k.lastFrame = m_frameNumber;
            skinDrawn.push_back(&d);
        }
        skinInfos.resize(skinDrawn.size());
    }
    // (Pieces not drawn for a while: gone.)
    if ((m_frameNumber & 63) == 0)
        for (auto it = s->skinned.begin(); it != s->skinned.end();) {
            if (it->second.lastFrame + 120 < m_frameNumber) {
                s->dead.push_back({DeathTag(), it->second.as, it->second.storage});
                it = s->skinned.erase(it);
            } else {
                ++it;
            }
        }

    // The TLAS's instances: those near the camera whose mesh is (or is now being) built, placed relative to the eye.
    RtBuffer& ib = s->instanceBuf[m_frameIndex];
    std::vector<VkAccelerationStructureInstanceKHR> inst;
    std::vector<GeoRecord> geo;                  // (parallel to inst)
    inst.reserve(s->instances.size());
    geo.reserve(s->instances.size());
    const uint32_t cutSampler = FixedSamplerSlot(0, m_bumpSampler);   // (repeating)
    auto record = [&](VkDeviceAddress vertices, VkDeviceAddress indices, uint32_t stride, Texture* texture,
                      float alphaRef, uint32_t uvOffset) {
        GeoRecord g{};
        g.vertices[0] = uint32_t(vertices);
        g.vertices[1] = uint32_t(vertices >> 32);
        g.indices[0] = uint32_t(indices);
        g.indices[1] = uint32_t(indices >> 32);
        g.stride = stride;
        g.uvOffset = uvOffset;
        const uint32_t image = texture ? BindlessImage(texture) : ~0u;
        g.texture = texture && image < (1u << 20) - 1 && cutSampler < 256 ? (image + 1) | cutSampler << 20 : 0;
        g.alphaRef = alphaRef;
        geo.push_back(g);
        return g.texture != 0;
    };
    for (const auto& [place, in] : s->instances) {
        const double dx = in.centre[0] - s->eye[0], dy = in.centre[1] - s->eye[1], dz = in.centre[2] - s->eye[2];
        const double reach = kRange + in.radius;
        if (dx * dx + dy * dy + dz * dz > reach * reach)
            continue;
        auto mi = s->meshes.find(in.mesh);
        if (mi == s->meshes.end())
            continue;
        if (!mi->second.address) {               // (a placed object's piece: built once a place of it is in range)
            if (!mi->second.queued) {
                mi->second.queued = true;
                s->toBuild.push_back(in.mesh);
            }
            continue;
        }
        const RtState::Mesh& m = mi->second;
        VkAccelerationStructureInstanceKHR v{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) v.transform.matrix[r][c] = float(in.world[r][c]);
            v.transform.matrix[r][3] = float(in.world[r][3] - s->eye[r]);
        }
        v.instanceCustomIndex = in.id;
        v.mask = in.mask;
        const bool cut = record(m.vertices, m.indices, m.stride, in.texture, in.alphaRef, in.uvOffset);
        v.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                  (cut ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
        v.accelerationStructureReference = m.address;
        inst.push_back(v);
    }
    s->tracedSkinned.clear();
    for (const RtState::SkinnedDraw* d : skinDrawn) {
        const RtState::Skinned& k = s->skinned[d->key];
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = k.as;
        VkAccelerationStructureInstanceKHR v{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) v.transform.matrix[r][c] = float(d->world[r][c]);
            v.transform.matrix[r][3] = float(d->world[r][3] - s->eye[r]);
        }
        v.instanceCustomIndex = uint32_t(d->key) & 0xFFFFFFu;
        v.mask = kMaskCaster;
        v.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        v.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(m_device, &ai);
        inst.push_back(v);
        record(d->vertices, d->indices, 32, nullptr, 0.0f, 0);
        s->tracedSkinned.insert(d->key);
    }
    const uint32_t count = uint32_t(inst.size());
    const VkDeviceSize instBytes = std::max<VkDeviceSize>(1, count) * sizeof(VkAccelerationStructureInstanceKHR);
    if (ib.size < instBytes) {
        if (ib.buffer) s->dead.push_back({DeathTag(), VK_NULL_HANDLE, ib});
        ib = {};
        if (!MakeBuffer(m_allocator, m_device, &ib, instBytes * 3 / 2 + 4096,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true) || !ib.mapped) {
            s->failed = true;
            Log("ray tracing: no memory for its instances");
            return false;
        }
    }
    if (count) std::memcpy(ib.mapped, inst.data(), size_t(count) * sizeof(inst[0]));
    RtBuffer& gb = s->geoBuf[m_frameIndex];
    const VkDeviceSize geoBytes = std::max<VkDeviceSize>(1, count) * sizeof(GeoRecord);
    if (gb.size < geoBytes) {
        if (gb.buffer) s->dead.push_back({DeathTag(), VK_NULL_HANDLE, gb});
        gb = {};
        if (!MakeBuffer(m_allocator, m_device, &gb, geoBytes * 3 / 2 + 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
            !gb.mapped) {
            s->failed = true;
            Log("ray tracing: no memory for its instances' records");
            return false;
        }
    }
    if (count) std::memcpy(gb.mapped, geo.data(), size_t(count) * sizeof(GeoRecord));
    VkAccelerationStructureGeometryKHR tg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tg.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tg.geometry.instances.data.deviceAddress = ib.address;
    VkAccelerationStructureBuildGeometryInfoKHR ti{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    ti.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    ti.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    ti.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    ti.geometryCount = 1;
    ti.pGeometries = &tg;
    if (s->tlasCapacity < count || !s->tlas) {
        const uint32_t capacity = std::max(1024u, std::max(count * 3 / 2, s->tlasCapacity));
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ti, &capacity, &sz);
        if (s->tlas || s->tlasStorage.buffer) s->dead.push_back({DeathTag(), s->tlas, s->tlasStorage});
        s->tlas = VK_NULL_HANDLE;
        s->tlasStorage = {};
        if (!MakeBuffer(m_allocator, m_device, &s->tlasStorage, sz.accelerationStructureSize,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false)) {
            s->failed = true;
            Log("ray tracing: no memory for its TLAS");
            return false;
        }
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = s->tlasStorage.buffer;
        ci.size = sz.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        vkCreateAccelerationStructureKHR(m_device, &ci, nullptr, &s->tlas);
        s->tlasCapacity = capacity;
    }
    VkAccelerationStructureBuildSizesInfoKHR tsz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ti, &count, &tsz);
    ti.dstAccelerationStructure = s->tlas;

    // One scratch for both (the TLAS's build waits for the BLAS's).
    const VkDeviceSize need = std::max(scratchNeed, tsz.buildScratchSize) + m_asScratchAlign;
    if (s->scratch.size < need) {
        if (s->scratch.buffer) s->dead.push_back({DeathTag(), VK_NULL_HANDLE, s->scratch});
        s->scratch = {};
        if (!MakeBuffer(m_allocator, m_device, &s->scratch, need * 3 / 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) {
            s->failed = true;
            Log("ray tracing: no memory for its scratch");
            return false;
        }
    }
    const VkDeviceAddress scratchBase = AlignUp(s->scratch.address, m_asScratchAlign);
    if (!building.empty() || !skinDrawn.empty()) {
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePtrs(building.size());
        for (size_t i = 0; i < building.size(); ++i) {
            infos[i].pGeometries = &geoms[i];
            infos[i].scratchData.deviceAddress = scratchBase + scratchAt[i];
            rangePtrs[i] = &ranges[i];
        }
        for (size_t i = 0; i < skinDrawn.size(); ++i) {
            skinInfos[i].pGeometries = &skinGeoms[i];
            skinInfos[i].scratchData.deviceAddress = scratchBase + skinScratchAt[i];
            infos.push_back(skinInfos[i]);
            rangePtrs.push_back(&skinRanges[i]);
        }
        vkCmdBuildAccelerationStructuresKHR(cmd, uint32_t(infos.size()), infos.data(), rangePtrs.data());
        for (const RtState::SkinnedDraw* d : skinDrawn) s->skinned[d->key].built = true;
        for (uint64_t key : building) {
            RtState::Mesh& m = s->meshes[key];
            m.built = true;
            s->meshBytes += m.storage.size;
            s->meshTriangles += m.triangles;
            ++s->builtMeshes;
        }
        Barrier(cmd, kBuild, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, kBuild, kAsRW);
    }
    ti.scratchData.deviceAddress = scratchBase;
    VkAccelerationStructureBuildRangeInfoKHR tr{count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &ti, &trp);
    Barrier(cmd, kBuild, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, kRayStages,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    s->tlasCount = count;
    s->tlasFrame = m_frameNumber;
    if (m_frameNumber >= s->logFrame + 600) {
        s->logFrame = m_frameNumber;
        Log("ray tracing: %u meshes built (%.1f MB, %llu triangles), %zu waiting; %zu instances, %u in the TLAS (%zu of "
            "them characters' pieces, %zu drawn)",
            s->builtMeshes, double(s->meshBytes) / 1048576.0, (unsigned long long)s->meshTriangles, s->toBuild.size(),
            s->instances.size(), count, skinDrawn.size(), s->skinnedFrame == m_frameNumber ? s->skinnedDraws.size() : 0);
    }
    return true;
}

// RVK_AoMode 2: ray-traced ambient occlusion (rt_ao.comp), accumulated over frames, blurred (ao_blur.frag) into
// m_aoFullTex - occlusion and view depth at full resolution, as the tone mapping takes AMD's. After RtUpdate this frame.
bool Device::RenderRtAo(VkCommandBuffer cmd)
{
    RtState* s = m_rt;
    const bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (!s || s->tlasFrame != m_frameNumber || !perspective || m_aoStrength <= 0.0f || !m_aoBlurPipeline)
        return false;
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    if (!s->ao || s->ao->m_width != w || s->ao->m_height != h) {
        for (Texture** t : {&s->aoHistory[0], &s->aoHistory[1], &s->aoOut, &s->aoBlur, &s->ao}) {
            if (*t) DestroyTexture(*t);
            const bool storage = t != &s->aoBlur && t != &s->ao;
            *t = CreateImage(w, h, storage ? Format::RGBA16F : Format::RG16F, 1, true, storage);
            if (!*t)
                return false;
        }
        s->aoFrame = ~0ull;
    }
    if (!RenderSceneSurface(cmd) || !RenderSceneMotion(cmd))
        return false;
    MakeDepthReadable(cmd);
    Texture* prev = s->aoHistory[s->aoCurrent];
    Texture* next = s->aoHistory[s->aoCurrent ^ 1];
    const bool continuous = s->aoFrame + 1 == m_frameNumber && m_prevViewProjValid;
    Transition(cmd, prev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, next, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, s->aoOut, VK_IMAGE_LAYOUT_GENERAL);
    // (What fragment passes wrote - the depth, the surfaces, the motion - for a compute pass.)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    const auto& v = m_aoView.m;
    struct Push { float proj[4], size[4], view[3][4], frame[4]; } push{
        {m_aoProj.m[2][2], m_aoProj.m[3][2], m_aoProj.m[0][0], m_aoProj.m[1][1]},
        {float(w), float(h), 3.0f * m_aoRadius, m_aoStrength},
        {{v[0][0], v[0][1], v[0][2], 0.0f}, {v[1][0], v[1][1], v[1][2], 0.0f}, {v[2][0], v[2][1], v[2][2], 0.0f}},
        {float(std::fmod(double(m_frameNumber % 4096) * 0.6180339887, 1.0)), continuous ? 1.0f : 0.0f, 0.0f, 0.0f}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->aoPipeline);
    VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as.accelerationStructureCount = 1;
    as.pAccelerationStructures = &s->tlas;
    const VkDescriptorImageInfo images[6] = {
        {m_pointSampler, m_depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_pointSampler, m_sceneSurface->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_pointSampler, m_sceneMotion->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_pointSampler, prev->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, next->m_view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, s->aoOut->m_view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo geos{s->geoBuf[m_frameIndex].buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[8] = {};
    for (uint32_t i = 0; i < 8; ++i) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                             : i < 5  ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                             : i < 7  ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                      : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (i > 0 && i < 7) wr[i].pImageInfo = &images[i - 1];
    }
    wr[0].pNext = &as;
    wr[7].pBufferInfo = &geos;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->aoLayout, 0, 8, wr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->aoLayout, 1, 1, &m_bindlessSet, 0, nullptr);
    vkCmdPushConstants(cmd, s->aoLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    s->aoCurrent ^= 1;
    s->aoFrame = m_frameNumber;
    // Blurred both ways (depth-aware, as ours).
    Transition(cmd, s->aoOut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    float dir[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    FullscreenPass(cmd, s->aoBlur, m_aoBlurPipeline, s->aoOut->m_view, s->aoOut->m_view, m_pointSampler, dir,
                   sizeof(dir), false);
    Transition(cmd, s->aoBlur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    dir[0] = 0.0f;
    dir[1] = 1.0f;
    FullscreenPass(cmd, s->ao, m_aoBlurPipeline, s->aoBlur->m_view, s->aoBlur->m_view, m_pointSampler, dir,
                   sizeof(dir), false);
    Transition(cmd, s->ao, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_aoFullTex = s->ao;
    return true;
}

// RVK_RtShadow 1, where a depth pre-pass segment ends (prepass.cpp PrepassEnd): this frame's structures and the sun's
// visibility traced from the pre-pass's depth (rt_shadow.comp), averaged over frames, blurred across and down
// (rt_shadow_blur.comp) into the mask the scene shader reads (FL.rtShadow) - in the frame's own command buffer, run
// after the pre-pass and before mainB, whose draws are already recorded. Once a frame (the first segment: the world's).
void Device::RenderRtShadow(uint32_t cause)
{
    (void)cause;
    Frame& f = m_frames[m_frameIndex];
    const bool perspective = m_aoProjValid && m_aoProj.m[2][3] == 1.0f && m_aoProj.m[3][3] == 0.0f;
    if (!f.split || !f.rays || f.raysRecorded || !perspective || !m_depth || !m_depthView || !m_scene || !m_shadowValid ||
        (m_rt && m_rt->shadowFrame == m_frameNumber))
        return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.rays, &bi);
    f.raysRecorded = true;
    VkCommandBuffer cmd = f.rays;
    RtState* s = nullptr;
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    bool ready = RtUpdate(cmd) && (s = m_rt) && s->shadowPipeline && s->blurPipeline;
    if (ready && (!s->shadowMask || s->shadowMask->m_width != w || s->shadowMask->m_height != h)) {
        for (Texture** t : {&s->shadowHistory[0], &s->shadowHistory[1], &s->shadowRaw, &s->shadowBlur, &s->shadowMask}) {
            if (*t) DestroyTexture(*t);
            *t = CreateImage(w, h, Format::RGBA16F, 1, true, true);
            ready = ready && *t;
        }
        s->shadowFrame = ~0ull;
    }
    RtBuffer* params = ready ? &s->shadowParams[m_frameIndex] : nullptr;
    if (params && !params->buffer &&
        (!MakeBuffer(m_allocator, m_device, params, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true) || !params->mapped))
        ready = false;
    if (!ready) {
        vkEndCommandBuffer(cmd);
        return;
    }
    const bool continuous = s->shadowFrame + 1 == m_frameNumber && m_prevViewProjValid;
    // The parameters (rt_shadow.comp Params, std140).
    struct Params { float proj[4], size[4], view[3][4], reproject[16], misc[4]; } p{};
    const auto& v = m_aoView.m;
    const float* sunDir = m_shadowSunDir;        // (the map's: FL.sunDir)
    const float sl = std::max(std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]), 1e-6f);
    const float cone = 0.01f * std::max(m_sunSoftness, 0.1f);   // (the map's penumbrae: 0.02 x softness wide a unit)
    const float proj[4] = {m_aoProj.m[2][2], m_aoProj.m[3][2], m_aoProj.m[0][0], m_aoProj.m[1][1]};
    std::memcpy(p.proj, proj, sizeof(proj));
    p.size[0] = float(w);
    p.size[1] = float(h);
    p.size[2] = cone;
    p.size[3] = float(std::fmod(double(m_frameNumber % 4096) * 0.6180339887, 1.0));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) p.view[r][c] = v[r][c];
        p.view[r][3] = -sunDir[r] / sl;
    }
    if (continuous) {
        d3d::Matrix reproject;
        CameraReprojection(m_aoView, m_aoProj, m_prevView, m_prevProj, &reproject);
        std::memcpy(p.reproject, &reproject, sizeof(p.reproject));
    }
    p.misc[0] = continuous ? 1.0f : 0.0f;
    p.misc[1] = float(kRange);
    p.misc[2] = 16.0f;
    std::memcpy(params->mapped, &p, sizeof(p));
    vmaFlushAllocation(m_allocator, params->allocation, 0, VK_WHOLE_SIZE);

    // The pre-pass's depth, read (its layout put back after, for mainB's tests).
    const VkPipelineStageFlags2 tests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    Texture* prev = s->shadowHistory[s->shadowCurrent];
    Texture* next = s->shadowHistory[s->shadowCurrent ^ 1];
    Transition(cmd, prev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, next, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, s->shadowRaw, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->shadowPipeline);
    VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as.accelerationStructureCount = 1;
    as.pAccelerationStructures = &s->tlas;
    const VkDescriptorImageInfo images[4] = {
        {m_pointSampler, m_depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_pointSampler, prev->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, next->m_view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, s->shadowRaw->m_view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo ubo{params->buffer, 0, sizeof(Params)};
    const VkDescriptorBufferInfo geos{s->geoBuf[m_frameIndex].buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[7] = {};
    for (uint32_t i = 0; i < 7; ++i) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                             : i < 3  ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                             : i < 5  ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                             : i < 6  ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                      : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (i > 0 && i < 5) wr[i].pImageInfo = &images[i - 1];
    }
    wr[0].pNext = &as;
    wr[5].pBufferInfo = &ubo;
    wr[6].pBufferInfo = &geos;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->shadowLayout, 0, 7, wr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->shadowLayout, 1, 1, &m_bindlessSet, 0, nullptr);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    s->shadowCurrent ^= 1;
    s->shadowFrame = m_frameNumber;
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, tests,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    // Blurred across, then down (into the mask, left readable for the scene's draws).
    auto blur = [&](Texture* from, Texture* to, float dx, float dy) {
        Transition(cmd, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmd, to, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->blurPipeline);
        const VkDescriptorImageInfo io[2] = {{m_pointSampler, from->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {VK_NULL_HANDLE, to->m_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet bw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        for (uint32_t i = 0; i < 2; ++i) {
            bw[i].dstBinding = i;
            bw[i].descriptorCount = 1;
            bw[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bw[i].pImageInfo = &io[i];
        }
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->blurLayout, 0, 2, bw);
        const float push[4] = {dx, dy, 0.5f * proj[2] * float(w), cone};
        vkCmdPushConstants(cmd, s->blurLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    };
    blur(s->shadowRaw, s->shadowBlur, 1.0f, 0.0f);
    blur(s->shadowBlur, s->shadowMask, 0.0f, 1.0f);
    Transition(cmd, s->shadowMask, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkEndCommandBuffer(cmd);
}

// FL.rtShadow.x: the mask's bindless image + 1 | sampler << 20 - when it is traced this frame (or was the last: the
// frame lights may be written before the pre-pass ends) and the map has the casters the rays lack; else 0.
uint32_t Device::RtShadowMaskBits()
{
    const RtState* s = m_rt;
    if (!s || !s->shadowMask || !m_shadowUntraced || !m_shadowValid || !RtShadowWanted() || s->shadowFrame == ~0ull ||
        s->shadowFrame + 1 < m_frameNumber)
        return 0;
    const uint32_t image = BindlessImage(s->shadowMask), sampler = FixedSamplerSlot(2, m_linearSampler);
    return image != ~0u && image < (1u << 20) - 1 && sampler < 256 ? (image + 1) | sampler << 20 : 0;
}

// RVK_RtDebug 1: what the rays hit, over the scene (rt_debug.comp).
void Device::RenderRtDebug(VkCommandBuffer cmd)
{
    RtState* s = m_rt;
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    if (!s->debug || s->debug->m_width != w || s->debug->m_height != h) {
        if (s->debug) DestroyTexture(s->debug);
        s->debug = CreateImage(w, h, Format::RGBA16F, 1, true, true);
        if (!s->debug)
            return;
    }
    d3d::Matrix invProj;
    if (!InvertMatrix(m_aoProj, &invProj))
        return;
    // Camera-relative: the view's rotation alone, inverted (transposed); then x the projection's inverse.
    const auto& v = m_aoView.m;
    d3d::Matrix invView{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) invView.m[i][j] = v[j][i];
    invView.m[3][3] = 1.0f;
    struct Push { d3d::Matrix invViewProj; float size[4]; } push{MulMatrix(invProj, invView), {float(w), float(h), 0, 0}};
    Transition(cmd, s->debug, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->debugPipeline);
    VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as.accelerationStructureCount = 1;
    as.pAccelerationStructures = &s->tlas;
    VkDescriptorImageInfo out{VK_NULL_HANDLE, s->debug->m_view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo geos{s->geoBuf[m_frameIndex].buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    wr[0].pNext = &as;
    wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    wr[1].dstBinding = 1;
    wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[1].pImageInfo = &out;
    wr[2].dstBinding = 2;
    wr[2].descriptorCount = 1;
    wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr[2].pBufferInfo = &geos;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 3, wr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 1, 1, &m_bindlessSet, 0, nullptr);
    vkCmdPushConstants(cmd, s->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    Transition(cmd, s->debug, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cmd, m_scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{};
    c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.dstSubresource = c.srcSubresource;
    c.extent = {w, h, 1};
    vkCmdCopyImage(cmd, s->debug->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_scene->m_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(cmd, m_scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace rvk
