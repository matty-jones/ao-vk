// Which static meshes stand where - for what keeps a model of the static world on the GPU (Brixelizer's distance
// field, brixelizer.cpp; the ray tracing's acceleration structures, raytrace.cpp). A mesh is an opaque draw whose
// vertices and indices the static arena keeps (static_gpu.cpp), known by its arena slots; drawn at a place, it is an
// instance there, kept while its slots live (casters behind the camera, which the game doesn't draw, still count). A
// mesh drawn somewhere new each frame and nowhere else (a door, a lift) moved: its old instance goes, and after a few
// moves it is left out.
#pragma once

#include "d3d7.h"

#include <cstdint>
#include <cstring>
#include <unordered_map>

#include <vulkan/vulkan.h>

namespace rvk {

inline uint64_t MixKey(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

// An arena slot (a buffer and the slot's start), as ArenaFree names it.
inline uint64_t SlotKey(VkBuffer buffer, VkDeviceSize offset) { return MixKey(uint64_t(buffer), uint64_t(offset)); }

inline uint64_t MeshKey(VkBuffer vb, VkDeviceSize vbOffset, VkBuffer ib, VkDeviceSize ibOffset, uint32_t vertexCount,
                        uint32_t indexCount, uint32_t stride)
{
    return MixKey(MixKey(MixKey(uint64_t(vb), vbOffset), MixKey(uint64_t(ib), ibOffset)),
                  MixKey(vertexCount, uint64_t(indexCount) << 8 | stride));
}

inline uint64_t PlaceKey(uint64_t mesh, const d3d::Matrix& world)
{
    uint64_t key = mesh;
    for (int i = 0; i < 16; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &world.m[i / 4][i % 4], 4);
        key = MixKey(key, bits);
    }
    return key;
}

class Placements {
public:
    static constexpr uint32_t kMoves = 3;        // a mesh moved this often is left out

    // A draw of `mesh` at `place` in frame `frame`. False: leave it out (it keeps moving). *moved: the instance it
    // left (to delete), else 0.
    bool Note(uint64_t mesh, uint64_t place, uint64_t frame, uint64_t* moved)
    {
        *moved = 0;
        Mesh& m = m_meshes[mesh];
        if (m.moves >= kMoves)
            return false;
        if (m.place != place && m.place) {
            if (m.frame == frame) {
                m.many = true;                   // two places in one frame: the same mesh placed twice
            } else if (!m.many && m.frame + 3 >= frame) {
                *moved = m.place;                // somewhere else than a frame or two ago, and nowhere else
                if (++m.moves >= kMoves)
                    return false;
            }
        }
        m.frame = frame;
        m.place = place;
        return true;
    }

    void Clear() { m_meshes.clear(); }

private:
    struct Mesh {
        uint64_t frame = 0, place = 0;
        bool many = false;
        uint32_t moves = 0;
    };
    std::unordered_map<uint64_t, Mesh> m_meshes;
};

}  // namespace rvk
