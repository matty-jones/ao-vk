// The ray tracing's shaders (raytrace.cpp): what a ray's hit needs beyond the TLAS - per TLAS instance (by its index)
// its geometry, so a hit on a cut-out (a fence's wire, a grille, a still plant; its instance not opaque) is kept only
// where its texture's alpha passes the draw's alpha test. The includer defines RT_GEO_BINDING (set 0).
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_nonuniform_qualifier : require

// (raytrace.cpp GeoRecord.) Addresses as two words (no 64-bit integers needed); texture: bindless image + 1 | sampler
// << 20, 0 = solid.
struct RtGeo {
    uvec2 vertices, indices;
    uint stride, uvOffset, texture;
    float alphaRef;
};
layout(set = 0, binding = RT_GEO_BINDING, std430) readonly buffer RtGeos { RtGeo rtGeo[]; };
layout(set = 1, binding = 0) uniform texture2D texImages[65536];
layout(set = 1, binding = 1) uniform sampler bindlessSamplers[256];
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RtWord { uint w; };

uvec2 RtAddress(uvec2 a, uint offset)
{
    uint carry;
    uint low = uaddCarry(a.x, offset, carry);
    return uvec2(low, a.y + carry);
}

uint RtLoad(uvec2 a) { return RtWord(a).w; }   // (a multiple of 4)

uint RtIndex(uvec2 indices, uint i)               // a 16-bit index (2-aligned: its word, and which half)
{
    uvec2 at = RtAddress(indices, i * 2u);
    uint shift = (at.x & 2u) * 8u;
    at.x &= ~3u;
    return (RtLoad(at) >> shift) & 0xFFFFu;
}

vec2 RtUv(RtGeo g, uint vertex)
{
    uvec2 at = RtAddress(g.vertices, vertex * g.stride + g.uvOffset);
    return vec2(uintBitsToFloat(RtLoad(at)), uintBitsToFloat(RtLoad(RtAddress(at, 4u))));
}

// The candidate hit on a cut-out: whether its texture's alpha there passes (its finest level: the rays see the leaf
// as drawn up close).
bool RtCutoutHit(rayQueryEXT q)
{
    RtGeo g = rtGeo[rayQueryGetIntersectionInstanceIdEXT(q, false)];
    if (g.texture == 0u)
        return true;
    uint tri = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)) * 3u;
    vec2 b = rayQueryGetIntersectionBarycentricsEXT(q, false);
    vec2 uv = RtUv(g, RtIndex(g.indices, tri)) * (1.0 - b.x - b.y) + RtUv(g, RtIndex(g.indices, tri + 1u)) * b.x +
              RtUv(g, RtIndex(g.indices, tri + 2u)) * b.y;
    uint img = (g.texture & 0xFFFFFu) - 1u, smp = (g.texture >> 20) & 0xFFu;
    float a = textureLod(sampler2D(texImages[nonuniformEXT(img)], bindlessSamplers[nonuniformEXT(smp)]), uv, 0.0).a;
    return a > g.alphaRef;
}

// A ray query run to its end, cut-outs tested on the way.
void RtProceed(rayQueryEXT q)
{
    while (rayQueryProceedEXT(q))
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && RtCutoutHit(q))
            rayQueryConfirmIntersectionEXT(q);
}
