#version 450
// Footprints in the sand (rvk/sand_prints.cpp, RVK_SandPrints): the print map round the eye, a frame's step. Each
// texel is a world texel's - the map laid toroidally (texel t holds the world texel w with w = t, modulo its size), so
// it follows the eye without being copied: what the eye's move brought in is cleared, the rest kept from the last map,
// relaxed a little (prints soften over minutes), and the frame's new prints stamped in. R: how far a print's dent is
// pressed (0..1), G: its rim pushed up round it (0..1).
layout(push_constant) uniform Params {
    ivec4 origin;       // xy: this frame's corner (world texels), zw: the last map's
    vec4 p;             // x: what is kept of the last map this frame (the relaxing), y: new prints, z: how deep
                        // (RVK_SandPrints), w: 1 = start afresh
} P;
layout(binding = 0) uniform sampler2D prev;
layout(std430, binding = 1) readonly buffer Prints { vec4 at[]; } S;   // xy: where, from the corner (world units),
                                                                        // zw: its heading (unit, world x / z)

layout(location = 0) out vec4 outPrint;

const int kN = 2048;
const float kCell = 0.02;

void main()
{
    const ivec2 t = ivec2(gl_FragCoord.xy);
    const ivec2 w = P.origin.xy + ((t - P.origin.xy) & (kN - 1));   // the world texel this one holds now
    vec2 v = vec2(0.0);
    if (P.p.w < 0.5 && all(greaterThanEqual(w, P.origin.zw)) && all(lessThan(w, P.origin.zw + kN)))
        v = texelFetch(prev, t, 0).rg * P.p.x;
    // The prints: an oval a little longer than a foot (loose sand gives round it), narrower at the heel; its floor
    // pressed flat, its walls round the edge, the sand it pushed aside a low, soft rim round it. (A bowl sloping from the
    // middle lay all in its own shadow under a low sun: a dark hole, not a print.)
    const vec2 at = (vec2(w - P.origin.xy) + 0.5) * kCell;
    const int count = int(P.p.y);
    for (int i = 0; i < count; ++i) {
        const vec4 s = S.at[i];
        const vec2 d = at - s.xy;
        if (dot(d, d) > 0.16) continue;
        const float along = dot(d, s.zw), across = dot(d, vec2(-s.w, s.z));
        const float width = along < 0.0 ? 0.07 : 0.085;                    // (the heel behind, the ball of the foot ahead)
        const float r = length(vec2(along / 0.16, across / width));
        const float dent = (1.0 - smoothstep(0.55, 1.0, r)) * P.p.z;
        const float rim = smoothstep(0.8, 1.15, r) * (1.0 - smoothstep(1.15, 1.9, r)) * P.p.z;
        v = vec2(max(v.x, dent), max(v.y, rim));
    }
    outPrint = vec4(v, 0.0, 1.0);
}
