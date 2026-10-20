#version 450
// RVK_RenderScale (hdr.cpp UpsampleDepth): the scene's depth, drawn at a fraction of the output's size, brought up to
// it - each output pixel the scene pixel at its centre - for what tests depth after the scene (name labels, the
// interface) and the motion blur.
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;
layout(push_constant) uniform Push {
    vec4 scale;         // scene pixels per output pixel x, y; the scene's width, height
} P;

void main()
{
    // (Output pixel i's centre is scene position i * scale + 0.5: the scene keeps D3D's half-pixel shift in its own
    // pixels - taa.frag.)
    ivec2 p = clamp(ivec2(floor((gl_FragCoord.xy - 0.5) * P.scale.xy + 0.5)), ivec2(0), ivec2(P.scale.zw) - 1);
    gl_FragDepth = texelFetch(sceneDepth, p, 0).r;
}
