#version 450
// RVK_Upscaler 1 (fsr2.cpp): the scene's motion vectors whole, for FSR 2 - which takes them as they are - where taa.frag
// fills them in itself: where no draw wrote one (the sky, effects), or it is nought, the camera's motion at the
// pixel's depth, as taa.frag's CameraMove finds it.
layout(set = 0, binding = 0) uniform sampler2D motion;     // screen motion since last frame, scene pixels (now - then)
layout(set = 0, binding = 1) uniform sampler2D depthTex;   // the scene's
layout(push_constant) uniform Push {
    mat4 reproject;     // this frame's clip -> last frame's clip (raw D3DMATRIX memory)
    vec4 size;          // the scene's width, height; object motion written; the camera's motion known
} P;
layout(location = 0) out vec4 outMotion;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 m = P.size.z > 0.5 ? texelFetch(motion, p, 0).xy : vec2(0.0);
    if (m == vec2(0.0) && P.size.w > 0.5) {
        float d = min(texelFetch(depthTex, p, 0).r, 1.0);
        vec2 ndc = vec2(gl_FragCoord.x / P.size.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / P.size.y * 2.0);
        vec4 prev = P.reproject * vec4(ndc, d, 1.0);
        if (prev.w > 1e-6) {
            vec2 prevNdc = prev.xy / prev.w;
            m = gl_FragCoord.xy - vec2((prevNdc.x + 1.0) * 0.5 * P.size.x, (1.0 - prevNdc.y) * 0.5 * P.size.y);
        }
    }
    outMotion = vec4(m, 0.0, 1.0);
}
