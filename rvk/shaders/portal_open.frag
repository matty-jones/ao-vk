#version 450
// A shop portal's opening (portal.cpp): where the doorway's surface is the surface seen - the scene's depth (a copy,
// binding 0) at or behind it - the depth is pushed to the far plane, so the interior drawn next shows there. Where
// anything is in front of it (the door, the recess's walls, a character) the depth stays.
// Its mode (origin.w): 0 - the doorway's rectangle (the street seen from a shop); 3 - one of the doorway's black
// triangles (origin, across and up its corners), drawn once each: a doorway's black is not always one flat rectangle (a
// pointed recess of four facets in Newland's midtech shop, wider than the walls in front of it - a rectangle through it
// opened the walls beside it too); 1 - the whole box shut first (the depth at the near plane), so the room drawn next
// shows in the opening only - a room is wider and deeper than the building it is set in, and its walls drawn in front
// of the street round the building showed (in the box, or over the whole screen with the camera in the doorway); 2 -
// the box given its depth back (the copy) where the room drawn left none nearer (drawn with GREATER: the room keeps its
// own).
// RVK_PORTAL_MARK (portal_mark.frag): a triangle's pixels, after the room is drawn, marked in the local-light target as
// lit by their own lamps - not the street's sun, ambient occlusion or reflections (occlusion.glsl, the traced passes).
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;
#ifdef RVK_PORTAL_MARK
layout(location = 0) out vec4 outLocal;
#endif
layout(push_constant) uniform Params {
    vec4 inv[4];       // rows of the inverse view-projection (D3D: a row vector times the matrix)
    vec4 origin;       // the rectangle's bottom-left corner (world), or a triangle's first; w: the mode
    vec4 across;       // the rectangle's bottom edge (world), or the triangle's second corner
    vec4 up;           // the rectangle's left edge (world), or the triangle's third corner
    vec4 frame;        // x, y: the target's size; z, w: the projection's m22, m32 (view depth from depth)
} p;

vec3 World(vec2 ndc, float z)
{
    vec4 h = ndc.x * p.inv[0] + ndc.y * p.inv[1] + z * p.inv[2] + p.inv[3];
    return h.xyz / h.w;
}

float ViewDepth(float d) { return p.frame.w / (d - p.frame.z); }

// Whether the pixel looks through the doorway's rectangle, or the triangle, with nothing in front of it.
bool Opening(float stored, bool triangle)
{
    vec2 ndc = vec2(gl_FragCoord.x / p.frame.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / p.frame.y * 2.0);
    vec3 a = World(ndc, 0.0), b = World(ndc, 1.0);
    vec3 o = p.origin.xyz, e1 = p.across.xyz, e2 = p.up.xyz;
    if (triangle) { e1 -= o; e2 -= o; }
    vec3 n = cross(e1, e2);
    float denom = dot(b - a, n);
    if (abs(denom) < 1e-12) return false;
    float t = dot(o - a, n) / denom;
    if (t <= 0.0) return false;
    // Where along its two edges (solved: a triangle's aren't square to each other); a hair over, so no crack is left
    // between two triangles.
    vec3 rel = a + (b - a) * t - o;
    float d11 = dot(e1, e1), d12 = dot(e1, e2), d22 = dot(e2, e2), r1 = dot(rel, e1), r2 = dot(rel, e2);
    float det = d11 * d22 - d12 * d12;
    if (abs(det) < 1e-12) return false;
    float u = (r1 * d22 - r2 * d12) / det, v = (r2 * d11 - r1 * d12) / det;
    const float hair = 1e-3;
    if (u < -hair || v < -hair || (triangle ? u + v > 1.0 + hair : u > 1.0 || v > 1.0)) return false;
    // The surface's view depth there (linear along the near-to-far segment, as the world position is), against the
    // scene's: something nearer than it (by more than 5 cm) keeps its depth.
    float plane = mix(ViewDepth(0.0), ViewDepth(1.0), t);
    return !(stored < 1.0 && ViewDepth(stored) < plane - 0.05);
}

void main()
{
    float stored = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
#ifdef RVK_PORTAL_MARK
    if (!Opening(stored, true)) discard;
    outLocal = vec4(1.0, 0.0, 0.0, 1.0);
#else
    int mode = int(p.origin.w + 0.5);
    if (mode == 1) {
        gl_FragDepth = 0.0;
    } else if (mode == 2) {
        gl_FragDepth = stored;
    } else if (Opening(stored, mode == 3)) {
        gl_FragDepth = 1.0;
    } else {
        discard;
    }
#endif
}
