#version 450
// RVK_AoMode 1 (cacao.cpp): AMD's ambient occlusion (FidelityFX CACAO; its output's red, 1 = open) as the tone mapping
// takes ours (ao_blur.frag): occlusion and view depth - here at the scene's full resolution (occlusion.glsl: bit 32).
layout(set = 0, binding = 0) uniform sampler2D cacao;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection: m[2][2], m[3][2]
} P;
layout(location = 0) out vec4 outAo;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, p, 0).r;
    outAo = vec4(clamp(texelFetch(cacao, p, 0).r, 0.0, 1.0), P.proj.y / (min(d, 0.999999) - P.proj.x), 0.0, 1.0);
}
