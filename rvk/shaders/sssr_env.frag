#version 450
// RVK_SsrMode 1 (sssr.cpp): the environment AMD's reflections fall back to (a ray leaving the screen or finding the
// sky, a surface too rough to trace) - the probe's atlas (env_common.glsl: five levels side by side) with what it hasn't
// seen filled in: the sky as the frame shows it where that direction is on screen and sky there (as ours, ssr.frag,
// takes the sky from the screen), else as the scene shader guesses it (ffp_main.glsl WetReflection: the fog's colour
// at the horizon, some sunlight, the cloud sky); the ambient light below. Without a probe (PBR off), all filled in.
#include "env_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D atlas;          // the probe's (unread without one)
layout(set = 0, binding = 1) uniform sampler2D scene;          // the HDR scene
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(push_constant) uniform Push {
    vec4 ambient;       // rgb: the ambient light, w: 1 = the probe's atlas is there
    vec4 sky;           // rgb: the sky's light guessed (ambient + fog + sun)
    vec4 cloudSky;      // rgb: the clouds' sky colour (at least this bright above)
    mat4 viewProj;      // the world camera's view x projection (raw D3DMATRIX memory: a direction's place on screen)
} P;
layout(location = 0) out vec4 outEnv;

void main()
{
    vec2 t = floor(gl_FragCoord.xy);
    int level = t.x < 128.0 ? 0 : t.y < 64.0 ? 1 : t.x < 160.0 ? 2 : t.x < 176.0 ? 3 : 4;
    vec3 tile = EnvTile(level);
    vec3 d = EnvOctDecode(clamp((gl_FragCoord.xy - tile.xy) / tile.z, vec2(0.0), vec2(1.0)));
    vec3 above = max(P.sky.rgb * 1.3, P.cloudSky.rgb);
    // That way on screen, where the frame shows the sky (depth at the clear value).
    vec4 clip = P.viewProj * vec4(d, 0.0);
    if (clip.w > 1e-4) {
        vec2 ndc = clip.xy / clip.w;
        if (all(lessThan(abs(ndc), vec2(1.0)))) {
            ivec2 size = textureSize(scene, 0);
            ivec2 p = clamp(ivec2((ndc * vec2(0.5, -0.5) + 0.5) * vec2(size)), ivec2(0), size - 1);
            if (texelFetch(depthTex, p, 0).r >= 1.0)
                above = texelFetch(scene, p, 0).rgb;
        }
    }
    vec3 env = mix(P.ambient.rgb * 0.5, above, smoothstep(-0.4, 0.1, d.y));
    if (P.ambient.w > 0.5) {
        vec4 a = texelFetch(atlas, ivec2(t), 0);
        env = mix(env, a.rgb, clamp(a.a, 0.0, 1.0));
    }
    outEnv = vec4(env, 1.0);
}
