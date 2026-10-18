// The sky's light in a direction (rvk sky.cpp SkyModel; docs/twilight-plan.md): one model the sky pass
// (sky_glow.frag), and later the haze, the far fog and the clouds read, so land and sky meet in the same colour.
// Fitted to the game: its brightness and part of its hue from the zone's fog colour (the game's horizon), the rest a
// twilight palette - a warm band on the sun's side, a cooler zenith, the pink band and the earth's dusky shadow
// opposite the sun around sunrise and sunset; by day a gentle gradient, by night nothing.
//
// The includer passes the model's four vectors (Device::SkyModel):
//   s0: the game's fog colour (rgb), w: how far the twilight palette replaces the game's sky (RVK_Twilight)
//   s1: towards the true sun (world, below the horizon too), w: the glow's strength (RVK_SkyGlow)
//   s2: towards the sun disc seen (the brightest of the game's sun sprites), w: 1 rising, 0 setting
//   s3: the glow's colour (the sun's light through the air, at the sky's brightness), w unused
#ifndef SKY_COMMON_GLSL
#define SKY_COMMON_GLSL

const vec3 kSkyLuma = vec3(0.3, 0.59, 0.11);

// How much it is twilight by the sun's height h (the sine of its elevation): all of it from ~6 degrees up to ~6 down,
// a little by day (a gentle gradient), gone ~13 degrees down (the night's sky is the game's).
float SkyTwilightAmount(float h)
{
    return smoothstep(-0.22, -0.1, h) * mix(0.15, 1.0, 1.0 - smoothstep(0.06, 0.4, h));
}

// How far round towards a direction's azimuth: 1 looking at it, 0 away (horizontal parts; 0.5 when either is upright).
float SkyToward(vec3 dir, vec3 to)
{
    vec2 a = dir.xz, b = to.xz;
    if (dot(a, a) < 1e-6 || dot(b, b) < 1e-6)
        return 0.5;
    return 0.5 + 0.5 * dot(normalize(a), normalize(b));
}

// The twilight sky's colour towards dir (world, unit).
vec3 SkyTwilight(vec3 dir, vec4 s0, vec4 s1)
{
    float fogL = max(dot(s0.rgb, kSkyLuma), 0.02);
    vec3 zone = s0.rgb / fogL;                   // (the zone's hue at unit brightness)
    float h = s1.y;
    float v = clamp(dir.y, 0.0, 1.0);
    float toward = SkyToward(dir, s1.xyz);
    float away = pow(1.0 - toward, 1.5);
    float band = exp(-v / 0.1);                  // the horizon's band (~6 degrees)
    float high = smoothstep(0.0, 0.8, v);
    // Down from ~7 degrees up (golden) to ~5 down (red): the light along the horizon on the sun's side.
    float dusk = 1.0 - smoothstep(-0.08, 0.12, h);
    vec3 warm = mix(vec3(1.0, 0.78, 0.52), vec3(1.0, 0.55, 0.28), smoothstep(0.0, 0.6, dusk));
    warm = mix(warm, vec3(0.95, 0.36, 0.22), smoothstep(0.6, 1.0, dusk));
    vec3 zenith = mix(vec3(0.42, 0.52, 0.8), vec3(0.36, 0.34, 0.62), dusk);
    vec3 c = mix(zenith, mix(vec3(1.0), zone, 0.5), band * 0.6);
    c = mix(c, warm, band * toward * toward);
    // Opposite the sun, with it near the horizon: the pink band ~2..15 degrees up, the earth's shadow under it.
    float near = smoothstep(-0.14, -0.04, h) * (1.0 - smoothstep(0.03, 0.1, h));
    float belt = smoothstep(0.02, 0.07, v) * (1.0 - smoothstep(0.12, 0.26, v));
    c = mix(c, vec3(0.92, 0.58, 0.64), belt * away * near * 0.7);
    c = mix(c, vec3(0.4, 0.44, 0.6), (1.0 - smoothstep(0.0, 0.05, v)) * away * near * 0.6);
    // A third of the zone's own hue kept; as bright as its fog, darker high up and away from the sun, brightest low
    // towards it, dimming as the sun goes down.
    c = mix(c / max(dot(c, kSkyLuma), 1e-3), zone, 0.3);
    float L = fogL * (0.75 + 0.2 * band + 0.55 * band * toward * toward - 0.25 * high) *
              mix(0.25, 1.0, smoothstep(-0.2, 0.02, h));
    return c / max(dot(c, kSkyLuma), 1e-3) * L;
}

float SkyPhase(float cosT, float g)
{
    float gg = g * g;
    return (1.0 - gg) / pow(max(1.0 + gg - 2.0 * g * cosT, 1e-4), 1.5);
}

// The glow in the air round the sun disc (forward scattering: a tight lobe and a wide one, the wide one hugging the
// horizon) and along the horizon on its side - in the sun's light through the air. Fades as the true sun goes down.
vec3 SkyGlow(vec3 dir, vec4 s1, vec4 s2, vec4 s3)
{
    vec3 to = s2.y > -0.1 ? s2.xyz : s1.xyz;
    float cosT = dot(dir, to);
    float band = exp(-max(dir.y, 0.0) / 0.15);
    float toward = SkyToward(dir, to);
    float g = 0.012 * SkyPhase(cosT, 0.88) + 0.05 * SkyPhase(cosT, 0.55) * (0.4 + 0.6 * band) +
              0.25 * band * pow(toward, 4.0);
    return s3.rgb * (g * s1.w * smoothstep(-0.15, -0.02, s1.y));
}

// How far the twilight sky (as sky_glow.frag draws it over the game's) differs from the game's fog colour along the
// horizon towards a world direction: added wherever the air takes on the fog's colour - the land's far fog and haze,
// the mist, the clouds low in the sky - so they fade into the sky behind them as drawn (a warm band towards the sun,
// cooler away), no seam at the hills. 0 without twilight.
vec3 SkyHorizonDelta(vec3 dirW, vec4 s0, vec4 s1)
{
    float t = clamp(s0.w * SkyTwilightAmount(s1.y), 0.0, 1.0);
    float across = length(dirW.xz);
    if (t <= 0.0 || across < 1e-4)
        return vec3(0.0);
    vec3 h = normalize(vec3(dirW.x, 0.03 * across, dirW.z));
    return (SkyTwilight(h, s0, s1) - s0.rgb) * t;
}

#endif
