# Darkness: one setting for how dark the game gets

Status: plan, 2026-10-20. Built on branch `darkness` (phases 1-3); phase 4, the in-game tuning, open.

## Why

Players report the game too dark in some places, and there is no one control for it. How dark a frame ends up is
the product of a dozen separate factors, each tuned on its own:

| Where | What darkens | Setting |
| --- | --- | --- |
| `tonemap.frag` main | the darks and middle tones x0.62 at night (the sky x0.49) | `RVK_NightDark` 1 |
| `atmosphere.cpp` SkyAdaptLog | our sky ~10x dimmer at night than in the blue hour | none |
| `clouds.cpp` | the clouds at night x0.4 | `RVK_CloudNight` 0.4 |
| `night.cpp` | the game's night light faded out as our dim moon comes in | `RVK_MoonLight` 1 |
| `draw.cpp`, `rain.cpp` | under a storm, lit things' ambient x0.25, the ground's x0.5, the air x0.3 | `RVK_StormDark` 1 |
| `grading.cpp` NightByLight | indoors (no sun) a dim room counts as night: the night darkening applies | none |
| `tonemap.frag` Grade | the night tint, the night lookup table | `RVK_NightTint`, `RVK_NeonNight` |
| `hdr.cpp` | the exposure | `RVK_Exposure` |

They multiply. A night thunderstorm is the game's dim night ambient x0.25 (storm) x0.62 (night): near black. A dim
room with no sun gets the night's darkening on top of the game's own dim light. Each factor was right on its own
screenshot; together they go too far. Turning the night down (`RVK_NightDark`) does nothing for storms or rooms.

Two more things:

- **The ini keeps every setting.** `rvk_settings.cpp` Load writes every value back, so changing a default never
  reaches anyone who has run ao-vk before. A lighter default needs a migration.
- **Five different "how much is it night" ramps.** `m_sunPhase[3]` (sun 2 to 14 degrees down), the moon's (2 to 8),
  the clouds' (`-0.2 .. 0.03` in clouds.cpp and again in cloud_light.glsl Night()), the stars' (6 to 14),
  NightByLight (by the game's light and ambient). They were each tuned to look right, so they stay, but the darkness
  must hang off one of them.

The user's reference look (2026-10-20): the time **after the sun has set, before the night comes on**, the sun
about 2 to 8 degrees down. Then the land still has the game's dusk ambient, our night darkening has only begun
(x0.85-0.9), there is no moon yet, and the sky is at the peak of its adaptation (the blue hour). The new default
night should feel like that, a little darker.

## Design

### One setting: `RVK_Darkness`

`RVK_Darkness` (0..2, default 0.5 as planned; 0 since 0.7.0, the user's choice), in the Lighting section of the settings window, first entry: "Darkness: how dark
nights, storms and dim places get". The scale:

- 0: the lightest. No night darkening of ours, nights as light as the blue hour, storms half as dark.
- 0.5 (the default): about the blue hour's look, a little darker; nothing goes near black.
- 1: ao-vk's nights and storms as they were before this change.
- 2: very dark (the old `RVK_NightDark` 2, darker storms).

Every darkening factor reads it, in one place (`rvk/darkness.cpp`, `Device::UpdateDarkness`, once a frame after the
sun clock). The factors as functions of D = `RVK_Darkness`:

| Factor | D = 0 | 0.5 | 1 | 2 |
| --- | --- | --- | --- | --- |
| Night darkening in the tone mapping (the old NightDark strength) | 0 | 0.5 | 1 | 2 |
| Same, indoors (no sun known) | 0 | 0 | 0.5 | 1.5 |
| The sky at night, x | 2 | 1.4 | 1 | 0.56 |
| The clouds at night, x | 1.75 | 1.3 | 1 | 0.63 |
| The moon's light, x | 1.6 | 1.3 | 1 | 0.7 |
| A storm's darkening (StormOvercast), x | 0.6 | 0.8 | 1 | 1.4 |
| The brightness floor (below), display luma | 0.12 | 0.068 | 0.03 | 0 |

The sky's and clouds' factors fade in with the night itself (the sun from 8 to 14 degrees down), so the blue hour the
user likes is unchanged at every setting; only what comes after it is.

The finer settings stay as relative trims on top: `RVK_MoonLight`, `RVK_CloudNight`, `RVK_StormDark`, `RVK_NightTint`
keep their meaning (x what the darkness gives). `RVK_NightDark` is gone; `RVK_Darkness` takes its place.

### A brightness floor: the eye adapting

The multiplying factors are the problem; a cap on their product fixes it whatever the combination. Once a frame a
small compute pass (`scene_luma.comp`, after the scene's post passes, before the tone mapping) samples the HDR scene
on a 64 x 48 grid, leaves out the sky (depth at the clear value), puts each sample through the exposure and the
night darkening as the tone mapping will, and writes the log-average brightness of the land for the CPU (read when
the frame slot comes round again, as `sun_vis.comp`).

If that is below the floor for the darkness setting, the exposure is raised towards it: gain = floor / measured,
at most x2.5, eased (up over ~2.5 s, back down over ~1 s), as eyes adapt walking into the dark. Above the floor the
gain is 1: daylight, lit towns and normal nights are untouched. No world camera (login, loading) eases it back to 1.

It catches what the factors don't: a night thunderstorm, a dark cave, a dim basement, a zone whose night ambient the
game made very low. At D = 2 the floor is 0 (off): those who want it pitch dark get it. `RVK_DarkFloor` (ini only,
0..2, default 1) scales it for tuning; 0 switches it off.

A log line every 10 s: `darkness: setting 0.50, night 1.00 (indoors no); tone 0.50, sky x1.41, clouds x1.27, moon x1.30,
storm x0.80; land 0.041 (floor 0.068) -> gain 1.62`.

### The migration

`[Migrations] Darkness=1` in randy-vk.ini, once:

- an `RVK_NightDark` in the ini other than 1 (the player chose it): `RVK_Darkness` takes its value;
- else (the old default, or none): `RVK_Darkness` 0.5;
- `RVK_NightDark` is then removed from the ini, and from the presets' sections.

## Phases

1. `rvk/darkness.cpp`: the setting, the factors, the indoor rule, the migration; `RVK_NightDark` replaced. Demo
   `--darkness X` (`--night-dark` kept as its alias).
2. The floor: `scene_luma.comp`, its readback, the gain in the tone mapping's exposure; the log line; demo checks
   with a dark scene (`--water-night`).
3. CHANGELOG, README (`RVK_NightDark` -> `RVK_Darkness`), docs/rvk.md, night-plan.md pointing here.
4. In game: a clear night, a night thunderstorm, a dim room, the subway, a desert night, at D = 0, 0.5, 1, 2; the
   `darkness:` log's land brightness against the floor, to tune the floor's numbers.

## Open questions

- The floor's numbers are a first guess from the demo; the in-game log decides them (phase 4).
- Should the floor also hold by day (a dark interior at noon)? As built, yes: the floor doesn't know the hour.
- The night tint and the neon night grade are colour, not brightness, and stay as they are.
