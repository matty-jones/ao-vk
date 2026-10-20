// Light on clouds, shared by the cloud layer (cloud_layer.frag) and the game's cloud sheets drawn with it
// (ffp_main.glsl, MAT_CLOUDSHEET). The includer declares the frame lights (FL).
#include "sky_common.glsl"                     // (the twilight's colours along the horizon: SkyHorizonDelta)

// The game's sunlight on its own clouds (rvk clouds.cpp NoteCloudGameLight: its hue - amber at dawn, white by day), when
// known: our clouds lit in it keep to the game's palette (user, 2026-10-13: pink dawn clouds over a world the game
// lit gold stood out).
bool GameCloudHue(out vec3 hue)
{
    hue = unpackUnorm4x8(floatBitsToUint(FL.clouds2.z)).rgb;
    return FL.clouds2.w > 0.5 && max(hue.r, max(hue.g, hue.b)) > 0.0;
}

// How vivid the clouds' sunset is (docs/dusk-plan.md; user 2026-10-18: orange, red and pink clouds): RVK_Afterglow x
// the twilight's share of the sky now (RVK_Twilight; muted by an overcast, storms, rain), only with the sun low - 0 by
// day and by night, 1 at a full sunset (up to 2 with RVK_Afterglow 2).
float DuskVivid()
{
    float h = FL.sky[1].y;
    return clamp(FL.dusk.x * min(FL.sky[0].w / 0.85, 1.2) * SkyTwilightAmount(h, FL.sky[3].w) *
                 (1.0 - smoothstep(0.12, 0.3, h)), 0.0, 2.0);
}

// The sunset's colour on the clouds by the sun's height: gold, then orange, red-orange at the horizon; under it -
// the clouds still in the sun after the land - crimson-pink, then magenta.
vec3 DuskSunHue(float h)
{
    vec3 c = mix(vec3(1.0, 0.55, 0.3), vec3(1.0, 0.8, 0.55), smoothstep(0.03, 0.14, h));
    c = mix(vec3(1.0, 0.38, 0.22), c, smoothstep(0.0, 0.03, h));
    c = mix(vec3(0.95, 0.32, 0.38), c, smoothstep(-0.05, 0.0, h));
    return mix(vec3(0.78, 0.3, 0.55), c, smoothstep(-0.1, -0.05, h));
}

// The sunlight reaching the clouds: in the game's hue for its clouds when known (it reddens them itself at dawn and
// dusk), else reddened low in the sky (orange, then pink near the horizon); fading a little after it sets. At dawn and
// dusk the game's level light stays the frame's sun (rvk shadow.cpp: no shadows under ~3 degrees). Round sunset
// (DuskVivid) in the sunset's colours instead, and - the clouds high over the land - lit from below for ~5 degrees
// after the land's sun has gone, in the light of the glow (FL.sky[3]).
vec3 SunLight(vec3 sunL)
{
    float low = 1.0 - smoothstep(0.0, 0.35, sunL.y);
    low *= low;                                  // (golden first; pink only in its last few degrees)
    vec3 tint = mix(vec3(1.0), mix(vec3(1.0, 0.8, 0.58), vec3(1.0, 0.6, 0.5), smoothstep(0.75, 1.0, low)), low);
    vec3 hue, c;
    if (GameCloudHue(hue)) {
        // (FL.sunColor.w: the sun's brightness before a low sun is dimmed for the ground by the clouds hiding it)
        float strength = FL.sunColor.w / max(dot(hue, vec3(0.3, 0.59, 0.11)), 0.05);
        c = hue * strength * smoothstep(-0.12, 0.1, sunL.y);
    } else {
        float luma = dot(FL.sunColor.rgb, vec3(0.3, 0.59, 0.11));
        vec3 sun = luma > 1e-4 ? FL.sunColor.rgb * (FL.sunColor.w / luma) : vec3(FL.sunColor.w);
        c = sun * tint * smoothstep(-0.12, 0.1, sunL.y);
    }
    float v = DuskVivid();
    if (v > 0.0) {
        const vec3 kLuma = vec3(0.3, 0.59, 0.11);
        vec3 dusk = DuskSunHue(sunL.y);
        // (Its brightness the game's while the sun is up; after, the glow's - at half: the light through much air.)
        float lit = max(dot(c, kLuma), 0.5 * dot(FL.sky[3].rgb, kLuma) * smoothstep(-0.09, 0.0, sunL.y));
        c = mix(c, dusk / dot(dusk, kLuma) * lit, min(v, 1.0)) * max(v, 1.0);
    }
    return c;
}

// The glow under the clouds round sunset: their undersides lit by the light low in the sky (most with the sun at the
// horizon and a little after) - in the game's hue for its clouds, faint (a strong pink one stood out); else pink.
// Round a vivid sunset (DuskVivid) pink to magenta and stronger - the land now takes the sunset's colours too, so they
// no longer stand out from it - fading to a dim violet into the blue hour.
vec3 Afterglow(vec3 sunL)
{
    float g = smoothstep(-0.14, -0.02, sunL.y) * (1.0 - smoothstep(0.02, 0.12, sunL.y));
    vec3 hue;
    vec3 c = GameCloudHue(hue) ? hue * g * 0.35 : vec3(1.0, 0.5, 0.55) * g;
    float v = DuskVivid();
    if (v > 0.0) {
        vec3 pink = mix(vec3(1.0, 0.48, 0.45), vec3(0.85, 0.38, 0.68), 1.0 - smoothstep(-0.1, 0.0, sunL.y));
        vec3 vivid = pink * g * 0.9 + vec3(0.38, 0.34, 0.68) * 0.15 * FL.phase.z;
        c = mix(c, vivid, min(v, 1.0)) * max(v, 1.0);
    }
    return c;
}

// The sky's light on the clouds round sunset: tinted by the twilight sky over them (violet-blue overhead, warmer
// towards the sun) as vivid as the sunset (DuskVivid); 1 1 1 otherwise. Once per pixel (not per step).
vec3 DuskSkyTint()
{
    float v = min(DuskVivid(), 1.0);
    // Our own sky (RVK_ProcSky): the sky over them its colour (FL.skyUp) - a little of its blue by day in their shade,
    // more round sunset (its pinks and violets).
    if (FL.skyUp.w > 0.5) {
        vec3 c = FL.skyUp.rgb / max(dot(FL.skyUp.rgb, vec3(0.3, 0.59, 0.11)), 1e-4);
        return mix(vec3(1.0), c, mix(0.2, 0.5, v));
    }
    if (v <= 0.0)
        return vec3(1.0);
    vec3 up = normalize(vec3(FL.sky[1].x * 0.4, 0.75, FL.sky[1].z * 0.4));
    vec3 c = SkyTwilight(up, FL.sky[0], FL.sky[1], FL.sky[3]);
    c /= max(dot(c, vec3(0.3, 0.59, 0.11)), 1e-3);
    return mix(vec3(1.0), c, 0.5 * v);
}

// How much it is night for the clouds' light: 0 with the sun up .. 1 with it ~11 degrees down (rvk sky.cpp
// UpdateConditions: m_cond.cloudNight, by the sun's true height).
float Night()
{
    return FL.cond.x;
}

// How much the sky's light is raised at night (1 by day): lit only by the sky, cloud dimmed inside to 0.4 came out a
// third as light as the game's own night clouds (user: brightness 3 to look right) - the sky's light then carries the
// game's colour on its own.
float NightSky()
{
    return mix(1.0, 3.4, Night());               // (user: 1.7 x the first 2.0 in game)
}

// Low in the sky, the horizon's band (up to ~12 degrees), as much as the game fogs its clouds (amount): 0 .. amount.
float HorizonBand(vec3 dir, float amount)
{
    float low = 1.0 - smoothstep(0.0, 0.22, dir.y);
    return amount * low * low * (3.0 - 2.0 * low);
}

// The air's colour before clouds: the game's fog colour (haze) at the horizon (low: HorizonBand), where the clouds
// meet its sky; above, paler and whiter - the game's teal fog made the whole sky's clouds teal (user: whiter).
vec3 AirColour(vec3 haze, float low)
{
    float grey = dot(haze, vec3(0.3, 0.59, 0.11));
    return mix(mix(vec3(grey), haze, 0.35) * 1.25, haze, low);
}

// The moon's light on the clouds (rvk night.cpp: FL.moon, at night FL.sunDir its direction): cool and dim, a light
// the clouds take as they take the sun's - brightest towards it, silver edges looking at it.
vec3 MoonLight()
{
    return FL.moon.rgb * 1.2;
}

vec3 MoonDir()
{
    return -normalize(FL.sunDir.xyz + vec3(0.0, -1e-6, 0.0));
}

// How much the moon lights (0 none .. 1 a high moon at night): the sky's light on the clouds eased down by it, so
// those away from the moon are darker than those it lights.
float MoonShare()
{
    return clamp(dot(FL.moon.rgb, vec3(0.3, 0.59, 0.11)) / 0.25, 0.0, 1.0);
}
