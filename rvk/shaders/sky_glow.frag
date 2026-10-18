#version 450
// The sky's twilight and the glow round the sun (rvk sky.cpp DrawSkyGlow; sky_common.glsl): drawn over the game's sky
// once it is down, before the suns, the game's cloud sheets and the cloud layer - which cover it as they are thick.
// The twilight palette replaces the game's sky by its share (alpha: blended ONE, ONE_MINUS_SRC_ALPHA), the glow adds.
#include "sky_common.glsl"

layout(push_constant) uniform Params {
    mat4 invViewProj;   // clip -> world, the sky's camera (raw D3DMATRIX memory, as sun.frag)
    vec4 view;          // xy: the target's size
    vec4 sky0, sky1, sky2, sky3;   // the sky model (sky_common.glsl)
} P;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

void main()
{
    outGlow = outLocal = outMotion = outAlbedo = vec4(0.0);
    vec2 ndc = vec2(gl_FragCoord.x / P.view.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / P.view.y * 2.0);
    vec4 a = P.invViewProj * vec4(ndc, 0.0, 1.0), b = P.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    float t = clamp(min(P.sky0.w, 1.0) * SkyTwilightAmount(P.sky1.y, P.sky3.w), 0.0, 1.0);
    vec3 c = t > 0.0 ? SkyTwilight(dir, P.sky0, P.sky1, P.sky3) * t : vec3(0.0);
    outScene = vec4(c + SkyGlow(dir, P.sky1, P.sky2, P.sky3), t);
}
