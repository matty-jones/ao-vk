# Pebbles plan: stones and grit on flat ground

Status (2026-10-20): built, on main for the next release (CHANGELOG.md, Unreleased).

The native ground (docs/native-ground-plan.md) has rocks where the heights say rock fell - cliff feet and rough slopes
(`RVK_GroundRocks`, rvk/ground_clutter.cpp) - but flat dirt, gravel and sand are bare up close: a photo of stones,
flat. This adds small loose stones on the flat ground that is dirt, gravel or sand, near the eye, so the ground at
your feet has real stones lying on it. Grass, roads, concrete and towns stay clear.

**Status (2026-10-19):** first version on branch `pebbles` (rvk/ground_pebbles.cpp), staged for the game to check.
Changed from the plan after looking at it in rvk_demo: 70 a cell at density 1, not 10 - 40 (16 a cell read as a few
stray stones, not gravel); rounder and higher out of the ground (flat ones half sunk were 1 - 3 cm high: invisible);
coloured as the ground under them with the rock's grain only in their brightness (the rock's own hue made brown stones
on grey gravel); made within 44 units, dropped past 52 (memory); the fade a sink into the ground (ffp.vert), not a
shrink. After the first in-game look: trails kept clear (by shape: a narrow strip of its type, not by texture - 790's
footpaths are the same type as its broad sand), three size classes with their own ranges, a cheap shading path, and
casters not remembered (measured in rvk_demo at 2560 x 1440 on gravel: about 0.2 ms of GPU at density 1). The classifier (tools/ground-data.py --types) matches the table below on 790, and Newland Desert, Tir County
and the Perpetual Wastelands use the same tile textures.

## What we know

- **The tiles' surface types** (`tile_type_data`, docs/ground-format.md: key 21 of the ground record, a u16 per
  tile). Decoded for Stret West Bank (790, 249 tiles): the first 24 tiles are the zone's base grounds and each is its
  own type (tile t has type t, t < 24); every other tile has the type of a base ground it blends to (a transition
  tile into gravel has the gravel's type); about 66 decorative / road tiles have type 0. On 790, by the base tile's
  mean colour and cells covered:

  | Type | Looks like | Cells |
  |---|---|---|
  | 0 | light sand-brown (also the catch-all of road / decal tiles) | 124k |
  | 3, 4, 13 | grass (green, dark green) | 74k, 170k, 3.6k |
  | 10 | dark grey gravel (the user's dumps: gravel against dirt) | 139k |
  | 14 | brown dirt | 172k |
  | 7 | pale sand | 5.9k |
  | 2, 5, 6, 8, 9, 11, 12, 15, 17 | browns / beiges (dirt, sand, rock) | 0.2k - 38k |
  | 16, 18, 19, 20 | greys (16 grey ground; 18, 19 near-white: concrete?) | 0.1k - 8.5k |

  So the type groups tiles by ground; which grounds get pebbles still has to be decided from the base tiles' looks.
  Other playfields number their own types (each playfield's first tiles its base grounds - to verify on two or three
  more, `tools/ground-data.py`).
- **In the game's memory:** `AnarchyGroundData_t` +0xD8 / +0xDC, the begin / end of a u16 vector (what
  `AnarchyGroundData_t::Archive` writes as `tile_type_data`, re/ds_grounddb.c:114) - next to the fields
  proxy/ddraw/rvk_ground.cpp already reads (+0x34 .. +0x68). Its codes' mask (+0x60) is 0x3FFF when it is present.
- **What exists to build on** (rvk/ground_clutter.cpp): procedural stone shapes (icosahedra, lumped, smooth normals
  with creases), placement per 64-cell patch on worker threads (`BuildNativeClutter`, async, 2 at a time), merged
  static meshes in the ground's format drawn in the ground's own two passes (`D.terrainAt.y` 2: terrain.glsl's
  clutter path - the rock texture tinted to the cell's colour, the ground's lightmap turned to their faces),
  placement on the drawn triangles (`NativeCellHeight`, internal.h), the small ones drawn within 90 units
  (`kSmallRange`), shadows cast. `GrassyTiles()`: a tile is grass by its texture's mean colour.

## Approach

1. **Read the types.** proxy/ddraw/rvk_ground.cpp reads +0xD8/+0xDC into `NativeGround::types` (u16 per tile; empty
   when absent - no pebbles then). `tools/ground-data.py --export` writes them too (the export format gets a version
   bump, `AOGND2`, so `rvk_demo --native-ground-file` sees them).
2. **Which grounds get pebbles** (render thread, once a playfield, like `GrassyTiles`): for each type, its base tile's
   texture (tile index = type, when that tile's own type is itself; else the type's most-used tile) classified:
   - green (as `GrassyTiles`) -> none;
   - smooth and grey/white, or strong straight lines (concrete, slabs, road) -> none;
   - otherwise (dirt, gravel, sand) -> pebbles, a density from the texture's fine contrast (gravel: many; sand: few,
     smaller).
   Type 0 is the catch-all of decals and roads: none unless its own base tile classifies as dirt and most of type 0's
   tiles look like it. A hand table can override per playfield and type (`randy-vk\terrain\pebbles.tsv`, optional) -
   the classification will be wrong somewhere, and a player can fix it without a build.
   Measure the classifier on 790's 24 types against the table above before trusting it.
3. **Where in a cell.** A transition tile is part one ground, part another: a pebble is kept only where its tile's
   pixel under it is not grass. Keep a small CPU copy of each pebble-type tile's texture (16 x 16, its colour; from
   the game's texture at load, or the mean per quadrant) and test the pixel at the pebble's cell coordinates, turned
   as the cell is (the turn table in docs/ground-format.md).
4. **Shapes and sizes.** Pebbles 0.04 - 0.25 units, a few grit specks smaller; mostly flat-lying (squashed more than
   the rocks), half sunk. 12-vertex icosahedra, indexed (not the rocks' 3 vertices a face): small ones barely need
   creases. A handful of bigger stones (0.3 - 0.5) on gravel.
5. **Where around the eye.** Far too many to place over whole patches (790 has ~450k dirt/gravel/sand cells): place
   in blocks of 8 x 8 cells (32 units) within ~40 units of the eye, made on workers as the eye moves (the rocks'
   async pattern), dropped beyond ~60. Density about 10 - 40 a cell. No popping at the edge: each pebble's size
   shrinks to nothing over the last ~8 units of the range (a vertex-shader scale by distance, or a per-block fade set
   in the draw record), so they grow in as you walk rather than appear.
6. **Drawing.** As the rocks: the ground's two passes, its clutter path - but tinted to the ground tile's own colour
   at that point (the cell's mean, `NativeMean`), with a random lighter/darker per pebble (a per-vertex value in an
   unused coordinate), and the ground's light. Shadows: none for pebbles under ~0.1 (contact shading in the lighting
   pass instead, or nothing), cast for the bigger stones. Grass capture, mist and terrain-sun heights exclude them as
   they exclude the rocks.
7. **Setting.** `RVK_Pebbles` (Terrain; 0..2 density, 1 by default; 0 none), hot-reloaded: blocks made again
   (as `SetNativeGroundRocks`). kVanilla entry 0.

## Not in this plan

- Pushing pebbles aside under feet (the grass's push map): pebbles don't bend; skip.
- Pebbles on the game's own ground (native ground off) or on `GNDA` mission playfields (no heights, no types).
- Debris (sticks, litter) by zone.

## Checks

- `tools/ground-data.py`: dump each type's base tile and the classifier's verdict for 790 and two other playfields;
  eyeball against the table above.
- `rvk_demo --native-ground-file` (790, AOGND2): a close view of the gravel at the user's spot (eye 1320.5 34 2913,
  yaw 1.0, pitch 0.45; and 1336 27 2930, yaw 0.35, pitch 0.75 - the cliff foot with gravel and grass), the
  validation log clean (only the known `vkDestroyDevice` leak messages at shutdown).
- No pebbles on grass, roads, concrete; transition tiles keep them on their dirt part only.
- Walk speed: no hitch as blocks are made (workers); frame time near gravel in `randy-vk.log`'s `render thread
  frame`, within ~0.3 ms of without.
- Stage `randy31-pebbles.dll`; the user checks Stret West Bank's gravel and dirt in game.
