# Dusk to night: vivid skies, one smooth transition

Status: built 2026-10-18 (staged randy31-dusk1..3): the dusk log and sun clock, the light handover, the vivid sky and
blue hour, the land's shade and the blue-hour grade, the clouds' sunset colours, the stars' fade, the sunset rays.
Open: the game's own dusk measured (the `dusk:` log over a forced sunset - §0) and in-game tuning; the moons' fade
(§6) left out - an alpha-blended lit disc can't be faded by its colour, and the game lights them dimly by day itself.
§8 step 1 built 2026-10-18 (randy31-sky1): the three tables, the sky pass in the dome's place, the brightness fitted
by an adaptation table (atmosphere.cpp SkyAdaptLog - the demo's sweep: 0.6 at 4 degrees under, 0.25 at 8, ~0.04 at
night), the zone's hint, weather, the night's floor. Steps 2-4 built the same day (randy31-sky2..5): the game's
sky tint quad (6 vertices) and horizon band (121) left out too; the sky view's horizon, the sky 35 degrees up and
overhead read back each frame (FL.skyRing, FL.skyUp) for the haze, far fog, mist, clouds and the land's shade (no
CPU copy of the atmosphere needed); the sun's colour on the land from its air mass (SunThroughAir); the brightness
recalibrated twice against the user's game (kZenithRef 0.18, the user's RVK_SkyBright 0.25 of 0.045). Open: step 5,
tuning in game (midday, night, a green zone, overcast, storm), and the GPU cost.

## Context

The dawn round (docs/twilight-plan.md, staged as randy31-dawn9, still uncommitted on main) added a twilight sky model
(`rvk/shaders/sky_common.glsl`, `rvk/sky.cpp` UpdateSkyModel/DrawSkyGlow), golden low-sun light, haze, a golden-hour
grade and cirrus. Two things are still missing:

- **The colours are timid.** The palette is mostly peach and gold in a band about 6° tall. `RVK_Twilight` defaults to
  0.5, and the "clear" factor then halves it again: the log shows clear 0.54, so the effective twilight is 0.27 even on
  a "clear" sky. There are no real orange, red or pink skies, and the clouds keep the game's hue.
- **Day to night snaps.** The user sees all three: the sky's colour pops, the land's light switches, and the whole
  screen darkens at once. The audit found why:
  - `NightAmount()` (rvk/grading.cpp:92) follows the game's light × height and its ambient, eased only about 0.2 s,
    and drives the night grade, NightDark, the night LUT and the bloom threshold.
  - The sun→moon handover happens in one frame (rvk/shadow.cpp:1482). The moon shadows switch on at full strength and
    `FL.moon.w > 0.5` gates are hard. The game's upward night light dies within about 6 frames (night.cpp:146).
  - Our sun colour fades over only the last 1.15° (shadow.cpp:1481).
  - Narrow ramps: DawnLight's day factor (3.4°), the terrain shade (1.7°), the golden-hour rise, MornHaze, and the
    glow's direction switch at −5.7° in sky_common.glsl:69.
  - None of the sun's direction or colour is eased. The per-frame eases depend on the frame rate.
  - Nothing of ours covers the sky after −12°, so any step in the game's dome, fog or stars shows directly.

User decisions (2026-10-18): **vivid** colours (orange-red horizon, pink and magenta clouds, deep violet-blue after
sunset, the land tinted to match so nothing clashes, a strength setting to dial it down). Fix all three snaps. This
round includes **blue hour**, **lit cloud undersides**, **stars fading in** and **sun rays at dusk**.

This plan is saved as **docs/dusk-plan.md**. twilight-plan.md and night-plan.md point to it.

Before starting, I'll ask whether to commit the dawn round first, so this round's diff stays separate.

## 0. Measure the game's dusk (diagnostics first)

The game's own day/night curve isn't decompiled, so measure it:

- A `dusk:` log line, about once a second while `RVK_TimeOfDay` / `RVK_TimeSpeed` are forced (rvk_time.cpp), else
  every 600 frames. It records:
  - the game clock's hour
  - the game light's direction and colour, and the scene ambient
  - the fog colour
  - the dome's colour (vertex colour × texture average at the dome draw; draw.cpp `NoteSkyMesh` area)
  - the sun sprites' tints
  - the stars layer's alpha and colour
  - our eased sun height, NightAmount, moonFade and twilight share
- Run: Mutant Domain plus one teal zone, `RVK_TimeOfDay` at about sunset − 1 h, `RVK_TimeSpeed` about 60, through to
  full night. Look for steps in the game's dome, fog, ambient, light or stars, and their hours and sun heights.
- This decides how much of the game's sky our model must cover (§3) and when the stars switch on (§6).

## 1. One sun clock (rvk/sky.cpp; consumers across rvk)

A single eased description of where the sun is, which everything reads instead of computing its own ramps:

- `UpdateSunClock()`, once a frame before the shadow map:
  - Eases the true sun's direction (slerp, time constant about 1 s of real time). It snaps on a zone change or a jump
    over about 26°, as `m_moonPos` does.
  - Holds the last direction on frames without a directional light, instead of jumping to night (night.cpp:122 today).
- It derives continuous weights from the eased elevation, published in a new FL `vec4 phase` (internal.h +
  frame_lights.glsl; std140 sizes kept in step):
  - `day`
  - `golden` (+15° to 0°)
  - `afterglow` (+2° to −8°)
  - `blue` (−4° to −14°)
  - `night` (−6° to −16°)
- **NightAmount by the sun's height:** `night` × a zone factor, the zone factor being the game's ambient that says the
  night is dark. All of these lose their per-frame factors for real-time eases (`exp(-dt/τ)`, using `m_frameClock` as
  sky.cpp does):
  - `m_daylight`, `m_sceneAmbient` (draw.cpp:2486) and `m_moonFade` (night.cpp:127)
  - the result: the night grade, NightDark, the LUT, the bloom threshold and night glow ramp over game minutes, not
    0.2 s
- Rewire the narrow ramps to the phase weights:
  - sky.cpp DawnLight's day factor and golden-hour rise
  - hdr.cpp:731 MornHaze (the rising/setting flag eased, not binary)
  - terrain_sun.cpp:113
  - grass.vert:306
  - sky_common.glsl:69 (blend the two directions, no switch)
  - cloud_light.glsl Night() (reads `phase.night`)

## 2. Light handover without a pop (rvk/shadow.cpp, rvk/night.cpp, shaders)

- **Sun light:** fades from +1° to −4° by the eased height (replaces lowFade's 1.15°). It reddens as it goes (§4).
- **Moon light:** fades in from −4° to −12° (× its own height, as now), so the two never overlap. The shadow map
  switches to the moon at −4°, where both are 0, so its direction jump is invisible.
- **Moon shadows:** strength × `m_moonFade` (no full-strength shadows on the first frame). The hard `FL.moon.w > 0.5`
  gates become that weight:
  - ffp_main.glsl:270, 642, 803
  - water.frag
  - grass.vert
  - CasterFade on `FL.sunDir`
- **The game's upward night light:** `keep` follows the moon's fade-in (1 − moonFade, not 1 − 4·moonFade), so the
  land's light hands from the game's night light to the moon over the same span.
- **The air glow** (hdr.cpp:741) and the sand fog's light (hdr.cpp:1165) cross-fade sun → moon by the same weights.

## 3. Vivid twilight and blue hour sky (sky_common.glsl, sky.cpp)

`SkyTwilight` reworked into one continuous model from +15° down to −16°. Our pass now covers the sky through the whole
transition, so steps in the game's dome are hidden, and hands the sky back to the game at full night.

- **Sunset (+6° to −2°):**
  - the warm band taller (to about 25° up on the sun's side) and wider in azimuth
  - colours: gold → orange → red-orange at the horizon, salmon and pink above it, and a magenta-violet zenith
- **Afterglow (−2° to −8°):**
  - the horizon on the sun's side a narrowing orange-red line under a pink-purple arch
  - the earth's shadow and a stronger Belt of Venus opposite the sun
- **Blue hour (−6° to −14°):** the glow gone. Deep blue to indigo overhead, violet low, fading to the game's night sky.
  Alpha (the pass's share over the game's dome) eases from the twilight strength to 0 across −12° to −16°, while the
  model's colour converges on the measured dome colour, so the hand-back is seamless.
- **Less of the zone's colour at twilight's peak:** the zone hue kept falls from 0.3 to about 0.12, and returns by
  day and night, so teal and green zones keep their identity.
- **`clear`:** only overcast, storm and rain mute the twilight. Broken cloud no longer does, as the clouds cover the
  sky behind them themselves. Today "clear" is 0.54 at fair weather.
- **Settings:**
  - `RVK_Twilight` default 0.5 → 0.85, and its range to 1.5. Above 1 the palette is boosted rather than more
    replaced: the "dial down" knob.
  - New `RVK_BlueHour` (0..1, default 1, "Sky"): the blue hour's depth and length.
- The 600-frame `sky model` log line gains the phase weights.

## 4. Land and grade match the sky (sky.cpp dawn tints, lighting.glsl, ffp_main, tonemap.frag)

- **Sun tint by elevation:**
  - gold above 10°
  - orange 10° to 3°
  - red-orange in the last 3° and below the horizon (while its light fades)
  - roughly 85% → 70% of its luminance (`m_dawnTint[0]`)
- **The shade's tint from the sky model:** the upper hemisphere's average of `SkyTwilight`, computed on the CPU from
  the same formulas, or from 8 sampled directions in a small mirror. Pink-violet at sunset, blue in the blue hour,
  the zone's colour by day. It replaces the fixed bluish shade (`m_dawnTint[1]`).
- **Grade (tonemap.frag Grade):**
  - golden hour as now
  - a new blue-hour term: cooler, darks less saturated, by `phase.blue`
  - the night tint and NightDark now ramp in under it from the eased night weight
  - dusk → blue hour → night become one continuous grade

## 5. Lit cloud undersides (cloud_light.glsl, cloud_layer.frag, ffp_main MAT_CLOUDSHEET)

- **`SunLight()`:** the cloud's sun colour follows the sky model's glow colour (orange → red → crimson). By height, the
  clouds stay sun-lit after the ground's sunset:
  - the layer to about −4°
  - the cirrus to about −8°
  - lit from below (the light's direction raised to graze the base)
- **`Afterglow()`:** stronger, in the sky's pink and magenta. It peaks −1° to −6° and then decays to a dim violet in
  the blue hour.
- **`GameCloudHue`:** blended towards our palette by `RVK_Twilight`. The user had disliked pink clouds over gold land,
  but the land now matches.
- **New `RVK_Afterglow`** (0..2, default 1, "Sky", under RVK_CloudsOn): how strongly clouds catch the sunset.

## 6. Stars and moons fade in (rvk/draw.cpp sky draws)

- **The sky's additive star layers** (the blended sky draws at infinity, draw.cpp ~599 / ~2496; identified in §0):
  - their colour × a star visibility from the eased elevation (none above −6°, full by −14°) × (1 − cloud cover)
  - if the game switches stars on later than −6°, we can only fade them from when it does: the fade starts at the
    game's switch (measured)
- **The moon meshes** (night.cpp NoteSkyMesh): alpha × a smaller fade (a pale daytime moon kept at about 30%, full by
  the blue hour).
- **New `RVK_StarFade`** (Bool, default on, "Sky").

## 7. Sun rays at dusk (volume.frag / hdr.cpp volumetrics, sun.cpp)

- **Low-sun rays in the scene:**
  - CasterFade's 2.9°–9.8° cut-off makes way for a dusk path: shafts through gaps in the clouds and between hills
    with the sun from about 12° down to −2°
  - in the glow's colour
  - their strength × `phase.golden` / `afterglow`
- **Rays in the sky:**
  - a screen-space radial blur from the sun's screen point over the sky and cloud occlusion (the cloud layer's alpha
    plus scene depth = far)
  - drawn only while the sun is on or just off screen and between about +10° and −4°
  - it shows beams fanning from a low sun through cloud breaks
  - cost target: under 0.2 ms at 1440p (half resolution, 32 taps)
- **New `RVK_DuskRays`** (0..2, default 1, "Effects", under RVK_VolOn).

## 8. A procedural sky in place of the game's dome (round 2)

**Why.** The user's dump at 20.5 h (rvk-frame-175155, sun ~5 degrees under) shows it: our twilight is laid *over*
the game's sky, by a share. Under it, the dome stays the zone's bright teal: the fog colour is 0.31 0.90 0.85, and the
dome is one opaque textured mesh. That teal shows through the sunset and fights it. The sky has to be ours.

**What the game draws** (that dump, in order):
- draw 0: the dome, a lit 304-vertex mesh with a 512x512 texture, opaque, lit only by the ambient (0x515151)
- draw 1: the stars, 500 additive points, drawn at every hour
- draws 2-8: the moons, 49-vertex discs, alpha-blended and then added
- draws 9-12: the game's cloud sheets, four 584-vertex meshes round the eye
- draws 13-16: the suns' ray sprites
- draw 17: a 121-vertex blended band

Only draw 0 is replaced. Everything after it is still drawn over our sky, as now.

**User decisions (2026-10-18):**
- a physical atmosphere, with art controls on top
- a hint of the zone's colour, adjustable
- our sky all the time (day, dusk, night), with one switch back to the game's dome

### The atmosphere (`rvk/atmosphere.cpp`, new; `shaders/atmos_*.comp`, `shaders/sky.frag`)

After Hillaire 2020 ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique"), as small lookup tables:

| Table | Size | Holds | Made |
|---|---|---|---|
| Transmittance | 256x64 | how much light gets through the air, by height and angle | when the air's parameters change |
| Multiple scattering | 32x32 | light scattered more than once | when the air's parameters change |
| Sky view | 192x108 | the sky's colour in every direction, for this sun (and the moon at night) | every frame, ~0.05 ms |

- **The air:** Earth-like Rayleigh (blue), Mie (haze, the halo round the sun) and ozone (the blue hour's blue: ozone
  takes orange light out of a sky lit by a sun under the horizon). Rubi-Ka's planet radius is unknown; Earth's is used.
- **The sun:** the sun clock's direction (`m_trueSunDir`, eased), so the sky follows the sun's height smoothly and on
  its own: a blue day, then golden and red at the horizon, the Belt of Venus and the earth's shadow, a blue hour,
  then night. Our sunset palette no longer has to be hand-made.
- **Brightness:** the sky is in the scene's HDR units. Its daytime zenith is set to the brightness of the game's dome
  as it would be (the fog's brightness), x `RVK_SkyBright`, so the sky still sits with the game's lighting and
  exposure. That scale is eased with the fog (§3), so a zone change or a game step doesn't pop.
- **Night:** a sun 18 degrees under lights nothing. The night sky is then:
  - the moon's light scattered by the same tables (`m_moonDir`, × `m_moonFade`): a faint blue glow round it
  - a dim floor of airglow and starlight, deep blue, at about the game's night-sky brightness, measured from its fog
  - the game's stars drawn over it (faded in by `RVK_StarFade`)
- **The zone's hint** (`RVK_SkyZone`, 0..1, 0.25): the sky's hue pulled towards the zone's own (its eased fog colour),
  at the same brightness - more near the horizon, less overhead. Teal zones keep a teal cast; 0 is pure Rubi-Ka-as-
  Earth, 1 almost the game's colours.
- **Weather:** overcast and storms (`SkyCloudiness`, `StormSky`) turn it grey and darker, towards the game's own
  stormy colour (`StormHue`, the green storm), as the dome did. Rain and an overcast lower the contrast. The cloud
  layer covers it anyway.

### Drawing it

- The pass that draws the twilight now (`DrawSkyGlow`, `sky_glow.frag`) becomes the sky pass. It runs once a frame at
  the dome's place: it samples the sky-view table for each pixel and writes the sky opaquely (no alpha share) at the
  far plane. The glow round the sun disc comes from the Mie term, so `SkyGlow()` goes.
- **The dome's draw is skipped:** the sky's first draw, if it is a lit, unblended mesh (draw 0 above), identified as
  `NoteDuskSky` does. Logged once per zone with its vertex count and texture. Anything else stays the game's.
- **Without a dome in a frame** (indoors, missions), nothing is drawn.

### The rest reads the same air

- **The land's far fog, haze and mist:** `SkyHorizonDelta` takes the horizon's colour from the sky-view table (its
  bottom row) instead of the hand palette, so the land fades into the sky drawn behind it.
- **The clouds** (`DuskSkyTint`, `AirColour`, `SunLight` in `cloud_light.glsl`): their sky light from the table
  (straight up, and towards the sun), and their sunlight from the transmittance table at their height. That keeps
  them sunlit after the land's sunset, from the physics rather than by hand. `RVK_Afterglow` stays as a boost.
- **On the CPU** (`AtmosphereCpu`, the same few formulas; single scattering is enough there):
  - The sun's colour on the land: the sun clock's height through the transmittance (golden, orange, red),
    replacing the hand-made `m_dawnTint[0]`. `RVK_DawnLight` scales it.
  - The shade's tint: the upper sky's average colour (9 directions), replacing `SkyTwilightCpu` and the hand-made
    shade tint. This is the scattering plan's phases 3 and 4 done properly.
- **The tone map's `Air()`:** keeps its density, and takes its colour from the table's horizon. Full aerial
  perspective (a 3D froxel table) is a later step.

### Settings

| Setting | Section | Range | Default | What it does |
|---|---|---|---|---|
| `RVK_ProcSky` | Sky | Bool | on | our sky in place of the game's dome; off: the game's dome, with the dusk round's twilight over it |
| `RVK_SkyZone` | Sky | 0..1 | 0.25 | how much of the zone's own sky colour is kept |
| `RVK_SkyBright` | Sky | 0.5..2 | 1 | the sky's brightness against the game's |

The existing settings take physical meanings:
- `RVK_Twilight`: how vivid dawn and dusk are (a saturation and warmth boost in the sun's band, 1 = physical)
- `RVK_SkyGlow`: the Mie halo's strength
- `RVK_BlueHour`: the ozone amount (0: none, a grey-orange dusk)

All three keep their names, so `randy-vk.ini` values carry over.

### Order (each staged as `randy31-sky1`, `sky2`, …)

1. The tables, the sky pass and the dome skipped. A demo `--sky-dome` flag (a dome mesh like the game's), so
   `--sun-sweep` shows the sky.
2. Night (moon scattering, floor), the zone's hint, weather.
3. Horizon, haze, mist and clouds from the table.
4. The CPU model: the sun's colour and the shade's tint.
5. Tuning against dumps: midday, the user's 20.5 h teal spot, a green zone, overcast, storm, night.

### Verification

- **Sweep:** `--sun-sweep 30 -20 51` in the demo, with a dome. It should show a blue day, a gold and red sunset, a
  blue hour and night, with no jumps (`tools/sun-sweep.py`). Repeat with `RVK_SkyZone` at 0, 0.25 and 1.
- **Cost:** the tables and the pass together under 0.15 ms at 1440p (`gpu ms` line). The CPU model under 0.1 ms a
  frame, or else made every few frames.
- **Validation:** `validation.log` clean apart from the known shutdown leaks.
- **In game:** the same spot at 20.5 h, midday and night, with `RVK_ProcSky` 0 and 1 compared. A frame dump confirms
  the dome draw is skipped and nothing else is.

## Order of work, each staged (randy31-dusk1, dusk2, …) with a CHANGELOG entry

1. §0 diagnostics + §1 sun clock + §2 handover: fixes the snaps. Staged early so the user can watch a forced dusk
   with `RVK_TimeSpeed`.
2. §3 vivid sky and blue hour.
3. §4 land and grade.
4. §5 clouds.
5. §6 stars.
6. §7 rays.

New settings go in rvk_settings.cpp's table, all names ≤ 15 characters:

| Setting | Changed |
|---|---|
| `RVK_BlueHour` | new |
| `RVK_Afterglow` | new |
| `RVK_StarFade` | new |
| `RVK_DuskRays` | new |
| `RVK_Twilight` | default and range |

Update CHANGELOG `## Unreleased` (New / Changed / Fixed: "day turned to night at once") and docs/rvk.md.

## Verification

- `cmake --build --preset linux-release`, with no validation errors from `tools/rvk-demo.sh`.
- **A dusk sweep in the demo:** new `--sun-sweep A B N` in tests/rvk_demo.cpp renders N frames from elevation A to B
  (e.g. +12° → −18°, 61 frames at 0.5°) with `--atmos 1 --clouds`, and writes a contact sheet plus each frame's mean
  colour. Check:
  - **Smoothness:** no step between adjacent frames larger than about 3× the median step, in luminance or in hue.
  - **Look:** sunset, afterglow, blue hour and night at the default settings, and at `RVK_Twilight` 0 (the game's sky,
    unchanged from today).
- `tools/crash-check.sh`. No `[Native]` switch changes, so no calllog-ab.
- **In game:** the §0 `dusk:` log over a forced dusk (`RVK_TimeOfDay`, `RVK_TimeSpeed` 60) shows NightAmount, moonFade,
  the sun's weights and the grade moving smoothly, with no jumps between seconds. The user watches:
  - Mutant Domain sunset → night and dawn
  - a teal or green zone
  - overcast and rain (the twilight muted, nothing popping)
  - midday (unchanged)
- GPU cost from the log's `gpu ms` line: the sky pass, the cloud changes and the rays together under about 0.35 ms at
  1440p.
