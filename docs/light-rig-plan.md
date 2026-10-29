# One light for day and night: the light rig

Status (2026-10-24, branch light-rig):
- Phases 0–2 are built and demo-tested, and look-check passes with the rig scenes' baselines at phase 2.
- Phase 3's first part (the ground's baked sky for lit things) is built, staged randy31-rig3.dll and liked in game
  ("looks great"); its demo numbers and baselines are still to come.
- Phase 4 (one land exposure) is built and demo-tested, staged randy31-rig5.dll; look-check baselines re-recorded
  at phase 4.
- Phase 5 (tuning in game, docs) to come.

## Why

Day and night are lit by separate means, each with its own fixes:

- **The ground.** Its sun and sky come from the game's hourly baked lightmap. By night that lightmap is dark and flat.
- **Lit things** (statics, characters). They get the game's directional light and its ambient. By night that light
  points *up* from under the ground; `night.cpp MoonScaledGameLight` fades it out while our moon is added.
- **Effects** (glints, light through leaves, highlights). They use our own `FL.sunColor`, which is gone 3° *above*
  the horizon.

What goes wrong:

- About eight shader branches flip from day to night in one frame on `FL.moon.w` ("the shadow map is the moon's") at
  about 2° under.
- From +3° to −2° there is no shadow light at all.
- About ten "how much is it night" ramps exist, and the night's darkenings multiply (darkness-plan.md).
- The ground and lit things take a storm through different formulas. In the user's dumps rvk-frame-183522 and -183647
  (a night storm in the desert), statics stood near black beside the ground.
- dusk-plan.md §2 promised weights in place of the switches; this was never done.

The user's choice, out of a physical rewrite, keeping the model as it is, and this: **one light rig** that every
surface reads. Its sun, sky and moon follow physical shapes by the sun's height. The game's lightmaps and light colours
stay as each zone's inputs. The night may look different: it is re-tuned, and the baselines are replaced.

## The rig (rvk.h `LightRig m_rig`, light_rig.cpp `UpdateLightRig`)

Once a frame, after the moon and the sky's model (sky.cpp UpdateEnvironment):

- **Physical shapes**, each relative to noon:
  - the sun on a surface facing it: the air's transmittance (`SunThroughAir`'s whole column), times its disc setting
    from +0.5° to −1°;
  - the sky's light on level ground: a table of typical clear-sky illuminance, from 20,000 lux at noon to 3 lux at 6°
    under and 0.0005 at night;
  - the moon: 2.7e-6 of the sun × `RVK_MoonLight` × `m_moonFade`.
- **The storm** takes the same share from each: direct light `× (1 − kStormSunTaken·o)`, the sky `× StormSkyLight()`.
- **The eye's adaptation:** `(land / noon) ^ −α`, eased over about 2 s in its log. `α = 0.9 − 0.05 × RVK_Darkness`.
  - Night to day is 1e-6 to 1e-8 physically, so α has to be near 0.9 for a night to show at a fifth to a tenth of the
    day. The first draft's 0.55–0.85 would have left it black.
- **Exposed, in the zone's units.** Each zone's day light is the unit: the game's sun luma, and its ambient taken back
  to noon by the sky's shape, measured while the sun is over about 25° and eased over 10 s. A zone not yet seen by
  day uses a default of sun 1 and ambient 0.4. From that:
  - `key`: the sun or the moon, whichever lights more, on a surface facing it;
  - `ambient`;
  - `keyShare`: the key's part of the light on level ground, which is what a shadow can take.
- **Pre-exposed, not exposed in the tone map.** The scene's half floats would not hold a night at a millionth of the
  day, so the adaptation is multiplied into the light before the scene is drawn.
- **The game now** is logged beside it: its sun luma (by night, its light from under the ground) and its ambient. The
  gains (rig over game) are what phases 2 and 3 apply.
- **Indoors** (no sun known): off. The game's light is used as it is.

Log: a "light rig:" line every 600 frames.

## Phases

0. **Measurement (done).**
   - rvk_demo `--game-day`: the game's own light by the sun's height, after its dusk log. The lightmap goes from 0.72
     grey to 303040 between +6° and −3°, the ambient from 0.37 to 0.29, and the light stays white, pointing up under
     the horizon.
   - rvk_demo `--rig-props`: a static box with the ground's texture beside the ground. `rig-props.txt` gives the
     screen rects of its face, its top and the ground beside it.
   - rvk_demo `--indoor`.
   - tools/sun-sweep.py: the face/ground and top/ground ratios per shot, their spread (flagged over 1.5×), and `--csv`.
   - tools/look-check.py: the scenes `sweep`, `dawn`, `nightprops`, `nightstorm` and `indoor`, with `prop-*` regions.
   - To do once the game isn't running: record their baselines with the current code (`--update` for the new scenes
     only), and run the sweep to get the old model's numbers.
1. **The rig computed and logged (done, randy31-rig1).** The look is unchanged. Check in the logs that the gains
   change smoothly over a dusk and a zone change, with no NaNs indoors.
2. **One key light, the gap closed** (built). What it became, narrower than first planned:
   - **The shadow map's choice of light is unchanged.** Both maps already fade to nothing at the swap
     (`ShadowStrength` by the sun's height, the moon's by its fade).
   - **The pops were the switches' non-shadow terms.** By day the ground's lightmap took the clouds and the storm
     through `FlatShade`; by night none of it, so a cloudy or stormy land brightened as the moon's map came in. The
     unlit block and grass.vert were the same.
   - **Now one formula, for the ground pass, the unlit block and grass.** The sun's parts (things' shadows, the
     clouds', the storm taking the sun) are weighted by `FL.cond.y` = the rig's `sunShare`: the sun's share of the
     land's light, physical, over noon's. The storm's dimmer sky applies at every hour.
   - **The hill shade no longer goes out over the sun's last 3°** (terrain_sun.cpp, rig on). `sunShare` takes it
     away. Before, ground in a hill's shade brightened 70% at sunset.
   - **Lit things:** the game's light from under the ground is gone, and its sun is faded out by the air and its
     disc's setting below 10° (`RigGameLight`, light_rig.cpp). The moon is added as before.
   - `RVK_LightRig` off: the old branches, exactly.
   - **Measured** with the sweep lit as in game (`--pixel-lighting --light-override --shadows`; the look-check rig
     scenes now pass those, `LIT`):
     - the face's two jumps at sunset (19× and 34× the median step) and the ground's 70% one are gone;
     - in a night storm, face/ground goes 1.69 → 0.84 and top/ground 1.96 → 0.98 (clear night 0.50 / 0.74);
     - indoors unchanged.
   - **Left for phase 3:** by night statics get the ambient alone and the ground gets lightmap + ambient, so
     face/ground sits at about 0.5 (day 1.4–1.9).
   - The first plan for phase 2, kept for reference:
   - The rig into `FrameLights`, 64 bytes: key dir + shadow weight, key rgb + keySun, sky rgb + storm, gains +
     keyShare.
   - Each `FL.moon.w > 0.5` branch becomes a weight:
     - ffp_main.glsl: the ground pass's moonShade/texShade split, `sunScale`, GroundAway/TerrainSun, and the unlit
       `!shadeSun` block, which becomes `current *= mix(1, shade, keyShare)`;
     - grass.vert, sand_fog.frag, the native_ground light fit, hdr.cpp's air glow and sand-fog moon.
   - The shadow map follows the key, faded by its weight rather than stopping between +3° and −2°; the moon's map
     starts once the moon lights more than the sun.
   - On statics, the rig's key replaces the game's sun, keeping its hue. `MoonScaledGameLight` and the game's light
     from under the ground go.
   - Do not reuse `m_shadowIsMoon`: it is already true in the gap, when no map is drawn.
   - `RVK_LightRig` (bool, 1; 0 the old path for A/Bs, kept for one release).
   - Check: no step flags from 6° to −10° on `sweep` and `dawn`.
3. **One sky and storm term.** Built first, simpler than planned:
   - **The ground's baked sky** (light_rig.cpp): the lightmap round the camera, read from the ground grass's
     capture (`GroundAt`, the eye's cell and a ring 5 units out, eased over ~2 s). Its sky part is all of it by night;
     by day, what the sun's physical share of the light on level ground leaves; in a storm, × `StormSkyLight`. It
     goes to the shaders as `FL.rigSky`.
   - **Lit things take it** (ffp_main.glsl, after AccumulateLights), by how far they face up: `0.5 + 0.5 n.y`.
     Not the ground, not the sky.
   - **Without grass there is no capture**, so lit things get nothing extra. The look-check rig scenes now run with
     `--grass-field` for this.
   - Still open from the first plan: the gains on the game's sun and ambient, and the native ground's split.
   - The first plan for phase 3:
   - `skyGain` on lit things' ambient and on the ground: the lightmap split into sun and sky by the native ground's fit
     `a + b·n` and `NativeSunShare`, each part times its gain.
   - `FlatShade` reads the storm and `keyShare` from the rig. The `moonNight` storm blend in draw.cpp (the
     randy31-stormnight fix) goes.
   - Check: face/ground within 1.5× over 15° to −22°, and between `nightprops` and `nightstorm`; `indoor` unmoved.
4. **The exposure curve.** Built, open-loop rather than measured:
   - **The land's exposure** (`LightRig landGain`, light_rig.cpp) is the target over the game's light. The target is
     the clear air's sun, sky and moon light over noon's, to the power 1 − α. The game's light is its lightmap round
     the camera plus its ambient, over the zone's noon (taken while the sun is over ~25°, and only once the zone has
     been seen steadily for 3 s: in its first frames the ambient is whatever draw set it first). It is clamped to
     0.2–1.5 and eased over 1.5 s.
   - **Storms are left out of both sides:** their darkness is in our lighting, and a measured, closed loop would
     have adapted it away.
   - **It drives tonemap.frag's old night step** (darks and middle tones down, the bright kept, the sky a little
     more), passed as a negative `grade2.w`. The darkness floor's gain is off under the rig.
   - **α = 0.935 − 0.03 × RVK_Darkness**, tuned so a moonlit game night lands where the old darkening put it:
     ×0.94 / 0.62 / 0.42 at RVK_Darkness 0 / 1 / 2, against the old 0.62 at 1. Moonless nights come out darker.
     In the demo's game-like night, land and sky match the old look within ~10% at all three settings.
   - **Measured, the sweep with grass** (look-check `sweep`): the land's sunset step (18× the median step) and the
     face's (21×, 37×) are gone; top/ground spreads 1.58× over the day (3.6× before).
   - **Kept as they were:** the sky's own adaptation (SkyAdaptLog), the clouds' night factors. So the sky and the
     clouds keep their tuned look, not yet one exposure with the land (open).
   - The first plan for phase 4:
   - Retired as darkenings: the blanket factor in tonemap.frag `nightDark` (it becomes a mild "brights kept" step of
     at least 0.9); `m_dark.skyLog`, `.clouds` and `.moon`; `RVK_CloudNight`.
   - The sky takes the rig's adaptation (atmosphere.cpp, its `pow(10, SkyAdaptLog + skyLog)`).
   - The darkness floor (`scene_luma.comp`) stays as a safety net, its most gain 2.5 → 1.5.
   - The night tint, blue and golden hours and the lookup tables stay as colour.
   - `RVK_Darkness` now sets α: a `kMigrations` entry resets it.
   - Check: `night0` / `night05` / `night2` ordered, no steps in the sweep, the floor's gain ≤ 1.2 on ordinary nights.
5. **Re-tune in game, then docs.**
   - Dumps at dusk, night, a night storm and dawn.
   - α and the moon's share tuned against the user's reference, the sun 2–8° under (darkness-plan.md).
   - dusk-plan.md §2 marked done; docs/settings.md regenerated; CHANGELOG (Changed: day and night share one light, the
     night looks different, `RVK_CloudNight` removed); the new baselines committed.

## Open questions

- **A zone first seen at night** has no day reference, and the default (sun 1, ambient 0.4) may be off for dim zones.
  Take it from the night ambient × the usual day/night ratio?
- **The game's own (not native) lightmaps** have no `a + b·n` fit. Fit each chunk on upload, or use one fraction per
  zone?
- **Scripted or odd game light** (coloured suns, fake ones) could push the gains around: clamp them to [0.25, 4], and
  ignore directional lights whose hue is far from the sun's.
- **Self-lit things** (lamps, signs, the game's unlit effects) are not lit by the rig, so the adaptation does not lift
  them at night. Right for lamps (they stand out of the dark); to check for effects drawn at the game's brightness.
- **PBR highlights** follow the rig's sun from phase 2: watch them for steps.
