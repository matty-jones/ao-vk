#version 450
// Phong tessellation (D.tess) of characters - every triangle split evenly at the draw's level - and of the native
// ground (D.terrain.y set) - each edge by its own length and distance from the eye, so a cell's edge is split alike
// by both triangles beside it, in this patch and the next (no cracks), and triangles out of view not at all. The
// vertex shader's outputs passed on per corner.
#include "constants.glsl"
#include "ffp_varyings.glsl"

layout(vertices = 3) out;

#define IN(type, name, loc) layout(location = loc) in type name[];
#define OUT(type, name, loc) layout(location = loc) out type t_##name[];
#define COPY(type, name, loc) t_##name[gl_InvocationID] = name[gl_InvocationID];
FFP_VARYINGS(IN)
FFP_VARYINGS(OUT)
// The draw's record index: the vertex shader's per-vertex flat output, handed to the eval stage as one patch value.
layout(location = 15) flat in uint vRecord[];
layout(location = 15) patch out uint tRecord;

// The native ground: an edge's segments - about a third of a unit up close, longer further out, none (1) by the fade
// distance (D.tess.z), none where both ends are gentle (ffp.tese leaves ground one can stand on flat: na, nb their
// normals). From its ends alone: the same for both triangles that share it.
float GroundEdgeLevel(vec3 a, vec3 b, vec3 na, vec3 nb)
{
    if (dot(na, na) > 0.0 && dot(nb, nb) > 0.0 && abs(normalize(na).y) > 0.8 && abs(normalize(nb).y) > 0.8) return 1.0;
    const float d = distance((a + b) * 0.5, C.eyePos.xyz);
    const float near = 1.0 - clamp(d / max(D.tess.z, 1.0), 0.0, 1.0);
    const float target = 0.35 + 0.02 * d;
    return clamp(1.0 + (distance(a, b) / target - 1.0) * near, 1.0, D.tess.x);
}

// All three corners beyond one side of the view, with a margin for how far the shape moves them: nothing to draw.
bool GroundOutOfView()
{
    const float m = 2.0;
    const vec4 a = gl_in[0].gl_Position, b = gl_in[1].gl_Position, c = gl_in[2].gl_Position;
    return (a.x > a.w + m && b.x > b.w + m && c.x > c.w + m) || (a.x < -a.w - m && b.x < -b.w - m && c.x < -c.w - m) ||
           (a.y > a.w + m && b.y > b.w + m && c.y > c.w + m) || (a.y < -a.w - m && b.y < -b.w - m && c.y < -c.w - m) ||
           (a.w < -m && b.w < -m && c.w < -m);
}

void main()
{
    gRecord = vRecord[0];                        // every vertex of a draw shares it
    tRecord = vRecord[0];
    FFP_VARYINGS(COPY)
    if (gl_InvocationID == 0) {
        if (floatBitsToUint(D.terrain.y) != 0u) {
            if (GroundOutOfView()) {
                gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = gl_TessLevelInner[0] = 0.0;
                return;
            }
            // (Outer level i: the edge opposite corner i.)
            gl_TessLevelOuter[0] = GroundEdgeLevel(vPosW[1], vPosW[2], vSmoothN[1], vSmoothN[2]);
            gl_TessLevelOuter[1] = GroundEdgeLevel(vPosW[2], vPosW[0], vSmoothN[2], vSmoothN[0]);
            gl_TessLevelOuter[2] = GroundEdgeLevel(vPosW[0], vPosW[1], vSmoothN[0], vSmoothN[1]);
            gl_TessLevelInner[0] = max(max(gl_TessLevelOuter[0], gl_TessLevelOuter[1]), gl_TessLevelOuter[2]);
            return;
        }
        // One level for every edge of the draw: an edge shared by two triangles is split alike on both (no cracks).
        float level = max(D.tess.x, 1.0);
        gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = level;
        gl_TessLevelInner[0] = level;
    }
}
