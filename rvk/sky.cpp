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
    const float t2 = toward * toward, away = std::pow(1.0f - toward, 1.5f), high = Smoothstep(0.0f, 0.8f, v);
    const float dusk = 1.0f - Smoothstep(-0.035f, 0.12f, h), after = 1.0f - Smoothstep(-0.14f, -0.03f, h);
    const float blue = (1.0f - Smoothstep(-0.2f, -0.07f, h)) * s3[3];
    const float tall = (0.08f + (0.35f - 0.08f) * t2 * dusk) * (1.0f - 0.75f * after);
    const float band = std::exp(-v / std::max(tall, 0.02f)), line = std::exp(-v / 0.035f);
    V3 warm = Mix({1.0f, 0.78f, 0.45f}, {1.0f, 0.5f, 0.2f}, Smoothstep(0.0f, 0.55f, dusk));
    warm = Mix(warm, {1.0f, 0.32f, 0.14f}, Smoothstep(0.55f, 1.0f, dusk));
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
    const float nearSun = Smoothstep(-0.14f, -0.04f, h) * (1.0f - Smoothstep(0.03f, 0.1f, h));
    const float belt = Smoothstep(0.02f, 0.07f, v) * (1.0f - Smoothstep(0.12f, 0.26f, v));
    c = Mix(c, {0.95f, 0.52f, 0.62f}, belt * away * nearSun * 0.85f);
    c = Mix(c, {0.36f, 0.4f, 0.62f}, (1.0f - Smoothstep(0.0f, 0.05f, v)) * away * nearSun * 0.7f);
    c = Mix(c, Mix({0.3f, 0.38f, 0.75f}, {0.5f, 0.42f, 0.72f}, std::exp(-v / 0.15f)), blue * (1.0f - band * t2 * (1.0f - after)));
    const float peak = std::max(dusk * (1.0f - blue), after * (1.0f - blue));
    c = c * (1.0f / std::max(Luma(c), 1e-3f));
    c = Mix(c, zone, 0.3f + (0.12f - 0.3f) * peak);
    const float boost = std::clamp(s0[3] - 1.0f, 0.0f, 0.5f) * 2.0f;
    const float l = Luma(c);
    c = Mix({l, l, l}, c, 1.0f + 0.5f * boost);
    c = {std::max(c.r, 0.0f), std::max(c.g, 0.0f), std::max(c.b, 0.0f)};
    const float L = fogL * (0.75f + 0.2f * band + 0.6f * band * t2 - 0.25f * high) *
                    (0.6f + 0.4f * Smoothstep(-0.2f, 0.02f, h));
    return c * (L / std::max(Luma(c), 1e-3f));
}

}  // namespace

// The world's conditions (rvk.h Conditions), with the sun's clock: the time of day's ramps, each set once here.
void Device::UpdateConditions()
{
    Conditions& c = m_cond;
    const float h = SunUp();
    c.sunKnown = SunKnown();
    c.sunUp = h;
    c.golden = Smoothstep(-0.035f, 0.0f, h) * (1.0f - Smoothstep(0.14f, 0.26f, h));
    c.afterglow = Smoothstep(-0.14f, -0.035f, h) * (1.0f - Smoothstep(0.0f, 0.035f, h));
    c.blueHour = Smoothstep(-0.26f, -0.19f, h) * (1.0f - Smoothstep(-0.087f, -0.035f, h));
    c.night = 1.0f - Smoothstep(-0.24f, -0.035f, h);
    c.moonNight = 1.0f - Smoothstep(-0.14f, -0.035f, h);
    // (By the sun's true height, not its light: that goes out with the sun still 3 degrees up, where its shadows end,
    // and the clouds' night brightening came on at once under a dusk sky still near daylight - blown out white, user.)
    c.cloudNight = 1.0f - Smoothstep(-0.2f, 0.03f, h);
    c.deepNight = c.sunKnown ? 1.0f - Smoothstep(-0.24f, -0.14f, h) : 1.0f;
    c.gameStars = c.sunKnown ? 1.0f - Smoothstep(-0.24f, -0.105f, h) : 1.0f;
    // (Without the fade - RVK_StarFade off - still none with the sun well up: none would show, and the field costs.)
    c.ourStars = !c.sunKnown ? 1.0f : !m_starFade ? 1.0f - Smoothstep(0.05f, 0.15f, h) : 1.0f - Smoothstep(-0.24f, -0.03f, h);
    c.heatNight = c.sunKnown ? 1.0f - std::clamp((h + 0.03f) / 0.1f, 0.0f, 1.0f) : 0.0f;
    // The grading's night: by the sun's height while it is known, else (indoors) by the game's light and ambient -
    // eased over a few seconds either way.
    const float grade = c.sunKnown ? c.night : NightByLight();
    c.grade = m_nightSet ? c.grade + (grade - c.grade) * (1.0f - std::exp(-m_frameDt / 3.0f)) : grade;
    m_nightSet = true;
}

// Once a frame, at its end (before the shadow map, which is drawn from the sun or the moon these find): the time of
// day and the weather's light, in order - each reads what the ones before it set.
void Device::UpdateEnvironment(VkCommandBuffer cmd)
{
    // The sun's true direction (FrameLights sunTrue), kept: frame lights written before this frame's sun is seen
    // read it (the cloud layer's: with the shadow light's direction - the moon's, above the horizon - it took night for
    // day and its clouds went dark). Eased over real time, with the dusk's phases from its height.
    UpdateSunClock();
    UpdateDarkness();                            // (RVK_Darkness: how dark this frame may get - darkness.cpp)
    // With the sun down, the moon's light (night.cpp) - the shadow map drawn from it. The valley mist (mist.cpp).
    UpdateMoon();
    UpdateMist();
    UpdateSkyModel();                            // (the sky's twilight and glow)
    UpdateAtmosphere(cmd);                       // (our own sky's tables, for the next frame: atmosphere.cpp)
    UpdateTerrainSun(cmd);                       // (the terrain's shade from a low sun: terrain_sun.cpp)
}

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
    // Rising or setting: the trend over the last half minute (mist.cpp), eased over a minute (the sun hovering near the
    // horizon flips it back and forth).
    {
        const float target = m_mistSunTrend >= 0.0f ? 1.0f : 0.0f;
        m_sunRising += (target - m_sunRising) * (1.0f - std::exp(-m_frameDt / 60.0f));
    }
    UpdateConditions();
    // The game's stars (draw.cpp: its sky's point lists, x this): in their dark (m_cond.gameStars) - and over ~20 s
    // from when the game begins drawing them, should it switch them on at once while the sky is dark.
    m_starsAge = m_duskSkyLast.stars > 0 && m_duskSkyLast.frame + 2 >= m_frameNumber ? m_starsAge + m_frameDt : 0.0f;
    m_starsShown = m_starFade ? m_cond.gameStars * Smoothstep(0.0f, 20.0f, m_starsAge) : 1.0f;
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
            m_duskSkyLast.domeAmbient & 0xFFFFFFu, m_duskSkyLast.stars, m_duskSkyLast.starVerts, double(m_cond.golden),
            double(m_cond.afterglow), double(m_cond.blueHour), double(m_cond.night), double(m_cond.grade),
            double(m_sunRising), double(m_moonFade), double(m_frameSunColor[0]), double(m_frameSunColor[1]),
            double(m_frameSunColor[2]));
    }
}

// The sunset rays' strength (volume.frag): RVK_DuskRays with the sun from ~10 degrees up to ~4 under (its glow
// lighting the air after it has set), none under a storm's deck.
float Device::DuskRaysNow() const
{
    const float h = SunUp();
    return std::max(m_duskRays, 0.0f) * Smoothstep(-0.07f, -0.01f, h) * (1.0f - Smoothstep(0.1f, 0.18f, h)) *
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
    // (Only an overcast, storms and rain mute the colours: broken cloud covers the sky behind it itself, and muting
    // for it left a fair evening's sunset at half strength.)
    // (Indoors - no sun known: a room's playfield, the alien ships - no sky and no twilight to colour its fog: the zone's
    // own colour stays, the alien ships' green.)
    const float clear = SunKnown() ? ClearSky() * (1.0f - 0.6f * std::clamp(m_mistWet, 0.0f, 1.0f)) : 0.0f;
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
        const float day = Smoothstep(-0.033f, 0.03f, up);
        const float k = std::clamp(m_dawnLight, 0.0f, 2.0f) * low2 * day * clear;
        // The sun's: towards orange at the horizon (more with a vivid sunset, RVK_Twilight), about 85% as bright as
        // the game's light.
        const float vivid = std::clamp(m[3] / 0.85f, 0.0f, 1.2f) * (1.0f - Smoothstep(0.04f, 0.2f, up));
        float sun[3] = {1.0f, 1.0f - (0.22f + 0.1f * vivid) * k, 1.0f - (0.45f + 0.17f * vivid) * k};
        // (Our own sky: the sun's light through its air instead - atmosphere.cpp SunThroughAir.)
        const bool ours = ProcSkyReady() && m_skyUp[3] > 0.5f;
        if (ours) {
            float through[3];
            SunThroughAir(up, through);
            const float w = 0.7f * std::min(k, 1.0f);
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
        m_dawnTint[1][3] = std::clamp(m_goldenHour, 0.0f, 1.0f) * Smoothstep(-0.14f, -0.03f, up) *
                           (1.0f - Smoothstep(0.14f, 0.3f, up)) * clear;
    }
    // The blue hour's grade (tonemap.frag Grade): the picture cooler, its darks greyer - as the sky - between sunset's
    // colours and the night's grade, which comes in under it (NightAmount).
    m_blueGrade = std::clamp(m_blueHour, 0.0f, 1.0f) * m_cond.blueHour * clear;
    // (No fog seen yet: no sky to fit the twilight to - the model off; the low sun's light above needs none.)
    if (!m_skyFogSet)
        std::memset(m_skyModel, 0, sizeof(m_skyModel));
    UpdateRainbow();
    if (m_frameNumber % 600 == 7)
        Log("sky model: sun up %.2f (%s), disc up %.2f; fog %.2f %.2f %.2f; twilight %.2f, glow %.2f (%.2f %.2f %.2f); "
            "clear %.2f; low sun's light %.2f %.2f %.2f, shade %.2f %.2f %.2f", double(to[1]), m[11] > 0.5f ? "rising" : "setting", double(seen[1]), double(m_skyFog[0]),
            double(m_skyFog[1]), double(m_skyFog[2]), double(m[3]), double(m[7]), double(m[12]), double(m[13]),
            double(m[14]), double(clear), double(m_dawnTint[0][0]), double(m_dawnTint[0][1]), double(m_dawnTint[0][2]),
            double(m_dawnTint[1][0]), double(m_dawnTint[1][1]), double(m_dawnTint[1][2]));
}

// Our night sky this frame (docs/sky-plan.md §2-4): how much of the stars the dark shows - from the sun ~2 degrees
// under (the brightest; the sky's own brightness hides the rest, sky_glow.frag) to all by ~14 -, less under an
// overcast or a storm; a shooting star now and then, from the sky's clock (one every 25 s / RVK_Meteors at most,
// 15-60 degrees up where you look, falling, each its own speed, length, width, brightness and colour); an aurora on some nights (the clock's 90-minute knots: about one night in six at
// RVK_Aurora 1, wandering in strength).
void Device::UpdateNightSky()
{
    const float clear = ClearSky();
    m_starsDark = m_cond.ourStars * clear;
    const double t = SkyClock();
    // The shooting star.
    m_meteor[1][3] = -1.0f;
    const float rate = std::clamp(m_nightSky.meteors, 0.0f, 3.0f);
    if (rate > 0.0f && m_starsDark > 0.3f) {
        // Each its own (the slot's knots): 20-60 degrees a second; burning 0.15-0.6 s, mostly short; its train fading
        // 0.08-0.25 s behind the head; mostly thin and faint, one in ~12 a fireball - wider, brighter, slower to fade;
        // blue-white, some orange, some green.
        const double slot = 25.0 / double(rate);
        const int64_t i = int64_t(std::floor(t / slot));
        const float speed = (20.0f + 40.0f * Knot(i, 103u)) / 57.29578f;
        const float burn = 0.15f + 0.45f * Knot(i, 93u) * Knot(i, 93u);
        const bool fireball = Knot(i, 105u) < 0.08f;
        const float fade = fireball ? 0.15f + 0.2f * Knot(i, 107u) : 0.08f + 0.17f * Knot(i, 107u);
        const double start = double(Knot(i, 91u)) * (slot - 2.0), ph = t - double(i) * slot - start;
        if (ph >= 0.0 && ph <= double(burn + 4.0f * fade)) {
            // (Where you look, within ~70 degrees either side: one anywhere in the sky was mostly missed - user,
            // 2026-10-19.)
            const float ahead = std::atan2(m_frameForward[2], m_frameForward[0]);
            const float el = (15.0f + 45.0f * Knot(i, 95u)) / 57.29578f, az = ahead + 2.4f * (Knot(i, 97u) - 0.5f);
            const float st[3] = {std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
            // (Its way across the sky: mostly down, up to ~70 degrees to either side.)
            float east[3] = {st[2], 0.0f, -st[0]};
            const float el2 = std::sqrt(east[0] * east[0] + east[2] * east[2]);
            for (float& e : east) e /= std::max(el2, 1e-4f);
            const float up[3] = {st[1] * east[2] - st[2] * east[1], st[2] * east[0] - st[0] * east[2],
                                 st[0] * east[1] - st[1] * east[0]};
            const float th = (Knot(i, 99u) - 0.5f) * 2.4f;
            float way[3], axis[3];
            for (int k = 0; k < 3; ++k) way[k] = -std::cos(th) * up[k] + std::sin(th) * east[k];
            axis[0] = st[1] * way[2] - st[2] * way[1];
            axis[1] = st[2] * way[0] - st[0] * way[2];
            axis[2] = st[0] * way[1] - st[1] * way[0];
            const float al = std::max(std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]), 1e-4f);
            for (int k = 0; k < 3; ++k) {
                m_meteor[0][k] = st[k];
                m_meteor[1][k] = axis[k] / al;
            }
            m_meteor[0][3] = speed * float(ph);
            m_meteor[1][3] = speed * burn;
            m_meteor[2][0] = speed * fade;
            m_meteor[2][1] = fireball ? 1.0f + 0.6f * Knot(i, 109u) : 0.55f + 0.45f * Knot(i, 109u);
            m_meteor[2][2] = fireball ? 2.5f + 1.5f * Knot(i, 111u) : 0.4f + 1.6f * Knot(i, 111u) * Knot(i, 111u);
            m_meteor[2][3] = Knot(i, 113u);
            const float hue = Knot(i, 115u);
            const float colour[3][3] = {{0.85f, 0.95f, 1.0f}, {1.0f, 0.8f, 0.55f}, {0.7f, 1.0f, 0.75f}};
            const int c = hue < 0.65f ? 0 : hue < 0.85f ? 1 : 2;
            for (int k = 0; k < 3; ++k) m_meteor[3][k] = colour[c][k];
        }
    }
    // The aurora.
    const float a = std::clamp(m_nightSky.aurora, 0.0f, 2.0f);
    float level = 0.0f;
    if (a > 0.0f) {
        const float th = 1.0f - 0.22f * a;
        level = Smoothstep(th, th + 0.08f, Noise(t, 5400.0, 71u)) * (0.6f + 0.4f * Noise(t, 300.0, 73u)) * std::min(a, 1.0f);
    }
    // (RVK_AuroraForce: every night, clear or not - to see one now.)
    m_auroraNow = m_nightSky.auroraForce > 0.0f ? m_nightSky.auroraForce * m_cond.night : level * m_cond.night * clear;
    if (m_frameNumber % 600 == 17 && (m_starsDark > 0.0f || m_auroraNow > 0.0f))
        Log("sky: night - stars %.2f, aurora %.2f%s", double(m_starsDark), double(m_auroraNow),
            m_meteor[1][3] >= 0.0f ? ", a shooting star" : "");
}

// A rainbow (docs/sky-plan.md §5; tonemap.frag Rainbow): in the minutes after rain - it rained in the last ~3 minutes
// and now no more than lightly -, with the sun out (the clouds letting it through, no storm) and lower than 42
// degrees (the bow's top would be under the horizon), the lower the sun the higher the bow and the stronger. In over
// ~20 s, out over ~2 minutes.
void Device::UpdateRainbow()
{
    if (m_rain > 0.15f)
        m_rainbowRainClock = m_frameClock;
    const float up = SunUp();
    const float since = float(m_frameClock - m_rainbowRainClock);
    float target = 0.0f;
    if (m_rainbowSetting > 0.0f && SunKnown()) {
        const float after = Smoothstep(0.0f, 10.0f, since) * (1.0f - Smoothstep(90.0f, 180.0f, since));
        const float light = (1.0f - Smoothstep(0.15f, 0.5f, m_rain)) * Smoothstep(0.5f, 0.8f, m_sunThrough) *
                            (1.0f - StormSky());
        const float sun = Smoothstep(0.0f, 0.05f, up) * (1.0f - Smoothstep(0.5f, 0.669f, up));
        target = std::clamp(m_rainbowSetting, 0.0f, 2.0f) * after * light * sun;
    }
    if (m_rainbowForce >= 0.0f)
        target = m_rainbowForce;
    const float tau = target > m_rainbow ? 20.0f : 120.0f;
    m_rainbow += (target - m_rainbow) * (m_rainbowForce >= 0.0f ? 1.0f : 1.0f - std::exp(-m_frameDt / tau));
    if (m_frameNumber % 600 == 19 && m_rainbow > 0.01f)
        Log("sky: a rainbow, %.2f (rain %.2f, %.0f s since it rained; sun up %.2f, through %.2f)", double(m_rainbow),
            double(m_rain), double(since), double(up), double(m_sunThrough));
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
        float view[4], sky[16], star[3][4], night[4], meteor[2][4], aurora[4];
    } push{};
    static_assert(sizeof(Push) == 256, "sky_glow.frag's push constants");
    if (!InvertMatrix(MulMatrix(m_view, m_proj), &push.invViewProj))
        return;
    push.view[0] = float(m_scene->m_width);
    push.view[1] = float(m_scene->m_height);
    push.view[2] = ours ? 1.0f : 0.0f;
    std::memcpy(push.sky, m_skyModel, sizeof(push.sky));
    // Our night sky (docs/sky-plan.md §2-4): the star frame - turning round a pole 35 degrees up towards +z with the
    // sun's hour, so the stars wheel through the night with it -, how much shows, the shooting star, the aurora.
    if (ours && m_nightSky.stars) {
        UpdateNightSky();
        const float pole[3] = {0.0f, 0.573576f, 0.819152f}, e1[3] = {1.0f, 0.0f, 0.0f}, e2[3] = {0.0f, 0.819152f, -0.573576f};
        const float* sd = m_trueSunDir;
        const float a = std::atan2(-(sd[0] * e2[0] + sd[1] * e2[1] + sd[2] * e2[2]), -(sd[0] * e1[0] + sd[1] * e1[1] + sd[2] * e1[2]));
        const float ca = std::cos(a), sa = std::sin(a);
        for (int i = 0; i < 3; ++i) {
            push.star[0][i] = ca * e1[i] + sa * e2[i];
            push.star[1][i] = -sa * e1[i] + ca * e2[i];
            push.star[2][i] = pole[i];
        }
        push.star[0][3] = m_starsDark;
        push.star[1][3] = std::max(m_nightSky.bright, 0.0f);
        push.star[2][3] = std::clamp(m_nightSky.twinkle, 0.0f, 1.0f);
        push.night[0] = std::max(m_nightSky.galaxy, 0.0f);
        push.night[1] = float(std::fmod(m_frameClock, 3600.0));
        push.night[3] = m_auroraNow;
        std::memcpy(push.meteor, m_meteor, sizeof(push.meteor));
        // (The twilight model's slots - unused under our own sky: sky_glow.frag reads them only over the game's - hold
        // the shooting star's look.)
        std::memcpy(push.sky, m_meteor[2], 8 * sizeof(float));
        for (int i = 0; i < 3; ++i)
            push.aurora[i] = std::pow(float((m_nightSky.auroraColour >> (16 - 8 * i)) & 0xFFu) / 255.0f, 2.2f);
    } else {
        push.meteor[1][3] = -1.0f;
    }
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

// A desert's air now (RVK_DesertAir, RVK_HeatShimmer; docs/desert-plan.md phase 4): on a desert or wasteland playfield
// (its climate's lean, kZoneSkies: -0.15 or clearer), by day and in fair weather - an overcast, a storm's sky or a
// sandstorm (it brings its own dust) takes it away. The shimmer wants the sun high too: the ground hot.
void Device::DesertAir(float out[2]) const
{
    out[0] = out[1] = 0.0f;
    if (m_skyZoneLean > -0.15f)
        return;
    const float cloudy = CloudCover();
    const float fair = (1.0f - Smoothstep(0.4f, 0.9f, cloudy)) * (1.0f - StormSky()) * (1.0f - std::clamp(4.0f * m_sand, 0.0f, 1.0f));
    const float h = SunUp();
    out[0] = std::clamp(m_desertAir, 0.0f, 1.0f) * fair * Smoothstep(-0.05f, 0.15f, h);
    out[1] = std::clamp(m_heatShimmer, 0.0f, 1.0f) * fair * Smoothstep(0.3f, 0.55f, h);
}

}  // namespace rvk
