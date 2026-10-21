// The water's surface from the game's own triangles (docs/water-mesh-plan.md): its flat triangles welded, stitched
// where coarse ones meet fine ones (T-junctions), and split until no edge is long, so the GPU can tessellate and wave
// it without cracks; per vertex how calm the waves are there (shores, falls) and how big its body of water is. Plain
// C++ (no Vulkan): tools build it on its own to check the dumped meshes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvk::water {

struct InVertex {                               // as Device::WaterVertex
    float pos[3];
    uint32_t colour;                            // ARGB: the game's tint and, in its alpha, its shore fade
};

struct SurfaceVertex {                          // water_surface.vert's SurfaceVertex
    float pos[3];
    float calm;                                 // 0 at a shore or a fall's lip .. 1 from kCalmDistance inward
    float body;                                 // its body's size: 0 a pond .. 1 the sea
    float alpha;                                // the game's vertex alpha, 0..1
    float pad[2];
};

struct Surface {
    std::vector<SurfaceVertex> vertices;
    std::vector<uint32_t> indices;              // triangles (patches of 3)
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    uint32_t colour = 0;                        // the first flat vertex's (the body's tint)
    // (For the log: what it took.)
    uint32_t inTriangles = 0, welded = 0, stitched = 0, splitRounds = 0, bodies = 0;
    float largestBody = 0.0f;
};

constexpr float kMaxEdge = 32.0f;               // longest edge after splitting (the GPU splits each up to 64 times)
constexpr float kCalmDistance = 6.0f;           // world units from a shore or a fall over which the waves come up

// flat: the surface's triangles (index triples into v); steep: the falls' and shore steps' (their corners are seams
// the surface must not move at).
void BuildSurface(const InVertex* v, size_t count, const std::vector<uint32_t>& flat, const std::vector<uint32_t>& steep,
                  Surface& out);

}  // namespace rvk::water
