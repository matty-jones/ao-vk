#version 450
// The scene's surfaces for AMD's effects (sssr.cpp; docs/amd-fx-plan.md): each pixel's world-space normal (rgb) and
// perceptual roughness (a), at the scene's size. The normal: the one the scene wrote into the motion vectors' zw
// (ffp_main.glsl: octahedral, view space; w 2 .. 4 a PBR surface's shading normal, 6 .. 8 any other's), else the
// depth buffer's (the triangles' own, as ssr.frag takes them). The roughness: the scene keeps none of its own, only
// how much each surface reflects (attachment 2's G: water, glossy and wet surfaces, smooth PBR materials) - the more
// it reflects, the smoother it is taken to be; one that doesn't reflect is rough (1: no reflection rays).
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D motion;          // zw: the scene's normals
layout(set = 0, binding = 2) uniform sampler2D reflectivity;    // G
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection: m[2][2], m[3][2], m[0][0], m[1][1]
    vec4 size;          // width, height; the roughness of a surface reflecting a little, and of a mirror
    vec4 view[3];       // the view matrix's rotation, rows (D3D: world -> view): view -> world is by its transpose
} P;
layout(location = 0) out vec4 outSurface;

float ViewZ(float d) { return P.proj.y / (min(d, 0.999999) - P.proj.x); }

vec3 ViewPos(vec2 pix)
{
    float z = ViewZ(texelFetch(depthTex, ivec2(clamp(pix, vec2(0.0), P.size.xy - 1.0)), 0).r);
    vec2 ndc = vec2((pix.x + 0.5) / P.size.x * 2.0 - 1.0, 1.0 - (pix.y + 0.5) / P.size.y * 2.0);
    return vec3(ndc.x * z / P.proj.z, ndc.y * z / P.proj.w, z);
}

vec3 OctDecode(vec2 o)
{
    vec3 n = vec3(o, 1.0 - abs(o.x) - abs(o.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

void main()
{
    ivec2 ipix = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, ipix, 0).r;
    if (d >= 1.0) { outSurface = vec4(0.0, 1.0, 0.0, 1.0); return; }   // the sky: up, rough (nothing reflects there)
    vec2 o = texelFetch(motion, ipix, 0).zw;
    vec3 p = ViewPos(vec2(ipix));
    vec3 n;
    if (o.y > 5.5) {
        n = OctDecode(vec2(o.x, o.y - 7.0));
    } else if (o.y > 1.5) {
        n = OctDecode(vec2(o.x, o.y - 3.0));
    } else {
        vec2 pix = vec2(ipix);
        vec3 dx1 = ViewPos(pix + vec2(1, 0)) - p, dx0 = p - ViewPos(pix - vec2(1, 0));
        vec3 dy1 = ViewPos(pix + vec2(0, 1)) - p, dy0 = p - ViewPos(pix - vec2(0, 1));
        n = normalize(cross(abs(dx1.z) < abs(dx0.z) ? dx1 : dx0, abs(dy1.z) < abs(dy0.z) ? dy1 : dy0));
    }
    if (dot(n, p) > 0.0) n = -n;                                   // facing the camera
    vec3 world = normalize(vec3(dot(P.view[0].xyz, n), dot(P.view[1].xyz, n), dot(P.view[2].xyz, n)));
    float r = texelFetch(reflectivity, ipix, 0).g;
    float rough = r < 0.004 ? 1.0 : mix(P.size.z, P.size.w, sqrt(clamp(r, 0.0, 1.0)));
    outSurface = vec4(world, rough);
}
