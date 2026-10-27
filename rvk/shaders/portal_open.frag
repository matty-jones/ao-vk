#version 450
// A shop portal's opening (portal.cpp): where the doorway's rectangle is the surface seen - the scene's depth (a copy,
// binding 0) at or behind its plane - the depth is pushed to the far plane, so the interior drawn next shows there.
// Where anything is in front of it (the door, the recess's walls, a character) the depth stays.
// Its mode (origin.w): 0 - that alone (the street seen from a shop); 1 - and the rest of the box shut (the depth at the
// near plane), so the room drawn next shows in the opening only - a room is wider and deeper than the building it is set
// in, and its walls drawn in front of the street round the building showed (in the box, or over the whole screen with
// the camera in the doorway); 2 - the rest of the box given its depth back (the copy), the room drawn.
// RVK_PORTAL_MARK (portal_mark.frag): the same pixels, after the room is drawn, marked in the local-light target as lit
// by their own lamps - not the street's sun, ambient occlusion or reflections (occlusion.glsl, the traced passes).
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;
#ifdef RVK_PORTAL_MARK
layout(location = 0) out vec4 outLocal;
#endif
layout(push_constant) uniform Params {
    vec4 inv[4];       // rows of the inverse view-projection (D3D: a row vector times the matrix)
    vec4 origin;       // the rectangle's bottom-left corner (world); w: the mode
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

// Whether the pixel looks through the doorway's rectangle with nothing in front of it.
bool Opening(float stored)
{
    vec2 ndc = vec2(gl_FragCoord.x / p.frame.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / p.frame.y * 2.0);
    vec3 a = World(ndc, 0.0), b = World(ndc, 1.0);
    vec3 n = cross(p.across.xyz, p.up.xyz);
    float denom = dot(b - a, n);
    if (abs(denom) < 1e-12) return false;
    float t = dot(p.origin.xyz - a, n) / denom;
    if (t <= 0.0) return false;
    vec3 hit = a + (b - a) * t, rel = hit - p.origin.xyz;
    float u = dot(rel, p.across.xyz) / dot(p.across.xyz, p.across.xyz);
    float v = dot(rel, p.up.xyz) / dot(p.up.xyz, p.up.xyz);
    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) return false;
    // The plane's view depth there (linear along the near-to-far segment, as the world position is), against the
    // scene's: something nearer than the plane (by more than 5 cm) keeps its depth.
    float plane = mix(ViewDepth(0.0), ViewDepth(1.0), t);
    return !(stored < 1.0 && ViewDepth(stored) < plane - 0.05);
}

void main()
{
    float stored = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    bool open = Opening(stored);
#ifdef RVK_PORTAL_MARK
    if (!open) discard;
    outLocal = vec4(1.0, 0.0, 0.0, 1.0);
#else
    int mode = int(p.origin.w + 0.5);
    if (mode == 2) {
        if (open) discard;
        gl_FragDepth = stored;
    } else if (open) {
        gl_FragDepth = 1.0;
    } else if (mode == 1) {
        gl_FragDepth = 0.0;
    } else {
        discard;
    }
#endif
}
