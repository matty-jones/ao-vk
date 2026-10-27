# Horizon plan: the land beyond the view distance, and the zones next door

## Why

The short view and the fog wall date AO more than its textures do. `RVK_ViewRange` already pushes the game's back
plane and fog out (up to 3x), and the native ground draws every cell of the playfield. Past that, though:

- **The playfield ends.** Nothing exists beyond the current zone's edge, so looking across a border shows only fog or
  void. The neighbours are not drawn because the game hosts one playfield at a time.
- **Placed objects stop early.** N3 keeps only the zones near the camera loaded (the view distance x its grid factor).
  Far buildings, cliffs and forests are missing even inside the current playfield, while the ground under them is drawn.
- **Distance has no scale.** The far ground fades into flat fog rather than into air. No skyline shows that the
  world continues.

The goal is a **far field**: the rest of Rubi-Ka, out to a horizon several kilometres away. It covers the ground,
water, buildings, cliffs and forests of the current playfield past the back plane and of the playfields around it,
placed by the planet map, lit by the same sun and lightmaps, and faded by the same atmosphere. It is static scenery
only, never characters.

## What is known (2026-10-25)

- **Where each playfield lies.** `cd_image/textures/PlanetMap/MapCoordinates.xml` has 91 entries giving each
  playfield's world offset (x, z, and scale, always 1). Together with each ground's size (`CHGA` header: cells x cell
  size), this gives a rectangle on one planet-wide grid. Rubi-Ka spans about x 31000..50000, z 24700..49200
  (19 x 25 km). See `tools/planet-layout.py` and the phase 0 results below.
- **The rectangles overlap, and the overlaps agree.** Zones share border strips, and inner zones (cities, Holes in the
  Wall) sit inside the larger zone that holds them. Where two grounds overlap, their heights are **the same terrain**
  once shifted by tens of units. The map offsets are planet-map precision, not exact:

  | Pair | Overlap | Best shift (x, z) | Height difference after it |
  |---|---|---|---|
  | Stret East Bank (635) / Upper Stret East Bank (650) | a 440-unit strip | 20, 32 | 0 (exact) |
  | Stret West Bank (790) / Holes in the Wall (791) | 791 is inside 790 | -16, 12 | 0 (exact) |
  | Newland Desert (565) / Newland (567) | 567 is inside 565 | 8, -12 | 0.1 median |
  | Southern / Central Artery Valley (610 / 590) | strip | no clean match within ±48 | 3-11 |
  | Varmint Woods / Central Artery Valley (600 / 590) | strip | no clean match within ±48 | 3-11 |

  So the offsets can be **refined from the data itself**: cross-correlate the overlaps, then solve all playfields
  together. The pairs that don't match need a wider search, or the zones' height scales differ (byte heights at
  0.2 a step against 0.3), or those borders really are different terrain.
- **Height datums agree.** The matched pairs need no vertical offset (dy 0), so world y is shared across zones.
- **Alternate versions share a rectangle.** Coast of Peace and Coast of Tranquility, Adonis City and Abyss, four
  Penumbras and three Nascenses each have identical coordinates. These are versions of one place (the Shadowlands'
  variants, or a zone before and after an event), and only one of each should be drawn.
- **The skyline is low.** Ground heights span 0..51 units (byte heights at 0.2 a step) up to about 150 in a few zones.
  Large placed objects are buildings, cliff pieces and forests, not single mountain meshes:

  | Playfield | Statels | Bounding radius > 10 | > 25 | > 50 | Largest |
  |---|---|---|---|---|---|
  | Stret West Bank | 14,838 | 2,664 | 50 | 4 | 122 |
  | Newland Desert | 101,657 | 1,487 | 34 | 12 | 183 |
  | Varmint Woods | 44,750 | 11,343 | 2,458 | 100 | 90 |

  So far placed objects are half the picture, not an extra. Without them the horizon is a flat, low line.
- **What we already have to build on.**
  - The native ground draws a playfield from its own decoded data (docs/native-ground-plan.md), with its
    lightmaps and tiles' colours. Heights, tiles and the six lightmaps for any playfield are in rdb.db
    (docs/ground-format.md; `tools/ground-data.py` decodes them).
  - The statel packs (ao-assets `statels`, `<client>/randy-vk/statels/<id>.bin`) already hold every placed object
    of a playfield, with geometry and alpha masks, whether the game has loaded it or not.
  - `RVK_ViewRange` (proxy/native/camera.cpp) already moves the back plane and cull by true distance.
  - The atmosphere (`RVK_Atmos`, docs/scattering-plan.md) fades by true distance.
- **Distribution.** The statel packs and mesh ids are made by ao-assets on the developer's machine and are not in the
  release zip. No Funcom data may be shipped, so a player's far-field data must be built from their own rdb.db.

## Phase 0 results (2026-10-25)

`tools/planet-layout.py` (out: `build/planet/`) lays every playfield out. It matches each overlapping pair by height
correlation (±200 units, whole cells), fits a height scale and base per pair, and solves all the corrections together
(Stret West Bank and Nascense Frontier fixed, false matches dropped and the solve rerun). It also draws each world
hill-shaded, with a seam map.

- **The layout works.** 44 places are joined into two solved layouts, with no pair left more than 8 units off. The
  Stret river runs on across zone lines, and coasts and canyons join. Corrections are up to about 30 units.
  Overlapping ground of the same height went from 27% at the map's places to 55% solved on Rubi-Ka, and from 20% to
  70% in the Shadowlands. Not joined: Broken Shores, the Omni-1 districts, Borealis (they match nothing within
  ±200), and the Shadowlands' small gardens and sanctuaries (alone by design). They keep their map places.
- **The Shadowlands are a world of their own** in the same coordinate range as Rubi-Ka's west (Penumbra lies on The
  Longest Road). By id: 4000 and up. They are laid out separately.
- **A ground's padding isn't terrain.** Past its size (`CHGA` header), the grid repeats its edge samples, so only the
  own size is used.
- **Neighbours are the same terrain, but not always at the same height.** Of the 93 matched pairs, 58 have the same
  heights. In 35, the same shapes are levelled differently. Often that is exactly the ratio of their headers' height
  steps: Central Artery Valley (step 0.3) is 1.500x Upper Stret East Bank (0.2), and Deep Artery Valley (0.2) is
  0.668x Varmint Woods (0.3). But some 0.3-step zones match 0.2-step neighbours at 1.0 (Newland Desert / Newland), so
  the step alone doesn't decide it. **Open:** the scale the game really draws each zone with. `randy31-horizon1.dll`
  logs it per playfield (`native ground: heights - a byte step of ...`). The far field must use the game's scale, and
  where neighbours still differ at a border, blend their heights across the overlap.
- **Versions of one place:** Coast of Peace / Tranquility (100% the same heights), the Nascenses (95-98%), and Adonis
  City / Abyss (82%) share a place. The Penumbras are about 40 units apart, so they aren't grouped. The far field
  draws the version last visited.
- **Overlaps need no footprint rule.** Where grounds overlap they are mostly the same terrain, so the current playfield
  wins inside its own size, and the overlap strip is where neighbours blend.
- **Water is a list of bodies per playfield.** It sits at the end of the playfield resource (`rdb_1000001`; Gamecode
  `PlayfieldAnarchy_t::GetWaters`, `n3WaterData_t`): u32 count; each body has u32 kind (0..4), u32 vertices,
  vertices (f32 x y z), u32 triangles, triangles (3 x u16). Each body is a small polygon at its own level. Borealis
  has nine bodies (y 10, 40, 55, 65, 90, and a fall from 40 to 9.9), matching its logged water surfaces of 10..90.58.
  The scan in phase 0 found it in 142 playfields, not indoors. There is no sea level: ground height 0 is just the
  floor under the water. Still to pin down: where the block starts (phase 0 scanned for it), and the kinds (0 flat,
  1 falls; 2 and 4 seen in Stret West Bank).
- **rdb.db from the DLL is viable.** It uses a rollback journal (not WAL), so a second read-only connection only takes
  shared locks beside the game's (DatabaseController.dll has SQLite built in). The game also exports
  `ResourceDatabase_t::GetRawStream(Identity_t)`, but that runs on the game thread and in its heap. Our own read-only
  handle on a worker is the plan for phase 6.
- **Still to do in game:** the height scale per zone (above); a look at a zone border at `RVK_ViewRange` 1.5 and 3
  (screenshots for before and after).

## How it works

1. **Bake, once per playfield.** The bake writes a compact far-field pack per playfield (`randy-vk/horizon/<id>.bin`):
   - its refined world offset and its footprint (the cells it really owns, not its padding);
   - heights every 4 cells (16 units);
   - a colour map: each cell's tile, mean albedo of the full-quality texture, unlit;
   - the six lightmaps, downsampled;
   - its water bodies (the playfield resource's polygons, each at its own level);
   - the placed objects as **HLOD**: per 512-unit cell, buildings, rocks and cliffs are merged and simplified (vertex
     clustering) with mean texture colours as vertex colours. Trees are kept as instances of a few
     **impostors** per tree mesh: octahedral views baked once, in an atlas;
   - the night's lamps: points from the statel file's light lists, with their colours.

   The whole of Rubi-Ka should come to tens of MB at most, held in GPU memory, not the game's 32-bit address space.
2. **Choose what to draw.** On arrival, the current playfield plus every playfield whose footprint lies within the
   horizon distance is drawn. Where footprints overlap, the current playfield wins inside its own footprint. One version of
   each alternate set is drawn, the one the server last put us in, else a default.
3. **Draw the far field** where the scene shows sky: past the scene's back plane, in a pass of its own with its own depth.
   - **Ground** as GPU chunks with distance LOD (CDLOD-style morphing, so nothing pops), lit by the sun and the hour's
     lightmap.
   - **Water** to the far edge of each zone, and the open sea beyond the coast.
   - **HLOD meshes and tree impostors** at their cells.
   - **Lamps** by night: points of light, so the cities glow on the horizon after dark.

   The far field takes its share of `RVK_Darkness` and reads `m_cond` for the hour and weather, like everything
   else (the October audit's rules).
4. **Hand-over and air.** The scene's own ground and objects end at the back plane, and the far field begins there.
   The seam is a short dithered blend band, and the far field covers the current playfield past it too, so no gap
   shows. With the horizon on, the land fades by the atmosphere's aerial perspective over its true distance, not
   into the flat fog colour at the back plane. Storms and fog banks shorten it as they do now.

## Phases

Each phase is checked in game before the next starts.

### Phase 0: check the assumptions

- **Offsets.** Make `tools/planet-layout.py`: every playfield's rectangle, the overlaps cross-correlated over a wider
  search (±200 units), and a least-squares solve for all offsets together. Report the pairs that don't match and why
  (height scale, real difference). Render the solved layout as one top-down height picture of Rubi-Ka: the zones
  should join with no cliffs at the borders.
- **Footprints.** Find how a ground marks the cells it doesn't own, so overlaps can be resolved. Candidates are height 0
  with a sea tile, the building map, or the planet map's own zone outlines.
- **Alternates and the Shadowlands.** Work out which entries are versions of one place, and whether the Shadowlands
  zones have a layout of their own (the `normal/` map folder?) or are left out.
- **Sea level.** The water height per zone, from the game's water draws (docs/water-mesh-plan.md) or the ground data.
- **The edge today, in game.** What a border looks like now from inside, at `RVK_ViewRange` 1.5 and 3. Take
  screenshots for the before and after.
- **Reading rdb.db from the DLL.** Check that a read-only SQLite open of the file the game has open works under Wine.
  This decides where the bake lives (phase 6).

### Phase 1: far ground of the current playfield and its neighbours

- ao-assets `horizon` command: the ground part of the pack (heights, colours, lightmaps, footprint, offset).
- rvk: the far-field pass with its own depth, the ground with LOD, lit, faded by the atmosphere, and the hand-over
  band at the back plane.
- Setting `RVK_Horizon` (0 off, else the distance in km, e.g. 2..12, default about 6). It needs a cost tag, a
  `kMigrations` entry if its default changes, and `docs/settings.md` regenerated.
- Test: `rvk_demo --horizon-test` on Stret West Bank's pack and its neighbours' (from a saved eye at a border).
  Add a dusk scene to `tools/look-check.py`.

### Phase 2: water and sea

The neighbours' water to their edges, and the open sea past the planet's coasts, through the far-water shading the
near water uses at distance.

### Phase 3: far buildings, rocks and cliffs (HLOD)

The pack's merged cells, drawn for the current playfield past N3's loaded zones and for the neighbours. They are lit
as the scene's statics are at distance: sun, sky and the lightmap under them.

### Phase 4: far forests (impostors)

Tree impostors, instanced and lit from their baked normals, swaying a little in the wind, faded in where the live
trees (and `RVK_DetailRange`) end.

### Phase 5: the night's lights

Lamp points from the light statels, sized by distance and blooming with HDR. Cities show on the horizon by night,
under `RVK_Darkness`.

### Phase 6: the bake in the DLL (distribution)

For players without ao-assets, the bake moves into the DLL: it reads the player's own rdb.db (SQLite, public domain,
compiled in) on a worker thread, the first time a playfield is visited, and caches the result. No Funcom data ships.
The same path could later build the statel packs, so the shadows of unloaded objects reach players too.

## Rules and limits

- **Scenery only.** No characters, NPCs, pets or anything the server sends. The far field is drawn from static data,
  so it shows nothing the game wouldn't show by walking there. N3's rings and the cull are unchanged.
- **Nothing in the game's address space.** Packs load into GPU memory, so they don't add to the 32-bit process's
  heap.
- **Off is the game's view.** With `RVK_Horizon` 0, nothing changes from today.

## Open questions

- The bake's home: ao-assets first (fast to iterate), then the DLL before release (phase 6)? Or straight into the DLL?
- How far is enough? At 6 km the horizon covers about two zones in each direction; at 12 km most of a continent.
- Planet curvature: none in the game. Hiding the far edge with a slight drop (a few metres at 10 km) may read better
  than a perfectly flat world.
