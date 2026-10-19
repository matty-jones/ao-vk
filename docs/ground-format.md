# The game's ground data

What a playfield's ground is made of, as stored in the client's `cd_image/rdb.db`, and how DisplaySystem.dll decodes
it: the research for drawing the ground in rvk itself (docs/terrain-plan.md). `tools/ground-data.py <playfield> <dir>`
decodes one and renders its heights, its tiles and its lightmaps. Checked on Stret West Bank (790), which decodes into
the zone as the planet map shows it, and against the game's camera positions in frame dumps.

Decompiles: `re/ds_ground_format.c` (`AnarchyGroundData_t(ObjectArchive_c*)`, `Archive`, `AnarchyGroundDataDB_t::
Instantiate`), `re/ds_ground_decode.c`, `re/ds_ground_decode2.c` (`Patch_t::DecompressTileMap`,
`DecompressHeightMap`, `CreateBuildingMap`), `re/ds_grounddb.c`, `re/ds_hitex.c` (the far patches' baking),
`re/ds_tess.c` (the split tree), `re/lod_ds.c`, `re/n3_lod.c`, `re/fq_N3.dll.c` (`n3GroundRenderer_t`).

## Tables

| Table | Key | What |
|---|---|---|
| `rdb_1000009` | playfield id | the ground: heights, tiles, building map, a small baked picture per patch (436 playfields) |
| `rdb_1000007` | playfield id | its baked lighting: six 4-bit maps, one texel a cell, through the day (320 playfields) |
| `rdb_1010006` | tile texture id | the tiles' textures, 256 x 256 (24-byte name block + JPEG); `rdb_1010021` / `1010022` the lower qualities |
| `rdb_1000001` | playfield id | the playfield's name and more (e.g. "Stret West Bank") |
| `rdb_1000003` | playfield id | its placed objects (statels; ao-assets `statels.py`) |

`rdb_1000008` and `rdb_1000039` start with the playfield's size too (not decoded; `1000008` is mostly zero bytes).

## rdb_1000009: two formats

- `CHGA`: 325 playfields, every named outdoor zone (Rubi-Ka, the Shadowlands, ...; about 808 km^2), 320 of them
  with lightmaps. Below.
- `GNDA`: 111 small playfields without a name or lightmaps (2-unit cells, up to 1000 x 500; 15 km^2 in all): the same
  header and tile table, then 16-bit data without the archive - not decoded yet. Probably missions' outdoor maps.

### CHGA

Little endian:

| Offset | Type | What |
|---|---|---|
| 0 | char[4] | `CHGA` |
| 4 | u32 | bytes after this field |
| 8 | u32 | 1 |
| 12 | u16, u16 | the playfield's size in cells (790: 700 x 920) - the grid below is padded beyond it |
| 16 | f32 | world units a cell (4, or 2) |
| 20 | f32 | world units a height step (0.2 mostly; also 0.6, 0.5, 0.4, 0.3, 0.002) |
| 24 | u16 n | the tile textures: n |
| 26 | u16[n] | each tile's texture id in `rdb_1010006` (790: 249) - a tile code's low 14 bits index this |
| ... | | a `fun::ObjectArchive_c` of class `AnarchyGroundDataDB_t` |

The archive: its class name, a table of keys (each a type digit and a name, `obj` last), an object header, then the
fields - u8 key (the key's index in that table), u32 type, u32 element size, u32 bytes, the bytes. Types seen: 3 an
int32, 9 an array of variable-size elements (each u32 size + bytes), 17 an array of object references. The fields, in
order (the same names `AnarchyGroundData_t::Archive` writes):

| Key | Field | What (790) |
|---|---|---|
| 4 | `map_width` | 769: heightmap samples across (cells + 1, padded so the patches fit) |
| 5 | `map_height` | 1025 |
| 6 | `map_modulo` | 769 |
| 7 | `tiletexture_count` | 249 (as the header's table) |
| 8 | `heightmap_compressed_data` | per patch: zlib, (P+1)^2 samples |
| 9 | `heightmap_small_data` | per patch: 4 bytes (byte heights: its corners' heights / 256, shared with the neighbours) or 8 (16-bit heights) |
| 10 | `tilemap_compressed_data` | per patch: zlib, P x P u16 tile codes |
| 11 | `buildingmap_compressed_data` | per patch, optional: zlib, (2P)^2 bits |
| 20 | `texture` | per patch: an `RTexture_t` (`GroundTextureCreator_t`: flags, width, height, pixelformat, bitmap), after the fields |
| 21 | `tile_type_data` | u16 per tile: its surface type (790: 0..23); in memory `AnarchyGroundData_t` +0xD8..+0xDC (read for the pebbles: docs/pebbles-plan.md) |

**Patches.** P, the cells a patch side, is the lowest of (map_width - 1)'s and (map_height - 1)'s trailing zero bits,
at most 6: P = 64 for 790 (12 x 16 patches). Patches are row-major, x first. Patch (px, pz) covers samples
[px P .. px P + P] x [pz P .. pz P + P] (its edge samples shared with the next).

**Heights** (`Patch_t::DecompressHeightMap`): zlib-inflate (P+1)^2 deltas, then sum them down each column, then along
each row - bytes wrapping at 256 (or 16-bit with 8-byte small data). A byte height is then the height x 256
(`FUN_10035db8`). World y = that 16-bit value x the ground's height unit (`AnarchyGround_t` +0x1A0, `SetScale`: the
header's step, / 256 when every patch has byte heights) - for byte heights the byte x the step: 790's 0..255 -> 0..51
units, as rvk's hill shading measures the zone.

**World position.** Sample (column c, row r) is at x = c x cell, z = r x cell: the rows run along +z. (Checked: the
frame dumps' eyes stand a few units over the decoded ground - 2.1 units where the screenshot shows the camera just
over the path - and the far patches' baked textures run along +z the same way.)

**Tiles.** A cell's code: the low 14 bits the tile (an index into the header's table: its texture; into
`tile_type_data`: its surface type), masked by the data's +0x60 (0x3FFF with `tile_type_data`, else 0xFF), the top 2
its turn (all four about equally common - the game turns the tiles against repeats). Drawn, each cell has its own copy
of the tile, 0..1 across it, clamped (docs/rvk.md, "Ground texturing"). With (a, b) a point's place across the cell
along x and z (0..1), its tile coordinates by the turn - from 484 cells of two in-game frame dumps against the
decoded codes (all that matched their texture, 423, fit; the rest were merged cells):

| Turn | u, v |
|---|---|
| 0 | a, 1 - b |
| 1 | 1 - b, 1 - a |
| 2 | 1 - a, b |
| 3 | b, a |

Two turns and two mirrors across a diagonal - the mirrored mappings seen in the game's draws.

**Building map** (`Patch_t::CreateBuildingMap`): 1 bit a half cell (the patch's 2P x 2P), inverted, expanded into a
paletted texture. **Not yet known:** what draws with it (a mask under buildings?).

**Per-patch textures** (key 20): a small picture of each patch (`GroundTextureCreator_t`); probably the 16 x 16
textures the game draws its farthest ground with.

## rdb_1000007: the baked lighting

u32 offsets (one past each image's end), then six PNGs, greyscale, 4-bit texels two to a byte (the high nibble
first, as `SetShadowMap` unpacks them): each (cells across) / 2 x (cells down) bytes - one texel a cell over the
whole padded grid (790: 768 x 1024).
Six times of day: the morning (dark, shadows one way), brighter, noon (the brightest), the afternoon, the evening,
the night (nearly black). Trees' and buildings' shadows are baked in. Values 2..15.

Gamecode.dll hands one to the ground (`AnarchyGround_t::SetShadowMap(data, w, h, shade)`, its caller FUN_100b7906
reading an object's map and a shade): unpacked into a 64 x 64 paletted texture per patch - the lightmap its lighting
pass multiplies by - through a palette of 16 greys, `(i / 15 x 188 + 4) x (1 - shade)` (x a tint, white unless the
environment's flag), set on the ground's palette (+0xA04). Two dumps: at 7.05 h (of the day's 27) map 0, shade about
0.53; at 18.0 h map 3, about 0.45 - the hour picks a map, no blending. The native ground takes whatever the game hands
over (the import hooked), so the hour's rule needn't be known.

## What it costs to hold (790)

| | Size |
|---|---|
| heights, 769 x 1025 x 16 bits | 1.6 MB |
| tile codes, 768 x 1024 x 16 bits | 1.6 MB |
| six lightmaps, 768 x 1024 x 4 bits | 2.4 MB (as one 8-bit array texture: 4.7 MB) |
| 249 tiles, 256 x 256, mipmapped | 87 MB as RGBA8, 22 MB as BC1 - in GPU memory, not the game's address space |

Read once when a playfield loads; nothing about it depends on the camera.

## For the native renderer

What the game does that this replaces: `AnarchyGround_t` (a split tree rebuilt on the game's thread as the camera
moves, two draws a chunk, the far patches baked into 2-texels-a-unit pictures, a radius of full quality). Instead:
the heights and tile codes as textures, the tiles as one texture array, the lightmaps as another; the ground drawn on
the GPU, every cell with its own tile at every distance (their mipmaps doing what the baking did), lit by the
lightmap of the hour (and, later, by the sun from the real heights: docs/terrain-plan.md, Phase 2).

Settled (docs/native-ground-plan.md): the turns (above); the lightmaps (the game's own handed over, its palette); the
hook point (the game's own `AnarchyGround_t`: its data at +0x17C, read the first time it is tessellated; its scale at
+0x198..+0x1A0); collision - `SetScale` gives the heights' scale to `HMOccluder_t`, which keeps its own heightmap, so
not drawing the game's ground changes nothing there.

Still open:
- what the building map is drawn with (nothing of the native ground needs it);
- `GNDA` (the game draws those playfields).
