# Changelog

What changed in each ao-vk release, newest first. Downloads and install notes: the
[releases page](https://github.com/wannabuh/ao-vk/releases).

## Unreleased

### New
- The ground is drawn by ao-vk itself (`RVK_NativeGround`, on): every patch of land shows its own textures out to the
  horizon instead of the game's blurry baked pictures past a short ring, and walking no longer makes the game rebuild
  the ground (no stutter, less memory). Cliffs, rock, shadows, mist and wet ground work on it as before, and texture
  packs' ground maps too. Turn it off to get the game's own ground back.
- On that ground, valleys, cliff faces and the feet of canyon walls are darker where less of the sky reaches them
  (`RVK_GroundAo`, 0.6; 0 for none). Canyon walls no longer show the dark and green triangles of the ground's
  coarse light and tile colours, and the grass grows on it as before.
- Up close, that ground's hills and the feet and rims of cliffs are rounded instead of made of flat 4 m triangles
  (`RVK_GroundTess`, 0.6; 0 for the game's shapes), and the rock on cliff faces stands out of them in ledges and
  cracks (`RVK_CliffDepth`, 0.4; 0 for flat faces).
- On that ground, hillsides and cliffs take their light from where the sun is now, so slopes brighten and fall into
  shade as it moves (`RVK_GroundSun`, 1; 0 for the game's light as baked for the hour), and when the game moves to the
  next hour's light the land fades into it over a few seconds instead of changing all at once.
- Rocks and boulders lie at the feet of cliffs and canyon walls, and stones on rough slopes, in the colour of the
  rock around them and lit and shadowed like the ground (`RVK_GroundRocks`, 1; 0 for none, up to 2 for twice as
  many). Meadows, roads and towns are left clear.
- The ground at your feet has fine grit in it instead of a soft smear of its texture (`RVK_GroundGrain`, 0.6; 0 for
  none).
- Different ground textures side by side (gravel beside dirt, where the game has no transition tile) blend into each
  other along a ragged edge instead of meeting in a hard straight line along the ground's grid (`RVK_GroundBlend`, 1;
  0 for the hard edges). Roads and the game's own transitions stay as they are.
- On the ground drawn by ao-vk, cliffs' rock no longer starts along a row of straight-edged teeth under their rims
  (the ground's triangles); it fades in along a soft, wandering line.
- The feet and rims of cliffs that run at a slant are smooth slopes instead of a row of sharp triangular teeth.
- Texture packs' material maps for ground tiles (shine, wet look, crevices; `RVK_Pbr`) work on the ground drawn by
  ao-vk too, tile by tile as on the game's own ground.
- Distant ground is less blurry: the pictures the game bakes for the land past its detailed ring get twice the
  detail (`RVK_GroundFar`: 16 texels a cell by default; 8 for the game's, 32 for more at a large memory cost).
- The ground keeps its sharp, detailed texture further out (`RVK_GroundRange`, 1.5x the game's furthest setting by
  default, up to 2.5 - beyond 2 the game stutters as you move; 1 for the game's own). Past the game's limit the land turned into a blurry baked picture a
  short way from you.
- Cliffs and canyon walls show layered rock instead of long vertical streaks: steep faces get a rock texture in the
  colour of the ground around them, with its own relief (`RVK_CliffRock`), instead of the ground's texture stretched
  down them (`RVK_CliffTex`; with `RVK_CliffRock` off, the ground's own texture at its true size), without the same
  picture repeating in rows (`RVK_TileHide`). Large areas, cliffs included, get soft lighter and darker patches
  instead of one flat colour (`RVK_GroundVary`, 0 to 2). All three are on by default and are in the new
  Terrain section of the options; flat ground keeps the game's own tile layout.
- You see further: the game's view distance x1.5 (`RVK_ViewRange`, 1 for the game's own, up to 3), and distant land
  and mountains are kept and fogged by their true distance. Before, the game cut them off along a flat plane in front
  of the camera, so far mountains popped in and out as you turned.
- Rocks and shrubs are shown further out (`RVK_DetailRange`, 2x the game's by default; 1 for the game's own).
  They used to vanish about 150 m away while their shadows stayed on the ground.
- AI missions' PF map shows its rooms black on a green background (`RVK_AlienMap`, on by default). The map
  had become black on black, so you couldn't tell which room you were in.
- Dawn and dusk skies: a warm glow round the sun and along the horizon under it, a cooler sky overhead and a pink
  band opposite the sun around sunrise and sunset, in place of the game's one flat colour. `RVK_Twilight` sets how far
  these colours replace the game's sky (0.5 by default, 0 for the game's), `RVK_SkyGlow` the glow (1; 0 for none).
  Clouds and rain mute them. The distant land, the haze, the valley mist and the clouds low in the sky take on the
  same colours, so the hills fade into the sky behind them.
- Our own sky in place of the game's (`RVK_ProcSky`, on): a physical atmosphere that follows the sun - a blue day
  sky, gold then red and pink at sunset with the pink band and the earth's shadow opposite, a violet and then cobalt
  blue hour, a dark night under the game's stars and moons. The game's bright zone colours no longer fight the
  sunset; `RVK_SkyZone` keeps a hint of each zone's colour by day (0.25; 0 none, 1 mostly the zone's), `RVK_SkyBright`
  sets its brightness and the far haze's (1). Overcast and storms grey it. The distant land, the haze and the clouds
  fade into its horizon, the clouds and the shade take the colour of the sky over them, and a low sun's light is
  coloured by the air it passes through.
- Vivid sunsets and sunrises: the horizon under the sun goes gold, then orange and red, with pink above it and a
  violet sky overhead; after the sun has set, a red line on the horizon under a pink-purple arch. `RVK_Twilight` now
  defaults to 0.85 and goes up to 1.5 for stronger colours. Only an overcast, storms and rain mute them now, not a
  few clouds.
- Blue hour: after sunset (and before sunrise) the sky deepens to blue and indigo before the night's sky takes over,
  and the picture turns cooler with it (`RVK_BlueHour`, 1; 0 for none).
- Clouds take the sunset's colours: gold and orange, then red and pink, lit from below for a while after the sun has
  gone from the land, then violet (`RVK_Afterglow`, 1; 0 for the game's hues, 2 stronger). The land's shade takes the
  sky's pink-violet tint at the same time, so the land and the sky match.
- Stars come out slowly as the sky darkens after sunset, and fade before sunrise (`RVK_StarFade`, on).
- Sunset rays: with the sun low, beams of its light between the hills and through the gaps in the clouds
  (`RVK_DuskRays` under volumetric light, 1; 0 for none).
- Morning haze: the air is thicker round sunrise, and a little round sunset (`RVK_MornHaze`, 1; 0 for none).
- Low sun light: round sunrise and sunset the land, grass, water and everything the sun reaches turn golden to
  orange, and the shade a little cooler, instead of the game's grey light (`RVK_DawnLight`, 1; 0 for the game's
  colours, 2 stronger). Cloud and storms mute it.
- The sun's disc turns golden below about 25 degrees and orange near the horizon, instead of staying white-hot.
- Golden-hour colour grade round sunrise and sunset: warmer light, slightly cooler shade, a touch more contrast
  (`RVK_GoldenHour` under Colour grading, 0.5; 0 for none).
- Thin high wisps of cloud, most on clear days, that catch the light round sunrise and sunset (`RVK_Cirrus`, off by
  default: under the new sky they showed as coarse streaks; 1 to try them).

### Fixed
- Day turned to night all at once: the picture darkened and took on its night colours within a second, the land's
  light switched to the moon's with its shadows at full strength, and the hills' shade and the low sun's warm tint
  went out together. The change from dusk to night now follows the sun as it sinks, over several minutes of game time.
- Under a storm the land brightened the moment night fell.
- Replacement textures from `materials\` or texture packs whose alpha channel isn't see-through-ness (the bank's
  front plate, where it is a glow mask) showed black where that alpha was 0.
- Free camera (Ctrl+Shift+C): things at the edges of the screen vanished when looking a different way from the game's
  camera, and hills hid what was behind them even when flying high above.
- The ground and its grass lit up and darkened as you turned the camera: the sun was added on top of the ground's
  baked-in sunlight whenever only a few lamps were about. They now stay at the darker, intended level. Without pixel
  lighting this was the case everywhere; with it, the grass still had it.
- Ground grass stayed bright under a thunderstorm that darkened the land around it.
- Ground grass grew up through tree trunks, roots and rocks. It now stops at their base.
- Ambient occlusion showed through fog as dark smudges in corners, most visibly in AI missions' thick fog. It
  now fades with the fog.
- Distant land and valley mist glowed white towards a sun hidden behind thick cloud or a storm. The haze's
  glow towards the sun now dims with the light the clouds let through.
- Under a thunderstorm the land stayed lit as if under a clear sky, and the distance, trees, the fog over the
  water and the rain mist stood pale under the dark green sky (distant trees paler still than the ground under
  them). The sky's light on the ground, grass and everything else now dims with the storm too, and the air fades the
  far land into the storm's own sky. `RVK_StormDark` sets how much.
- The suns still showed as a wide bright halo through dense cloud and storms. Their glow over the clouds
  (`RVK_SunBleed`) now fades with the light the clouds let through, gone under a full cloud deck.

### Diagnostics
- `RVK_HeapCheck` (in `randy-vk.ini` only): checks the game's memory for corruption at points each frame and logs the
  first place it is found broken - to track down a crash in the ground grass (1 cheap, 2 thorough and slow).
- With the sun near the horizon, `randy-vk.log` gets a `dusk:` line every couple of seconds (the game's light, ambient,
  fog, sky dome and stars, and ours), and a `time:` line with the hour while `RVK_TimeOfDay` / `RVK_TimeSpeed` run
  the clock: watch a sunset go by and see what changes when.

## 0.6.1 - 2026-10-18

### New
- **In-game console** for the renderer's settings: press the key left of 1. Change any setting by name
  (applied at once and saved), with completion and history; `find`, `list`, `changed`, `reset`, `try` and
  `preset` commands, and the hotkeys' actions.
- **Neon look:** coloured light (signs, screens, coloured lamps) keeps its colour instead of washing out to
  white (`RVK_NeonSat`), and bloom has a tight core with a wide haze, wider in rain and storms at night
  (`RVK_BloomWide`). `RVK_NeonNight` adds an optional night colour grade: teal shadows, richer lights.
- Ctrl+Shift+J on a dungeon's walls, floors or pillars now names the tile piece they are built from, so the
  ao-assets workbench can open it for editing (an edit changes that piece in every dungeon room using it).
- Texture packs: put a pack's maps in `randy-vk\packs\<name>\`. They are used for any texture `materials\`
  has nothing for. Packs are found at startup and listed in the log.

### Fixed
- Squares around glowing effects such as the Temple of the Three Winds portal: their see-through corners were
  shaded as if solid. (#15)
- Glowing effects (the Temple of the Three Winds portal, for one) could be seen glowing through walls and rocks in
  front of them.
- Floating damage and XP numbers (and other text over the world) were dimmed by smoke and changed by fog, bloom
  and colour grading. They are now drawn over the finished picture, still hidden behind walls.
- Light beams and glows on lamps (under the billboards in the cities, for example) swayed like trees and grew
  leaves.
- Grass on steep hillsides stood in visible rows, and grass grew in front of walls at the foot of grassy banks.
- Waterfalls pouring from pipes (the subway and other narrow streams) showed as a wide dark wall of water,
  and the streams themselves were missing.
- The PF map's black areas showed the world through them when the interface refresh rate (`RVK_UiRate`)
  was on.
- Held weapons swayed and bent like plants, most visibly while sitting. (#12)
- AOReloaded (`version.dll`) not loading when compatibility settings were set on `AnarchyOnline.exe`.
  The renderer now loads the `version.dll` beside it if Windows skipped it. (#7)
- Valley mist ended in a hard line against the sky at the hilltops. (#14)
- A rare crash when another thread ran code while the renderer patched the game's function tables.

### Diagnostics
- `[Native] Pick=off|on|verify` in `randy-vk.ini` for problems clicking on characters. `off` uses the
  game's own picking, `verify` runs both and logs where they disagree. (#16)
- Frame captures (Ctrl+Shift+F9) show which character each drawn object belongs to.
- Frame captures also save the water's mesh beside the capture (`...-water.bin`) and list each waterfall and
  how it was drawn.

## 0.6.0 - 2026-10-16

Weather and storms. Clouds, weather, smoke, water and leaves are out of Experimental and on by default.

### New
- **Thunderstorms:** dark storm cloud out to the horizon, the land darkening under it, the game's
  yellow-green storm cast, grey rain veils under the far clouds.
- **Rain** that falls as water: slants with the wind, gusts in curtains, kept off by roofs and canopies,
  stops indoors.
- **Lightning:** each of the game's strikes gets its own branching bolt, lighting the land and the clouds.
- **Sand and ash storms:** a lit, volumetric dust fog that parts round obstacles and thins in the lee of
  hills and buildings; the sun dim and red behind it.
- **Storm wind:** grass, plants, trees, smoke, water, rain and clouds lean and gust together; storms blow
  leaves, straw and twigs across the ground.
- **Clouds that change:** they churn and change shape as they drift; storm cloud boils and rolls.
- **Trees:** canopies shaded as one rounded mass, each tree keeping its own colour.
- **Presets** rebuilt: Ultra, Modern (a step lighter), Classic+ and Original.
- Settings: `RVK_CloudQual`, `RVK_CloudMorph` (Advanced tab); `RVK_RainVeils`, `RVK_StormForce`,
  `RVK_SandForce`, `RVK_StrikeRate` (`randy-vk.ini`).
- Settings window: fewer, plainer options, with sections for clouds, weather, smoke, water and leaves.

### Fixed
- Colour banding in smooth skies; stripes and noise in clouds; lightning making clouds noisy.
- Names over characters drawn flat, after the effects (no blur, fog or bloom).
- Rain and puddles seeming to follow a running character.
- The game's storm sheet and lightning glow making the sky flicker.

### Diagnostics
- Free camera: Ctrl+Shift+C (mouse and WASD).
- Ctrl+Shift+Alt+F9 captures the next 60 frames.

## 0.5.1 - 2026-10-14

### Fixed
- Names drawn as black bars and objects with missing textures after zone changes: room for 65,536 textures
  instead of 4,096.

### Diagnostics
- The periodic log line reports how many textures are loaded; frame captures show each texture's state.

## 0.5.0 - 2026-10-14

### New
- **Sky, air and night:** height haze, sun glow, light rays through clouds, valley mist at night and dawn,
  redrawn suns, dawn and dusk in the game's colours, moonlit nights with moon shadows (`RVK_NightDark`),
  city ships fading into the haze.
- **Clouds, rain and smoke** (Experimental): volumetric clouds following the game's weather with cloud
  shadows; redrawn rain with splashes and rings; smoke as a simulated fluid.
- **Waterfalls** (Experimental): falling sheets with spray, mist and foam.
- **Shadows:** things behind you keep casting; ground grass casts shadows from lamps and character lights;
  shadows fade as the sun nears the horizon.
- **Anti-aliasing:** TAA keeps particles bright; FXAA (`RVK_Fxaa`) for when TAA is off.
- Options (F10): separate **Renderer** and **Advanced** tabs.
- Faster: batched drawing (West Bank city ~95 to 139 FPS on the test machine), quicker planet map reopen,
  cheaper interface text, no hitch every 60 seconds, a pipeline cache (`randy-vk.cache`) for faster starts.
- `[Native]` parts on by default.

### Changed
- `RVK_SmokeDensity` is now `RVK_SmokeThick` (value carried over); `RVK_ShadowMem` is on/off.
- Debug switches (`RVK_PbrDebug`, `RVK_ShadowView`, `RVK_Occlusion`, `RVK_TimeOfDay`, `RVK_TimeSpeed`) are
  `randy-vk.ini` only.
- A non-number value in `randy-vk.ini` falls back to the default.

### Fixed
- Several waterfalls at once (e.g. Broken Shores) froze the game.
- Shadow casters flickering on walls.

### Diagnostics
- Crash and hang reports in `randy-vk.log`; slow frames (over 50 ms) logged with the time of day; two
  clients in one folder get separate logs.

## 0.4.1 - 2026-10-11

### Fixed
- The game not starting on some machines with `version.dll` installed: AOReloaded now patches only the
  game's own modules.

## 0.4.0 - 2026-10-11

### New
- **Swimmable water** (Experimental): a simulated surface with bow waves, wakes, splashes and rings.
- **Leaves** on trees, shrubs and palms (Experimental): fluttering, casting shadows, falling in gusts.
- Side-loaded PBR materials with an environment probe (`docs/materials.md`); picking hotkeys for modders
  (Ctrl+Shift+I texture, Ctrl+Shift+J mesh).
- Renderer tab: Experimental section, larger fonts, view distance settings.

### Fixed
- "Mountains of water": each body of water is flat at its own level.
- Falling leaves landing back in the crown.
- PBR sun highlight on objects in shade.

### Diagnostics
- Pop-in log and a GPU memory report in frame dumps.

## 0.3.1 - 2026-10-09

### Fixed
- Walls of water in the Shadowlands; a small pool flooding Jobe harbour.
- Grass vanishing behind characters; Lush Fields turning flat green after an AI mission; clearer grass
  trails.
- Timer bars and screen sprites faded or blurred.
- The planet map (PF map) drawn black.
- Signs, bridge ropes and wide props swaying.
- Objects displaced when drawn before the frame's lighting was set.
- Very large textures freezing the game; memory buffers staying at their burst size.

## 0.3.0 - 2026-10-07

### New
- **Water:** waves, ripples, refraction, depth colour, caustics, reflections, shore foam; a slider from
  "the game's water made richer" to fully realistic.
- **Presets:** Original, Classic+, Modern, Ultra.
- New defaults (Modern): darker shadows, ground grass on, lighter on video memory.

### Fixed
- Grass growing through platforms and floors.
- Grass and water show "needs HDR" in the options.

## 0.2.0 - 2026-10-07

### New
- **Ground grass** (`RVK_GrassOn`): wind, gusts, sun shadows, trails where characters walk.
- Faster: static meshes kept on the GPU, batched draws, `RVK_UiRate` for the interface, a depth pre-pass
  for cut-outs.
- Soft leaf edges (`RVK_FolEdges`); steadier anti-aliasing.
- `randy-vk.ini` edits apply while the game runs; Ctrl+Shift+H hides the interface.

### Fixed
- Sandstorm tint throwing a dark, hard-edged shadow over the foreground.
- Shadows on and from the sky.
