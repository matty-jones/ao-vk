#version 450
// The sky view table (rvk atmosphere.cpp; atmos_common.glsl; Hillaire 2020 §5.3): the sky's light in every direction
// from the eye, this frame - x the world's azimuth all round, y the elevation (squared towards the horizon, where the
// sky changes fastest: the middle row is the horizon). Marched from the eye to the air's top or the ground: the sun's
// light scattered once (Rayleigh, Mie) and more (the multiple scattering table), the moon's once, the earth's shadow
// on the air at dusk. Then the art on top: the brightness fitted to the game's sky, a hint of the zone's colour,
// the night's floor, the sky's colours (ours or the game's: RVK_SkyColours), weather's grey. Read by the sky pass
// (sky_glow.frag) and later the haze and clouds.
#include "atmos_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D transmittance;
layout(set = 0, binding = 1) uniform sampler2D multiScatter;
layout(push_constant) uniform Push {
    vec4 size;          // the table's width, height; z w: the air's tune (Mie, ozone)
    vec4 sun;           // towards the sun (world, below the horizon too); w the eye's height (km)
    vec4 moon;          // towards the moon; w its light (0: none)
    vec4 scale;         // rgb: the sun's light x the sky's brightness (fitted to the game's); w the twilight's boost
    vec4 zone;          // rgb: the zone's hue (unit luma); w how much of it (RVK_SkyZone)
    vec4 weather;       // rgb: the overcast's hue (unit luma); w how much (0 clear .. 1 a storm's sky)
    vec4 night;         // rgb: the night sky's floor (airglow, starlight); w the halo round the sun (its Mie lobe, x)
    vec4 dust;          // rgb: a desert's dust (unit luma); w how much (RVK_DesertAir: 0 none)
    vec4 keyTop, keyMid, keyLowSun, keyLowAway;   // rgb: the sky's colours as ratios to the physics (docs/sky-plan.md
                                                  // §1): overhead, 35 degrees up, low towards the sun / away from it
} P;
layout(location = 0) out vec4 outColor;

vec3 Trans(float r, float mu) { return textureLod(transmittance, AtmosTransUv(r, mu), 0.0).rgb; }
vec3 Multi(float r, float mu) { return textureLod(multiScatter, AtmosMultiUv(r, mu), 0.0).rgb; }

// The sky's colours (RVK_Sky*, or the game's - atmosphere.cpp SkyKeyRatios): the ratio to the physics in a direction
// - low, then 35 degrees up, then overhead; low, between the sun's side and the far side. (atmosphere.cpp
// KeyRatioAt the same, to take it back out of the readback.)
vec3 KeyRatio(vec3 d, vec3 sunDir)
{
    float a = 0.5;
    if (dot(d.xz, d.xz) > 1e-8 && dot(sunDir.xz, sunDir.xz) > 1e-8)
        a = 0.5 + 0.5 * dot(normalize(d.xz), normalize(sunDir.xz));
    vec3 r = mix(P.keyLowAway.rgb, P.keyLowSun.rgb, a);
    r = mix(r, P.keyMid.rgb, smoothstep(0.05, 0.57, d.y));
    return mix(r, P.keyTop.rgb, smoothstep(0.57, 1.0, d.y));
}

// The table's direction for its pixel (and the sky pass's inverse: sky_glow.frag SkyViewUv).
vec3 ViewDir(vec2 uv)
{
    float az = (uv.x - 0.5) * 2.0 * kAtmosPi;
    float v = uv.y * 2.0 - 1.0;
    float el = sign(v) * v * v * 0.5 * kAtmosPi;
    return vec3(cos(el) * cos(az), sin(el), cos(el) * sin(az));
}

void main()
{
    vec2 uv = gl_FragCoord.xy / P.size.xy;
    vec3 d = ViewDir(uv);
    vec3 o = vec3(0.0, kGroundR + P.sun.w, 0.0);
    float tGround = AtmosSphere(o, d, kGroundR), tTop = AtmosSphere(o, d, kTopR);
    float tEnd = min(tGround > 0.0 ? tGround : tTop, 600.0);   // (the far horizon's air: no more than 600 km)
    vec3 sunDir = normalize(P.sun.xyz), moonDir = normalize(P.moon.xyz);
    float cs = dot(d, sunDir), cm = dot(d, moonDir);
    float phR = AtmosRayleighPhase(cs), phM = AtmosMiePhase(cs, 0.8);
    float phRm = AtmosRayleighPhase(cm), phMm = AtmosMiePhase(cm, 0.8);
    const int kSteps = 32;
    vec3 through = vec3(1.0), L = vec3(0.0), Lm = vec3(0.0);
    float tPrev = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        // (Steps growing along the ray: short near the eye, where the air is thickest.)
        float u = (float(i) + 1.0) / float(kSteps), t = tEnd * u * u, dt = t - tPrev;
        vec3 p = o + d * (t - 0.5 * dt);
        tPrev = t;
        float h = length(p);
        vec3 up = p / h;
        AtmosMedium m = AtmosAt(h - kGroundR, P.size.zw);
        float mu = dot(up, sunDir), muM = dot(up, moonDir);
        // (The earth's shadow: none of the sun where the ground is between.)
        vec3 sunT = AtmosSphere(p, sunDir, kGroundR) > 0.0 ? vec3(0.0) : Trans(h, mu);
        vec3 scat = m.rayleigh + vec3(m.mie);
        // (The halo round the sun - the Mie lobe - at P.night.w: the game's sun sprites and our disc's glare give it
        // already; whole, it stacked into a wide white glare round a low sun.)
        vec3 S = (m.rayleigh * phR + m.mie * phM * P.night.w) * sunT + scat * Multi(h, mu);
        vec3 Sm = P.moon.w > 0.0 && AtmosSphere(p, moonDir, kGroundR) < 0.0 ? (m.rayleigh * phRm + m.mie * phMm) * Trans(h, muM)
                                                                        : vec3(0.0);
        vec3 stepT = exp(-m.extinction * dt);
        vec3 inv = 1.0 / max(m.extinction, vec3(1e-6));
        L += through * (S - S * stepT) * inv;
        Lm += through * (Sm - Sm * stepT) * inv;
        through *= stepT;
    }
    vec3 c = L * P.scale.rgb + Lm * P.moon.w;
    const vec3 kLuma = vec3(0.3, 0.59, 0.11);
    float l = dot(c, kLuma);
    // Dawn and dusk more vivid (RVK_Twilight above its physical 1): the colour pushed from grey, most with the sun low.
    float low = 1.0 - smoothstep(0.0, 0.35, abs(sunDir.y + 0.03));
    c = max(mix(vec3(l), c, 1.0 + P.scale.w * low), vec3(0.0));
    // The zone's colour, a hint: its hue at the same brightness - more low in the sky than overhead.
    float horizon = 1.0 - smoothstep(0.0, 0.6, abs(d.y));
    c = mix(c, l * P.zone.rgb, P.zone.w * mix(0.35, 1.0, horizon));
    // The night's floor - the sky never quite black -, then the sky's colours (ours or the game's) over it all.
    c += P.night.rgb;
    c *= KeyRatio(d, sunDir);
    // A desert's dust (RVK_DesertAir): the lowest sky paler, brighter and warmer, the fine dust the day's heat lifts -
    // a band fading out by ~17 degrees up, most of it in the lowest few; overhead stays blue.
    if (P.dust.w > 0.0) {
        float y = max(d.y, 0.0), band = (1.0 - smoothstep(0.0, 0.3, y)) * (0.6 + 0.4 * (1.0 - smoothstep(0.0, 0.08, y)));
        c = mix(c, dot(c, kLuma) * 1.45 * P.dust.rgb, 0.7 * P.dust.w * band);
    }
    // Weather: an overcast's grey, a storm's darker, its hue.
    l = dot(c, kLuma);
    c = mix(c, l * P.weather.rgb * (1.0 - 0.35 * P.weather.w), P.weather.w);
    outColor = vec4(c, 1.0);
}
