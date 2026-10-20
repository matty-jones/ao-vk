#version 450
// RVK_Upscaler 2 (fsr3.cpp): the tone mapping's alpha - how much of each pixel effects and the rain's near streaks
// made - as red: FSR 3's reactive mask. (The FidelityFX SDK makes its own views of what it is given: not the swizzled
// view fsr2.cpp hands FSR 2.)
layout(set = 0, binding = 0) uniform sampler2D toned;
layout(location = 0) out vec4 outReactive;

void main()
{
    outReactive = vec4(texelFetch(toned, ivec2(gl_FragCoord.xy), 0).a);
}
