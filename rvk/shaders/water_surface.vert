#version 450
// The water's surface from the game's own triangles (rvk/water.cpp DrawWater, rvk/water_surface.h): each vertex as
// the preprocessing left it, on to the tessellation (water_surface.tesc, water_surface.tese).
#include "water_common.glsl"

// (rvk/water_surface.h SurfaceVertex.) calm: 0 at a shore or a fall, 1 inward; body: its body of water's size (0 a
// pond .. 1 the sea); alpha: the game's vertex alpha.
struct SurfaceVertex { float x, y, z, calm; float body, alpha, pad0, pad1; };
layout(set = 0, binding = 3, std430) readonly buffer Surface { SurfaceVertex surfaceVertices[]; };

layout(location = 0) out vec3 sPos;         // world position, at rest
layout(location = 1) out vec3 sInfo;        // calm, body, alpha

void main()
{
    SurfaceVertex s = surfaceVertices[gl_VertexIndex];
    sPos = vec3(s.x, s.y, s.z);
    sInfo = vec3(s.calm, s.body, s.alpha);
    gl_Position = W.viewProj * vec4(sPos, 1.0);   // (the control stage's view test)
}
