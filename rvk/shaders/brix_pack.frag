#version 450
// RVK_GiMode 1 (brixelizer.cpp): AMD's indirect light (FidelityFX Brixelizer GI's diffuse output: the light reaching
// each surface) as the tone mapping takes ours (gi_blur.frag): light and view depth - here at the scene's full
// resolution (occlusion.glsl: bit 64).
layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection: m[2][2], m[3][2]
} P;
layout(location = 0) out vec4 outGi;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, p, 0).r;
    vec3 c = texelFetch(diffuse, p, 0).rgb;
    c = any(isnan(c)) || any(isinf(c)) ? vec3(0.0) : max(c, vec3(0.0));
    outGi = vec4(c, P.proj.y / (min(d, 0.999999) - P.proj.x));
}
