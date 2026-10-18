# Dawn and twilight skies: glow, warm light, cirrus, golden-hour grade

Status: all steps built 2026-10-18 (staged randy31-dawn1..9): sky model and glow pass (RVK_Twilight, RVK_SkyGlow); haze, far fog, mist and low clouds in the sky's colours (SkyHorizonDelta), morning haze (RVK_MornHaze); low sun light (RVK_DawnLight: the ground's light pass tinted by where the sun reaches, not by its facing - by facing a 2 degree sun turned level desert grey); warm sun disc; golden-hour grade (RVK_GoldenHour); cirrus (RVK_Cirrus). Also storms: the air, the sky light on the land and the suns' glow dimmed under the deck. In-game tuning open.

Continued in docs/dusk-plan.md: the vivid palette, the blue hour, the smooth day-to-night handover.

## Context

The user's dawn dump (`C:\linux\logs\rvk-frame-022831.*`, Mutant Domain at 06:45, sky "clear") looks flat:

- **Sky:** only the game's dome, one olive colour from horizon to zenith. Nothing of ours touches sky pixels except
  the valley mist.
- **Sun:** Rubi-Ka has two suns. The one the game lights with is at y -0.025 and draws with tint 0. The one we see is
  at y 0.136 (about 8°), and its orange and blue sprites add up to white. sun.cpp only warms a sun below ~5°, so this
  one stays white-hot, with no glow in the air around it.
- **Ground:** the game's sun light is grey (0.55–0.64) and its ambient grey (`0x565656`). The dunes come out evenly
  grey-brown: no gold on the sunny side, no cool shade. Scattering-plan phases 3 and 4 (sunlight coloured by the air,
  ambient from the sky) are still open.
- **Haze:** weak (land haze ×0.10), and the same colour in every direction.
- **Clouds:** none at all on a clear sky (sheets ×0, layer cloudy 0), so nothing up there catches the dawn colour.

User decisions (2026-10-18):
- Sky: build a real twilight gradient, with a strength setting that blends from the game's dome to the full
  twilight palette.
- Clear skies get sparse high cirrus wisps.
- This round includes all four parts: sky glow and gradient, warm sun with cool shade, sun disc and morning haze,
  and a dawn colour grade.

Builds on docs/scattering-plan.md (phase 1 done: tonemap.frag `Air()`) and docs/sky-weather-plan.md. A new
**docs/twilight-plan.md** records this plan, and the scattering plan's phases 2–4 point to it.

## Shared piece: one sky model (`rvk/shaders/sky_common.glsl`, new)

Everything below reads the sky's colour in a direction from one function, so the sky, the land's haze, the far fog
and the clouds all agree (no seam where the dunes meet the sky).

- `SkyTwilight(vec3 dirW)`: an analytic twilight sky, driven by the true sun's height (`FL.sunTrue`) and fitted to the
  game's colours. The horizon starts from the game's fog colour and the zenith from the dome's colour (measured, see
  below). Then:
  - On the sun's side: a warm horizon band, wide in azimuth, peach to orange to red as the sun drops through about
    +6° to -6°.
  - Towards the zenith: darker and cooler (a blue-violet shift of the dome's hue).
  - Opposite the sun: the pink "Belt of Venus" band at about 5–15° up, with the earth's dusky shadow below it. Only
    with the sun between about -8° and +4°.
  - At midday it fades to a gentle gradient. Below -12° it is off.
- `SkyGlow(vec3 dirW)`: forward scattering round the sun (a narrow and a wide Henyey-Greenstein lobe, g about 0.85
  and 0.5) in the air-reddened sun colour, centred on the visible sun sprite's direction.
- `SkyHorizon(vec3 dirW)`: the twilight and glow colours along the horizon in that direction. This is the colour the
  haze and fog fade into.
- Parameters go in new `FrameLights` fields `sky0..sky3` (rvk/internal.h, mirrored in shaders/constants.glsl):
  - dome zenith and horizon colours, eased over seconds
  - twilight strength and glow strength
  - the visible sun's direction and its air-reddened colour
- The dome's colour is measured cheaply: the game's sky draw's vertex colours and texture average (noted in draw.cpp
  like `NoteSkyMesh`). Fallback: the fog colour lightened.

## 1. Sky glow and twilight gradient (new `rvk/shaders/sky_glow.frag`, `rvk/sky.cpp`)

- A fullscreen pass into the HDR scene, drawn once a frame right after the game's dome and before the suns, the
  game's sheets and our cloud layer. That way the clouds and suns sit on top of it.
  - Trigger: the first of these in a frame — the VisualSunRays draw (where `DrawSun` already goes, rvk/draw.cpp
    ~2447), a sky sheet, or the first non-sky draw — guarded by a `m_skyGlowFrame` frame number.
  - Depth test LEQUAL at the far plane, no depth write.
- Blend ONE, ONE_MINUS_SRC_ALPHA. Output rgb = twilight × a + glow and alpha a = `RVK_Twilight` × that pixel's
  twilight share. The dome is replaced by the twilight palette in proportion, and the glow adds on top.
- New pipeline next to the sun's (`m_sunPipeline` in sun.cpp shows the pattern). Add the shader to CMakeLists' glslc
  list, and `sky_common.glsl` to the DEPENDS lists.
- Settings (rvk_settings.cpp, "Sky"):
  - `RVK_Twilight` (0..1, default 0.5): how far the twilight palette replaces the game's dome (0: the dome as the
    game draws it).
  - `RVK_SkyGlow` (0..2, default 1): the glow round the sun and the horizon band.
- Log every 600 frames: the sun's height, the dome and fog colours, the twilight horizon and zenith colours.

## 2. Warm sun, cool shade (scattering phases 3 and 4)

- **The sun's colour:** in `RenderShadowMap` (rvk/shadow.cpp ~1495), `m_frameSunColor` is multiplied by the air's
  transmittance at the sun's height. Same curve as `sky_common`: golden from about 20° down, orange-red at the
  horizon, keeping about 70% of its luminance (the game's grey becomes gold, not dark). Through FL.sunColor this
  reaches grass, water, leaves, clouds and the air glow.
  - In `lighting.glsl` the game's directional sun light gets the same tint. A per-frame `sunTint` goes in the `sky*`
    fields, applied to the frame's sun light only, not to point lights.
- **Sky light in the shade:** the ambient is tinted towards the sky. The average of `SkyTwilight` over the upper
  hemisphere, as a colour at the ambient's own luminance: cool blue-violet at dawn, the zone's colour by day.
  - Lit draws: the `ambient` sum in lighting.glsl.
  - The terrain: the game draws it as a base pass times a lighting multiply pass, so its light is one baked colour
    per vertex. In ffp_main the multiply pass's colour L is split into the scene ambient a (`m_sceneAmbient`,
    already measured in draw.cpp ~2466) and a sun part s = max(L - a, 0). The result is
    a·skyTint + s·sunTint·TerrainSun: sun-facing dune faces gold, their backs cool. First verify with a dump that the
    multiply pass's colours sit at or above `RS_AMBIENT`.
- Setting `RVK_DawnLight` (0..1, default 1; 0: the game's own colours). Its effect is strongest with the sun under
  about 20° and faint at midday.

## 3. Sun disc and morning haze

- **Sun disc** (rvk/sun.cpp ~149, sun.frag):
  - Warm by the sprite's own height over a wider range (orange below about 12°, golden to about 25°). The orange
    sprite's tint leads, so the orange and blue pair no longer adds up to white.
  - With the true sun in twilight, the disc is softer and less glaring (no starburst spikes at dawn).
  - The glow round it now comes from `SkyGlow`.
- **Morning haze** (rvk/hdr.cpp `LandHaze`, tonemap.frag `Air`):
  - Density × (1 + `RVK_MornHaze` × dawn) as the sun rises from -6° to +15°, where dawn is 0..1; dusk gets half of
    it.
  - The light it scatters is `SkyHorizon` in the pixel's direction, replacing the flat haze colour: the far dunes
    glow warm towards the sun and go blue-grey away from it.
  - The game's far fog: in tonemap, c += (SkyHorizon(dir) − fogColour) × that pixel's linear fog share (from
    P.fog.x / P.fog.y). The game's fog end then matches the new horizon with no shader change in ffp.
  - The valley mist (`Mist`) is lit the same way.
- `RVK_MornHaze` (0..2, default 1, "Effects", under RVK_Atmos).

## 4. Golden-hour grade (tonemap.frag `Grade`, rvk/grading.cpp)

- A golden-hour factor from the true sun's height, at its peak for a sun between about -2° and 8°, at dawn and
  dusk. With it the highlights go warm and the shadows slightly cool (split by luminance), contrast rises a little
  (+0.06) and saturation a little (+0.05).
- Read from the FL `sky*` fields (the tonemap's push constants are already near 256 bytes).
- `RVK_GoldenHour` (0..1, default 0.5, "Colour grading").

## 5. Cirrus on clear skies (rvk/shaders/cloud_layer.frag, rvk/clouds.cpp)

- A thin 2D sheet high above the volumetric layer (about 5× `RVK_CloudHeight`). The view ray meets its plane, and
  the existing layer noise is sampled stretched along the wind into streaks, two octaves with a slow drift.
- Coverage: `RVK_Cirrus` × (0.35 + 0.65 × (1 − cloudiness)), so it is sparse and strongest when clear, and hidden
  behind thick layer cloud anyway.
- Lit by `SunLight` + `Afterglow` (cloud_light.glsl). Being high, it stays pink and gold for a while after the
  ground's sunset and before its sunrise. Hazed by `SkyHorizon` near the horizon. Composited behind the layer's own
  cloud (the march's transmittance).
- Limit: drawn only when the cloud layer pass runs, which is when the game draws its puffs (it does in this zone).
  Logged.
- `RVK_Cirrus` (0..2, default 1, "Sky", under RVK_CloudsOn).

## Order of work

1. sky_common + FL fields + the sky glow pass (§1), then tune against the dump in-game.
2. Haze, mist and far fog through SkyHorizon (§3 haze), so the land meets the new sky without a seam.
3. Warm sun and cool shade (§2).
4. Sun disc (§3), grade (§4).
5. Cirrus (§5).

Each step is staged for the game as CLAUDE.md asks (`randy31-dawn1.dll`, `dawn2`, …) and gets a CHANGELOG `New` entry
naming its setting.

## Verification

- `cmake --build --preset linux-release`.
- `tools/rvk-demo.sh` with `--atmos 1 --sun-dir` set to a dawn angle (e.g. `-0.99 -0.03 0.1`), `--sun-colour 0.6 0.6
  0.6`, `--clouds`, `--sky-time` for a clear sky. Compare PNGs at the sun at +20°, +5°, 0°, -4°, -10° with each
  setting at 0 and at its default. `validation.log` must be clean. If the demo's sky has no dome draw, add a
  `--sky-dome RRGGBB` flag to rvk_demo.cpp.
- `tools/crash-check.sh`.
- No `[Native]` switch changes, so calllog-ab is not needed.
- In game: the user checks this spot at dawn, plus midday (should stay close to today's look), dusk, night (nothing
  new should show), a teal or green zone, and rain or a sandstorm (twilight should give way to the storm's sky). A
  frame dump plus the 600-frame log lines confirm the fitted colours.
- GPU cost from the log's `gpu ms` line: sky glow pass and cirrus under about 0.15 ms together at 1440p.
