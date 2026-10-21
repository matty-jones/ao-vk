// Which static meshes stand where - for what keeps a model of the static world on the GPU (Brixelizer's distance
// field, brixelizer.cpp; the ray tracing's acceleration structures, raytrace.cpp). A mesh is an opaque draw whose
// vertices and indices the static arena keeps (static_gpu.cpp), known by its arena slots; drawn at a place, it is an
// instance there, kept while its slots live (casters behind the camera, which the game doesn't draw, still count). A
// mesh drawn somewhere new each frame and nowhere else (a door, a lift) moved: its old instance goes, and after a few
// moves it is left out. A new place is young until it has stood for a while: one that goes unseen before that (a
// moving thing of a mesh placed several times - the same weapon on two characters, machinery turning) goes too, and a
// mesh that keeps leaving places behind so is left out as well - else they pile up, a new one each frame.
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
    static constexpr uint64_t kStill = 30;       // frames a place is drawn over before it counts as standing
    static constexpr uint64_t kGone = 4;         // frames a young place may go unseen
    static constexpr uint32_t kLeftBehind = 8;   // young places of a mesh gone this often: it is left out

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
        if (!m_young.empty()) {
            auto y = m_young.find(place);
            if (y != m_young.end()) y->second.last = frame;
        }
        return true;
    }

    // An instance made for a place Note kept: young until it has stood kStill frames.
    void Added(uint64_t mesh, uint64_t place, uint64_t frame) { m_young[place] = {mesh, frame, frame}; }

    // Once a frame: gone(place) for each young place unseen for kGone frames (its instance to delete).
    template <class F>
    void Expire(uint64_t frame, F&& gone)
    {
        for (auto it = m_young.begin(); it != m_young.end();) {
            const Young& y = it->second;
            if (y.last >= y.first + kStill) {
                it = m_young.erase(it);          // (it stands: kept as the others)
            } else if (frame > y.last + kGone) {
                Mesh& m = m_meshes[y.mesh];
                if (++m.leftBehind >= kLeftBehind)
                    m.moves = kMoves;
                gone(it->first);
                it = m_young.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Seen this frame or the last, or standing (a young place unseen since may have moved on: not traced meanwhile).
    bool Current(uint64_t place, uint64_t frame) const
    {
        auto y = m_young.find(place);
        return y == m_young.end() || y->second.last + 1 >= frame;
    }

    void Clear()
    {
        m_meshes.clear();
        m_young.clear();
    }

private:
    struct Mesh {
        uint64_t frame = 0, place = 0;
        bool many = false;
        uint32_t moves = 0, leftBehind = 0;
    };
    std::unordered_map<uint64_t, Mesh> m_meshes;
    struct Young {
        uint64_t mesh, first, last;
    };
    std::unordered_map<uint64_t, Young> m_young;
};

}  // namespace rvk
