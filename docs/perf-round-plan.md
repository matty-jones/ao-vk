# Performance round, October 2026 (branch `perf`)

A long session on frame time, CPU and GPU, measured in rvk_demo under a Wine prefix of its own (`~/.wine-perftest`,
its `C:\linux` a link to the game's: the tests can't freeze the game's wineserver). Pictures compared before and after
each step; the look-check and the validation layer after each.

## Where the time went (start of the round)

- **In game** (Borealis by the bridges, 1440p at 75%, FSR 3, frame generation, ray-traced shadows and AO, AMD's GI):
  GPU-bound, ~12.1 ms of GPU a 13.5 ms frame - the ground's lighting 2.25, sun shadows 1.46, GI 1.19, FSR 1.17,
  point shadows 1.09, AO 0.88, the pre-pass 0.58, the sky 0.54, water 0.52. The game thread waited 10 of its 13.5 ms.
- **The city bench** (`tools/city-bench.sh`, West Bank-shaped): the render thread the limit, 6.0 ms against the GPU's
  4.1. (Its `CheckStatelDraw` - ~7% - is the bench's own: its objects never match the playfield's placement, so the
  check never ends; in the game it stops after ~30 matches.)

## What the GPU's driver said

`tools/shader-stats.sh` (VK_KHR_pipeline_executable_properties, natively): the scene shader's pixel stage took 168
registers and spilled 144 bytes - 12 warps a core of 48 (25% occupancy) - for every scene draw.
- Without the ground's code: 128. Without the frame lights' loop as well: 96. Without the lighting entirely: 80.
- The ground's own: its TerrainMap and relief were held through the lights loop (the hungriest part); laid before
  it, 128 (still a spill). Its floor without lights, PBR, sun, relief: still 128 - the one-pass ground's copy of the
  map and the parallax together; without both 96. (Putting back only the hexagons' weights instead of copying the map:
  no change in time - left out.)

## Done (each its own commit; pictures identical unless said)

| Change | Measured |
|---|---|
| Scene shader built without the ground's code for non-ground draws (`ffp_*_ng.frag`) | city scene GPU 1.27 -> 0.93 ms |
| Ground shader: map and relief released before the lighting (168 -> 128) | ground bench scene 2.29 -> 1.97 ms (within run-to-run noise of the async rocks) |
| A build without the frame lights' loop for draws none reaches (`ffp_*_nf.frag`, 96) | city scene 0.93 -> 0.67 ms; with 40 lamps identical |
| Remembered casters dense (vector + index, not a node map) | render thread 5.98 -> 5.75 ms |
| Inline equality (SSE2) for the per-draw compares; 32-bit offset divisions | render thread 5.75 -> ~5.54 ms |
| Spot lights' cubes: only the faces and casters their cone reaches | no false negative in 120k random tests; spot test pictures identical (60/100/170 deg) |

| Spot lights: grass outside the cone left out of their cubes (and not stopping a cube being kept); the cone test without acos/asin; the casters' hash a word at a time | 3 floodlights over the city bench: grass blades into cubes 60.6k -> 37.6k a frame; point scan 0.18 -> 0.09 ms (the byte-wise hash had made it 0.5 on some runs) |
| The native ground's rocks in 32-unit blocks, each culled alone - by the view, each cascade, each cube face (the ranges still the patch's: the same rocks drawn) | ground bench total 3.54 -> 3.37 ms; a scree slope (pf800 file) sun shadows 0.12 -> 0.09, total 2.95 -> 2.90. The user's Borealis dump: rock patches were 97% / 93% of cascades 0 / 1's indices |

| Sun cascades in turns: of four, the second every other frame, the two widest every fourth (was: the second every frame, the two widest every other); all at once after a gap, a new map, the rays' layers coming or going, the sun moving, or the cascade's centre (eye and view direction) moving 5% of its half width | city bench sun shadows GPU 0.29 -> 0.19 ms, their CPU 0.82 -> 0.71; pictures identical (shadow test, walker); `RANDYVK_CASCADE_TURNS=0` draws every cascade every frame |

| Lamps in the air (RVK_VolLamps): the range fade and a spot's cone before the march through its cube, not after | city bench, 12 lamps and 3 floodlights: volumetric 1.09 -> 1.03 ms, picture identical (the march itself: 32 steps, ~0.76 ms with no lamps, left) |

City bench overall: GPU 4.08 -> 3.43 ms, render thread 6.0 -> ~5.5 ms. `RANDYVK_FULL_SHADER=1` draws every draw with
the full shader build, `RANDYVK_SPOT_CONE=0` renders spot lights' cubes whole (comparisons).

## Measured and left

- Water: 1.4 ms for a lake half the screen - its pixel shader (reflection steps, refraction), the same with the grid
  as with the game's triangles. Less would be lower quality.
- AMD's GI (Brixelizer, already traced at half) ~1 ms, FSR 3 ~0.75 ms at 1440p: fixed costs at these settings.
- The ground's rocks: ~0.5 ms of the ground bench's scene, mostly per pixel (the ground's full shader).

- The game's cloud sheets (`RVK_CloudSheet`): over Borealis ~9 pixel-shader runs per screen pixel (30 puffs drawn
  as sheets). A short path in the shader for them (no lighting) and leaving the local-light and albedo targets
  unwritten both left the time as it was (0.14 ms for 10 puffs at 1080p): it is the rasterising - reverted.
- The rocks' coarse shape for all of them (20 faces, not 80): 0.02 ms of the scene - their cost is per pixel (the
  ground's full shader, the rock read from three sides), not their vertices. Left as they are.
- Ray-traced sun shadows: their trace shows in the "depth pre-pass" interval (+0.48 ms at 1080p, city bench).
- Async compute (the next frame's shadow maps beside this frame's post passes): one queue family today, and it
  needs a second queue, semaphores and the shadow maps' ownership handed between queues - a larger change than
  this round; and the in-game frame is GPU-bound throughout, so overlap would win only where the passes leave
  units idle. Not started.
- The render thread over the city bench after this round: no line above 2.5% (remembered casters' cache misses
  as they are stamped, the cascades' cull loop); memcpy ~6% (the frame's copied vertices). Further gains need the
  persistent shadow list (below).

## Next

1. In game: the frame dump's spot lights now print their cones (`cone theta/phi`) - how much the bridges' floodlights
   save depends on theirs.
2. The ground's shader below 128: its own slimmer build (the one-pass map and the parallax are what hold it).
3. The render thread: a persistent shadow item list for the remembered casters and placed objects (rebuilt only when
   they change); instanced statics (docs/render-batching-plan.md P3).
4. Static sun shadow cascades kept while the camera stands still (re-rendered only when a cascade moves).
