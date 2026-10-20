# Frame rate plan: upscaling, deferred point lights, and what else is left

Status: Phase 0 done. Phase 1 (upscaling) built 2026-10-20 on main: `RVK_RenderScale` (default 100) staged as
`randy31-renderscale1.dll` for the in-game check; 1a-1c and the mip bias are in, 1d's demo checks done (results under
Phase 1). In game at night 75% gives 101 -> ~125 fps and makes
the game thread the limit; Phase 2a measured: deferred lights deferred (see 2a result). Next: Phase 4 (CPU).

The goal is a higher frame rate with the same look. This plan covers the GPU work that is left after the October 2026
performance round, and gives a verdict on each of the rendering techniques the user asked about (deferred lighting,
visibility buffers, clustered shading, mesh shaders, ReSTIR / radiance caching, neural rendering, temporal
reconstruction, DLSS). Each phase measures first, has its own setting, and is staged for an in-game check before the
next one starts.

## Where the frame stands

At the lamp-lit outpost: playfield 566, eye about (367.7 32.8 317.7), `timeofday 13`, 2560x1440, RTX 4070 Ti. Numbers
from `randy31-prepasseq1.dll` with `sceneprof 1`, averaged over 600 frames.

- **84 fps, 11.9 ms a frame.** It was about 50 fps at this spot before this round. The game thread still waits 5.7 ms
  a frame for the GPU, so the frame rate is GPU-bound.
- **GPU total 11.1 ms.** Scene 3.26 ms, broken down as:

  | Scene part | ms |
  | --- | --- |
  | Ground lighting | 2.23 |
  | Statics | 1.13 |
  | Foliage | 1.06 |
  | Effects | 1.05 |
  | Sky | 0.47 |
  | Characters | 0.15 |

  The scene parts sum to more than the scene total: with `sceneprof` on, part of the ground's time is booked under
  "ground grass" instead.

  Outside the scene:

  | Pass | ms |
  | --- | --- |
  | Ground grass (with `sceneprof` off; it takes in part of the scene's ground time) | 2.8 |
  | Smoke simulation | 0.78 |
  | Global illumination | 0.6 |
  | Clouds | 0.6 |
  | Volumetric light | 0.36 |
  | Depth pre-pass | 0.32 |
  | Tone mapping | 0.31 |
  | TAA + sharpen | 0.3 |
  | Ambient occlusion | 0.29 |
  | Motion blur | 0.25 |
  | Sun shadows | 0.3 |
  | Point shadows | 0.22 |
  | Interface | 0.32 |

- **Shader runs per screen pixel:** statics (pre-passed) 0.41, terrain light 0.41, foliage 4.57, sky 5.37,
  characters 0.38.
- **CPU, game thread:** render lists 3.9 ms, interface 0.85 ms, scene update 0.8 ms. Below about 6 ms of GPU time the
  game thread becomes the limit, around 150 fps.

How to measure, and the traps:
- In game, use `sceneprof 1`. The `gpu ms` and `scene shading` lines in `logs/randy-vk.log` cover 600 frames each.
  Stand still for at least 10 s after any setting change, and ignore the first window after a change: it straddles the
  change.
- The profiling sweep (console `profile`, Ctrl+Shift+O) times each enhancement switched off in turn. It takes about
  6-7 min and you must stand still.
- In the demo, use the scratch `timedemo.sh` / `bestof.py` pattern: `rvk_demo` under plain wine, not through
  `rvk-demo.sh` (the validation layer makes the profiler's queries very slow), with `RANDYVK_SCENE_PROFILE=1`.
  - The demo forest is `--leaf-tree <abs>/build/trees/20842.tree,<abs>/build/trees/13147.tree --leaf-forest 8
    --size 2560 1440 --hdr --taa --frames 800`. Tree paths must be absolute. Without `--hdr` there is no scene profile
    and no TAA.
  - Take the best of 3 runs per variant.
- The user often runs another GPU project, and the game itself may be running: check `nvidia-smi` first, or ask them
  to pause it. Shared GPU load swings timings by about 30%.
- After any change that should keep the look, run `tools/look-check.py` and a validation run.

## Phase 0 (done): the October round

Done in commits 388219c through b50e580:
- Water out of view is skipped.
- Smoke out of view, or hidden behind houses, is simulated less often.
- Every tree's leaves go into the depth pre-pass.
- The native ground is drawn in one pass.
- Far plants' leaf edges are dithered (`RVK_FolDither` 20). The soft-edge pass that stays for near plants tests depth
  strictly.
- **Biggest win:** pre-passed draws test depth EQUAL and write no depth (`RVK_PrepassEq`). Before, the depth write
  combined with a discarding shader forced the depth test after the shader, so every see-through fragment of a leaf
  card was shaded. At the outpost foliage went from 3.24 to 1.06 ms.

Measured and ruled out, with the reason:

| Idea | Why not |
| --- | --- |
| Light-range trimming | The light grid already culls; only one 121 m lamp is "always". |
| Sun-shadow screen mask | 0.5 ms at most, and approximate. |
| Shader specialization | Stripping the terrain code from the plant shader gave nothing. |
| Lowering `kFoliageCore` to 0.8 | No saving. |
| Cheaper lighting on leaf edges | Lighting was only about 25% of the edge pass. |

## Verdicts on the techniques

- **Temporal reconstruction (upscaling): yes, and first (Phase 1).** Nearly every GPU millisecond above scales with
  pixel count: the scene, grass, ground, and the post passes. Rendering at 67% per axis shades 44% of the pixels.
  Expect roughly 4-6 ms off the outpost's 11 ms. We already have the pieces: Halton jitter (hdr.cpp BeginScene), object
  motion vectors (`m_motionVectors`), TAA history with neighbourhood clipping (taa.frag), and a sharpen pass.
- **DLSS: not feasible as a plain integration.** Anarchy Online is a 32-bit process, so `randy31.dll` is 32-bit. As far
  as is known, NVIDIA ships the DLSS runtime (`nvngx_dlss.dll` / NGX) for 64-bit only, so it cannot be loaded into our
  process.
  - The way round it is a separate 64-bit helper process that shares Vulkan images (external memory and semaphores)
    with us, through Wine's wow64 winevulkan. That means a frame of latency or a synchronous round trip, plus a second
    process to ship and keep alive. Not worth it before our own upscaler exists.
  - Check this assumption once (the DLSS SDK on GitHub lists its binaries) before closing the door for good.
  - FSR 2 (AMD FidelityFX, MIT licence, has a Vulkan backend) can be compiled into a 32-bit DLL. It's the fallback if
    our own upscaler's quality falls short.
- **Frame generation: maybe, last (Phase 5).** It doubles the displayed frame rate, not the simulated one, and adds
  latency. It needs a swapchain-level pacing scheme. Only after the GPU work is down.
- **Deferred lighting: a hybrid for point lights only, if the night measurement justifies it (Phase 2).**
  - Full deferred doesn't fit. The game's fixed-function texture stage cascades, vertex colours, per-draw fog,
    overbright modes, the two-pass ground and blended effects would all need G-buffer encodings, and transparents stay
    forward anyway.
  - Its main benefit, shading each pixel once, we already have through the depth pre-pass plus `RVK_PrepassEq`. Solid
    surfaces run about 0.4 shades per screen pixel.
  - What a deferred point-light pass would still add: the light loop moves out of the uber-shader, which is huge and
    probably runs at poor occupancy, into a compute pass; and light culling becomes per screen tile with depth bounds
    instead of per 16 m world cell (`lighting.glsl` line ~127, `FL.lightGrid`).
  - The scene already writes 5 targets (scene 16F, glow 16F, local fraction 8, motion+normal 16F, albedo 8) at
    32 bytes a pixel. A thin G-buffer is mostly there.
- **Clustered shading: yes, as part of Phase 2.** It's the screen-space replacement for the world grid.
- **Mesh shaders: no.** Geometry isn't the bottleneck: about 2,200 draws a frame, and foliage is about 17k vertices.
  The game streams Direct3D 7 draws one at a time.
- **Visibility buffer: no.** It needs all geometry and materials resident on the GPU and indexable by triangle ID. The
  game's immediate-mode Direct3D 7 stream with fixed-function state is the opposite of that.
- **ReSTIR / radiance caching: no, as a frame rate measure.** They are quality techniques for many shadowed lights and
  global illumination, usually with ray tracing, and they make a frame more expensive. If we ever want better lamp
  shadows or GI, it's a quality project of its own.
- **Neural rendering: no**, except through an upscaler (see DLSS).

## Phase 1: temporal upscaling (`RVK_RenderScale`)

Goal: the 3D scene and its passes render at a fraction of the output resolution. The TAA resolves it to full
resolution. The interface stays at full resolution.

Setting: `RVK_RenderScale` (Anti-aliasing section, Choice `100 85 75 67 50`, default 100 until it is proven; 15
characters at most). It requires TAA: with TAA off it acts as 100. Console name `renderscale`.

### Results so far (2026-10-20)

Demo, best of 3, RTX 4070 Ti, GPU total in ms:

| Scene | 100 | 85 | 75 | 67 | 50 |
| --- | --- | --- | --- | --- | --- |
| Forest, 1440p (no post effects) | 2.13 | 1.78 | 1.54 | 1.35 | 1.04 |
| Native ground pf790 + bloom, AO, GI, volume, contact, blur, clouds, 1440p | 3.80 | | 2.62 | | |

- Scene passes cost ~0.62-0.66 of 100% at 75% (pixels 0.56). The upscale costs ~0.04 ms over the plain TAA, the depth
  upsampling 0.04 ms at 1440p.
- Look: aligned with 100% (no shift); 75% a little softer on bark, 50% visibly softer; grass and a moving character
  show no ghost trail; rain streaks hold. 100% is unchanged (look-check; the old TAA path is kept bit for bit).
- How it's built: one depth image at the scene's size during the scene; after the TAA, `UpsampleDepth` draws it into
  a second, output-size depth image and the two swap (`SwapDepth`), so the motion blur, name labels and the interface
  test against full-size depth; `BeginScene` swaps back. The game's viewports, scissors, clears and back-buffer blits
  are scaled while the scene is the target (`ViewportState`, `SceneRect`). The scene's motion vectors are in scene
  pixels (`FL.screen`). The scene keeps D3D's half-pixel shift in its own pixels, so output pixel i is scene position
  i * scale + jitter (taa.frag `Upscale`).
- In game (2026-10-20, `randy31-renderscale1.dll`), lamp-lit outpost at night (`timeofday 1`), 1440p, all lights on:
  100% 101-105 fps (GPU 9.5 ms, waiting for the GPU 3.3 ms a frame); 75% 119-130 fps (GPU 6.3 ms, waiting 0.1 ms).
  At 75% the game thread is the limit: 8.0 ms a frame, 4.2 of it the game's render lists. No artefacts noticed.
- Open: frames with no 3D at all (loading screens) draw the interface into the scaled scene, so it is upscaled
  there; in-game fps at the outpost and in a city; whether `RVK_Sharpen` should rise with the scale.

### FSR 2.2 as a second upscaler (2026-10-20, `RVK_Upscaler` 1)

AMD's FSR 2.2.1 in `third_party/fsr2` (MIT; patched for 32-bit Vulkan handles, its README lists the changes), its
passes compiled by `tools/gen_fsr2_shaders.py` (glslc + spirv-cross in place of AMD's Windows tool). `rvk/fsr2.cpp`
feeds it the tone mapped scene (LDR), the scene's depth, the motion vectors filled in where none were written
(`motion_fill.frag`), the jitter (+ 0.5 * (1 - scale) scene pixels: our scene keeps D3D's half-pixel shift, FSR 2
assumes Vulkan's - without it the picture stood half an output pixel off at 50%), and the tone mapping's alpha as
its reactive mask (effects, and now the near rain's streaks); its output goes through our sharpening. Mean difference
from 100% on the demo forest at 1440p: ours 4.95 at 75%, 6.33 at 50%; FSR 2 6.35 -> 4.96 at 50% after the shift fix
(about our 75%). The walker at 50%: FSR 2 closer (1.89 vs 2.20), no ghosting either way. Weak spot: rain streaks
at 50% fade more than with ours. Cost: to measure with the GPU idle.

### 1a. Lower-resolution scene targets

- In `device.cpp` CreateMainTargets, size these by the scale: `m_scene`, `m_glow`, `m_localFraction`,
  `m_motionVectors`, `m_albedo`, and the depth (`EnsureDepth`). `m_ldrMain` stays at the full size. Recreate them when
  the scale changes, between frames: the same path as a resize.
- Every post pass that sizes itself from `m_scene` or the depth already follows (bloom, AO, GI, volume, sand fog, SSR,
  contact shadows, DOF, scene luma). Audit each `CreateImage(m_width, m_height ...)` in hdr.cpp, clouds.cpp, smoke.cpp,
  rain.cpp, grass.cpp, water.cpp and interface.cpp for full-size assumptions.
- **The game's viewports and scissors** are in back-buffer pixels. While drawing into the scene target, scale them in
  `ApplyDynamicState` (the viewport and scissor block near the top) by the scene and back-buffer size ratio.
  Pre-transformed (XYZRHW) vertices are mapped through `C.viewport` in ffp.vert (`rhw` branch) to NDC, which stays
  correct with a scaled Vulkan viewport. Check any shader that uses `C.viewport` for pixel sizes.
- Pixel-size constants in screen space need checking: point sprite and line sizes; the smoke / cloud / rain
  half-resolution buffers; and noise in `gl_FragCoord` units (dither and IGN patterns are fine at any scale).

### 1b. Jitter and the upscaling resolve

- Jitter is generated in scene pixels: `m_taaJitter` uses `2/w` with w the scene width (hdr.cpp ~409). Use a longer
  Halton sequence at low scales; FSR's rule is `8 * (1/scale)^2` phases.
- taa.frag becomes an upscaler:
  - Output at full size. Read `current` (the tone mapped scene, at scene size) at the output pixel's position in
    scene space, minus the jitter.
  - Accumulate with a sample weight that falls off with the distance between the output pixel's centre and the
    jittered sample (a Lanczos- or Gaussian-weighted splat). Neighbourhood clipping uses the 3x3 scene-space
    neighbourhood. History is at full size.
  - Motion vectors are in scene pixels: scale them to output pixels.
  - Depth (nearest of 3x3) is read at scene resolution.
- Order: the tone mapping (scene size) goes into `m_tonemapped` (scene size), then the TAA upscales into `m_ldrMain`
  (full) or the motion blur's input (full). The motion blur and sharpen run at full size. The motion blur reads
  depth: give it scene-space depth coordinates.
- Hold the history at a fixed size. A scale change resets it (`size.z` history valid = 0).

### 1c. What touches the scene at full size after the scene

- **Name labels** (hdr.cpp `DrawHeldLabels`) are drawn on the final picture with the scene's depth attached, so
  labels hide behind walls. A scene-size depth can't be attached to a full-size target. Options:
  - Upsample the depth into a full-size depth image (a nearest-of-2x2 resolve pass). Preferred, and needed anyway if
    anything else depth tests after the scene.
  - Or depth test in the label shader against the scene depth as a texture.
- **The game's own reads of the back buffer** (screenshots, render-to-texture, the "copy" / "read-back" ends in
  prepass.cpp's segment log) read `m_main`, which is `m_scene` during the scene phase. A copy from the scene to a
  game texture of back-buffer size must become a scaled blit. Find every place that copies from `m_main` /
  `m_target` (device.cpp, rvk_surface.cpp's Blt paths) and handle a size mismatch.
- **Frame dumps** (`DumpTargets`) write the scene targets at their own size; that's fine, but note the scale in the
  dump text.
- **The interface layer** (interface.cpp, `RVK_UiRate`) composites at full size. Unchanged.
- **Water's refraction / reflection copies** and the environment probe read the scene: check their sizes.

### 1d. Quality checks

Demo scenes for each scale:
- the forest (thin leaves, dither),
- `--grass-walk` (grass blades are the hardest thin detail),
- the native ground at a grazing view,
- a storm with rain (particles are new each frame),
- a moving camera.

Compare against 100% at the same frame. Watch for: shimmering on grass and leaves, ghosting behind moving
characters, blurred text-like textures (signs), and the sharpen strength (RVK_Sharpen may need to rise with the
scale).

Mip bias: at lower scale, textures should sample with a negative LOD bias of `log2(scale)` so they stay as sharp as
at full resolution (sampler `mipLodBias`; the bindless samplers are created in resources.cpp). Without it the upscaled
picture looks soft.

### 1e. Rollout

- Stage at a scale of 75 for the user, with `renderscale 100` to compare.
- Record fps at the outpost and in a city.
- The default stays 100 until the user picks one. Changelog entry under New.

Expected cost: about 3 sessions, with 1a/1b the bulk. The main risks are the game's back-buffer reads (1c) and
thin-detail shimmer (1d).

## Phase 2: point lights at night (measure, then maybe a deferred and clustered light pass)

### 2a. Measure (one evening in game)

- At night (`timeofday 1`) at the lamp-lit outpost, with `sceneprof 1`, toggle `lightover 0` / `1` (`RVK_LightOver`:
  all nearby lights per pixel), then `pixellight 0` / `1` (`RVK_PixelLight`).
- Also run the profiling sweep there at night. The earlier sweep (before Phase 0) showed per-pixel lighting 7.5 ms and
  light override 2.2 ms. Those numbers included the overdraw that `RVK_PrepassEq` has since removed, so they will be
  lower now.
- Decide: if the point lights' share is still above about 2 ms at 1440p (after Phase 1, at the chosen scale), go on
  to 2b. Otherwise stop here and write the result into this plan.

### 2a result (2026-10-20): stop here for now

GPU ms at the outpost by night, 1440p (from the `gpu ms` lines; `lightover` / `pixellight` toggled):

| Render scale | all on | `pixellight 0` | point lights' share |
| --- | --- | --- | --- |
| 100 | 9.5 | 6.2 | ~3.3 (per-pixel ~1.9, light override ~1.35) |
| 75 | 6.3 | 4.4 | ~1.9 |
| 67 | 5.6 | 3.9-4.8 | ~1.6 |

At the scale worth playing at (75) the share is at the 2 ms line, and the frame is no longer GPU-bound: the game
thread takes 8 ms while the GPU needs 6.3. A deferred or tiled light pass would lower GPU time without raising the
frame rate. 2b / 2c wait until the CPU side (Phase 4) brings the game thread under the GPU again, or for players at
100%.

### 2b check (2026-10-20): how many tested lights reach the pixel

After the sweep (per-pixel lighting 1.97 ms at night, light override 0.91 of it): `RVK_PbrDebug` 11 counts the frame
lights the loop tests at each pixel and those whose range reaches it; a frame dump's `-scene.bmp` gives the counts
exactly (v = x / (1 - x), x = byte^2.2, x 16). The city bench with 90 lamps (`--city-bench --city-lamps 90
--pixel-lighting --light-override --taa`): 10.3 tested, 8.4 reaching - 81%. The ones that don't reach leave the loop
at its distance test, so screen tiles would save well under a fifth of it: the cost is the lights that do reach.
At the outpost (frame dump of 2026-10-20, 75%): 4.1 tested, 2.9 reaching (70%) over 93% of the screen; under 4% of
pixels test 8 or more. **Screen tiles (2b) and deferred lights (2c): not worth it** - they would skip about one cheap
test a pixel.

What the loop's cost is (city bench, 40 lamps, 1440p at 75%, best of 3, a temporary switch): all 3.24 ms; the loop
skipped 2.81; the loop and each light's constants read but no lighting 3.02; no point-shadow taps 3.20; no PBR
highlight 3.24; lights under 2% at the pixel skipped 3.25; without the grid's cell mask 3.27. So half is walking the
lights, half their lighting, neither shadows nor highlights. Walking them together per subgroup (one light index for
all its pixels, subgroupOr of the masks - uniform-buffer reads at one index) changed nothing: not kept.

Kept instead: point-shadow cubes whose light and casters in range are unchanged (a fingerprint of each caster's
matrix, buffers and texture; none swaying; no grass in range) keep their map up to 30 frames (pointshadow.cpp);
pixel-identical in the bench (its cube pass 0.06 -> 0.01 ms). In game the cube pass is 0.33 ms; how much it keeps
there shows in the "point shadows: ... kept as drawn" log line.

### 2b. Screen-tile light lists (clustered, forward)

The cheaper half, and it keeps forward shading:
- A compute pass after the depth pre-pass. It builds, for 16x16 pixel tiles (with the tile's min / max depth from the
  pre-pass depth, optionally split into 2-4 depth slices), the list of frame lights whose sphere touches the tile's
  box.
- The uber-shader's light loop (`lighting.glsl`, the `FL.lightGrid` block) reads its tile's list instead of the 16 m
  world cell's.
- It gains when many lamps' ranges overlap in the world but not on screen, and when a pixel's world cell holds lamps
  hidden behind walls.
- Setting: hidden `RVK_TileLights` (on / off) for A/B.

### 2c. Deferred point lights (only if 2b leaves the light loop expensive)

- The forward pass keeps the sun, ambient, the game's own light, fog, emissive and the material modes. It writes what
  a light needs into the existing targets:
  - albedo: `m_albedo` already;
  - the shading normal: `m_motionVectors.zw` already, octahedral, but only for PBR surfaces; it needs to be written
    for all lit surfaces;
  - roughness / specular strength: `m_localFraction` has room, or a new 8-bit target.
- A compute pass per 16x16 tile reads depth, normal, albedo and roughness, loops over the tile's lights (2b's list)
  and adds the light into `m_scene` (or a light target added at the tone mapping). It's the same light function as
  lighting.glsl, moved into a shared include.
- What stays forward, lit as now:
  - blended draws (two-pass foliage edges, effects, water, glass);
  - characters (tessellated / skinned; they could join later);
  - anything with `F_LIGHTING` off.

  A per-pixel "deferred" bit (a free alpha channel, e.g. `outLocal.a`) tells the light pass which pixels it owns.
- Point shadows (pointshadow.cpp cube maps) are read the same way in the compute pass.
- The light's headroom (`RVK_Headroom`, soft roll-off) and `outLocal`'s "local lights' fraction" must come out the
  same. Check with the look check, plus a night demo scene with lamps (add one: `--lamps N` around the forest).
- Setting: hidden `RVK_DeferLights` for A/B. Changelog under Changed once it's on.

Expected cost: 2b in 1 session; 2c in 2-3 sessions. 2c only if the numbers say so.

## Phase 3: the smaller items at the outpost (any time, cheap)

Each is measure first, then a fix:
- **Effects 1.05 ms.** Unexamined. Find which effect draws dominate: the frame dump's draw list by class, or
  `pbrdebug` views. Candidates: large additive sprites covering the screen, the game's glow cards.
- **Smoke simulation 0.78 ms.** It was 0.67-0.79 before and after the hidden-smoke change. Check the `smoke` log line
  for how many volumes are simulated at full rate here, and why: in view, unhidden, grace frames.
- **Foliage still 4.57 shader runs per screen pixel**, though now cheap (1.06 ms). Probably plants kept out of the
  pre-pass (the log's "cut-out (blended) 14" and the far LOD foliage). Look at what the prepass log keeps out, and
  whether those are the bulk of the runs.
- **Sky 5.37 runs per pixel.** Cheap (0.47 ms), but several full-screen layers. It could be merged if it ever
  matters.
- **Ground lighting 2.23 ms**, the biggest scene item. It reads tile normal maps per pixel (the one-pass change kept
  that). Options:
  - cache the ground's lit normal and material in a clipmap around the eye (a large change);
  - skip normal-map reads past a distance where they are below a pixel (a cheap LOD);
  - lower the ground's tessellation factor in the distance.

  Measure first with `pbrdebug` and RVK_GroundTess, RVK_NormalMaps toggles.

### The sweep at the outpost by night (2026-10-20, 75%, `randy31-groundsweep1.dll`)

Baseline 6.87 ms GPU (135 fps), 6.90 at the end. GPU ms each step saves (switched off): per-pixel lighting 1.97 (light
override 0.91 of it), native ground 0.64, PBR 0.62, smoke 0.46, point shadows 0.44, normal maps 0.44, GI 0.39,
parallax 0.37, ground grass 0.35, ground variation 0.28, clouds 0.28, motion blur 0.22, atmosphere 0.21, the
ground's slopes' sun 0.18 / tile blending 0.16 / sky visibility 0.15 / repeats hidden 0.13 / grain 0.10, bloom
0.10. Render scale 100: 101 fps (no GPU line: the switch restarts the profile's window). Rocks, pebbles and sand
ripples off read 35-80 fps: the switch rebuilds the ground's data - not a steady cost. Cheaper on than off: the
depth pre-pass, TAA, HDR, plant soft edges, our sky, cliff depth and rock.

First changes from it (staged `randy31-groundsmoke1.dll`):
- The ground's variation computed once a fragment (the one-pass ground with material maps varied the same point
  twice); bit-identical in the demo.
- Smoke small on the screen steps less often (under 12% of the screen's height half as often, under 5% a third).

## Phase 4: CPU side (after the GPU is under about 7 ms)

The game thread's render lists take 3.9 ms. Native ports (proxy/native) and draw batching (docs/render-batching-plan.md)
are the levers there; this plan doesn't cover them beyond noting when they start to matter: once the `wait for the
GPU` figure in the `cpu ms` line drops near 0.

### First profile (2026-10-20, outpost by night, 75%, `tools/profile-game.sh 10`)

The game thread is busy all frame (8.0 ms): render lists 4.2, scene update 0.8, interface 0.7, the rest the game's own
logic. By module: randy31.dll 44% (native ports ~29%, rvk + the proxy ~9%, CRT ~5%), N3.dll 14%, Wine / ntdll /
wow64 ~17%, GUI.dll 8%, Gamecode.dll 5%, DisplaySystem.dll 5%. No single hot spot; ours, largest first:
- `scene::Describe` 3.3% (+ `Readable` 0.6% and part of the Wine share): readable checks by `VirtualQuery`, a system
  call, cleared at every described visual. Fixed in 9652c13 (cached by region); staged `randy31-memcheck1.dll`.
  Result in game, same spot: render lists 4.2 -> 2.4 ms, game thread 8.0 -> 7.2 ms, ~125 -> ~140 fps (the GPU at 6.4
  ms is nearly the limit again); 115-120 queries a scene update; the Wine share of the game thread 8.1 -> 4.7%,
  `Describe` 3.3 -> 1.8% (left: its maps and the attachment walk).
- `visual::CullLights` 3.2%: the game's loop over every scene light per lit visual. A faster form must keep the
  game's enable / disable order (calllog-ab).
  Second round (fd29145, staged `randy31-gamethread1.dll`): `Describe` keeps each visual's attachment walk while the
  attachments (now re-indexed only when they change) and its parent stay; `CullLights` asked for no lights among 9+
  only switches off those on (calllog-ab the same in every scene, `sfxlights` added for it). The full speed-up of
  `CullLights` would need the lights' positions kept for a frame, but lights carried by characters can move during
  the render (attractors): decide from its new log line (calls, share asking for none, lights a call).
- The pop-in log (`[Native] PopLog`, a diagnostic) ~1% when on (the user's ini: off since 2026-10-20).
- CPU skinning bounds on the game thread (`SkinPositions`, `BoundsOf`) ~1.6%; the mesh-handle map ~0.7%.

## Phase 5: frame generation (optional, last)

Only when Phases 1-3 are done and the user wants more displayed fps:
- An interpolated frame between two rendered ones from motion vectors (FSR 3's approach), presented between them.
- It needs present pacing in device.cpp's present path and costs a frame of latency.
- The interface layer is composited after interpolation so text doesn't warp.
- Setting `RVK_FrameGen`, off by default.

## Order and hand-off

1. Phase 1 (upscaling): start with 1a + 1b in the demo at a fixed scale, then 1c, 1d, 1e.
2. Phase 2a measurement: needs the user in game at night. Ask for it during Phase 1.
3. Phase 3 items between the bigger steps, whenever a session has spare time.
4. Phase 2b / 2c per the measurement.
5. Phase 5 on request.

Each step:
- builds;
- is staged as `randy31-<name>.dll` with ao-wine.sh line 15 pointed at it;
- passes the look check and validation;
- gets a changelog entry if players notice it;
- has its status line updated at the top of this file.
