# Desert plan: sand that reads as sand

**Status (2026-10-19):** on branch `desert` (off `pebbles`). Phases 0 to 5 are built and checked in rvk_demo
on Newland Desert's real data (`tools/ground-data.py 565 --export F --map 4 --shade 0`), with every new setting at 0
pixel-identical to before. Phases 0-2 were checked in game (the user's `rvk-frame-130919`: the ripples' fade and the
paths fixed after it), as were 3 and 4 (glints, shadows at a low sun - several fixes since); phase 5 is staged.

## Context

The user's frame dump `rvk-frame-112254` (Newland Desert, playfield 565, about 11 h, the sun 46 degrees up, the
native ground on) shows these problems:

- **The dunes look like smooth felt.** From mid-distance out, the sand is one flat orange, with no ripples and nothing
  that shows the wind. The faces turned away from the sun were barely 10% darker than the sunny ones: their light is
  the game's lightmap (one texel a 4-unit cell) turned by `NativeSunTurn`, and the game's bake has a large flat share
  of sky in it.
- **Up close the tile reads as cracked mud.** The user wants the game's texture kept and the detail added on top.
- **The pebbles are wrong for sand.** There are grey and white stones all over a dune field.
- **There's no desert air.** The far dunes are as saturated as the near ones, the sky stays deep blue to the horizon,
  and there's no heat shimmer.

## Phase 0: which cells are sand (built)

- The pebble classifier (`rvk/ground_pebbles.cpp`) says which surface types are loose sand: warm, fine, saturated and
  bright (the orange dune sand). The deserts' pale tile (Newland Desert's paths, the Wastelands' flats: the same
  texture) was sand at first; it is packed ground, so it was dropped (the user, 2026-10-19: the path should read as a
  harder path) and is dirt again, with no ripples and the dirt's pebbles. Checked offline with `tools/ground-data.py 565|570|551 --types`, which
  uses the same rule. `randy-vk\terrain\pebbles.tsv` gets an optional fifth column, sand (1 or 0).
- The table is made whenever the playfield has surface types (`EnsurePebbleTable`), not only while pebbles are drawn.
  Each tile's sand weight is its verdict times the share of its texels that are its ground (a transition tile into
  grass is half sand).
- The weight goes to the means texture's alpha (it was always 0xFF), filtered, so it fades over a cell's edge. Both
  ground passes have the texture (`NativeSand` in terrain.glsl).

## Phase 0.5: parallax for any material with a height map (built)

- `<type>_<id>_h.png` (greyscale, white high) is a new side-loaded map. It is kept in the normal map's alpha (the
  per-draw record has no free slot), and without an `_n.png` the normals are made from the height. A draw whose
  normal map has one is marked `MAT_HEIGHT`.
- `RVK_DepthSteps` (Materials, 1, 0.5..3; `FL.terrain3.z`) scales every march's step count (meshes, tiles,
  ripples, and their walks towards the sun): fewer for speed, more for sharper edges at flat views.
- `RVK_Parallax` (Materials, 1) scales the depth: the full 0..1 of the height is 4% of a texture repeat deep. A
  `_h.txt` per texture for its own depth, as first planned, is left for later; one depth for all is enough until a
  pack needs otherwise.
- The march (`rvk/shaders/parallax.glsl`) builds the surface's frame from the screen derivatives, as `NormalMapNormal`
  does, so no vertex tangents are needed. It uses 6 steps head-on to 24 at grazing angles, with the hit point
  interpolated, faded by distance (gone by 22 units) and where a texel is a few pixels small. From the point it meets,
  a few steps towards the sun darken the sunlight in the relief's own shadow. No depth is written. Stage 0, the
  normal map, the material and the emissive maps are all read at the moved coordinates with the pixel's own
  derivatives.
- On the native ground, a cell's tile with a height is marched the same way in both passes. The maps texture is then
  bound in the base pass too (`m_native.anyHeight`).
- Checked: a synthetic brick floor (`rvk_demo --pbr-maps DIR --pixel-lighting`, `floor_h.png` + `floor_d.png`).

## Phase 1: sand ripples (built; `RVK_SandRipple`, Terrain, 1)

- Two sizes, both laid across a fixed bearing for each playfield (`SandWindBearing`: the weather's wind turns by the
  minute; ripples are laid by the winds of months):
  - small ripples 0.36 units apart and 2.8 cm deep (0.7 to 1 of it in patches: down to 0.3, the shallow patches read
    as the ripples' shadows switching off, `rvk-frame-162907`), wandering and forking (warped noise; its finer warp
    and the tile's own relief on sand both scaled down by `RVK_RippleSoft`, Terrain, 0.3, `FL.sand2.x`: at 0 both
    read as noise, `rvk-frame-163429`), with
    a gentle side into the wind and a steep one away from it;
  - the wind's larger ripples, 5.5 units apart and 6 cm deep, only in patches (a hillside of them in rows read as
    corduroy).
- The noise is read once a pixel, with its gradient. Over the few centimetres a march looks round a pixel, the
  ripples' phase is that value plus the gradient times the step, so each step is a `fract`, not four noise reads. This
  took the cost from +0.9 ms to +0.25 ms at 2560 x 1440 on Newland Desert.
- Up close, the parallax marches them in both passes (to about 45 units, their depth there x `RVK_RippleDepth`, Terrain,
  1, 0..3 - at first `RVK_Parallax`'s, but bricks drawn shallow left the dunes flat; 3-10
  steps by how many wavelengths the ray crosses, rays followed to 83 degrees off the normal: a trough only hides behind
  its crest where the eye looks flatter than the crest's ~23-degree lee, past ~12 units from the game's camera, and
  the first build's 22 units, steeper rays and the setting ignored left no depth to see in game, the user's
  `rvk-frame-153557`; a few towards the sun for the troughs' shadow, on the
  lightmap's sun share). Their slope bends the lighting pass's normal, through `gLightmapRelief`. Their depth tints
  the base pass's colour: troughs darker, crests lighter. Without that, the ripples barely showed when the sun ran
  along their crests, which it does at 565's noon.
- Each size fades out as its wavelength nears a few pixels, measured across its crests (along the phase's gradient),
  not by the pixel's longest side: looking along the ground a pixel is a long thin strip, and ripples whose crests run
  away from the eye stay well apart on screen long after that strip is longer than their wavelength. The first build
  faded them all there, so they stopped a few metres out at a grazing view (the user's `rvk-frame-130919`). This
  costs about +0.4 ms at 1440p, as more of the sand has them. There are none on slip faces past about 32 degrees
  (gone by 34), and the rock grit (`RVK_GroundGrain`) is left off sand.

## Phase 2: dune lighting (built; `RVK_DuneLight`, Terrain, 0.6)

- `NativeSandLight` (terrain.glsl), mixed with `NativeSunTurn` by the sand weight. As for the rocks, 0.75 of the
  light where the bake says the sun reached is the sun's, by the smooth normal's Lambert over the flat ground's. Flat
  sand stays as baked; a dune's far side falls toward the sky's level, a little cooler. It fades out as the sun sets.

## Phase 3: sand clutter (built)

- Pebbles on sand: far fewer (density 0.05, from 0.25), in the sand's own hue (`TerrainClutterBase`: no mix towards
  grey). The rocks and pebbles also no longer take the ripples' tone in their base pass (their record has the cells'
  sand too).
- Glints (`RVK_SandGlint`, Terrain, 0.5; `FL.sand.z`; parallax.glsl `SandGlint`): a grid of cells, some holding a
  grain (the setting scales how many and how bright), each a point at a hashed spot with a facet turned at random
  about the normal; it shines where the facet is near the sun's and eye's half-way direction. The cells are 3 cm up
  close and double at each step out (neighbouring steps blended), so a grain stays a pixel or two across out to about
  60 units, fewer of them farther out. Each grain is drawn round on screen, about a pixel across (its distance from
  the pixel taken through the ground's screen derivatives), and most mirror faintly, a few brightly: sized on the
  ground by the pixel's longest side, as the cells are, they were dashes several pixels wide and one high at the
  game's view along the ground (the user's `rvk-frame-152956`). The light is added after the lighting pass's stages (they clip at 1, as D3D's)
  and divided by the sand's mean colour, as the PBR highlights are, so they shine white and well over the bloom's
  threshold. The first build used a fixed 3 cm grid added inside the stages: the glints faded out a few units from
  the eye and came out a dull orange no brighter than 1, and in game the user saw none.

## Phase 4: desert air (built; `RVK_DesertAir`, Effects, 1; `RVK_HeatShimmer`, Effects, 0.5)

- How much of each now: `Device::DesertAir` (sky.cpp). Desert and wasteland playfields are those whose climate leans
  their skies -0.15 or clearer in `kZoneSkies` (Newland Desert, the Wastes, Mort, the Holes, the Longest Road, Arid
  Rift). It needs fair weather (an overcast, a storm's sky or a sandstorm takes it away) and the sun up; the shimmer
  wants it from about 17 degrees, all of it from about 33. Both go to the shaders in `FL.desert` (with the
  projection's m00 and m11 for the shimmer's view rays), and the `land haze:` log line shows them.
- Dust: our sky's lowest band (atmos_view.frag, the push's `dust`) paler, brighter and warmer, fading out by about 17
  degrees up. The haze over the land takes its colour from that horizon (tonemap.frag `HazeColour`, its own paling
  warm instead of cool), and `LandHaze` makes the air thicker (x1.8 at 1) and its plain colour warm.
- Heat shimmer (tonemap.frag `HeatShimmer`): the scene read up to about 1.5 pixels away (at 1440 lines), bilinear, in
  thin wide cells of value noise drifting upwards. Only on land past 60 to 140 units, with rays near level (from about
  9 degrees down to 3 up), and not where the moved read lands on something much nearer.
- Checked in the demo with `--hdr --ground-fog 1276` (the scene fogged as the game fogs Newland Desert, with our sky
  over it) and `--desert-air`, `--heat-shimmer`. No measurable cost.

## Phase 5: footprints in the sand (built; `RVK_SandPrints`, Terrain, 1)

- Where: the characters the grass follows (`UpdatePushTrail`'s heads: a body's centre, its lowest point). Each leaves a
  print 0.7 units on from its last, 11 cm beside its way, left and right by turns (only the body is known, not its
  feet); a jump of over 4 units starts afresh. `Device::AddSandPrint` lays one by hand (`rvk_demo --print-walk`).
- The map (`rvk/sand_prints.cpp`, `sand_prints.frag`): RG16F, 2048 texels a side at 2 cm (41 units), laid toroidally
  round the eye, two images by turns (one frame's map read, the next written). A full-screen pass each frame clears
  what the eye's move brought in, relaxes the rest (to a third in 150 s) and stamps the frame's prints (64 at most; the
  rest wait). R: the dent (an oval 32 by 14-17 cm, the heel narrower, its floor flat to half its size), G: a low rim
  round it. At 4 cm a texel a print's walls filtered almost flat.
- Drawn on sand only (`SandPrintAt`, `FL.sand2.yzw`): in the march its floor 3.7 cm under the ripples' crests (about
  2.3 under the sand's mean; at 2.5 it lay no lower than a ripple's trough, at 4.5 a low sun left all of its floor in
  shadow) with the ripples pressed flat in it; in the lighting pass its slope on the normal; in the base pass its floor
  12% darker, its rim 4% lighter. With it, a relief's own shadow in the march takes 0.75 of the light (the sun's), not
  all of it: a print's floor, and the ripples' troughs, had gone near black.
- Not yet: ruts deeper than a few cm displaced in the tessellation; prints on other loose ground.

## Checks

- `rvk_demo --native-ground-file build/pf565.gnd --sky-zone 565 --pixel-lighting --eye 2159.9 30 1519.7 --view-yaw 3
  --view-pitch 0.2 --sun-dir 0.63 -0.72 -0.29` (the user's spot; the game draws with pixel lighting on), with
  `--sand-ripple`, `--dune-light` and `--parallax` at 0 for the old picture. A low sun across the ripples:
  `--sun-dir 0.55 -0.25 0.8`.
- The air: the same with `--hdr --ground-fog 1276 --view-pitch 0.05`, `--desert-air 0` and `--heat-shimmer 0` for
  the old picture.
- In game: the same spot against `rvk-frame-112254.bmp`, and `render thread frame` in `randy-vk.log`.
