// Our own sky (docs/dusk-plan.md §8; RVK_ProcSky): a physical atmosphere after Hillaire 2020 in three small tables
// (atmos_common.glsl) - the transmittance and the multiple scattering made once (again when the air's tune changes),
// the sky view every frame from the sun clock's sun (sky.cpp UpdateSunClock) and the moon - read by the sky pass in
// place of the game's dome (sky.cpp DrawSkyGlow; the dome's draw skipped in draw.cpp). The art on top: its brightness
// fitted to the game's own sky by day, a hint of the zone's colour (RVK_SkyZone), weather's grey, the night's floor.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr uint32_t kTransW = 256, kTransH = 64, kMultiW = 32, kMultiH = 32, kViewW = 192, kViewH = 108;
// The sky's brightness to fit the game's: what the tables give for its light 1 that should show as bright as the
// game's fog by day (RVK_SkyBright 1) - measured, not the zenith's estimate (0.012): with that, the horizon came out
// 6-10 x the fog's brightness (the tone map's roll-off hid it in the sky, but the haze, the far land and the clouds,
// which take the horizon's colour, went white). 0.18: the user's choice in game (0.045 at RVK_SkyBright 0.25,
// 2026-10-18).
constexpr float kZenithRef = 0.18f;

// The eye's adaptation to the sky going dark: how much brighter the sky is shown (log10) by the sun's height - the
// game has no adaptation of its own, and at the day's exposure the sky went black 5 degrees after sunset. Fitted in
// the demo's dusk sweep (tools/sun-sweep.py, no clouds, RVK_NightDark 0, each step held 8 s - the sun eases over
// one; tone mapped) to a sky overhead-ish of 0.4 at 2 degrees under the horizon, 0.32 at 4, 0.24 at 6, 0.17 at 8,
// 0.12 at 10, 0.08 at 12, 0.055 at 14, ~0.03 at night (the day's ~0.5): a deep blue hour, a dark night. (Brighter - 0.42 at 6, 0.25 at 8 - the game at 21 h, the sun 7 degrees under, still showed a light blue sky:
// user, 2026-10-18.)
float SkyAdaptLog(float sunUpSine)
{
    static const float kDeg[] = {-20.0f, -18.0f, -16.0f, -14.0f, -12.0f, -10.0f, -8.0f, -6.0f, -4.0f, -2.0f, 0.0f,
                                 5.0f, 15.0f, 30.0f};
    static const float kLog[] = {1.2f, 1.22f, 1.375f, 1.56f, 1.78f, 1.96f, 1.98f, 2.23f, 1.74f, 1.16f, 0.72f,
                                 0.48f, 0.15f, 0.0f};
    constexpr int n = int(sizeof(kDeg) / sizeof(kDeg[0]));
    const float e = std::asin(std::clamp(sunUpSine, -1.0f, 1.0f)) * 57.29578f;
    if (e <= kDeg[0]) return kLog[0];
    if (e >= kDeg[n - 1]) return kLog[n - 1];
    for (int i = 0; i + 1 < n; ++i)
        if (e <= kDeg[i + 1])
            return kLog[i] + (kLog[i + 1] - kLog[i]) * (e - kDeg[i]) / (kDeg[i + 1] - kDeg[i]);
    return 0.0f;
}

float HalfToFloat(uint16_t h)
{
    const uint32_t s = uint32_t(h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    uint32_t bits;
    if (e == 0) {
        if (m == 0) {
            bits = s;
        } else {                                 // (subnormal)
            float f = float(m) / 1024.0f / 16384.0f;
            return (h & 0x8000u) ? -f : f;
        }
    } else if (e == 31) {
        bits = s | 0x7F800000u | (m << 13);
    } else {
        bits = s | ((e + 112u) << 23) | (m << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

constexpr uint32_t kRingRow = 62;               // (the sky view's row ~2 degrees over the horizon: v 0.575)
constexpr uint32_t kUpRow = 87, kZenithRow = kViewH - 1;   // (~35 degrees up; straight up)
constexpr VkDeviceSize kRingBytes = kViewW * 8;  // (RGBA16F, one row)
constexpr VkDeviceSize kSlotBytes = kRingBytes * 3;   // (a frame slot's: the horizon, 35 degrees up, overhead)


// The sky's colours (docs/sky-plan.md §1): how much of each time of day's keys by the sun's height - night, the blue
// hour, dusk (and dawn), day - each full at its knot and eased into the next, so they always add up to 1.
void KeyPhases(float sunUpSine, float w[4])
{
    static const float kKnot[4] = {-16.0f, -9.0f, -1.0f, 12.0f};   // (degrees: night, blue hour, dusk, day)
    const float e = std::asin(std::clamp(sunUpSine, -1.0f, 1.0f)) * 57.29578f;
    float n[4] = {};
    if (e <= kKnot[0]) n[0] = 1.0f;
    else if (e >= kKnot[3]) n[3] = 1.0f;
    else
        for (int i = 0; i < 3; ++i)
            if (e <= kKnot[i + 1]) {
                const float t = Smoothstep(kKnot[i], kKnot[i + 1], e);
                n[i] = 1.0f - t;
                n[i + 1] = t;
                break;
            }
    w[0] = n[3];                                  // (day, dusk, blue hour, night: the keys' order)
    w[1] = n[2];
    w[2] = n[1];
    w[3] = n[0];
}

// A key colour's ratio to its reference: both RRGGBB as seen on screen (gamma 2.2 - the sky's light shows about so:
// the physical 0C132B overhead by day shows as about 3D476D, user dump 2026-10-19), the ratio of their light. (A
// channel at 0 taken as half a step: a black key darkens, and no reference divides by 0.)
void KeyRatio(uint32_t key, uint32_t ref, float out[3])
{
    for (int c = 0; c < 3; ++c) {
        const int shift = 16 - 8 * c;
        out[c] = std::pow((float((key >> shift) & 0xFFu) + 0.5f) / (float((ref >> shift) & 0xFFu) + 0.5f), 2.2f);
    }
}

// The ratio in a direction (y up, a 0 away from the sun .. 1 towards it): atmos_view.frag KeyRatio.
void KeyRatioAt(const float r[4][3], float y, float a, float out[3])
{
    const float mid = Smoothstep(0.05f, 0.57f, y), top = Smoothstep(0.57f, 1.0f, y);
    for (int c = 0; c < 3; ++c) {
        const float low = r[3][c] + (r[2][c] - r[3][c]) * a;
        const float m = low + (r[1][c] - low) * mid;
        out[c] = m + (r[0][c] - m) * top;
    }
}

// The sky's light as an RRGGBB key (gamma 2.2, as KeyRatio reads them).
uint32_t Hex(const float rgb[3])
{
    uint32_t h = 0;
    for (int c = 0; c < 3; ++c)
        h = (h << 8) | uint32_t(std::pow(std::clamp(rgb[c], 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f + 0.5f);
    return h;
}

}  // namespace

// The sky's colours this frame as ratios to the physics (atmos_view.frag KeyRatio): overhead, 35 degrees up, low
// towards the sun, low away from it - the keys of the times of day the sun's height is between, blended.
void Device::SkyKeyRatios(float out[4][3]) const
{
    for (int i = 0; i < 4; ++i) out[i][0] = out[i][1] = out[i][2] = 1.0f;
    float w[4];
    KeyPhases(SunUp(), w);
    const uint32_t* k = m_skyColours.keys;
    // (Each time of day's keys: overhead, low towards the sun, low away - only dusk tells the two sides apart.)
    static const uint32_t kKeys[4][3] = {{kSkyDayTop, kSkyDayLow, kSkyDayLow},
                                         {kSkyDuskTop, kSkyDuskSun, kSkyDuskAway},
                                         {kSkyBlueTop, kSkyBlueLow, kSkyBlueLow},
                                         {kSkyNightTop, kSkyNightLow, kSkyNightLow}};
    float top[3] = {}, sun[3] = {}, away[3] = {};
    for (int p = 0; p < 4; ++p) {
        float r[3][3];
        for (int j = 0; j < 3; ++j) KeyRatio(k[kKeys[p][j]], kSkyKeyRef[kKeys[p][j]], r[j]);
        for (int c = 0; c < 3; ++c) {
            top[c] += w[p] * r[0][c];
            sun[c] += w[p] * r[1][c];
            away[c] += w[p] * r[2][c];
        }
    }
    // (35 degrees up: between overhead and the low sky's mean, as the ratios go - geometrically.)
    for (int c = 0; c < 3; ++c) {
        out[0][c] = top[c];
        out[1][c] = std::sqrt(std::max(top[c] * 0.5f * (sun[c] + away[c]), 0.0f));
        out[2][c] = sun[c];
        out[3][c] = away[c];
    }
}

// The sun's light through the air at a height (the sine of its elevation; atmos_common.glsl's air, the column over
// the ground x the Kasten-Young air mass): its colour at unit brightness, softened (a third of the reddening, in the
// log: through the whole air at the horizon nearly no green or blue is left - the land would go blood red; a third
// still turned the sand saturated orange at 3 degrees, so a fifth).
void Device::SunThroughAir(float up, float out[3]) const
{
    const float e = std::max(std::asin(std::clamp(up, -1.0f, 1.0f)) * 57.29578f, -0.5f);
    const float mass = 1.0f / (std::sin(e / 57.29578f) + 0.50572f * std::pow(e + 6.07995f, -1.6364f));
    const float mie = 0.5f + 0.5f * std::clamp(m_skyGlow, 0.0f, 2.0f), ozone = std::clamp(m_blueHour, 0.0f, 1.0f) * 1.2f;
    const float column[3] = {5.802e-3f * 8.0f + 4.44e-3f * 1.2f * mie + 0.65e-3f * 15.0f * ozone,
                             13.558e-3f * 8.0f + 4.44e-3f * 1.2f * mie + 1.881e-3f * 15.0f * ozone,
                             33.1e-3f * 8.0f + 4.44e-3f * 1.2f * mie + 0.085e-3f * 15.0f * ozone};
    for (int i = 0; i < 3; ++i) out[i] = std::exp(-0.2f * column[i] * mass);
    const float l = std::max(0.3f * out[0] + 0.59f * out[1] + 0.11f * out[2], 1e-4f);
    for (int i = 0; i < 3; ++i) out[i] /= l;
}

// What the game fogs in its fog colour, under our own sky: into its horizon instead (the ring's mean) - the rain's
// mist, the smoke, the cloud puffs (the scene's own draws fade into the ring towards each pixel: draw.cpp).
void Device::OurSkyFog(float rgb[3]) const
{
    if (!OwnSkyFog())
        return;
    float mean[3] = {};
    for (const auto& r : m_skyRing)
        for (int i = 0; i < 3; ++i) mean[i] += r[i] * 0.125f;
    for (int i = 0; i < 3; ++i) rgb[i] = std::min(mean[i], 1.0f);
}

// Our sky this frame and last: the sky pass may use the table (made at the end of the frame before).
bool Device::ProcSkyReady() const
{
    return m_procSky && m_atmosView && m_atmosViewFrame != ~0ull && m_atmosViewFrame + 2 >= m_frameNumber &&
           m_atmosView->m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

bool Device::OwnSkyFog() const
{
    return ProcSkyReady() && m_skyRing[0][3] > 0.5f && SunKnown();
}

// Once a frame, after the sky's model (outside the scene's rendering): the tables brought up to date.
void Device::UpdateAtmosphere(VkCommandBuffer cmd)
{
    if (!m_procSky || !m_hdr || !m_atmosViewPipeline || !m_atmosTransPipeline || !m_atmosMultiPipeline)
        return;
    auto make = [&](Texture*& t, uint32_t w, uint32_t h) {
        if (!t) t = CreateImage(w, h, Format::RGBA16F, 1, true);
        return t != nullptr;
    };
    if (!make(m_atmosTrans, kTransW, kTransH) || !make(m_atmosMulti, kMultiW, kMultiH) || !make(m_atmosView, kViewW, kViewH))
        return;
    // The air's tune: the Mie's amount by RVK_SkyGlow (the halo, the haze), the ozone's by RVK_BlueHour.
    const float tune[2] = {0.5f + 0.5f * std::clamp(m_skyGlow, 0.0f, 2.0f), std::clamp(m_blueHour, 0.0f, 1.0f) * 1.2f};
    if (!m_atmosReady || tune[0] != m_atmosTune[0] || tune[1] != m_atmosTune[1]) {
        float p[4] = {float(kTransW), float(kTransH), tune[0], tune[1]};
        FullscreenPass(cmd, m_atmosTrans, m_atmosTransPipeline, m_blackTexture->m_view, m_blackTexture->m_view,
                       m_linearSampler, p, sizeof(p), false);
        Transition(cmd, m_atmosTrans, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        p[0] = float(kMultiW);
        p[1] = float(kMultiH);
        FullscreenPass(cmd, m_atmosMulti, m_atmosMultiPipeline, m_atmosTrans->m_view, m_atmosTrans->m_view,
                       m_linearSampler, p, sizeof(p), false);
        Transition(cmd, m_atmosMulti, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_atmosTune[0] = tune[0];
        m_atmosTune[1] = tune[1];
        m_atmosReady = true;
        Log("sky: the atmosphere's tables made (Mie x%.2f, ozone x%.2f)", double(tune[0]), double(tune[1]));
    }
    // The horizon all round, from this frame slot's last copy (its commands done): eight colours, each the mean of a
    // 45-degree sector of the row just over the horizon.
    if (m_atmosRingCpu && m_atmosRingWritten[m_frameIndex]) {
        vmaInvalidateAllocation(m_allocator, m_atmosRingAllocation, kSlotBytes * m_frameIndex, kSlotBytes);
        const uint16_t* px = reinterpret_cast<const uint16_t*>(m_atmosRingCpu + kSlotBytes * m_frameIndex);
        // The sky over the land (the clouds' and the shade's light): overhead twice, 35 degrees up all round once.
        {
            float up[3] = {}, zen[3] = {};
            const uint16_t* rowUp = px + kViewW * 4, *rowZen = px + kViewW * 8;
            for (uint32_t x = 0; x < kViewW; ++x)
                for (int c = 0; c < 3; ++c) {
                    up[c] += HalfToFloat(rowUp[x * 4 + c]);
                    zen[c] += HalfToFloat(rowZen[x * 4 + c]);
                }
            for (int c = 0; c < 3; ++c) m_skyUp[c] = std::max((2.0f * zen[c] + up[c]) / (3.0f * float(kViewW)), 0.0f);
            m_skyUp[3] = 1.0f;
        }
        // The sky as the physics made it there (the sky's colours this slot's table was made with taken out): the
        // keys' references are measured from it (the "sky: keys" log).
        float (&applied)[4][3] = m_skyKeyApplied[m_frameIndex];
        if (applied[0][0] > 0.0f) {
            for (int c = 0; c < 3; ++c) {
                float up = 0.0f, zen = 0.0f;
                const uint16_t* rowUp = px + kViewW * 4, *rowZen = px + kViewW * 8;
                for (uint32_t x = 0; x < kViewW; ++x) {
                    up += HalfToFloat(rowUp[x * 4 + c]);
                    zen += HalfToFloat(rowZen[x * 4 + c]);
                }
                m_skyPhys[0][c] = std::max(zen / float(kViewW), 0.0f) / std::max(applied[0][c], 1e-3f);
                m_skyPhys[1][c] = std::max(up / float(kViewW), 0.0f) / std::max(applied[1][c], 1e-3f);
            }
            m_skyPhys[0][3] = m_skyPhys[1][3] = 1.0f;
        }
        const float sunH[2] = {-m_trueSunDir[0], -m_trueSunDir[2]};
        const float sunHL = std::sqrt(sunH[0] * sunH[0] + sunH[1] * sunH[1]);
        float nearest = -2.0f, farthest = 2.0f;
        // (The table's x: the azimuth -pi..pi, atan(z, x); sector k centred at k * 45 degrees.)
        for (int k = 0; k < 8; ++k) {
            float sum[3] = {};
            int n = 0;
            for (int i = -12; i < 12; ++i) {
                const float az = float(k) * 0.785398f + (float(i) + 0.5f) * (6.2831853f / float(kViewW));
                int x = int(std::floor((az / 6.2831853f + 0.5f) * float(kViewW)));
                x = ((x % int(kViewW)) + int(kViewW)) % int(kViewW);
                for (int c = 0; c < 3; ++c) sum[c] += HalfToFloat(px[x * 4 + c]);
                ++n;
            }
            for (int c = 0; c < 3; ++c) m_skyRing[k][c] = std::max(sum[c] / float(n), 0.0f);
            m_skyRing[k][3] = 1.0f;
            // (The low sky towards the sun and away from it, physical: the sectors nearest and farthest from it.)
            const float cosA = sunHL > 1e-4f ? (std::cos(float(k) * 0.785398f) * sunH[0] +
                                                std::sin(float(k) * 0.785398f) * sunH[1]) / sunHL : 0.0f;
            if (applied[0][0] > 0.0f && (cosA > nearest || cosA < farthest)) {
                float r[3];
                KeyRatioAt(applied, 0.039f, 0.5f + 0.5f * cosA, r);   // (the ring's row: ~2 degrees up)
                for (int s = 0; s < 2; ++s) {
                    if (s == 0 ? cosA <= nearest : cosA >= farthest) continue;
                    (s == 0 ? nearest : farthest) = cosA;
                    for (int c = 0; c < 3; ++c) m_skyPhys[2 + s][c] = m_skyRing[k][c] / std::max(r[c], 1e-3f);
                    m_skyPhys[2 + s][3] = 1.0f;
                }
            }
        }
        // (The sectors towards the sun hold its glow - the haze has its own glow towards it: as the air's colour they
        // washed the land and the clouds out. Each no brighter than 1.3 x the ring's median, its hue kept.)
        float l[8], sorted[8];
        for (int k = 0; k < 8; ++k)
            sorted[k] = l[k] = 0.3f * m_skyRing[k][0] + 0.59f * m_skyRing[k][1] + 0.11f * m_skyRing[k][2];
        std::sort(sorted, sorted + 8);
        // (... and none brighter than the game's own fog x RVK_SkyBright: the far land faded into our bright horizon
        // came out a glaring cream haze - user, 2026-10-18.)
        const float cap = std::min(1.3f * 0.5f * (sorted[3] + sorted[4]),
                                   std::max(0.3f * m_skyFog[0] + 0.59f * m_skyFog[1] + 0.11f * m_skyFog[2], 0.02f) *
                                       0.25f * std::clamp(m_skyBright, 0.25f, 4.0f));
        for (int k = 0; k < 8; ++k)
            if (l[k] > cap && l[k] > 0.0f)
                for (int c = 0; c < 3; ++c) m_skyRing[k][c] *= cap / l[k];
    }
    // The game's sky by day: its fog's brightness with the sun well up (eased; 0.7 until seen in this zone).
    const float fogL = std::max(0.3f * m_skyFog[0] + 0.59f * m_skyFog[1] + 0.11f * m_skyFog[2], 0.02f);
    const float h = SunUp();
    if (m_skyFogSet && h > 0.2f)
        m_skyDayFogL += (fogL - m_skyDayFogL) * (1.0f - std::exp(-m_frameDt / 10.0f));
    struct Push { float size[4], sun[4], moon[4], scale[4], zone[4], weather[4], night[4], dust[4], key[4][4]; } push{};
    static_assert(sizeof(Push) == 192, "atmos_view.frag's push constants");
    push.size[0] = float(kViewW);
    push.size[1] = float(kViewH);
    push.size[2] = tune[0];
    push.size[3] = tune[1];
    for (int i = 0; i < 3; ++i) push.sun[i] = -m_trueSunDir[i];
    push.sun[3] = 0.3f + std::max(m_frameEye[1], 0.0f) * 0.001f;   // (the eye's height, km: the ground's ~300 m up)
    const float adapt = std::pow(10.0f, SkyAdaptLog(h) + m_dark.skyLog);   // (RVK_Darkness lighter or darker: darkness.cpp)
    const float scale = std::clamp(m_skyBright, 0.25f, 4.0f) * std::max(m_skyDayFogL, 0.05f) / kZenithRef * adapt;
    for (int i = 0; i < 3; ++i) push.scale[i] = scale;
    push.scale[3] = std::clamp(m_twilight - 0.5f, 0.0f, 1.0f);   // (RVK_Twilight over 0.5: dusk more vivid)
    // The moon's light in the air (night.cpp): a faint glow round it.
    for (int i = 0; i < 3; ++i) push.moon[i] = -m_moonDir[i];
    push.moon[3] = scale * 0.004f * std::clamp(m_moonFade, 0.0f, 1.0f) * std::max(m_moonLight, 0.0f);
    // The zone's hue (its eased fog colour), RVK_SkyZone of it.
    for (int i = 0; i < 3; ++i) push.zone[i] = m_skyFog[i] / fogL;
    // (By day only: the zone's colour is its day sky's - kept through the blue hour and night, its teal turned the
    // night sky a light cyan.)
    push.zone[3] = m_skyFogSet ? std::clamp(m_skyZone, 0.0f, 1.0f) * Smoothstep(-0.07f, 0.05f, h) : 0.0f;
    // Weather: an overcast greys it, a storm darker, in the storm's own colour (StormHue: its green).
    {
        const float cloudy = CloudCover();
        const float storm = StormSky();
        float hue[3] = {0.5f + 0.5f * push.zone[0], 0.5f + 0.5f * push.zone[1], 0.5f + 0.5f * push.zone[2]};
        StormHue(hue);
        const float l = std::max(0.3f * hue[0] + 0.59f * hue[1] + 0.11f * hue[2], 1e-3f);
        for (int i = 0; i < 3; ++i) push.weather[i] = hue[i] / l;
        push.weather[3] = std::clamp(std::max(0.6f * Smoothstep(0.5f, 0.95f, cloudy), storm), 0.0f, 1.0f);
    }
    // The night's floor: airglow and starlight, a deep blue at a little of the game's night sky's brightness.
    {
        const float night[3] = {0.55f, 0.75f, 1.6f};   // (unit luma)
        const float dark = std::pow(10.0f, m_dark.skyLog);
        for (int i = 0; i < 3; ++i) push.night[i] = night[i] * fogL * 0.08f * dark;
    }
    push.night[3] = 0.35f * std::clamp(m_skyGlow, 0.0f, 2.0f);   // (the halo round the sun: RVK_SkyGlow)
    // A desert's dusty air (RVK_DesertAir): a pale, warm band along the horizon. (The haze over the land takes its
    // colour from this horizon: tonemap.frag HazeColour.)
    {
        float desert[2];
        DesertAir(desert);
        const float dusty[3] = {1.1f, 1.0f, 0.82f};   // (unit luma)
        for (int i = 0; i < 3; ++i) push.dust[i] = dusty[i];
        push.dust[3] = desert[0];
    }
    // The sky's colours (docs/sky-plan.md §1: RVK_Sky*), as ratios to the physics; kept with this frame slot to take
    // them back out of its readback.
    SkyKeyRatios(m_skyKeyRatio);
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 3; ++c) {
            push.key[i][c] = m_skyKeyRatio[i][c];
            m_skyKeyApplied[m_frameIndex][i][c] = m_skyKeyRatio[i][c];
        }
    FullscreenPass(cmd, m_atmosView, m_atmosViewPipeline, m_atmosTrans->m_view, m_atmosMulti->m_view, m_linearSampler,
                   &push.size[0], sizeof(push), false);
    // Its horizon row copied for the CPU (read back when this frame slot comes round again).
    if (!m_atmosRingBuffer) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kSlotBytes * kFramesInFlight;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bi, &ac, &m_atmosRingBuffer, &m_atmosRingAllocation, &info) == VK_SUCCESS)
            m_atmosRingCpu = static_cast<uint8_t*>(info.pMappedData);
        else
            m_atmosRingBuffer = VK_NULL_HANDLE;
    }
    if (m_atmosRingBuffer) {
        Transition(cmd, m_atmosView, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy regions[3] = {};
        const uint32_t rows[3] = {kRingRow, kUpRow, kZenithRow};
        for (int i = 0; i < 3; ++i) {
            regions[i].bufferOffset = kSlotBytes * m_frameIndex + kRingBytes * VkDeviceSize(i);
            regions[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            regions[i].imageOffset = {0, int32_t(rows[i]), 0};
            regions[i].imageExtent = {kViewW, 1, 1};
        }
        vkCmdCopyImageToBuffer(cmd, m_atmosView->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_atmosRingBuffer, 3, regions);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        m_atmosRingWritten[m_frameIndex] = true;
    }
    Transition(cmd, m_atmosView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_atmosViewFrame = m_frameNumber;
    if (m_frameNumber % 600 == 13 || (m_frameNumber % 60 == 13 && std::getenv("RVK_SKY_LOG")))
        Log("sky: horizon ring luma %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f, fog %.2f",
            double(0.3f * m_skyRing[0][0] + 0.59f * m_skyRing[0][1] + 0.11f * m_skyRing[0][2]),
            double(0.3f * m_skyRing[1][0] + 0.59f * m_skyRing[1][1] + 0.11f * m_skyRing[1][2]),
            double(0.3f * m_skyRing[2][0] + 0.59f * m_skyRing[2][1] + 0.11f * m_skyRing[2][2]),
            double(0.3f * m_skyRing[3][0] + 0.59f * m_skyRing[3][1] + 0.11f * m_skyRing[3][2]),
            double(0.3f * m_skyRing[4][0] + 0.59f * m_skyRing[4][1] + 0.11f * m_skyRing[4][2]),
            double(0.3f * m_skyRing[5][0] + 0.59f * m_skyRing[5][1] + 0.11f * m_skyRing[5][2]),
            double(0.3f * m_skyRing[6][0] + 0.59f * m_skyRing[6][1] + 0.11f * m_skyRing[6][2]),
            double(0.3f * m_skyRing[7][0] + 0.59f * m_skyRing[7][1] + 0.11f * m_skyRing[7][2]), double(fogL));
    if ((m_frameNumber % 600 == 13 || (m_frameNumber % 60 == 13 && std::getenv("RVK_SKY_LOG"))) && m_skyPhys[0][3] > 0.5f) {
        float w[4];
        KeyPhases(h, w);
        Log("sky: keys (sun %.1f deg; day %.2f dusk %.2f blue %.2f night %.2f): physical top %06X mid %06X low sun-side "
            "%06X away %06X; ratios top %.2f %.2f %.2f low %.2f %.2f %.2f", double(std::asin(std::clamp(h, -1.0f, 1.0f)) * 57.29578f),
            double(w[0]), double(w[1]), double(w[2]), double(w[3]), Hex(m_skyPhys[0]), Hex(m_skyPhys[1]), Hex(m_skyPhys[2]),
            Hex(m_skyPhys[3]), double(m_skyKeyRatio[0][0]), double(m_skyKeyRatio[0][1]), double(m_skyKeyRatio[0][2]),
            double(m_skyKeyRatio[2][0]), double(m_skyKeyRatio[2][1]), double(m_skyKeyRatio[2][2]));
    }
    if (m_frameNumber % 600 == 13)
        Log("sky: ours (sun up %.3f; brightness x%.0f, adapted x%.1f - the game's sky by day %.2f; zone %.2f; weather %.2f; "
            "moon %.4f)", double(h), double(scale), double(adapt), double(m_skyDayFogL), double(push.zone[3]), double(push.weather[3]),
            double(push.moon[3]));
}

void Device::DestroyAtmosphere()
{
    for (Texture** t : {&m_atmosTrans, &m_atmosMulti, &m_atmosView})
        if (*t) { DestroyTextureNow(*t); *t = nullptr; }
    m_atmosReady = false;
    m_atmosViewFrame = ~0ull;
    if (m_atmosRingBuffer) vmaDestroyBuffer(m_allocator, m_atmosRingBuffer, m_atmosRingAllocation);
    m_atmosRingBuffer = VK_NULL_HANDLE;
    m_atmosRingCpu = nullptr;
    for (bool& w : m_atmosRingWritten) w = false;
    for (auto& r : m_skyRing) r[3] = 0.0f;
    m_skyUp[3] = 0.0f;
}

}  // namespace rvk

