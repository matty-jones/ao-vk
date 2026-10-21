#version 450
// The water's surface (water_surface.vert): each edge split by its own length and distance from the eye - the same for
// both triangles that share it, so the waves open no cracks (the preprocessing left no T-junctions: rvk/water_surface.cpp)
// - and patches out of view not at all, as the native ground does (ffp.tesc).
#include "water_common.glsl"

layout(vertices = 3) out;

layout(location = 0) in vec3 sPos[];
layout(location = 1) in vec3 sInfo[];
layout(location = 0) out vec3 tPos[];
layout(location = 1) out vec3 tInfo[];
layout(location = 2) patch out float tSpacing;   // the patch's longest segment (the waves fade out where it is coarse)

// An edge's segments: W.surface.y long up close, W.surface.z longer per unit of distance, at most W.surface.w.
float EdgeLevel(vec3 a, vec3 b)
{
    const float d = distance((a + b) * 0.5, W.camera.xyz);
    const float target = W.surface.y + W.surface.z * d;
    return clamp(distance(a, b) / target, 1.0, W.surface.w);
}

// All three corners beyond one side of the view, with a margin for the tallest waves: nothing to draw.
bool OutOfView()
{
    const float m = 3.0;
    const vec4 a = gl_in[0].gl_Position, b = gl_in[1].gl_Position, c = gl_in[2].gl_Position;
    return (a.x > a.w + m && b.x > b.w + m && c.x > c.w + m) || (a.x < -a.w - m && b.x < -b.w - m && c.x < -c.w - m) ||
           (a.y > a.w + m && b.y > b.w + m && c.y > c.w + m) || (a.y < -a.w - m && b.y < -b.w - m && c.y < -c.w - m) ||
           (a.w < -m && b.w < -m && c.w < -m);
}

void main()
{
    tPos[gl_InvocationID] = sPos[gl_InvocationID];
    tInfo[gl_InvocationID] = sInfo[gl_InvocationID];
    if (gl_InvocationID != 0)
        return;
    if (OutOfView()) {
        gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = gl_TessLevelInner[0] = 0.0;
        tSpacing = 0.0;
        return;
    }
    // (Outer level i: the edge opposite corner i.)
    const float l0 = EdgeLevel(sPos[1], sPos[2]), l1 = EdgeLevel(sPos[2], sPos[0]), l2 = EdgeLevel(sPos[0], sPos[1]);
    gl_TessLevelOuter[0] = l0;
    gl_TessLevelOuter[1] = l1;
    gl_TessLevelOuter[2] = l2;
    gl_TessLevelInner[0] = max(max(l0, l1), l2);
    tSpacing = max(max(distance(sPos[1], sPos[2]) / l0, distance(sPos[2], sPos[0]) / l1), distance(sPos[0], sPos[1]) / l2);
}
