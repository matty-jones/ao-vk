#version 450
// A shop portal's opening (portal.cpp): where the doorway's rectangle is the surface seen - the scene's depth (a copy,
// binding 0) at or behind its plane - the depth is pushed to the far plane, so the interior drawn next shows there.
// Where anything is in front of it (the door, the recess's walls, a character) the depth stays.
// RVK_PORTAL_MARK (portal_mark.frag): the same pixels, after the room is drawn, marked in the local-light target as lit
// by their own lamps - not the street's sun, ambient occlusion or reflections (occlusion.glsl, the traced passes).
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;
#ifdef RVK_PORTAL_MARK
layout(location = 0) out vec4 outLocal;
#endif
layout(push_constant) uniform Params {
    vec4 inv[4];       // rows of the inverse view-projection (D3D: a row vector times the matrix)
    vec4 origin;       // the rectangle's bottom-left corner (world)
    vec4 across;       // its bottom edge (world)
    vec4 up;           // its left edge (world)
    vec4 frame;        // x, y: the target's size; z, w: the projection's m22, m32 (view depth from depth)
} p;

vec3 World(vec2 ndc, float z)
{
    vec4 h = ndc.x * p.inv[0] + ndc.y * p.inv[1] + z * p.inv[2] + p.inv[3];
    return h.xyz / h.w;
}

float ViewDepth(float d) { return p.frame.w / (d - p.frame.z); }

void main()
{
    vec2 ndc = vec2(gl_FragCoord.x / p.frame.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / p.frame.y * 2.0);
    vec3 a = World(ndc, 0.0), b = World(ndc, 1.0);
    vec3 n = cross(p.across.xyz, p.up.xyz);
    float denom = dot(b - a, n);
    if (abs(denom) < 1e-12) discard;
    float t = dot(p.origin.xyz - a, n) / denom;
    if (t <= 0.0) discard;
    vec3 hit = a + (b - a) * t, rel = hit - p.origin.xyz;
    float u = dot(rel, p.across.xyz) / dot(p.across.xyz, p.across.xyz);
    float v = dot(rel, p.up.xyz) / dot(p.up.xyz, p.up.xyz);
    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) discard;
    // The plane's view depth there (linear along the near-to-far segment, as the world position is), against the
    // scene's: something nearer than the plane (by more than 5 cm) keeps its depth.
    float stored = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float plane = mix(ViewDepth(0.0), ViewDepth(1.0), t);
    if (stored < 1.0 && ViewDepth(stored) < plane - 0.05) discard;
#ifdef RVK_PORTAL_MARK
    outLocal = vec4(1.0, 0.0, 0.0, 1.0);
#else
    gl_FragDepth = 1.0;
#endif
}
