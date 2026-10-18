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
        const float k = m_skyFogSet ? 1.0f - std::exp(-dt / 2.0f) : 1.0f;
        for (int i = 0; i < 3; ++i) m_skyFog[i] += (fog[i] - m_skyFog[i]) * k;
        m_skyFogSet = true;
    }
    const float vary = std::clamp(m_cloudParams.vary, 0.0f, 1.0f);
    const float cloudy = m_cloudParams.enable ? SkyCloudiness() * vary : 0.0f;
    const float clear = (1.0f - 0.7f * cloudy) * (1.0f - StormSky()) * (1.0f - 0.6f * std::clamp(m_mistWet, 0.0f, 1.0f));
    std::memcpy(m, m_skyFog, sizeof(m_skyFog));
    m[3] = std::clamp(m_twilight, 0.0f, 1.0f) * clear;
    float to[3];
    {
        const float* d = m_sunLuminance > 0.0f ? m_sunDir : m_trueSunDir;
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
    m[11] = m_mistSunTrend >= 0.0f ? 1.0f : 0.0f;
    // The glow's colour: the sun's light through the air at the disc's height (golden from ~25 degrees, orange low),
    // at the sky's brightness - brighter round sunrise and sunset, through the clouds as they let it.
    const float low = 1.0f - std::clamp(seen[1] / 0.4f, 0.0f, 1.0f), low2 = low * low;
    const float tint[3] = {1.0f, 1.0f - 0.4f * low2, 1.0f - 0.75f * low2};
    const float fogL = std::max(0.3f * m_skyFog[0] + 0.59f * m_skyFog[1] + 0.11f * m_skyFog[2], 0.02f);
    const float t = std::clamp((to[1] - 0.06f) / (0.4f - 0.06f), 0.0f, 1.0f);
    const float dawn = 1.0f - t * t * (3.0f - 2.0f * t);
    const float b = fogL * (0.8f + 1.2f * dawn) * std::clamp(m_sunThrough, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) m[12 + i] = tint[i] * b;
    // A low sun's light (RVK_DawnLight): golden then orange on what faces it - its light through much air -, the
    // shade cooler - lit by the sky alone, bluer than the game's grey ambient. Strongest with the sun under ~20
    // degrees, none by day; the moon's nights untouched; grey under cloud as the light is spread by it.
    {
        const float up = to[1];
        const float lowT = 1.0f - std::clamp(up / 0.35f, 0.0f, 1.0f), low2 = lowT * lowT;
        const float day = std::clamp((up + 0.06f) / 0.06f, 0.0f, 1.0f);   // (gone with the sun ~3 degrees under)
        const float k = std::clamp(m_dawnLight, 0.0f, 2.0f) * low2 * day * clear;
        // The sun's: a third less blue and a sixth less green at the horizon, about 85% as bright as the game's light.
        float sun[3] = {1.0f, 1.0f - 0.22f * k, 1.0f - 0.45f * k};
        const float sl = 0.3f * sun[0] + 0.59f * sun[1] + 0.11f * sun[2];
        const float keep = 1.0f - 0.15f * std::min(k, 1.0f);
        // The sky's in the shade: a little bluer at the same brightness (more turned the desert grey).
        float sky[3] = {1.0f - 0.06f * k, 1.0f, 1.0f + 0.15f * k};
        const float kl = 0.3f * sky[0] + 0.59f * sky[1] + 0.11f * sky[2];
        for (int i = 0; i < 3; ++i) {
            m_dawnTint[0][i] = sun[i] / sl * keep;
            m_dawnTint[1][i] = sky[i] / kl;
        }
        m_dawnTint[0][3] = k;
        // The golden hour's grade (RVK_GoldenHour, tonemap.frag Grade): most with the sun from ~2 degrees under the
        // horizon to ~8 up, gone by ~17; muted by cloud as the light.
        const float rise = std::clamp((up + 0.1f) / 0.07f, 0.0f, 1.0f), fall = std::clamp((up - 0.14f) / 0.16f, 0.0f, 1.0f);
        m_dawnTint[1][3] = std::clamp(m_goldenHour, 0.0f, 1.0f) * rise * rise * (3.0f - 2.0f * rise) *
                           (1.0f - fall * fall * (3.0f - 2.0f * fall)) * clear;
    }
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
    if (!m_skyGlowPipeline || !m_rendering || m_target != m_scene || !m_glow || !m_localFraction || !m_motionVectors ||
        !m_albedo || m_external || (m_skyModel[3] <= 0.0f && m_skyModel[7] <= 0.0f))
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
    std::memcpy(push.sky, m_skyModel, sizeof(push.sky));
    FlushGroup();
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    const VkViewport full{0.0f, 0.0f, float(m_scene->m_width), float(m_scene->m_height), 0.0f, 1.0f};
    const VkRect2D area{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &full);
    vkCmdSetScissor(cmd, 0, 1, &area);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyGlowPipeline);
    vkCmdPushConstants(cmd, m_skyGlowLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    m_cache = StateCache{};                      // (not the scene's pipeline: its state set again)
    m_arenaBound = m_bindlessBound = false;
}

}  // namespace rvk
