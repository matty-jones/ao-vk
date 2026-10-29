#version 450
// RVK_GiMode 1 (brixelizer.cpp): AMD's indirect light (FidelityFX Brixelizer GI's diffuse output: the light reaching
// each surface) as the tone mapping takes ours (gi_blur.frag): light and view depth - here at the scene's full
// resolution (occlusion.glsl: bit 64). On a character (ffp_main.glsl marks its normals, w 10 and up) averaged with
// last frame's where it was then (its own motion): AMD's probes, one per 8 x 8 pixels at half size, are few across
// a character and rarely found again on its curves, so its light changed from frame to frame - it flickered. The
// world's surfaces settle in AMD's own history and are taken as they are.
layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 2) uniform sampler2D last;      // last frame's output
layout(set = 0, binding = 3) uniform sampler2D motion;    // the scene's: xy pixels, now - then; w its normal's kind
layout(push_constant) uniform Push {
    vec4 proj;          // D3D projection: m[2][2], m[3][2]; 1 = last is last frame's; this frame's share on characters
} P;
layout(location = 0) out vec4 outGi;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depthTex, p, 0).r;
    vec3 c = texelFetch(diffuse, p, 0).rgb;
    c = any(isnan(c)) || any(isinf(c)) ? vec3(0.0) : max(c, vec3(0.0));
    float z = P.proj.y / (min(d, 0.999999) - P.proj.x);
    vec4 m = texelFetch(motion, p, 0);
    if (P.proj.z > 0.5 && m.w > 9.5) {
        ivec2 then = ivec2(floor(vec2(p) + 0.5 - m.xy));
        if (all(greaterThanEqual(then, ivec2(0))) && all(lessThan(then, textureSize(last, 0)))) {
            vec4 h = texelFetch(last, then, 0);
            // (The same surface then: its view depth near this one's - not the ground behind its edge.)
            if (abs(h.a - z) < 0.05 * z + 0.05)
                c = mix(h.rgb, c, P.proj.w);
        }
    }
    outGi = vec4(c, z);
}
