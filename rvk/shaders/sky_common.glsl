// The sky's light in a direction (rvk sky.cpp SkyModel; docs/twilight-plan.md): one model the sky pass
// (sky_glow.frag), and later the haze, the far fog and the clouds read, so land and sky meet in the same colour.
// Fitted to the game: its brightness and part of its hue from the zone's fog colour (the game's horizon), the rest a
// twilight palette - a warm band on the sun's side, a cooler zenith, the pink band and the earth's dusky shadow
// opposite the sun around sunrise and sunset; by day a gentle gradient, by night nothing.
//
// The includer passes the model's four vectors (Device::SkyModel):
//   s0: the game's fog colour (rgb), w: how far the twilight palette replaces the game's sky (RVK_Twilight; above 1:
//       the same, more saturated)
//   s1: towards the true sun (world, below the horizon too), w: the glow's strength (RVK_SkyGlow)
//   s2: towards the sun disc seen (the brightest of the game's sun sprites), w: 1 rising, 0 setting
//   s3: the glow's colour (the sun's light through the air, at the sky's brightness), w RVK_BlueHour (0..1)
#ifndef SKY_COMMON_GLSL
#define SKY_COMMON_GLSL

const vec3 kSkyLuma = vec3(0.3, 0.59, 0.11);

// How much it is twilight by the sun's height h (the sine of its elevation): all of it from ~6 degrees up through
// sunset, the afterglow and the blue hour, a little by day (a gentle gradient); handed back to the game's night sky
// ~12 to ~16 degrees down (blue: RVK_BlueHour 0..1 - without it, as the dawn round had it, gone by ~13).
float SkyTwilightAmount(float h, float blue)
{
    return smoothstep(mix(-0.22, -0.28, blue), mix(-0.1, -0.2, blue), h) * mix(0.15, 1.0, 1.0 - smoothstep(0.06, 0.4, h));
}

// How far round towards a direction's azimuth: 1 looking at it, 0 away (horizontal parts; 0.5 when either is upright).
float SkyToward(vec3 dir, vec3 to)
{
    vec2 a = dir.xz, b = to.xz;
    if (dot(a, a) < 1e-6 || dot(b, b) < 1e-6)
        return 0.5;
    return 0.5 + 0.5 * dot(normalize(a), normalize(b));
}

// The twilight sky's colour towards dir (world, unit) - vivid (docs/dusk-plan.md, user 2026-10-18: orange-red at the
// horizon, pink above, a violet zenith, then a deep blue hour). Its brightness the game's (its fog colour: the
// horizon's), its hue mostly the palette's - a little of the zone's kept, more by day and by night.
//   sunset (the sun ~7 degrees up to ~2 under): the warm band on the sun's side tall (to ~20 degrees) and wide, gold
//     to orange to red-orange low, salmon and pink above it, the zenith blue going violet; opposite, the pink band
//     ~2..15 degrees up with the earth's dusky shadow under it.
//   afterglow (~2 to ~8 under): the warm band narrowing to a red-orange line under a pink-purple arch.
//   blue hour (~5 to ~14 under): deep blue to indigo overhead, violet low, the warmth gone.
// s3.w: RVK_BlueHour. s0.w above 1: the palette more saturated (RVK_Twilight up to 1.5).
vec3 SkyTwilight(vec3 dir, vec4 s0, vec4 s1, vec4 s3)
{
    float fogL = max(dot(s0.rgb, kSkyLuma), 0.02);
    vec3 zone = s0.rgb / fogL;                   // (the zone's hue at unit brightness)
    float h = s1.y;
    float v = clamp(dir.y, 0.0, 1.0);
    float toward = SkyToward(dir, s1.xyz);
    float t2 = toward * toward;
    float away = pow(1.0 - toward, 1.5);
    float high = smoothstep(0.0, 0.8, v);
    float dusk = 1.0 - smoothstep(-0.035, 0.12, h);       // 0 with the sun ~7 up .. 1 at ~2 under
    float after = 1.0 - smoothstep(-0.14, -0.03, h);           // 0 at ~2 under .. 1 at ~8 under
    float blue = (1.0 - smoothstep(-0.2, -0.07, h)) * s3.w;      // the blue hour deepening
    // The warm band on the sun's side: taller towards the sun and round sunset, narrowing in the afterglow.
    float tall = mix(0.08, 0.35, t2 * dusk) * (1.0 - 0.75 * after);
    float band = exp(-v / max(tall, 0.02));
    float line = exp(-v / 0.035);                         // (the horizon's last line of colour)
    // Its colours: gold, then orange, then red-orange as the sun goes down; deeper in the afterglow.
    vec3 warm = mix(vec3(1.0, 0.78, 0.45), vec3(1.0, 0.5, 0.2), smoothstep(0.0, 0.55, dusk));
    warm = mix(warm, vec3(1.0, 0.32, 0.14), smoothstep(0.55, 1.0, dusk));
    warm = mix(warm, vec3(0.9, 0.26, 0.16), after);
    // Above it: salmon, then pink, then a pink-purple arch.
    vec3 rose = mix(vec3(1.0, 0.62, 0.5), vec3(0.98, 0.5, 0.58), dusk);
    rose = mix(rose, vec3(0.75, 0.42, 0.7), after);
    // Overhead: blue, going violet round sunset, deep blue to indigo in the blue hour.
    vec3 zenith = mix(vec3(0.42, 0.55, 0.85), vec3(0.42, 0.38, 0.72), dusk);
    zenith = mix(zenith, vec3(0.24, 0.28, 0.62), blue);
    vec3 c = mix(zenith, mix(vec3(1.0), zone, 0.5), exp(-v / 0.1) * 0.5 * (1.0 - dusk));
    float roseBand = exp(-v / max(2.5 * tall, 0.05)) * (1.0 - band);
    c = mix(c, rose, roseBand * mix(0.3, 1.0, toward) * dusk);
    c = mix(c, warm, band * t2);
    c = mix(c, vec3(0.95, 0.3, 0.12), line * t2 * after * (1.0 - blue));
    // Opposite the sun, with it near the horizon: the pink band ~2..15 degrees up, the earth's shadow under it.
    float near = smoothstep(-0.14, -0.04, h) * (1.0 - smoothstep(0.03, 0.1, h));
    float belt = smoothstep(0.02, 0.07, v) * (1.0 - smoothstep(0.12, 0.26, v));
    c = mix(c, vec3(0.95, 0.52, 0.62), belt * away * near * 0.85);
    c = mix(c, vec3(0.36, 0.4, 0.62), (1.0 - smoothstep(0.0, 0.05, v)) * away * near * 0.7);
    // The blue hour: low in the sky a violet glow, the rest its blue.
    c = mix(c, mix(vec3(0.3, 0.38, 0.75), vec3(0.5, 0.42, 0.72), exp(-v / 0.15)), blue * (1.0 - band * t2 * (1.0 - after)));
    // Vivid: the zone's own hue kept less at twilight's height; more saturated still above RVK_Twilight 1.
    float peak = max(dusk * (1.0 - blue), after * (1.0 - blue));
    c = c / max(dot(c, kSkyLuma), 1e-3);
    c = mix(c, zone, mix(0.3, 0.12, peak));
    float boost = clamp(s0.w - 1.0, 0.0, 0.5) * 2.0;
    c = max(mix(vec3(dot(c, kSkyLuma)), c, 1.0 + 0.5 * boost), vec3(0.0));
    // As bright as the game's fog, darker high up and away from the sun, brightest low towards it, dimming as the sun
    // goes down.
    // (The game's fog darkens with the hour itself: in the blue hour only a little darker than it - deeper, it went
    // near black before the night's sky came.)
    float L = fogL * (0.75 + 0.2 * band + 0.6 * band * t2 - 0.25 * high) * mix(0.6, 1.0, smoothstep(-0.2, 0.02, h));
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
    // (The disc seen while it is up, the true sun's way as it goes under - blended: switched, the glow jumped across.)
    vec3 to = normalize(mix(s1.xyz, s2.xyz, smoothstep(-0.14, -0.06, s2.y)) + vec3(0.0, 1e-5, 0.0));
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
vec3 SkyHorizonDelta(vec3 dirW, vec4 s0, vec4 s1, vec4 s3)
{
    float t = clamp(min(s0.w, 1.0) * SkyTwilightAmount(s1.y, s3.w), 0.0, 1.0);
    float across = length(dirW.xz);
    if (t <= 0.0 || across < 1e-4)
        return vec3(0.0);
    vec3 h = normalize(vec3(dirW.x, 0.03 * across, dirW.z));
    return (SkyTwilight(h, s0, s1, s3) - s0.rgb) * t;
}

// Our own sky's horizon towards a world direction (RVK_ProcSky; rvk atmosphere.cpp: FL.skyRing, eight colours round
// the horizon read back from the sky view table) - and as SkyHorizonDelta, how far it differs from the game's fog
// colour: added where the air takes the fog's colour, so the land fades into our sky, not the zone's.
vec3 SkyHorizonRing(vec3 dirW, vec4 ring[8])
{
    float az = atan(dirW.z, dirW.x) / 0.78539816;             // (sectors of 45 degrees from +x towards +z)
    float f = fract(az / 8.0) * 8.0;
    int i = int(floor(f)) & 7;
    return mix(ring[i].rgb, ring[(i + 1) & 7].rgb, f - floor(f));
}

// The air's colour towards a world direction, for what the game colours by its fog (base: the game's fog colour as
// the includer paled it): with our own sky its horizon there, wholly - base + (ring - fog) left the base's
// difference to the fog behind, the zone's teal (the valley mist a teal half-disc round a low sun: user,
// 2026-10-18); else base and the twilight's difference.
vec3 SkyAirAll(vec3 base, vec3 dirW, vec4 s0, vec4 s1, vec4 s3, vec4 ring[8])
{
    if (ring[0].w > 0.5)
        return SkyHorizonRing(dirW, ring);
    return base + SkyHorizonDelta(dirW, s0, s1, s3);
}

vec3 SkyHorizonDeltaAll(vec3 dirW, vec4 s0, vec4 s1, vec4 s3, vec4 ring[8])
{
    if (ring[0].w > 0.5)
        return SkyHorizonRing(dirW, ring) - s0.rgb;
    return SkyHorizonDelta(dirW, s0, s1, s3);
}

#endif
