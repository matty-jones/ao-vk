#version 450
// The sky pass (rvk sky.cpp DrawSkyGlow). With RVK_ProcSky (docs/dusk-plan.md §8): our own sky, in place of the
// game's dome (its draw skipped) - the sky view table (atmos_view.frag) for each pixel's direction, written opaque at
// the far plane, our night sky over it (docs/sky-plan.md §2-4: stars, the galaxy, shooting stars, auroras); the
// game's moons, suns and cloud sheets drawn over it as before. Without: the twilight and
// the glow round the sun (sky_common.glsl) over the game's dome, before its moons, suns and sheets - the twilight
// palette replacing the dome by its share (alpha: blended ONE, ONE_MINUS_SRC_ALPHA), the glow added.
#include "sky_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D skyView;   // (atmos_view.frag; a black texture without it)
layout(push_constant) uniform Params {
    mat4 invViewProj;   // clip -> world, the sky's camera (raw D3DMATRIX memory, as sun.frag)
    vec4 view;          // xy: the target's size; z: 1 our own sky (the table), 0 the twilight over the game's
    vec4 sky0, sky1, sky2, sky3;   // the sky model (sky_common.glsl) - or, under our own sky, the shooting star's look:
                                   // sky0 how fast its train fades (rad of the head's travel), width (px), brightness,
                                   // a seed; sky1 its colour
    // Our night sky (docs/sky-plan.md §2-4; sky.cpp UpdateNightSky):
    vec4 star0, star1, star2;      // xyz: the star frame's axes in the world (it turns with the sun round a tilted
                                   // pole); w: how much of the stars shows, their brightness, their twinkle
    vec4 night;                    // x the galaxy's band; y the clock (s); z (unused); w the aurora
    vec4 meteor0, meteor1;         // a shooting star: its start (xyz), how far its head is along its path (w, rad);
                                   // the axis its path turns about (xyz), where it burns out (w, rad; < 0 none)
    vec4 auroraColour;             // rgb: the aurora's green (its glow low down)
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

// ---- Our night sky ----
const float kPi = 3.14159265;

uvec3 Pcg3(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 Rand3(uvec3 v) { return vec3(Pcg3(v)) * (1.0 / 4294967295.0); }

// Value noise (2D, 3D) and a few octaves of it.
float Noise2(vec2 p)
{
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    uvec2 c = uvec2(ivec2(i) + 32768);
    float a = Rand3(uvec3(c, 5u)).x, b = Rand3(uvec3(c + uvec2(1, 0), 5u)).x;
    float d = Rand3(uvec3(c + uvec2(0, 1), 5u)).x, e = Rand3(uvec3(c + uvec2(1, 1), 5u)).x;
    return mix(mix(a, b, u.x), mix(d, e, u.x), u.y);
}
float Noise3(vec3 p)
{
    vec3 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    uvec3 c = uvec3(ivec3(i) + 32768);
    float n000 = Rand3(c).x, n100 = Rand3(c + uvec3(1, 0, 0)).x, n010 = Rand3(c + uvec3(0, 1, 0)).x;
    float n110 = Rand3(c + uvec3(1, 1, 0)).x, n001 = Rand3(c + uvec3(0, 0, 1)).x, n101 = Rand3(c + uvec3(1, 0, 1)).x;
    float n011 = Rand3(c + uvec3(0, 1, 1)).x, n111 = Rand3(c + uvec3(1, 1, 1)).x;
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y), mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y),
               u.z);
}
float Fbm3(vec3 p) { return 0.5 * Noise3(p) + 0.3 * Noise3(p * 2.03) + 0.2 * Noise3(p * 4.11); }
float Fbm2(vec2 p) { return 0.6 * Noise2(p) + 0.4 * Noise2(p * 2.07); }

// The stars on a cube round the sky: 128 x 128 cells a face, each with a star by chance - less towards a face's
// corners, where its cells take less of the sky, so they lie evenly. About 9000 down to magnitude 6.5 (Earth's
// naked-eye sky), as many faint as real skies have: N(< m) ~ 10^(0.6 m).
const float kCells = 128.0;
const float kStarChance = 0.175;

vec3 FaceDir(uint face, vec2 uv)
{
    vec3 d = face < 2u ? vec3(face == 0u ? 1.0 : -1.0, uv) : face < 4u ? vec3(uv.x, face == 2u ? 1.0 : -1.0, uv.y)
                                                                        : vec3(uv, face == 4u ? 1.0 : -1.0);
    return normalize(d);
}

// A star's colour by its temperature (t 0 red, ~3000 K .. 1 blue-white, ~12000 K), the faint ones paler (the eye
// sees no colour in them).
vec3 StarColour(float t, float faint)
{
    vec3 c = t < 0.5 ? mix(vec3(1.0, 0.62, 0.38), vec3(1.0, 0.96, 0.9), t * 2.0)
                     : mix(vec3(1.0, 0.96, 0.9), vec3(0.72, 0.82, 1.0), t * 2.0 - 1.0);
    return mix(c, vec3(0.92, 0.94, 1.0), faint);
}

// The stars' light in direction s (the star frame), a pixel pa (rad) across: each a Gaussian of 0.9 pixel - its light
// the same at any resolution; narrower, read at the pixels' centres under TAA's jitter and the sky's slow turn, it
// flickered as if twinkling (user, 2026-10-19). The two cells nearest each way looked at.
vec3 Stars(vec3 s, float pa, float t, float twinkle)
{
    vec3 a = abs(s);
    uint face;
    vec2 uv;
    if (a.x >= a.y && a.x >= a.z) { face = s.x > 0.0 ? 0u : 1u; uv = s.yz / a.x; }
    else if (a.y >= a.z) { face = s.y > 0.0 ? 2u : 3u; uv = s.xz / a.y; }
    else { face = s.z > 0.0 ? 4u : 5u; uv = s.xy / a.z; }
    vec2 g = (uv * 0.5 + 0.5) * kCells, base = floor(g - 0.5);
    const float sigma = 0.9, A = 7943.28, B = 0.12589;   // (10^(0.6 x 6.5), 10^(0.6 x -1.5))
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            vec2 c = base + vec2(i, j);
            if (any(lessThan(c, vec2(0.0))) || any(greaterThanEqual(c, vec2(kCells))))
                continue;
            uvec3 key = uvec3(uvec2(c), face * 977u + 13u);
            vec3 r = Rand3(key);
            vec2 sp = (c + r.yz) / kCells * 2.0 - 1.0;
            if (r.x > kStarChance * pow(1.0 + dot(sp, sp), -1.5))
                continue;
            vec3 r2 = Rand3(key ^ uvec3(0x9E3779B9u, 0x7F4A7C15u, 0x85EBCA6Bu));
            vec3 sd = FaceDir(face, sp);
            float px = length(cross(sd, s)) / pa;
            if (px > 4.5 || dot(sd, s) <= 0.0)
                continue;
            // (Its light as the eye takes it: magnitudes compressed - 10^(-0.25 m), not -0.4 -, so the faint ones
            // show over the night sky while the brightest still glare.)
            float mag = log(r2.x * (A - B) + B) / (0.6 * log(10.0));
            float flux = pow(10.0, -0.25 * mag);
            float tw = 1.0 + twinkle * sin(t * (9.0 + 7.0 * r2.y) + r2.z * 60.0) * sin(t * (14.0 + 5.0 * r2.z) + r2.y * 40.0);
            float shape = exp(-0.5 * px * px / (sigma * sigma)) / (2.0 * kPi * sigma * sigma);
            sum += StarColour(r2.y * r2.y, smoothstep(0.16, 0.03, flux)) * flux * shape * max(tw, 0.0);
        }
    return sum;
}

// The galaxy's band: a great circle of faint light, mottled, darker dust lanes along its middle, a warmer bulge at its
// core.
vec3 Galaxy(vec3 s)
{
    const vec3 kNormal = normalize(vec3(0.3, 0.25, 0.92)), kCore = normalize(vec3(0.95, 0.0, -0.31));
    float b = dot(s, kNormal);
    float band = exp(-b * b / (2.0 * 0.13 * 0.13));
    if (band < 0.01)
        return vec3(0.0);
    float core = exp(-(1.0 - dot(s, kCore)) / 0.06);
    float n = Fbm3(s * 3.5);
    float dust = smoothstep(0.45, 0.72, Fbm3(s * 8.0 + 11.0)) * exp(-b * b / (2.0 * 0.045 * 0.045));
    float l = band * (0.35 + 0.9 * n) * (1.0 + 2.5 * core) * (1.0 - 0.75 * dust);
    return mix(vec3(0.78, 0.84, 1.0), vec3(1.0, 0.86, 0.68), clamp(core * 1.5, 0.0, 1.0)) * l;
}

// A shooting star along its great circle: a head moving fast along it - brightening, flaring, burning out - and its
// train: each point of the path lit as the head passed, as bright as the head was then, fading over a fraction of a
// second from then on, so the train fades from its tail end and lingers a moment after the head is gone. (Faded as a
// whole, it looked wrong - user, 2026-10-19.)
vec3 Meteor(vec3 d, float pa)
{
    vec3 start = P.meteor0.xyz, axis = P.meteor1.xyz;
    float head = P.meteor0.w, end = P.meteor1.w, fade = max(P.sky0.x, 1e-4), width = P.sky0.y;
    float off = asin(clamp(dot(d, axis), -1.0, 1.0)) / pa;
    if (abs(off) > 4.0 * width + 1.0)
        return vec3(0.0);
    vec3 dp = normalize(d - axis * dot(d, axis)), travel = cross(axis, start);
    float phi = atan(dot(dp, travel), dot(dp, start));
    if (phi < -2.0 * pa || phi > min(head, end) + 3.0 * pa * width)
        return vec3(0.0);
    // (How bright the head was when it passed here: in fast, a flicker or two, out over its last quarter.)
    float x = clamp(phi / end, 0.0, 1.0);
    float life = smoothstep(0.0, 0.1, x) * (1.0 - smoothstep(0.7, 1.0, x)) * (0.75 + 0.25 * sin(x * 23.0 + P.sky0.w * 40.0));
    float train = phi <= head ? exp(-(head - phi) / fade) : 0.0;
    float across = exp(-0.5 * off * off / (width * width));
    float h = (phi - head) / (1.2 * pa * width);
    float headGlow = head < end ? exp(-0.5 * h * h) * (0.75 + 0.25 * sin(head / end * 23.0 + P.sky0.w * 40.0)) : 0.0;
    return P.sky1.rgb * P.sky0.z * (2.0 * life * train + 3.0 * headGlow * smoothstep(0.0, 0.1, head / end)) * across;
}

// An aurora: two curtains of light towards the pole (+z), ~450 and ~680 km off, folding as they run east-west - each a
// wall from ~95 km up: where the pixel's ray meets it (its height there, three steps to the folded wall), green low
// with a sharp lower edge, red-violet high and fading out at the top, each ray at its own height; rays along it,
// a slow pulse, brighter where seen edge-on.
vec3 Aurora(vec3 d, float t)
{
    if (d.y < 0.0 || d.z < 0.05)
        return vec3(0.0);
    const float R = 6360.0;
    vec3 sum = vec3(0.0);
    float edgeOn = min(length(d.xz) / max(d.z, 1e-3), 4.0);
    for (int k = 0; k < 2; ++k) {
        float D0 = k == 0 ? 450.0 : 680.0, gain = k == 0 ? 1.0 : 0.6, at = D0 / d.z;
        for (int it = 0; it < 3; ++it) {
            float x = d.x * at;
            float D = D0 + 320.0 * (Fbm2(vec2(x * 0.006 + float(k) * 7.0, t * 0.01)) - 0.5) +
                      35.0 * sin(x * 0.025 + t * 0.15 + float(k) * 2.0);
            at = D / d.z;
        }
        float x = d.x * at;
        float h01 = (at * d.y + at * at / (2.0 * R) - 95.0) / 160.0;
        if (h01 < -0.05 || h01 > 1.45)
            continue;
        // (Its top fading out - a ray at a time, each ending at its own height -, not cut: cut at 1.2 it stood as an
        // edge against the sky - user, 2026-10-19.)
        float top = 0.55 + 0.75 * Noise2(vec2(x * 0.09 + 17.0, t * 0.05 + float(k) * 5.0));
        float profile = smoothstep(-0.05, 0.04, h01) * exp(-max(h01, 0.0) * 2.4) *
                        (1.0 - smoothstep(0.7, 1.4, h01)) * (1.0 - smoothstep(top - 0.3, top + 0.1, h01));
        float rays = 0.3 + 0.7 * pow(Noise2(vec2(x * 0.12, t * 0.25 + float(k) * 3.0)), 2.0);
        float pulse = 0.65 + 0.35 * Noise2(vec2(x * 0.008, t * 0.15 + float(k) * 11.0));
        vec3 col = mix(P.auroraColour.rgb, vec3(0.85, 0.2, 0.55), smoothstep(0.35, 0.9, h01));
        sum += col * profile * rays * pulse * gain * edgeOn;
    }
    return sum * 0.9;
}

void main()
{
    outGlow = outLocal = outMotion = outAlbedo = vec4(0.0);
    vec2 ndc = vec2(gl_FragCoord.x / P.view.x * 2.0 - 1.0, 1.0 - gl_FragCoord.y / P.view.y * 2.0);
    vec4 a = P.invViewProj * vec4(ndc, 0.0, 1.0), b = P.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    if (P.view.z > 0.5) {
        vec3 sky = textureLod(skyView, SkyViewUv(dir), 0.0).rgb;
        // Our night sky over it: the stars, the galaxy, a shooting star, an aurora - dimmed through the air near the
        // horizon (its air mass) and lost in a bright sky (the moon's glow, the blue hour), as by eye.
        float shown = P.star0.w;
        if (shown > 0.0 && dir.y > -0.02) {
            // (A pixel's angle: to the next pixel's ray - right whatever the sky's camera's projection.)
            vec2 ndc2 = ndc + vec2(2.0 / P.view.x, 0.0);
            vec4 a2 = P.invViewProj * vec4(ndc2, 0.0, 1.0), b2 = P.invViewProj * vec4(ndc2, 1.0, 1.0);
            float pa = max(length(cross(dir, normalize(b2.xyz / b2.w - a2.xyz / a2.w))), 1e-6), t = P.night.y;
            float through = exp(-0.25 * (1.0 / max(dir.y + 0.04, 0.02) - 1.0 / 1.04));
            vec3 s = vec3(dot(dir, P.star0.xyz), dot(dir, P.star1.xyz), dot(dir, P.star2.xyz));
            float lowTwinkle = P.star2.w * (0.35 + 0.65 * (1.0 - clamp(dir.y, 0.0, 1.0)));
            vec3 lit = Stars(s, pa, t, lowTwinkle) * 20.0 * P.star1.w;
            if (P.night.x > 0.0) lit += Galaxy(s) * 0.06 * P.night.x;
            if (P.meteor1.w >= 0.0) lit += Meteor(dir, pa);
            if (P.night.w > 0.0) lit += Aurora(dir, t) * P.night.w;
            float ls = dot(lit, vec3(0.3, 0.59, 0.11)), sl = dot(sky, vec3(0.3, 0.59, 0.11));
            float seen = ls * ls / (ls * ls + 0.16 * sl * sl + 1e-12);
            sky += lit * shown * through * seen;
        }
        outScene = vec4(sky, 1.0);
        return;
    }
    float t = clamp(min(P.sky0.w, 1.0) * SkyTwilightAmount(P.sky1.y, P.sky3.w), 0.0, 1.0);
    vec3 c = t > 0.0 ? SkyTwilight(dir, P.sky0, P.sky1, P.sky3) * t : vec3(0.0);
    outScene = vec4(c + SkyGlow(dir, P.sky1, P.sky2, P.sky3), t);
}
