// Sky weather (docs/sky-weather-plan.md): how cloudy the sky is, clear .. overcast, from the clock. The game draws the
// same flat sheets in every sky and says nothing of the weather but its rain, so the sky's own state comes from here:
// smooth noise over time - one sky lasting half an hour to an hour, changing over minutes - from the wall
// clock (UTC: AO's time runs from real time, so the same moment has the same sky on every login). Each zone leans it
// by its climate (phase 3): deserts and wastelands clearer, coasts, islands and forests cloudier (RVK_SkyZones).
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

// A knot's value, 0..1 (its index hashed).
float Knot(int64_t i, uint32_t salt)
{
    uint64_t x = uint64_t(i) * 0x9E3779B97F4A7C15ull ^ (uint64_t(salt) << 32);
    x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27; x *= 0x94D049BB133111EBull; x ^= x >> 31;
    return float(x >> 40) / 16777216.0f;
}

// Smooth value noise over time: knots `period` seconds apart, eased between.
float Noise(double t, double period, uint32_t salt)
{
    const double u = t / period;
    const int64_t i = int64_t(std::floor(u));
    const float f = float(u - double(i)), s = f * f * (3.0f - 2.0f * f);
    return Knot(i, salt) + (Knot(i + 1, salt) - Knot(i, salt)) * s;
}

// Rubi-Ka's outdoor playfields by climate (resource ids, rdb 1000001 names): how far their skies lean, in the noise's
// units before the mapping (0.6 of them span clear .. overcast) - a desert's sky is clear about half the time,
// overcast seldom; a coast's broken or overcast about half. Others (cities, the Shadowlands' own skies): 0.
struct ZoneSky { uint32_t playfield; float lean; };
constexpr ZoneSky kZoneSkies[] = {
    // deserts and wastelands
    {565, -0.20f},   // Newland Desert
    {566, -0.12f},   // Newland City
    {567, -0.12f},   // Newland
    {551, -0.18f},   // Wailing Wastes
    {560, -0.20f},   // Mort
    {570, -0.20f},   // The Perpetual Wastelands
    {760, -0.15f},   // 4 Holes
    {791, -0.15f},   // Holes in the Wall
    {795, -0.15f},   // The Longest Road
    {670, -0.10f},   // Clondyke
    {615, -0.08f},   // Southern Fouls Hills
    {620, -0.08f},   // Eastern Fouls Plains
    {6011, -0.18f},  // Arid Rift
    {6012, -0.12f},  // Neretva Canyons
    {6550, -0.10f},  // Uturn Canyon
    // coasts, islands, the sea
    {556, 0.15f},    // Coast of Peace
    {656, 0.15f},    // Coast of Tranquility
    {665, 0.15f},    // Broken Shores
    {585, 0.12f},    // Aegean
    {500, 0.10f},    // Parnassos
    {501, 0.10f},    // Parnassos Island of Ceremony
    {6010, 0.12f},   // Serenity Islands
    {4894, 0.12f},   // Coast of Harmony
    // forests, green and wet country, the river
    {505, 0.12f},    // Avalon
    {600, 0.08f},    // Varmint Woods
    {605, 0.10f},    // Belial Forest
    {716, 0.10f},    // Omni Forest
    {717, 0.10f},    // Greater Omni Forest
    {685, 0.10f},    // Galway County
    {687, 0.10f},    // Galway Shire
    {695, 0.06f},    // Lush Fields
    {630, 0.04f},    // Pleasant Meadows
    {646, 0.06f},    // Tir County
    {647, 0.06f},    // Greater Tir County
    {625, 0.08f},    // Milky Way
    {635, 0.05f},    // Stret East Bank
    {650, 0.05f},    // Upper Stret East Bank
    {790, 0.05f},    // Stret West Bank
    {6551, 0.08f},   // Uturn Forest
};

}  // namespace

void Device::SetSkyPlayfield(uint32_t playfield)
{
    if (playfield == m_skyPlayfield)
        return;
    m_skyPlayfield = playfield;
    m_skyZoneLean = 0.0f;
    for (const ZoneSky& z : kZoneSkies)
        if (z.playfield == playfield)
            m_skyZoneLean = z.lean;
    if (m_skyZoneLean != 0.0f)
        Log("sky: playfield %u leans its skies %s (%+.2f)", playfield, m_skyZoneLean < 0.0f ? "clearer" : "cloudier",
            double(m_skyZoneLean));
}

// Seconds on the sky's clock: the wall clock (UTC), or a test's (SetSkyTime).
double Device::SkyClock() const
{
    if (m_skyTime >= 0.0)
        return m_skyTime;
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

// How cloudy the sky is: 0 clear .. 1 overcast. Knots 90 minutes apart, a lesser wander over 25: a sky lasts about half
// an hour (fair, scattered, broken) to an hour (clear); clear about a fifth of the time, overcast a seventh.
float Device::SkyCloudiness() const
{
    const double t = SkyClock();
    const float v = 0.75f * Noise(t, 5400.0, 17u) + 0.25f * Noise(t, 1500.0, 29u) +
                    m_skyZoneLean * std::clamp(m_cloudParams.zones, 0.0f, 1.0f);
    return std::clamp((v - 0.25f) / 0.6f, 0.0f, 1.0f);
}

// The wind's way when the game gives none (its rain's wind gives one): turning slowly over hours, from the clock -
// x, z unit vector.
void Device::SkyWind(float out[2]) const
{
    const double t = SkyClock();
    const float a = 6.2831853f * (Noise(t, 14400.0, 41u) * 1.6f + Noise(t, 3600.0, 53u) * 0.4f);
    out[0] = std::cos(a);
    out[1] = std::sin(a);
}

// The sky's state by name (the log).
const char* Device::SkyName(float c)
{
    return c < 0.08f ? "clear" : c < 0.3f ? "fair" : c < 0.55f ? "scattered" : c < 0.8f ? "broken" : "overcast";
}

namespace {

float SmoothRamp(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct V3 { float r, g, b; };
V3 operator*(V3 a, float k) { return {a.r * k, a.g * k, a.b * k}; }
V3 operator+(V3 a, V3 b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
V3 Mix(V3 a, V3 b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
float Luma(V3 c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

// sky_common.glsl SkyTwilight on the CPU (keep the two alike): the twilight sky's colour towards dir (world, unit),
// from the model's s0, s1 and s3 (m_skyModel 0..3, 4..7, 12..15).
V3 SkyTwilightCpu(const float dir[3], const float* s0, const float* s1, const float* s3)
{
    const float fogL = std::max(0.3f * s0[0] + 0.59f * s0[1] + 0.11f * s0[2], 0.02f);
    const V3 zone{s0[0] / fogL, s0[1] / fogL, s0[2] / fogL};
    const float h = s1[1];
    const float v = std::clamp(dir[1], 0.0f, 1.0f);
    float toward = 0.5f;
    {
        const float a = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]), b = std::sqrt(s1[0] * s1[0] + s1[2] * s1[2]);
        if (a > 1e-3f && b > 1e-3f) toward = 0.5f + 0.5f * (dir[0] * s1[0] + dir[2] * s1[2]) / (a * b);
    }
    const float t2 = toward * toward, away = std::pow(1.0f - toward, 1.5f), high = SmoothRamp(0.0f, 0.8f, v);
    const float dusk = 1.0f - SmoothRamp(-0.035f, 0.12f, h), after = 1.0f - SmoothRamp(-0.14f, -0.03f, h);
    const float blue = (1.0f - SmoothRamp(-0.2f, -0.07f, h)) * s3[3];
    const float tall = (0.08f + (0.35f - 0.08f) * t2 * dusk) * (1.0f - 0.75f * after);
    const float band = std::exp(-v / std::max(tall, 0.02f)), line = std::exp(-v / 0.035f);
    V3 warm = Mix({1.0f, 0.78f, 0.45f}, {1.0f, 0.5f, 0.2f}, SmoothRamp(0.0f, 0.55f, dusk));
    warm = Mix(warm, {1.0f, 0.32f, 0.14f}, SmoothRamp(0.55f, 1.0f, dusk));
    warm = Mix(warm, {0.9f, 0.26f, 0.16f}, after);
    V3 rose = Mix({1.0f, 0.62f, 0.5f}, {0.98f, 0.5f, 0.58f}, dusk);
    rose = Mix(rose, {0.75f, 0.42f, 0.7f}, after);
    V3 zenith = Mix({0.42f, 0.55f, 0.85f}, {0.42f, 0.38f, 0.72f}, dusk);
    zenith = Mix(zenith, {0.24f, 0.28f, 0.62f}, blue);
    V3 c = Mix(zenith, Mix({1.0f, 1.0f, 1.0f}, zone, 0.5f), std::exp(-v / 0.1f) * 0.5f * (1.0f - dusk));
    const float roseBand = std::exp(-v / std::max(2.5f * tall, 0.05f)) * (1.0f - band);
    c = Mix(c, rose, roseBand * (0.3f + 0.7f * toward) * dusk);
    c = Mix(c, warm, band * t2);
    c = Mix(c, {0.95f, 0.3f, 0.12f}, line * t2 * after * (1.0f - blue));
    const float nearSun = SmoothRamp(-0.14f, -0.04f, h) * (1.0f - SmoothRamp(0.03f, 0.1f, h));
    const float belt = SmoothRamp(0.02f, 0.07f, v) * (1.0f - SmoothRamp(0.12f, 0.26f, v));
    c = Mix(c, {0.95f, 0.52f, 0.62f}, belt * away * nearSun * 0.85f);
    c = Mix(c, {0.36f, 0.4f, 0.62f}, (1.0f - SmoothRamp(0.0f, 0.05f, v)) * away * nearSun * 0.7f);
    c = Mix(c, Mix({0.3f, 0.38f, 0.75f}, {0.5f, 0.42f, 0.72f}, std::exp(-v / 0.15f)), blue * (1.0f - band * t2 * (1.0f - after)));
    const float peak = std::max(dusk * (1.0f - blue), after * (1.0f - blue));
    c = c * (1.0f / std::max(Luma(c), 1e-3f));
    c = Mix(c, zone, 0.3f + (0.12f - 0.3f) * peak);
    const float boost = std::clamp(s0[3] - 1.0f, 0.0f, 0.5f) * 2.0f;
    const float l = Luma(c);
    c = Mix({l, l, l}, c, 1.0f + 0.5f * boost);
    c = {std::max(c.r, 0.0f), std::max(c.g, 0.0f), std::max(c.b, 0.0f)};
    const float L = fogL * (0.75f + 0.2f * band + 0.6f * band * t2 - 0.25f * high) *
                    (0.6f + 0.4f * SmoothRamp(-0.2f, 0.02f, h));
    return c * (L / std::max(Luma(c), 1e-3f));
}

}  // namespace

// The sun's clock (docs/dusk-plan.md), once a frame before the moon and the sky's model: the true sun - the game's
// brightest directional light, which by night points up from under the ground - eased over about a second of real
// time (a step in the game's light becomes a sweep), held through frames without it; and from its height the dusk's
// phases, each a wide ramp, so day turns to night over game minutes rather than at once (user, 2026-10-18: the sky's
// colour popped, the land's light switched and the whole picture darkened together). Sun heights are sines: 0.035 ~2
// degrees, 0.14 ~8, 0.24 ~14.
void Device::UpdateSunClock()
{
    const double now = m_frameClock;
    m_frameDt = m_sunClockLast < 0.0 ? 0.0f : float(std::clamp(now - m_sunClockLast, 0.0, 0.25));
    m_sunClockLast = now;
    if (m_sunLuminance > 0.0f) {
        // (First sight, or after seconds without - a zone change, indoors: taken as it is, no sweep across the sky.)
        const bool gap = m_sunLightClock < 0.0 || now - m_sunLightClock > 2.0;
        m_sunLightClock = now;
        if (gap) {
            std::memcpy(m_trueSunDir, m_sunDir, sizeof(m_trueSunDir));
        } else {
            const float k = 1.0f - std::exp(-m_frameDt / 1.0f);
            float d[3], l = 0.0f;
            for (int i = 0; i < 3; ++i) {
                d[i] = m_trueSunDir[i] + (m_sunDir[i] - m_trueSunDir[i]) * k;
                l += d[i] * d[i];
            }
            l = std::sqrt(l);
            if (l > 1e-4f)
                for (int i = 0; i < 3; ++i) m_trueSunDir[i] = d[i] / l;
        }
    }
    const float h = SunUp();
    // The golden hour: the sun from ~15 degrees down to the horizon, and a little under it.
    m_sunPhase[0] = SmoothRamp(-0.035f, 0.0f, h) * (1.0f - SmoothRamp(0.14f, 0.26f, h));
    // The afterglow: the sun from the horizon to ~8 degrees under (most within the first 2).
    m_sunPhase[1] = SmoothRamp(-0.14f, -0.035f, h) * (1.0f - SmoothRamp(0.0f, 0.035f, h));
    // The blue hour: ~2 to ~15 degrees under (all of it from 5 to 11).
    m_sunPhase[2] = SmoothRamp(-0.26f, -0.19f, h) * (1.0f - SmoothRamp(-0.087f, -0.035f, h));
    // Night: from ~2 degrees under to ~14.
    m_sunPhase[3] = 1.0f - SmoothRamp(-0.24f, -0.035f, h);
    // Rising or setting: the trend over the last half minute (mist.cpp), eased over a minute (the sun hovering near the
    // horizon flips it back and forth).
    {
        const float target = m_mistSunTrend >= 0.0f ? 1.0f : 0.0f;
        m_sunRising += (target - m_sunRising) * (1.0f - std::exp(-m_frameDt / 60.0f));
    }
    // The game's stars (draw.cpp: its sky's point lists, x this): none with the sun ~6 degrees under, all by ~14 - and
    // over ~20 s from when the game begins drawing them, should it switch them on at once while the sky is dark.
    m_starsAge = m_duskSkyLast.stars > 0 && m_duskSkyLast.frame + 2 >= m_frameNumber ? m_starsAge + m_frameDt : 0.0f;
    // (No sun known - a zone without one: by the game's own.)
    const float dark = SunKnown() ? 1.0f - SmoothRamp(-0.24f, -0.105f, h) : 1.0f;
    m_starsShown = m_starFade ? dark * SmoothRamp(0.0f, 20.0f, m_starsAge) : 1.0f;
    // The grading's night: by the sun's height while it is known, else (indoors) by the game's light and ambient -
    // eased over a few seconds either way.
    const float night = SunKnown() ? m_sunPhase[3] : NightByLight();
    m_nightAmount = m_nightSet ? m_nightAmount + (night - m_nightAmount) * (1.0f - std::exp(-m_frameDt / 3.0f)) : night;
    m_nightSet = true;
    // The dusk log (docs/dusk-plan.md §0): with the sun near the horizon, every 2 seconds or so - the game's light, its
    // ambient, fog, dome and stars as they go dark, and ours.
    if (h > -0.4f && h < 0.3f && (m_frameNumber % 120) == 11) {
        float fog[4], params[4];
        SceneFog(fog, params);
        Log("dusk: sun up %.3f (the game's %.3f, light %.2f %.2f %.2f%s), ambient %.2f; fog %.2f %.2f %.2f; dome %u verts "
            "texture %u ambient %06X; stars %u draws %u verts; golden %.2f afterglow %.2f blue %.2f night %.2f -> grade %.2f; "
            "rising %.2f; moon %.2f; frame sun %.2f %.2f %.2f",
            double(h), double(m_sunLuminance > 0.0f ? -m_sunDir[1] : 0.0f), double(m_sunColor[0]), double(m_sunColor[1]),
            double(m_sunColor[2]), m_sunLuminance > 0.0f ? "" : ", none this frame", double(m_sceneAmbient),
            double(fog[0]), double(fog[1]), double(fog[2]), m_duskSkyLast.domeVerts, m_duskSkyLast.domeTexture,
            m_duskSkyLast.domeAmbient & 0xFFFFFFu, m_duskSkyLast.stars, m_duskSkyLast.starVerts, double(m_sunPhase[0]),
            double(m_sunPhase[1]), double(m_sunPhase[2]), double(m_sunPhase[3]), double(m_nightAmount),
            double(m_sunRising), double(m_moonFade), double(m_frameSunColor[0]), double(m_frameSunColor[1]),
            double(m_frameSunColor[2]));
    }
}

// The sunset rays' strength (volume.frag): RVK_DuskRays with the sun from ~10 degrees up to ~4 under (its glow
// lighting the air after it has set), none under a storm's deck.
float Device::DuskRaysNow() const
{
    const float h = SunUp();
    return std::max(m_duskRays, 0.0f) * SmoothRamp(-0.07f, -0.01f, h) * (1.0f - SmoothRamp(0.1f, 0.18f, h)) *
           (1.0f - StormSky());
}

float Device::SunUp() const
{
    const float* d = m_trueSunDir;
    return -d[1] / std::max(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1e-6f);
}

// A draw in the sky (the dusk log): the game's dome - its biggest lit mesh - and its stars (point lists).
void Device::NoteDuskSky(uint32_t primitive, uint32_t vertexCount)
{
    if (m_duskSky.frame != m_frameNumber) {
        if (m_duskSky.frame != ~0ull) m_duskSkyLast = m_duskSky;
        m_duskSky = DuskSky{};
        m_duskSky.frame = m_frameNumber;
    }
    if (primitive == d3d::PointList) {
        ++m_duskSky.stars;
        m_duskSky.starVerts += vertexCount;
    } else if (m_rs[d3d::RS_LIGHTING] && vertexCount > m_duskSky.domeVerts) {
        const Texture* t = m_textures[0];
        m_duskSky.domeVerts = vertexCount;
        m_duskSky.domeTexture = t ? t->m_rdbId : 0u;
        m_duskSky.domeAmbient = m_rs[d3d::RS_AMBIENT];
    }
}

// The sky's twilight and glow (docs/twilight-plan.md; sky_common.glsl): its model for this frame, s0..s3 - the game's
// fog colour (eased over ~2 s: a zone's change shouldn't pop) and RVK_Twilight; towards the true sun and RVK_SkyGlow;
// towards the sun disc seen and whether it is rising; the glow's colour. The twilight gives way to cloud, storms and
// rain (their own grey skies); the glow too, as the clouds hide the sun.
void Device::UpdateSkyModel()
{
    float* m = m_skyModel;
    std::memset(m_skyModel, 0, sizeof(m_skyModel));
    for (auto& t : m_dawnTint) t[0] = t[1] = t[2] = 1.0f, t[3] = 0.0f;
    float fog[4], params[4];
    SceneFog(fog, params);
    const float dt = m_skyFogClock < 0.0 ? 1.0f : float(std::clamp(m_frameClock - m_skyFogClock, 0.0, 1.0));
    m_skyFogClock = m_frameClock;
    if (fog[3] > 0.5f) {
        // (Over ~5 s: a step in the game's fog at dusk - its colour by the hour - a sweep, not a pop.)
        const float k = m_skyFogSet ? 1.0f - std::exp(-dt / 5.0f) : 1.0f;
        for (int i = 0; i < 3; ++i) m_skyFog[i] += (fog[i] - m_skyFog[i]) * k;
        m_skyFogSet = true;
    }
    const float vary = std::clamp(m_cloudParams.vary, 0.0f, 1.0f);
    const float cloudy = m_cloudParams.enable ? SkyCloudiness() * vary : 0.0f;
    // (Only an overcast, storms and rain mute the colours: broken cloud covers the sky behind it itself, and muting
    // for it left a fair evening's sunset at half strength.)
    const float overcast = SmoothRamp(0.55f, 0.95f, cloudy);
    const float clear = (1.0f - 0.7f * overcast) * (1.0f - StormSky()) * (1.0f - 0.6f * std::clamp(m_mistWet, 0.0f, 1.0f));
    std::memcpy(m, m_skyFog, sizeof(m_skyFog));
    m[3] = std::clamp(m_twilight, 0.0f, 1.5f) * clear;
    float to[3];
    {
        const float* d = m_trueSunDir;           // (eased: UpdateSunClock)
        const float l = std::max(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1e-6f);
        for (int i = 0; i < 3; ++i) to[i] = -d[i] / l;
    }
    std::memcpy(m + 4, to, sizeof(to));
    m[7] = std::max(m_skyGlow, 0.0f) * clear;
    // The disc seen: the brightest of the game's sun sprites lately (its tint over its size), else the true sun.
    float seen[3] = {to[0], to[1], to[2]};
    if (m_sunSpritesFrame + 3 >= m_frameNumber) {
        float best = 0.0f;
        for (const SunSprite& s : m_sunSprites) {
            const float b = (0.3f * s.colour[0] + 0.59f * s.colour[1] + 0.11f * s.colour[2]) * s.size;
            if (b > best) { best = b; std::memcpy(seen, s.dir, sizeof(seen)); }
        }
    }
    std::memcpy(m + 8, seen, sizeof(seen));
    m[11] = m_sunRising;
    // The glow's colour: the sun's light through the air at the disc's height (golden from ~25 degrees, orange low),
    // at the sky's brightness - brighter round sunrise and sunset, through the clouds as they let it.
    const float low = 1.0f - std::clamp(seen[1] / 0.4f, 0.0f, 1.0f), low2 = low * low;
    const float tint[3] = {1.0f, 1.0f - 0.55f * low2, 1.0f - 0.85f * low2};   // (to a deep orange at the horizon)
    const float fogL = std::max(0.3f * m_skyFog[0] + 0.59f * m_skyFog[1] + 0.11f * m_skyFog[2], 0.02f);
    const float t = std::clamp((to[1] - 0.06f) / (0.4f - 0.06f), 0.0f, 1.0f);
    const float dawn = 1.0f - t * t * (3.0f - 2.0f * t);
    const float b = fogL * (0.8f + 1.2f * dawn) * std::clamp(m_sunThrough, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) m[12 + i] = tint[i] * b;
    m[15] = std::clamp(m_blueHour, 0.0f, 1.0f);
    // A low sun's light (RVK_DawnLight): golden then orange on what faces it - its light through much air -, the
    // shade cooler - lit by the sky alone, bluer than the game's grey ambient. Strongest with the sun under ~20
    // degrees, none by day; the moon's nights untouched; grey under cloud as the light is spread by it.
    {
        const float up = to[1];
        const float lowT = 1.0f - std::clamp(up / 0.35f, 0.0f, 1.0f), low2 = lowT * lowT;
        // (Gone with the sun ~2 degrees under - before the moon takes the shadow map, whose light high in the sky
        // the ground's tint took for a sun reaching everything: the sand turned orange at once.)
        const float day = SmoothRamp(-0.033f, 0.03f, up);
        const float k = std::clamp(m_dawnLight, 0.0f, 2.0f) * low2 * day * clear;
        // The sun's: towards orange at the horizon (more with a vivid sunset, RVK_Twilight), about 85% as bright as
        // the game's light.
        const float vivid = std::clamp(m[3] / 0.85f, 0.0f, 1.2f) * (1.0f - SmoothRamp(0.04f, 0.2f, up));
        float sun[3] = {1.0f, 1.0f - (0.22f + 0.1f * vivid) * k, 1.0f - (0.45f + 0.17f * vivid) * k};
        // (Our own sky: the sun's light through its air instead - atmosphere.cpp SunThroughAir.)
        const bool ours = ProcSkyReady() && m_skyUp[3] > 0.5f;
        if (ours) {
            float through[3];
            SunThroughAir(up, through);
            const float w = std::min(k, 1.0f);
            for (int i = 0; i < 3; ++i) sun[i] = 1.0f + (through[i] - 1.0f) * w;
        }
        const float sl = 0.3f * sun[0] + 0.59f * sun[1] + 0.11f * sun[2];
        const float keep = 1.0f - 0.15f * std::min(k, 1.0f);
        // The sky's in the shade: a little bluer at the same brightness (more turned the desert grey) - and round a
        // vivid sunset the hue of the twilight sky over it (sky_common.glsl SkyTwilight, averaged round the upper
        // sky): pink-violet, so the shade under a red sky isn't the day's grey.
        float sky[3] = {1.0f - 0.06f * k, 1.0f, 1.0f + 0.15f * k};
        if (ours) {
            // (Our own sky: the hue of the sky over the land, as drawn - FL.skyUp - at the low sun's strength.)
            const float l = std::max(0.3f * m_skyUp[0] + 0.59f * m_skyUp[1] + 0.11f * m_skyUp[2], 1e-4f);
            const float w = 0.45f * std::min(k, 1.0f);
            for (int i = 0; i < 3; ++i) sky[i] += (m_skyUp[i] / l - sky[i]) * w;
        } else if (vivid > 0.0f && m_skyFogSet) {
            V3 sum{0.0f, 0.0f, 0.0f};
            for (int j = 0; j < 8; ++j) {
                const float a = float(j) * 0.785398f;
                const float d[3] = {0.82f * std::cos(a), 0.57f, 0.82f * std::sin(a)};   // (~35 degrees up)
                sum = sum + SkyTwilightCpu(d, m, m + 4, m + 12);
            }
            const float zen[3] = {0.0f, 1.0f, 0.0f};
            sum = sum + SkyTwilightCpu(zen, m, m + 4, m + 12) * 2.0f;
            const float sl2 = std::max(Luma(sum), 1e-3f);
            const float w = 0.45f * std::min(vivid, 1.0f) * std::min(k, 1.0f);
            sky[0] += (sum.r / sl2 - sky[0]) * w;
            sky[1] += (sum.g / sl2 - sky[1]) * w;
            sky[2] += (sum.b / sl2 - sky[2]) * w;
        }
        const float kl = 0.3f * sky[0] + 0.59f * sky[1] + 0.11f * sky[2];
        for (int i = 0; i < 3; ++i) {
            m_dawnTint[0][i] = sun[i] / sl * keep;
            m_dawnTint[1][i] = sky[i] / kl;
        }
        m_dawnTint[0][3] = k;
        // The golden hour's grade (RVK_GoldenHour, tonemap.frag Grade): most with the sun from ~2 degrees under the
        // horizon to ~8 up, gone by ~17; muted by cloud as the light.
        m_dawnTint[1][3] = std::clamp(m_goldenHour, 0.0f, 1.0f) * SmoothRamp(-0.14f, -0.03f, up) *
                           (1.0f - SmoothRamp(0.14f, 0.3f, up)) * clear;
    }
    // The blue hour's grade (tonemap.frag Grade): the picture cooler, its darks greyer - as the sky - between sunset's
    // colours and the night's grade, which comes in under it (NightAmount).
    m_blueGrade = std::clamp(m_blueHour, 0.0f, 1.0f) * m_sunPhase[2] * clear;
    // (No fog seen yet: no sky to fit the twilight to - the model off; the low sun's light above needs none.)
    if (!m_skyFogSet)
        std::memset(m_skyModel, 0, sizeof(m_skyModel));
    if (m_frameNumber % 600 == 7)
        Log("sky model: sun up %.2f (%s), disc up %.2f; fog %.2f %.2f %.2f; twilight %.2f, glow %.2f (%.2f %.2f %.2f); "
            "clear %.2f; low sun's light %.2f %.2f %.2f, shade %.2f %.2f %.2f", double(to[1]), m[11] > 0.5f ? "rising" : "setting", double(seen[1]), double(m_skyFog[0]),
            double(m_skyFog[1]), double(m_skyFog[2]), double(m[3]), double(m[7]), double(m[12]), double(m[13]),
            double(m[14]), double(clear), double(m_dawnTint[0][0]), double(m_dawnTint[0][1]), double(m_dawnTint[0][2]),
            double(m_dawnTint[1][0]), double(m_dawnTint[1][1]), double(m_dawnTint[1][2]));
}

// The sky's twilight and glow drawn over the game's sky (sky_glow.frag): once a frame, after its dome, before the
// moons, the suns, the game's cloud sheets and the cloud layer.
void Device::DrawSkyGlow()
{
    if (m_skyGlowFrame == m_frameNumber)
        return;
    m_skyGlowFrame = m_frameNumber;
    const bool ours = ProcSkyReady();
    if (!m_skyGlowPipeline || !m_rendering || m_target != m_scene || !m_glow || !m_localFraction || !m_motionVectors ||
        !m_albedo || m_external || (!ours && m_skyModel[3] <= 0.0f && m_skyModel[7] <= 0.0f))
        return;
    struct Push {
        d3d::Matrix invViewProj;
        float view[4], sky[16];
    } push{};
    static_assert(sizeof(Push) == 144, "sky_glow.frag's push constants");
    if (!InvertMatrix(MulMatrix(m_view, m_proj), &push.invViewProj))
        return;
    push.view[0] = float(m_scene->m_width);
    push.view[1] = float(m_scene->m_height);
    push.view[2] = ours ? 1.0f : 0.0f;
    std::memcpy(push.sky, m_skyModel, sizeof(push.sky));
    FlushGroup();
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    const VkViewport full{0.0f, 0.0f, float(m_scene->m_width), float(m_scene->m_height), 0.0f, 1.0f};
    const VkRect2D area{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &full);
    vkCmdSetScissor(cmd, 0, 1, &area);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyGlowPipeline);
    VkDescriptorImageInfo view{m_linearSampler, ours ? m_atmosView->m_view : m_blackTexture->m_view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &view;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyGlowLayout, 0, 1, &w);
    vkCmdPushConstants(cmd, m_skyGlowLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    m_cache = StateCache{};                      // (not the scene's pipeline: its state set again)
    m_arenaBound = m_bindlessBound = false;
}

}  // namespace rvk
