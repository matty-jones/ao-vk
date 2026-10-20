# Sky pass: our own stars, the game's colours or ours (each editable), meteors, rainbows, auroras

Status: built 2026-10-19 (staged randy31-skycol1..8): the colour keys and presets, our stars, galaxy, shooting stars,
auroras, rainbows. The game's-colours mode (RVK_SkyColours 1) was built and taken out again (below). Open: in-game
tuning - the stars', galaxy's and aurora's brightness against a real night; a rainbow after real rain.

Built otherwise than planned:
- The keys are read as on screen (gamma 2.2: the physical 0C132B overhead by day shows as 3D476D in the user's dump).
- The game's-colours mode (RVK_SkyColours 1) is out. Modelling the game's sky from its draws' states read the
  desert's gold as black (the dome's texture, 204188, is a dark night sky; the tint's and layers' textures have no
  alpha and take their colour from states the model didn't follow). Reading its sky as drawn (the scene shrunk at
  the sky's end) read grey: the gold is the zone's fog colour (0.89 0.68 0.15 there), which the game lays over its
  far sky layers - and which our sky leaves off its own draws. Following the game's colours meant fogging the sky
  again; the user chose to leave the fog off the sky and keep the colour keys (tuned by hand, skypreset) instead.
- The stars are Gaussians of 0.9 pixel: at 0.65, read at the pixels' centres under TAA's jitter and the sky's turn,
  they flickered as if twinkling at RVK_Twinkle 0. Shooting stars come every ~25 s, where the camera looks.
- Frame dumps show each draw's fog (colour, start-end).
- The stars' magnitudes are compressed (10^(-0.25 m)) and scaled for the night picture (the tone map shows the night
  sky at ~0.3 of its light): at the physical -0.4 only magnitude 4 and brighter showed. A pixel's angle is measured in
  the shader from the next pixel's ray.
- The aurora is two folded curtains met analytically (where the ray meets each wall, its height there), not a 12-step
  march: the march stepped over the thin sheets and drew stripes.
- The rainbow is drawn in the tone map (sky depth only), not a pass of its own.
- The sky pass's push constants are 256 bytes (as the bloom layout's already).

Planned (2026-10-19). Builds on our own sky (`RVK_ProcSky`, docs/dusk-plan.md §8: rvk/atmosphere.cpp), the
nights (docs/night-plan.md) and the sky's weather (docs/sky-weather-plan.md).

## Context

`RVK_ProcSky` (rvk/atmosphere.cpp, built 2026-10-18) replaced the game's dome with a physical atmosphere. It looks
right physically, but:
- It has no stars of its own. The game's 500 additive points (one pixel each) are still drawn over it, faded by
  `RVK_StarFade`.
- Its colours are Earth's. The game's sky (its dome texture × the ambient, the time-of-day tint quad, the zone's
  horizon band) has very different hues. `RVK_SkyZone` only borrows the fog colour, and only by day.
- Its colours can't be tinkered with. They come from the physics plus a few strength knobs.

The user's decisions (2026-10-19):
- **AO colours mode**: keep our sky's light (sunsets, glow, smooth transitions) and recolour it with the palette the
  game would have shown, read from the draws we skip.
- **Editable colours**: `RRGGBB` values in `randy-vk.ini`, hot-reloaded, and settable from the console. They are
  hidden from the options window, so AOReloaded needs no change.
- **Procedural alien stars**: no Earth catalogue.
- **Extras, all four**: shooting stars, rainbows after rain, sky presets, auroras.

**Reference dumps** (user, 2026-10-19, 10 h, a desert, same spot; in `~/.wine-prk/drive_c/linux/logs/`):
- `rvk-frame-202109` is the game's sky:
  - The dome (304 vertices, a 512² texture, lit by ambient `0x717171`) gives an even warm gold: `#d8b75f` at the
    top of the screen, `#e0b459`, `#e9ab2b` just over the horizon, up to `#ffc65d` towards the sun.
  - Next come the stars (500 points), the moons, the tint quad, four sheets, the sun rays, and the band (121
    vertices).
- `rvk-frame-202113` is ours: `#3d476d` overhead, `#4f6189` halfway down, `#6c7c98` low, a pale haze at the
  horizon.
- These two dumps are the before/after check for `RVK_SkyColours` 1 at this spot: the result should read as gold, not
  blue, with our sun glow and gradient kept.

Step 0 of the work: copy this plan into `docs/sky-plan.md` (project convention), and set the stale status line in
`docs/night-plan.md` to "built (0.5.0); phase 5 tuning open".

## 1. Sky colours: two modes, every colour editable

**Key colours.** The sky's colour is defined at nine keys. Each is an `RRGGBB` setting, with names of 15 characters
or fewer:

| Setting | Where in the sky |
|---|---|
| `RVK_SkyDayTop` / `RVK_SkyDayLow` | day: overhead / the horizon |
| `RVK_SkyDuskSun` / `RVK_SkyDuskAway` / `RVK_SkyDuskTop` | sunrise and sunset: horizon towards the sun / opposite (the pink band) / overhead |
| `RVK_SkyBlueTop` / `RVK_SkyBlueLow` | blue hour: overhead / the horizon |
| `RVK_SkyNightTop` / `RVK_SkyNightLow` | night: overhead / the horizon |

**How they apply: as a ratio to the physics, not as a flat paint.**
- Each key has a built-in *reference* colour: what the physical sky gives there at default settings. These are
  measured once from the demo's sun sweep using a new log line (`sky: keys ...`) and baked in.
- The applied correction is `user / reference` per channel.
- With the defaults the ratio is exactly 1, so today's sky is unchanged, with all its physical detail (gradients,
  the glow, the earth's shadow) kept.
- A darker or bluer `RVK_SkyNightTop` darkens or blues the night overhead, including its brightness.
- **Weights** per pixel of the sky view: the phase from `m_sunPhase` (golden, afterglow, blue, night; day =
  1 − their sum), the elevation (top ↔ low over 0–60°), and, for dusk, the azimuth to the sun (towards ↔ away).
- The corrections are computed on the CPU into a small push block (9 RGB ratios plus the phase weights). They are
  applied in `atmos_view.frag` after the night floor and before weather. They stay out of the weather and dust terms,
  so overcast and storm greys stay greys.

**Mode: `RVK_SkyColours`** (Choice `0 1`, under `RVK_ProcSky`; 0 = ours, 1 = the game's). This setting is shown in
the options window.
- **Ours (0)**: the nine ini colours, as above.
- **The game's (1)**: the same correction path, but its targets come from the game's own sky this frame:
  - **Capture** (new `NoteGameSkyColour` in `rvk/sky.cpp`, called from `rvk/draw.cpp` at the three skip sites around
    line 2505):
    - **The dome** (the lit unblended mesh): for each vertex, its elevation from the mesh centre (`m_world`) and its
      UV → the texel from `Texture::m_thumb` (already a 16×16 grid for big textures) × the lit ambient (material
      ambient × `RS_AMBIENT` + emissive) → an 8-band gradient by elevation. This is cached by (vertex count, texture
      id, ambient), so it is recomputed only on change.
    - **The sky tint quad** (6-vertex strip, blend SRCALPHA/INVSRCALPHA): its vertex diffuse RGBA × its 8×8
      texture's mean alpha, blended over the gradient as the game blends it. In the reference dump its vertex colour
      is an opaque teal (`105 144 153`), yet the sky is gold, so its strength must come from that texture's alpha.
      `Texture::m_thumb` is RGB only, so a mean alpha is added next to `m_mean`.
    - **The horizon band** (121 vertices): its colour (material × texture mean `m_mean`) laid over the lowest bands.
  - **Eased** over about 3 s: the game steps its colours by the hour.
  - **Hue and brightness, separately**:
    - The hue is taken fully: `RVK_SkyGame`, 0..1, default 1.
    - The brightness is part ours, part the game's: `RVK_SkyGameLum`, 0..1, default 0.5. It is mixed in log space,
      and the ratio is clamped to ×0.25–4.
    - Why both: in the reference dumps the game's daytime sky is a bright gold (luma about 180/255) where ours is
      about 72. Taking the hue alone would make a dark brown-gold sky.
    - Our adaptation (`SkyAdaptLog`) still sets the brightness at dusk and at night.
  - **At dusk**: the game's hue is applied less towards the sun by the golden-hour weight, so the sunset's glow stays.
    This weakening is controlled by the existing `RVK_Twilight`.
  - **With no capture yet** (indoors, a new zone): falls back to the fog colour, as `RVK_SkyZone` does now.
  - **`RVK_SkyZone`**: only acts in mode 0 (in mode 1 the game's colour is the whole point).
- **No feedback loop**: the CPU reads the sky view back (horizon ring, 35° up, zenith) and uses it to fit the
  correction. That readback is of the corrected sky, so it is divided by the ratio applied that frame to recover the
  physical colour. Everything downstream (haze, far fog, mist, clouds, land shade: `m_skyRing`, `m_skyUp`) reads the
  corrected sky and so follows the palette for free.

**Settings plumbing** (`proxy/ddraw/rvk_settings.cpp`, `proxy/ddraw/rvk_console.cpp`):
- New `Colour` type = 4. The value is stored as a float (24-bit exact). The ini reads and writes it as hex `RRGGBB`.
- All nine colours go in `kHidden`, so `RvkSettings_*` never shows them to AOReloaded.
- `ParseValue` in the console accepts `RRGGBB` / `#RRGGBB` for a Colour setting. `list` and `changed` print hex.
- A new `SetSkyColours(...)` in `rvk/rvk.h` is wired where `SetSky` is now (rvk_settings.cpp around line 640).

**Sky presets.** New console command `skypreset [name]` writes the nine colours. With no name it lists the presets:
- `earth`: the references, i.e. the defaults
- `rubika`: teal days, violet dusk, green-black nights
- `alien`: magenta dusk, cyan blue hour
- `noir`: desaturated, deep nights

This follows the existing `preset` command's pattern (rvk_settings.cpp around line 1101) but is a separate table, so
the global presets don't touch the colours.

## 2. Our own stars (in the sky pass, `rvk/shaders/sky_glow.frag`, the "ours" branch)

**`RVK_Stars`** (Choice `0 1`, default 1 = ours, in the Sky section): 0 keeps the game's points. With 1, the game's
star point lists are not drawn: return early at `rvk/draw.cpp` around line 3149, where the sky's point lists are
recognised.

**The star field**, computed analytically per pixel, so no textures are needed:
- **Frame**: directions are taken into a star frame that turns about a tilted pole (an invented latitude about 35°).
  Its angle follows the sun clock's hour angle (from `m_trueSunDir`'s azimuth), so the stars wheel through the night
  in step with the sun and moons, and the same hour always gives the same sky.
- **Cells**: a cube-face grid at two densities, with a hash per cell giving 0 or 1 stars:
  - a position jittered inside the cell
  - a magnitude from a power law (many faint, few bright; about 3,000 visible at full dark)
  - a colour from a temperature of 3,000–12,000 K, via a small blackbody fit
- **Drawing**: each star is a Gaussian about 1.2 px wide in screen space. The pixel's angular size comes from
  `invViewProj`, so stars are the same at every FOV and resolution, and steady under TAA jitter (no sub-pixel
  flicker). Bright ones get HDR values, so bloom catches them.
- **Visibility** = `m_starsShown` (the existing `RVK_StarFade`) × extinction near the horizon (an analytic air mass)
  × a wash by the sky's own brightness (`star / (star + k·skyLum)`, so the moon's glow and the blue hour hide faint
  stars first) × (1 − overcast).
- **Clouds** are drawn after the sky pass, so they cover the stars on their own.
- **Twinkle**: each star's brightness flickers over time from its hash, more towards the horizon (`RVK_Twinkle`,
  0..1, default 0.5).
- **Galaxy band**: a great circle in the star frame. Its brightness is fbm noise with darker dust lanes, very faint,
  slightly warm at its core. `RVK_Galaxy` (0..2, default 1).
- **Settings**: `RVK_StarBright` (0.25..3, default 1, under `RVK_Stars`).
- **Push constants**: `sky_glow.frag`'s push block grows by `vec4 stars` (the frame angle, shown, twinkle, galaxy)
  and `vec4 night` (meteor, aurora). The `static_assert` in `DrawSkyGlow` (rvk/sky.cpp around line 439) is updated
  to match.

## 3. Shooting stars (`RVK_Meteors`, 0..2, default 1)

- **When**: on clear nights only, gated by the star visibility above. Meteors are scheduled from a hash of the sky
  clock (`m_skyTime` or the wall clock): about one every 40–90 s at 1.
- **On the CPU** (`rvk/sky.cpp`): it picks a start direction 20–70° up, a direction of travel, a length of 5–15°, and
  a duration of 0.3–0.8 s. It passes the active meteor (the head's direction and progress) in the push block.
- **In the shader**: the distance from the pixel's direction to the great-circle segment, with a bright head and a
  fading tail, tinted white-green. Drawn under clouds like the stars.

## 4. Auroras (`RVK_Aurora`, 0..2, default 1)

- **When**: some nights, chosen from the sky clock like the weather (`docs/sky-weather-plan.md`). Each night
  (90-minute knots) has an aurora chance of about 20% at 1, an intensity that wanders, and a heading. Only on clear
  nights: × (1 − cloudiness) × night × (1 − moon washout × 0.5).
- **Drawing** (`sky_glow.frag`): only for upward directions, and only when the push block's aurora strength > 0.
  - March 12–16 steps through a slab 100–250 km up (curved with the planet as in `atmos_common.glsl`).
  - The curtain: a 2D noise field of folded lines (domain-warped and drifting over time), extruded upwards with a
    falloff.
  - Colour by height: green low (557.7 nm), red-magenta on top.
- **Settings**: `RVK_AuroraCol` (Colour, hidden) for the green.
- **Cost target**: under 0.25 ms at 1440p when active, 0 when not (a uniform branch).

## 5. Rainbows (`RVK_Rainbow`, 0..2, default 1)

- **When**: the sun is between 0 and 42° up, it reaches the eye (`m_sunThrough` > 0.5), and the game's rain has
  stopped within the last ~3 minutes and is decaying. The state comes from the rain strength in `rvk/rain.cpp`.
  Strength fades in over about 20 s and fades out over about 2 minutes.
- **Drawing**: a small additive pass after the cloud layer (`DrawClouds` in `rvk/clouds.cpp`), only on pixels at the
  far plane (sky and clouds, not the land), so the bow shows in front of the clouds it forms in.
  - The angle from the anti-solar point: the primary at 40.6–42.4° (red outside).
  - The secondary at 50–53°, reversed and about 40% as strong.
  - Alexander's dark band between them darkened slightly, and the inside of the primary brightened slightly.
  - The spectrum from a small analytic fit.
  - It fades towards the horizon's haze and grows fainter as the sun rises.

## Order of work (each staged for the game: `randy31-sky1` … `sky6`, with a CHANGELOG entry)

1. **Colour keys**:
   - the `Colour` type, the ini and console hex handling, the nine colours, and the ratio path in `atmos_view.frag`
   - the `sky: keys` log; references measured from `--sun-sweep 30 -20 51` and baked in
   - check: defaults give a pixel-identical sky
2. **`RVK_SkyColours` 1**: capturing the game's dome, tint and band; easing; logging the captured gradient once per
   change; `RVK_SkyGame`.
3. **Stars**: `RVK_Stars`, the galaxy band, twinkle; the game's stars skipped.
4. **Presets and meteors**: `skypreset`, `RVK_Meteors`.
5. **Auroras**.
6. **Rainbows**.

Files mainly touched:
- `rvk/atmosphere.cpp`, `rvk/shaders/atmos_view.frag`
- `rvk/sky.cpp`, `rvk/shaders/sky_glow.frag`, a new `rvk/shaders/rainbow.frag` (added to the glslc list in
  `CMakeLists.txt`)
- `rvk/draw.cpp` (the skip sites and the star points), `rvk/rvk.h`
- `proxy/ddraw/rvk_settings.cpp`, `proxy/ddraw/rvk_console.cpp`
- `tests/rvk_demo.cpp`, `CHANGELOG.md`, `docs/sky-plan.md`, `docs/rvk.md`

## Verification

- **Build**: `cmake --build --preset linux-release`.
- **`tools/rvk-demo.sh`** with `--sky-dome RRGGBB --proc-sky 1`. `validation.log` should show only the known
  shutdown noise.
  - `--sun-sweep 30 -20 51` contact sheet (`tools/sun-sweep.py`):
    - step 1: identical to before (the colours at their defaults)
    - each preset: smooth, with no jumps between frames
  - New demo flags:
    - `--sky-colours 1` with `--sky-dome` (a fake dome: our sky picks up its hue)
    - `--stars` with a sun at −20°: a night shot showing stars, the galaxy, and an aurora forced with
      `--aurora 1`
    - `--rainbow 1`: forces the rainbow state, with the sun at 20° behind the camera
- **Cost**: the `gpu ms` log line. The sky pass with stars and galaxy should stay under +0.1 ms at 1440p; with an
  aurora, under +0.25 ms.
- **`tools/crash-check.sh`**. No `[Native]` change, so no calllog-ab.
- **In game**: stage each build per CLAUDE.md, then check:
  - `RVK_SkyColours` 0 and 1 compared at noon, 20.5 h (the teal spot) and night. Also at the reference spot at 10 h,
    with a new dump set beside `rvk-frame-202109` / `202113` and the same row samples taken from both.
  - `skynighttop 05081a` from the console, taking effect live
  - a forced night with `RVK_TimeOfDay`: stars wheel, are hidden behind clouds, and fade at dawn
  - rain stopping with a low sun behind you
