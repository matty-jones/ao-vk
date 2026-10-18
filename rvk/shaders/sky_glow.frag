#version 450
// The sky pass (rvk sky.cpp DrawSkyGlow). With RVK_ProcSky (docs/dusk-plan.md §8): our own sky, in place of the
// game's dome (its draw skipped) - the sky view table (atmos_view.frag) for each pixel's direction, written opaque at
// the far plane; the game's stars, moons, suns and cloud sheets drawn over it as before. Without: the twilight and
// the glow round the sun (sky_common.glsl) over the game's dome, before its moons, suns and sheets - the twilight
// palette replacing the dome by its share (alpha: blended ONE, ONE_MINUS_SRC_ALPHA), the glow added.
#include "sky_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D skyView;   // (atmos_view.frag; a black texture without it)
layout(push_constant) uniform Params {
    mat4 invViewProj;   // clip -> world, the sky's camera (raw D3DMATRIX memory, as sun.frag)
    vec4 view;          // xy: the target's size; z: 1 our own sky (the table), 0 the twilight over the game's
    vec4 sky0, sky1, sky2, sky3;   // the sky model (sky_common.glsl)
} P;

layout(location = 0) out vec4 outScene;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outLocal;
layout(location = 3) out vec4 outMotion;
layout(location = 4) out vec4 outAlbedo;

// The sky view table's coordinates for a direction (atmos_view.frag ViewDir's inverse).
vec2 SkyViewUv(vec3 d)
{
    float az = atan(d.z, d.x);
    float el = asin(clamp(d.y, -1.0, 1.0));
    float v = sign(el) * sqrt(abs(el) / (0.5 * 3.14159265));
    return vec2(az / (2.0 * 3.14159265) + 0.5, v * 0.5 + 0.5);
}

void main()
{
    outGlow = outLocal = outMotion = outAlbedo = vec4(0.0);
    vec2 ndc = vec2(gl_FragCoord.x / P.view.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / P.view.y * 2.0);
    vec4 a = P.invViewProj * vec4(ndc, 0.0, 1.0), b = P.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    if (P.view.z > 0.5) {
        outScene = vec4(textureLod(skyView, SkyViewUv(dir), 0.0).rgb, 1.0);
        return;
    }
    float t = clamp(min(P.sky0.w, 1.0) * SkyTwilightAmount(P.sky1.y, P.sky3.w), 0.0, 1.0);
    vec3 c = t > 0.0 ? SkyTwilight(dir, P.sky0, P.sky1, P.sky3) * t : vec3(0.0);
    outScene = vec4(c + SkyGlow(dir, P.sky1, P.sky2, P.sky3), t);
}
