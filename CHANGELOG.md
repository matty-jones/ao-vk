# Changelog

What changed in each ao-vk release, newest first. Downloads and install notes: the
[releases page](https://github.com/wannabuh/ao-vk/releases).

## Unreleased

### New
- Texture packs: put a pack's maps in `randy-vk\packs\<name>\`. They are used for any texture `materials\`
  has nothing for. Packs are found at startup and listed in the log.

### Fixed
- AOReloaded (`version.dll`) not loading when compatibility settings were set on `AnarchyOnline.exe`.
  The renderer now loads the `version.dll` beside it if Windows skipped it. (#7)
- Valley mist ended in a hard line against the sky at the hilltops. (#14)
- A rare crash when another thread ran code while the renderer patched the game's function tables.

### Diagnostics
- `[Native] Pick=off|on|verify` in `randy-vk.ini` for problems clicking on characters. `off` uses the
  game's own picking, `verify` runs both and logs where they disagree. (#16)

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
