#version 450
// Phong tessellation of characters and curved static meshes (D.tess): each triangle edge that bends split at the
// draw's level, the vertex shader's outputs passed on per corner.
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

// A corner's pull on an offset v from it (ffp.tese): its share of v off what the corner holds the surface to.
vec3 Pull(vec3 s, vec3 v)
{
    float w = length(s);
    if (w <= 0.0) return vec3(0.0);
    vec3 d = s / w;
    return w > 1.5 ? v - d * dot(v, d)          // a crease: off its line
                   : w * d * dot(v, d);         // a smooth corner: off its tangent plane (weighted)
}

void main()
{
    gRecord = vRecord[0];                        // every vertex of a draw shares it
    tRecord = vRecord[0];
    FFP_VARYINGS(COPY)
    if (gl_InvocationID == 0) {
        // The draw's level on an edge that bends, 1 on one that stays straight - the Phong shape moves a point of an
        // edge only towards its two corners' tangent planes, so an edge lying in the plane of each smooth corner
        // (a flat wall's, a hard edge's) doesn't move: no use splitting it. Decided by the edge's two corners alone,
        // so the triangles on either side split it alike (no cracks). Outer level k: the edge facing corner k.
        // The edge a -> b moves by t (1 - t) (P_b - P_a)(b - a) (ffp.tese Pull): it bends unless that is ~0.
        float level = max(D.tess.x, 1.0), inner = 1.0;
        for (int k = 0; k < 3; ++k) {
            int a = (k + 1) % 3, b = (k + 2) % 3;
            vec3 e = vPosW[b] - vPosW[a];
            bool bends = length(Pull(vSmoothN[b], e) - Pull(vSmoothN[a], e)) > 0.01 * length(e);
            gl_TessLevelOuter[k] = bends ? level : 1.0;
            inner = max(inner, gl_TessLevelOuter[k]);
        }
        gl_TessLevelInner[0] = inner;
    }
}
