# Terrain plan: cliffs, surface, lighting, geometry, clutter

**Status (2026-10-19):** all four phases built - phase 1 on the game's ground and the native ground, phases 2-4 on the
native ground (docs/native-ground-plan.md), which draws the playfield from its own data. Checked in rvk_demo on
Stret West Bank's real data; not yet in game. Settings in the options' Terrain section; docs/rvk.md, "Ground
texturing" and "Native ground".

## Context

AO's terrain (`AnarchyGround_t`) is what makes the game look most dated. The frame dump
(`~/.wine-prk/drive_c/linux/logs/rvk-frame-195205.*`, Rubi-Ka canyon at dusk) shows the problems:

- **Cliffs smear.** The base texture's coordinates are projected from above, so on the vertical canyon wall it
  stretches into vertical streaks. The wall reads as one flat orange backdrop.
- **Surfaces are flat and repetitive.** The tile texture repeats visibly and has no detail up close. Large areas are
  one uniform colour.
- **Lighting is coarse and baked.** Each chunk has a 64×64 lightmap with the sun baked in. The screen-space AO radius
  (1 unit) is far too small to darken a canyon foot or a valley.
- **Shapes are low-poly**, and nothing lies on bare ground.

How it is drawn today, confirmed in the dump: 136 chunks, each drawn twice.
1. **Base pass:** unlit, the tile texture on coordinate set 0 (256², sometimes 512²).
2. **Lightmap pass:** lit, a multiply blend, depth test EQUAL, the 64×64 lightmap on set 1. Normal maps, PBR, relief
   and the dawn tint already run here (`rvk/shaders/ffp_main.glsl:653-801`).

A draw is recognised as terrain by `IsTerrain` (`rvk/shadow.cpp:412`), and the two passes by
`m_drawTerrainBase` / `m_drawTerrainLight` (`rvk/draw.cpp:2847`). The lightmap pass finds its chunk's base texture
through `m_terrainBases` / `TerrainChunkKey`. Nothing in rvk does triplanar mapping, detail textures, anti-tiling or
terrain tessellation yet.

All four directions are planned, in phases, with a new public-domain (CC0) rock and detail texture set. Each phase is
built only after the previous one has been checked in game.

## Status

- Done (Phase 1, first part): cliffs laid on their faces, hex-tiling, large-scale variation (its patches laid like
  the texture; procedural layers up cliffs were tried and dropped - blurry stripes, not rock), in
  `rvk/shaders/terrain.glsl`; the lighting pass's normal and PBR maps laid the same way. Settings `RVK_CliffTex`,
  `RVK_TileHide`, `RVK_GroundVary` (names are limited to 15 characters, hence not `RVK_TerrainCliff` etc.). Test:
  `rvk_demo --terrain-test`.
- Found in game (frame dump 2026-10-18): the near ground is not one plane per chunk as Phase 0 assumed. Each 4 x 4
  unit cell has its own four vertices and its own copy of the tile (0..1, turned or flipped one of eight ways, 0.25
  repeats per unit); only the far ground is one plane (1/256 repeats per unit, a map of the zone). So the fit solves
  each triangle's mapping and keeps the tile's scale (`FitTerrain`); the faces and the hexagons are laid in world space
  at that scale, and the slope is the triangle's own (the cells' corner normals are much flatter than a cliff). The
  far map is left as it is.
- Found next (frame dump 20:35, and all 849 of the game's ground textures measured offline): the game reads each
  cell's tile clamped, so anything read past 0..1 smeared into stripes (fixed: the faces read with a tiled sampler).
  And the ground's tiles are a hand-placed set - plain surfaces, transitions, edges, decal tiles (a patch of sand in
  grass, a manhole) - which neither a large-scale colour measure nor a wrap measure tells apart (diagonal transitions
  measure like plain sand). So flat ground keeps the game's layout and sampler, no hexagons; the hexagons are on the
  cliff faces only (Phase 1 item 3 narrowed). The game already turns each cell's tile one of eight ways against
  repeats.
- Done (Phase 1 item 2, one material for now): the cliffs' rock (`RVK_CliffRock`), Poly Haven's "Cliff Side" (CC0),
  embedded (`rvk/assets/terrain`, +2.4 MB), tinted to each cell's tile's mean colour, 24 units a repeat (at 9, in game,
  a canyon wall was rock wallpaper, and offsets up and down chopped its layers into a camouflage pattern: now sideways
  only, the layers bending in a slow wave along the cliff); its normal map gives the faces relief in the lighting pass. A second
  rock for dark or grey ground (Poly Haven's "Dark Rock 02") if the tint alone doesn't carry it.
- Done: `RVK_GroundRange` - the game's full-quality ground radius (150 in the world; its slider stops at 44) scaled,
  1..2.5 (x4 stuttered and cost ~900 MB of the 32-bit address space), by wrapping
  DisplaySystem.dll's `SetHQRadius` / `Tesselate` (docs/rvk.md, "Ground range"). Beyond the radius the game bakes
  each patch into one texture of 8 texels a cell: `RVK_GroundFar` raises that to 16 (or 32) by patching the
  `shl ecx, 3` in `CreateHiTexture` - its memory cost to be measured in game (the log's address space).
- Next: check in game; then the detail layer (Phase 1, item 5), then Phase 2.
- Researched: the game's ground data itself (docs/ground-format.md; `tools/ground-data.py`) - heights, tile codes,
  surface types and six baked lightmaps through the day, per playfield in rdb.db. The way past what the game's own
  ground renderer allows (its CPU split tree, its baked far patches, the 32-bit address space they fill): draw the
  ground in rvk from that data, on the GPU - every cell its own tile at every distance, no stutter, and the real
  heights for Phase 2's light and Phase 4's clutter. Its plan: docs/native-ground-plan.md.
- Next, the deserts: loose sand with ripples, dunes lit by the sun now, and the air over them - docs/desert-plan.md.
- Done: the native ground (`RVK_GroundOn`; docs/native-ground-plan.md) - checked in rvk_demo on synthetic and real
  data, not yet in game. Phase 2 (the terrain's light) now builds on its real heights.

## Phase 0: measure first (a short diagnostic, before writing shaders)

- **Coordinate mapping.** Extend the frame dump (`rvk/diag.cpp`) to fit each terrain chunk's set-0 coordinates against
  world x/z (least squares over its vertices), giving the scale, offset and residual. This confirms the base texture
  is projected from above, and gives the world size of one texture repeat, which the side projections must match.
  Keep the fit per draw: `Device` computes it once per chunk (cached under `TerrainChunkKey`) and passes it to the
  shader.
- **What the lightmap holds.** The game's `SetShadowMap` (`re/lod_ds.c:108`) unpacks a 4-bit shadow map into
  paletted textures. Log a few lightmap palettes at different times of day. If the texel is "shadow amount → palette
  colour", Phase 2 can separate sun from sky exactly instead of guessing.

## Phase 1: cliffs and surface (shader work on both terrain passes)

All of it lives in a new `rvk/shaders/terrain.glsl`, included by `ffp_main.glsl`. It runs only on terrain draws,
which get a new flag `F_TERRAIN` in `constants.glsl` and `internal.h`, set in `draw.cpp` next to `m_drawTerrainBase`
and `m_drawTerrainLight`.

**1. Side projection on steep slopes.**
- Compute a slope weight from the interpolated world normal (`vNormalW`), for example
  `smoothstep(0.55, 0.8, 1 - |n.y|)`.
- Where it is above zero, sample along x and along z, using world position and the measured texture scale. Blend
  the three projections by the normal, with a sharpened weight.
- The top projection is the game's own coordinates, unchanged, so flat ground looks exactly as before.
- Both passes compute the same weight from the same vertices. The lightmap pass's normal map and ORM map
  (`F_BUMPBASE`, `NormalMapNormal`, `ORMTEX` at `ffp_main.glsl:665,682`) switch to the same projection: triplanar
  normals with whiteout blending, so the relief follows the rock.

**2. Real rock on cliffs (the CC0 set).**
- Steep pixels blend from the game's texture to a rock material: CC0 rock with horizontal strata, plus albedo, normal
  and ORM maps.
- The rock is tinted to the chunk's ground texture's average colour (computed once on load), so AO's palette is kept:
  orange sandstone stays orange.
- Which rock goes with which ground texture: a small table, `terrain.tsv` (ground id → material). Any id not in it
  picks a rock by the texture's average hue and brightness.

**3. No repeating tiles.**
- Hex-tile stochastic sampling (Mikkelsen 2022) of the base texture on flat ground. Each hex cell gets a random offset
  and rotation, blended with luminance-preserving weights. This hides the repeat at mid range.
- Rotation is limited for textures with a clear direction.
- It costs three samples, so it fades out with distance and applies only to the top projection.

**4. Large-scale variation.**
- World-space noise, a precomputed 256² tileable texture sampled at about 1/100 and 1/400 units, varies brightness and
  saturation by about ±10-15%. Big uniform areas, like this canyon wall, stop looking painted.
- The cliffs get extra darkening at their feet and in concave parts, using the slope and a cheap height-difference
  term from the existing terrain-sun grid (`rvk/terrain_sun.cpp`).

**5. Detail up close.** Built on the native ground without a new asset (`RVK_GroundGrain`): the cliffs' rock high-passed
at two small sizes. As designed:
- A CC0 detail normal and albedo pair (soil grit, small stones) is tiled at about 1-2 units and blended over the
  texture within about 25 units of the camera.
- It uses the rock detail on steep pixels and the soil detail on flat ones, and fades out before it could shimmer.

**Assets.**
- The CC0 set: about 3 rock materials (sandstone strata, grey rock, dark volcanic) and 2 detail sets (soil, grass
  soil), each 1024² with albedo, normal and ORM maps. Source: ambientCG / Poly Haven, licence noted in the folder.
- They are prepared in `../ao-assets` and shipped in the release zip as `randy-vk\terrain\`.
- Loading reuses `rvk_materials.cpp`'s folder scan, PNG decoding and mip making (`rvk/material_maps.h`). They are
  bound through the bindless table (`texIdx`/`mat` slots, or a few frame-wide slots in `FL`).
- Without the folder, the side projection uses the game's own texture and the detail layer uses procedural noise, so
  nothing breaks.

**Settings** (new group "Terrain" in `proxy/ddraw/rvk_settings.cpp`), all on by default once checked:

| Setting | Controls |
|---|---|
| `RVK_CliffTex` | Side projection (done) |
| `RVK_CliffRock` | Rock material strength (0-1) |
| `RVK_TileHide` | Anti-tiling (done) |
| `RVK_GroundVary` | Strength of the large-scale variation (done) |
| `RVK_DetailTex` | Detail layer strength |

**Things to keep working:**
- The lightmap pass's depth-EQUAL test: positions are untouched in Phase 1.
- The base pass's albedo output feeds GI and wet ground, so the new colour reaches them automatically.
- Grass capture reads the base-pass vertices and the texture's grass classification (`grass.cpp`), not the shaded
  colour, so it is unaffected.

## Phase 2: terrain lighting (designed, built after Phase 1 is approved)

**1. A height field of the terrain.**
- Draw the terrain chunks from above into a 2048² height texture at about 1 unit per texel, around the eye. Rebuild it
  only when the eye moves far, or a new chunk appears.
- It reuses the terrain vertex buffers. It is a depth-only pass with an orthographic camera looking down, like the
  shadow cascades in `shadow.cpp`.

**2. Horizon AO.**
- Scan the horizon in 8-16 directions per texel (a compute pass), giving sky visibility and a bent normal.
- The lightmap pass multiplies its ambient by the sky visibility: canyon feet, valleys and crevices darken properly.
- Setting `RVK_TerrainAo`.

**3. Live sun.** Built on the native ground, more modestly than designed: the game's lightmap holds its sun and
sky together, and a plane in the slope's normal explains each hour's map well (docs/rvk.md, "Native ground"), so
instead of splitting it the map's sun term is turned to where the sun stands now (`RVK_GroundSun`) - flat ground as
baked, slopes following the sun - and the game's whole-hour swaps of the map fade over 8 s. The sky visibility came
from the heights themselves (`RVK_GroundAo`; points 1 and 2 not needed for the native ground).

## Phase 3: geometry (built, on the native ground)

- **Tessellation** (`RVK_GroundTess`): Phong tessellation of the native ground's patches through `ffp.tesc` /
  `ffp.tese`. Each edge's level from its ends and the eye alone (no cracks, within a patch or between two); creases
  not rounded; faded out by 60 units. Both passes alike, so depth EQUAL holds. docs/rvk.md, "Native ground".
- **Displacement** (`RVK_CliffDepth`): on steep ground only, the rock pushed out of the face by its albedo's
  brightness (the set has no height map), at most 0.4 units by default. Flat ground keeps its height (feet, grass).
- The game's own ground (native off) isn't tessellated: its chunks are rebuilt as the camera moves, and their edges
  don't meet vertex to vertex.

## Phase 4: ground clutter (built, on the native ground: rocks)

- Rocks and scree where the heights say rock fell - the feet of cliffs, rough slopes not tiled with grass - as
  procedural meshes, shaded smooth with sharp edges, (no assets), merged per patch and drawn in the native ground's own passes: its rock
  texture, its light, the cliff's colour; casting sun shadows (`RVK_GroundRocks`; docs/rvk.md, "Native ground").
- Not done: pebbles and debris on flat dirt (planned: docs/pebbles-plan.md), pushing
  them aside like the grass.

## Files (Phase 1)

| File | Change |
|---|---|
| `rvk/shaders/terrain.glsl` | New: side projection, hex tiling, variation, detail |
| `CMakeLists.txt` | Add `terrain.glsl` to the shader `DEPENDS` lists |
| `rvk/shaders/ffp_main.glsl` | Base pass colour; the lightmap pass's normal map and ORM through the terrain sampling |
| `rvk/shaders/constants.glsl`, `rvk/internal.h` | `F_TERRAIN`; per-draw texture scale and offset (DrawTransform has spare room in `tess` / `leaf` for terrain draws); frame-wide terrain texture slots and settings in `FL` |
| `rvk/draw.cpp` | Set the flag; compute and cache the coordinate fit under `TerrainChunkKey`; bind the rock material by ground id |
| `rvk/diag.cpp` | Phase 0 fit and palette logging in the frame dump |
| `proxy/ddraw/rvk_materials.cpp`, `rvk/material_maps.h` | Load the `terrain\` folder and `terrain.tsv`; average colour of ground textures |
| `proxy/ddraw/rvk_settings.cpp` | The terrain settings |
| `rvk/rvk.h` | Settings fields |
| `tests/rvk_demo.cpp` | `--terrain-test` scene: a heightfield with cliffs, the game's coordinate mapping and a tile texture |
| `docs/terrain-plan.md` | New: this plan |
| `docs/rvk.md` | Terrain section |
| `CHANGELOG.md` | Entries under `New` |

## Verification

1. **Build:** `cmake --build --preset linux-release`, with no new warnings.
2. **Demo:** `tools/rvk-demo.sh <out> --terrain-test` gives a PNG with no streaking on the cliff and no visible tile
   repeat, and `validation.log` stays clean. Also run with each terrain setting off: the result must equal the old
   output.
3. **Draw path:** `tools/calllog-ab.sh` is not affected (no `[Native]` change). `tools/crash-check.sh` still passes.
4. **In game:** stage the build (`randy31-terrain.dll`, `AO_RANDYVK_DLL` in `~/.wine-prk/drive_c/linux/ao-wine.sh`).
   The user revisits this canyon spot and compares a new frame dump (Ctrl key as before) against
   `rvk-frame-195205.bmp`.
   - Check grass ground, roads (transition tiles), wet ground in rain, and night.
   - Check the frame-time line in `randy-vk.log` (`render thread frame`): budget under about 0.3 ms at 1440p.
5. **Hot reload:** toggling each setting from AOReloaded's options window switches its effect live.
