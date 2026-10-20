# rvk: the Vulkan renderer

`rvk/` is a static library: a Vulkan 1.3 renderer whose front end is shaped like `IDirect3DDevice7`
(same enums, structure layouts and semantics), limited to what Anarchy Online uses
(`docs/d3d7-vocabulary.md`). Randy's `render_t` wrappers will map onto it one to one.

## Design

- **One uber-shader pair** (`rvk/shaders/ffp.vert`, `ffp.frag`) implements D3D7 fixed function:
  row-vector transforms, XYZRHW screen vertices (with D3D's integer pixel centres), up to 8
  point/spot/directional lights with material colour sources, vertex/table fog (linear/exp/exp2, range
  fog), texture-coordinate generation (camera-space position/normal/reflection) and texture matrices
  (incl. projected), the two-stage texture combiner, specular add and alpha test.
- **Per-draw constants** (`DrawConstants`, ~1.5 KB, std140) go into a per-frame ring buffer and are bound
  with push descriptors together with the two stage textures.
- **No pipeline compiles at run time**: three pipelines (point/line/triangle topology class) with
  everything else dynamic: viewport, cull, depth test/write/compare, topology, blend enable/equation
  (`VK_EXT_extended_dynamic_state3`) and the vertex layout per FVF (`VK_EXT_vertex_input_dynamic_state`).
  Attributes a format lacks read from a zero buffer bound at stride 0.
- **Targets**: the main colour target and any `CreateRenderTarget` texture are B8G8R8A8 images that can be
  drawn into and sampled; one D32 depth buffer (main size) is shared. Contents persist across target
  switches and frames (LOAD/STORE), like D3D. Image layouts are tracked per texture; a render target bound
  as a texture is switched to a readable layout automatically (ending and resuming rendering).
  `SetRenderTarget` resets the viewport, as D3D does. EndFrame blits the main target to the swapchain
  (recreated when out of date or after `Resize`) and can save a BMP.
- **Textures**: A8R8G8B8, X8R8G8B8, R5G6B5, A1R5G5B5, X1R5G5B5, A4R4G4B4, L8, A8, A8L8, DXT1-5 (BC1-3),
  with mip levels; D3D's 16-bit layouts match Vulkan's PACK16 formats, luminance/alpha formats use view
  swizzles. `UpdateTexture` uploads a region of one level through the frame's upload command buffer.
- **Depth pre-pass** (`rvk/prepass.cpp`, setting `RVK_Prepass`): the frame's main commands are split at the
  scene's depth clear into `mainA` (before it), a depth-only pre-pass (the clear, then the depth of every opaque
  draw that follows, recorded as the draws come) and `mainB` (the rest), submitted in that order. The scene's
  draws then find the nearest opaque depth already there and early-Z rejects what is hidden, so the expensive
  scene shader runs about once per pixel. "Opaque": no discard, depth written, not blended or blended by an alpha
  that is provably 1 (`AlphaOneCheck`); not characters, swaying plants, labels or particles. A segment ends with the
  scene's rendering (target switch, copy, flush, end of the scene) or at a second depth clear. `ffp.vert` declares
  `gl_Position` invariant so both passes produce the same depth; a pre-passed draw's `LESS` or `LESSEQUAL` becomes `EQUAL`, without a depth write (`RVK_PrepassEq`, hidden; off: `LESSEQUAL` with writes, as before). The write is what mattered: with it, a shader that can discard has its depth test after it, so every see-through fragment of a plant card in front of the visible surface ran the whole uber-shader before being dropped. Demo forest 1440p: foliage 1.58 -> 0.79 ms, 1.74 -> 0.35 shader runs a screen pixel. Backgrounds (draws ignoring depth, not writing it, before anything
  in the segment wrote depth) get the depth bounds test [clear, clear]: they are shaded only where the pre-pass found
  no opaque surface, which is the only place they stay visible.
  **Cut-outs** go in with a second pipeline, `prepass_cutout.frag` (`ffp_main.glsl` built with `RVK_PREPASS_CUTOUT`),
  which computes the draw's alpha as the main shader's cut-out test does and writes depth only for pixels that hide
  what is behind them in any draw order: an alpha tested draw that isn't blended where its test passes, and two-pass
  foliage's core (below). Other blended cut-outs stay out - a partly transparent pixel of theirs writes depth too, and
  drawn before an opaque pixel behind it that the pre-pass knew about, it would blend over a background the pre-pass
  had rejected. Swaying plants go in under the same rules. `RANDYVK_PREPASS_CUTOUT=0` keeps cut-outs out,
  `RANDYVK_PREPASS_SWAY=0` swaying plants.
- **Two-pass foliage** (setting `RVK_FolEdges`, Plants; on): the game draws its foliage blended with depth writes, so
  every partly transparent leaf pixel hides whatever is drawn behind it afterwards and shows whatever was drawn before
  - the tree behind, the sky, seen through some leaves' edges depending on draw order. A blended cut-out that writes
  depth is now drawn as its core (alpha >= 0.95, blended, with depth: its record's `motion.w` = 1) and its soft edges
  (the rest: `motion.w` = 2, its own record) are queued and drawn blended without depth writes over the finished
  scene (`Device::FlushEdges`: when the rendering ends - target switch, copy, flush, scene end - before a clear, and
  before a draw that writes no depth, so effects still come after them). The replay sets each edge's state (cull,
  depth compare, viewport, buffers, per-draw bindings) and goes through `ApplyDynamicState`. With the edges out of the
  depth, the core is exact for the cut-out pre-pass. The edges are depth-tested strictly (LESS), so wherever the
  plant's own core wrote depth they fail before their shader runs. Past `RVK_FolDither` (20 units; 0 = never), while
  the TAA is on, such a draw is not split. It is drawn once, with depth (`motion.w` = 3): a pixel is kept, opaque, where
  its alpha beats a noise value (`FoliageDitherCut`: interleaved gradient noise moved each frame, offset by the
  pixel's depth so the cards stacked over a pixel don't all keep or drop it together), and the TAA averages the
  pattern back into the soft edge. The pre-pass uses the same noise, so the crown is lit about once per pixel, not
  once per layer of edges. Demo forest, 1440p: frame 3.59 ms with every plant two-pass, 3.38 past 30, 3.18 past 15,
  2.96 with all of them dithered (3.21 with `RVK_FolEdges` off).
- **Interface refresh rate** (setting `RVK_UiRate`, General; 0 = every frame, the default): GUI.dll's drawing of the
  interface costs the game thread ~1.5 ms a frame (its view tree, and ~1,000 draws). With a rate, the interface is
  drawn only that many times a second into an image of its own, which is put over every frame. `rnative::gui`
  (proxy/native/gui.cpp) wraps GUI.dll's `WindowController_c::Render` - the interface's per-frame work (what is under
  the mouse, the pointer's shape, tooltips) runs every frame - and makes its top-level `View::_CallRender` calls, the
  drawing, return at once on frames that don't redraw (the backend decides: `RVK_UiRate`, or the renderer has no
  current layer). The renderer (rvk/interface.cpp) ends the scene where the interface's first draw would, draws a
  redrawn interface into `m_uiLayer` (cleared; every draw blends, its alpha accumulating coverage, so the layer is
  premultiplied) and blends the layer over the main target (ONE, INV_SRC_ALPHA) at the end of `Render`. Exact for
  alpha-blended and additive interface draws; an unblended one keeps its texture's alpha as coverage, one multiplying
  the frame is approximated. Hooks are only made with a rate set; the two entries' code is checked (MSVC's exception
  prologue calling GUI.dll's `_EH_prolog`), so another GUI.dll build is left alone (logged).
- **Native ground** (setting `RVK_GroundOn`, Terrain; on; `rvk/native_ground.cpp`, `proxy/ddraw/rvk_ground.cpp`,
  docs/native-ground-plan.md, docs/ground-format.md): rvk draws the playfield's ground itself, from the game's own
  data, instead of the game's `AnarchyGround_t`. The first time the game tessellates or updates a ground
  (`rnative::ground`'s hooks of `Tesselate` and N3's `n3GroundRenderer_t::RunFunction`, which runs each frame - its
  ground at +0xB4, whose `AnarchyGroundData_t` at +0x17C is new) the proxy decodes its patches' heights and tile codes
  (zlib, stb's; deltas summed down then across) and, inside later `RunFunction` calls (the ground alive then), has the
  game make each tile's texture (`FUN_10036594`), 24 a frame; then hands it all over (`Device::SetNativeGround`).
  (`Tesselate` itself runs only when the player crosses into another cell: `RunFunction` compares.)
  Gamecode's `AnarchyGround_t::SetShadowMap` (its import patched) hands over the hour's baked lightmap and shade; a
  ground handed over without one since it was read (the call came first) has it rebuilt from the ground's own patch
  lightmaps (their palette indices in our surfaces' CPU copies, the shade from the palette). The lightmap outlives a
  playfield's ground (it is the hour's). rvk
  builds a static patch of (P+1)^2 vertices per 64 cells (the game's format: set 0 the cell coordinates, set 1 the
  whole playfield's lightmap; `DrawSharedIndexed`, arena-resident; each cell split as the game splits it - what
  characters stand on - unless steep, a diagonal's ends more than a cell apart in height, and then along its diagonal
  of least height difference: a split fixed one way made a row of teeth along a cliff at a slant to the cells;
  `NativeCellAcross`, the rocks placed on the same triangles by `NativeCellHeight`) and a tile-code texture (each cell's tile's bindless
  image and turn). The first ground draw of the game's in a pass draws every patch in view for it with the game's
  state, and every ground draw of the game's is skipped (`NativeGroundReplaces`): the base pass's stand-in texture is
  black, terrain.glsl's native mode reads each pixel's cell's tile (turned, the continuous coordinates' derivatives,
  the game's clamped sampler; `D.terrain.y` the tile-code texture); the lighting pass's texture is the playfield's
  lightmap through the game's palette. So cliff faces and rock (tinted by each cell's tile's smallest mip level), sun
  shadows, hill shade, mist, wet ground and fog apply as on the game's ground. While it draws, the game's own ground
  does the least (`rnative::ground::SetNative`: radius 8, far patches baked at 2 texels a cell). A ground the game
  stops updating for 10 presented frames (a loading screen, indoors) is dropped and the next read anew. The tiles' side-loaded
  albedo maps (in the tile-code texture) and normal maps (a second per-cell texture, `D.terrain.z`: the lighting pass
  reads each cell's tile's own, turned) come with them, made again when a tile's maps or the settings change
  (`NativeMapsSignature`). The sky's visibility over the land (`RVK_GroundAo`, 0.6): a worker thread scans the horizon
  from every height sample, 12 directions to 64 cells, once a playfield (under a second); the lighting pass's lightmap
  is darkened by the hidden part (`D.terrain.w`, `FL.terrain2.x`) - cliff faces, valleys, the feet of walls. The
  lightmap is read at the pixel's own cell (world x, z), blurred over the cells round it on cliffs by their steepness
  (one texel a cell up a 40-unit face drew hard triangles where it crossed from a dark cell to a lit one); the rock's
  tint is the cells' tiles' mean colours filtered between cells (a means texture: `D.terrainAt.x` on the native base
  pass) - per cell it jumped at each crossing (green wedges where grass tiles met a cliff). The ground grass reads the
  native ground per cell (`NativeGroundRgb`: the cell's tile turned; `NativeLightRgb`: the lightmap's texels, kept on
  the CPU). Tessellated near the eye (`RVK_GroundTess`, 0.6: Phong's shape; `RVK_CliffDepth`, 0.4 units): ffp.tesc
  splits each edge by its length and distance (about a third of a unit a segment up close, none from 60 units,
  `kNativeTessDistance`; none where both ends are gentler than ~37 degrees) from its ends alone, so both triangles beside it agree, in this patch and the next, and
  drops triangles out of view; ffp.tese rounds the steep ground (each corner's pull by its slope, ~37 to ~53 degrees: where one can stand the
  ground stays the game's triangles, which characters stand on) towards the vertex normals (the heights' own), not across
  creases (per edge, by how far its ends' normals differ: a cliff's foot and rim keep their shape, Phong overshot them
  into spikes), fading out by the same distance, and pushes the rock out of faces steeper than ~45 degrees by its albedo's brightness
  against its mean at a coarse level (`terrain_rock.glsl`: the lays the fragment shader textures with). Both passes
  tessellate alike (`invariant gl_Position`), so the lighting pass's equal depth test holds; the tessellated draws
  stay out of the depth pre-pass, and lighting and shadow lookups keep the flat triangles' positions. The hour's
  lightmap is fitted as a + b.n of the slope (`FitNativeLight`, least squares over the cells not in a baked shadow:
  84-92 % of Stret West Bank's maps' variance; b's bearing the baked sun's, -129 degrees for 6 p.m. where the game's
  directional light stood at -114), and the lighting pass scales it by (a + b'.n) / (a + b.n) with b' b turned to the
  bearing of the frame's sun (`NativeSunTurn`, `FL.terrainLight`; `RVK_GroundSun`, 1): flat ground as baked, slopes
  following the sun through the hour (the game's ground pass adds the lightmap to its ambient and local lights, so
  it is the lightmap alone that turns). A new hour's lightmap fades in from the last over 8 s (`FL.terrain2.y, z`;
  the game swaps them whole). Rocks (`rvk/ground_clutter.cpp`, `RVK_GroundRocks`, 1): scree at the feet of cliffs
  (a cell under the steepest higher cell within two, by how steep) and a few stones on rough slopes not tiled with
  grass, from the heights alone (none on flat ground); small meshes (icosahedra, split once for the big ones,
  lumped and flattened; shaded smooth over folds under ~50 degrees, sharp at their edges), turned, leaning half with the ground, merged per patch into static meshes in the ground's
  format (set 0 the cell they fell from: their tint, the cliff's colour) and drawn in the ground's own passes after
  its patches (`m_native.clutter`; `D.terrainAt.y` 2): the rock laid from above and both sides by their faces
  (terrain.glsl `TerrainClutter`), the ground's lightmap where they lie turned to their faces by the lightmap's fit
  (`NativeRockLight`: where the bake says the sun reached - the texel against the fit's a + b.y, `NativeSunShare` -
  0.6 of it is the sun's, lit by the face's smooth normal against the frame's sun over the flat ground's, the live
  shadow on that part only; the rest the sky's, darker on their undersides. Not by the fit's b: its slopes all face
  up, so it can't say how a face turned down or away is lit - one hour's b.y of -0.25 lit their undersides three
  times their tops - and with the shadow map their only shading, a low sun's self-shadowing showed hard facets.
  The ground's slopes' turn, too, fades out in a baked shadow). They cast sun shadows (`IsShadowCaster`), aren't tessellated nor seen by
  the grass. Made on worker threads (5-8 ms a patch), 2 patches at a time, within 200 units; small ones drawn within
  90, dropped past 450. Pebbles (`rvk/ground_pebbles.cpp`, `RVK_Pebbles`, 1; docs/pebbles-plan.md): on the
  tiles whose surface type (`NativeGround::types`, tile_type_data) is dirt, gravel or sand by its base tile's look
  (`Texture::m_line`, `m_edge`, `m_grain`, measured at upload: straight joints, grain; green and smooth grey none;
  `randy-vk\terrain\pebbles.tsv` sets a playfield's type by hand), and only on the texels of a tile that are that
  ground (16 x 16, not green, near the base tile's colour), not on trails (a strip of its type at most 5 cells
  across covering at most 40 of the 81 cells round it); up to 70 a cell, 12-corner indexed stones, in blocks of
  8 x 8 cells made on workers within 44 units (~2 ms a block), dropped past 52; three sizes in their own pieces,
  drawn to 16 (grit), 28 and `kPebbleRange` (40) units (`D.terrainAt.y` 3, z the range: ffp.vert sinks them 0.6 into
  the ground over its last quarter). Coloured as the ground under them with the rock's grain, each its own shade by
  a hash of set 0 (where it lies, the same at its corners); shaded cheaply (one rock read along the side they face
  most, no relief, no lightmap blur - the lighting pass was two thirds of their cost). They cast no sun shadows: the
  contact shadows give each its short soft one; the map's were sharp and black to their end, streaks at a low sun
  (and, remembered by the caster cache, every block walked past went on casting out of view). The tiles'
  material maps (RVK_Pbr): each cell's in the maps texture's B, A; the lighting pass is marked PBR with one of them
  standing in (`m_native.anyOrm`) and reads each cell's own (terrain.glsl `NativeOrm`, turned as its tile; the rock's
  on faces and rocks), its colour from the cells' tiles; a cell whose tile has none is lit without PBR, as the game's
  ground draws per tile. Grain at the feet (`RVK_GroundGrain`, 0.6; terrain.glsl `TerrainGrain`, base pass): the
  rock's luminance at 0.9 and 1.37 units, turned apart, each over the same spot three mip levels coarser (a high-pass:
  the photo's bands and patches at that size made blotches, its grit is left), from above, fading out as a repeat
  shrinks under ~80 pixels. Tiles blended across cells' edges (`RVK_GroundBlend`, 1; terrain.glsl `NativeTop`, base
  pass): the 2 x 2 cells about the nearest corner over a band of 0.3 of a cell each side of an edge, wandered by noise,
  each neighbour's tile mirrored about the shared edge, weighted with its brightness; only edges whose tiles' colours
  there disagree (`NativeEdgeOff`: a road's slabs, the game's transition tiles keep their edges). The lighting pass's
  normal and material maps keep each cell's own. Its cliffs' share (terrain.glsl `TerrainMapAt`) from the interpolated normal
  (the game's ground: each triangle's own, which on the native ground cut the rock off at the triangles' edges under
  a rim), starting earlier (~23 to ~45 degrees) and wandered by noise. Loose sand (docs/desert-plan.md): which tiles are
  sand comes from the pebbles' table (`PebbleTable::sand`: its ground taken for sand - warm, bright, fine; or the hand
  table's fifth column - times the share of its texels that are that ground), made whenever the playfield has surface
  types (`EnsurePebbleTable`), into the means texture's alpha (filtered: it fades over a cell's edge; both passes have
  it, `NativeSand`). On it: wind ripples (`RVK_SandRipple`, 1; shaders/parallax.glsl) - small ones 0.36 units apart,
  2.8 cm deep, the wind's larger ones 5.5 apart in patches, crests across a fixed bearing for each playfield
  (`SandWindBearing`), warped by value noise read once a pixel with its gradient (each step after that a fract);
  marched in both passes alike within ~45 units, their depth there x `RVK_Parallax` (parallax: the cells' coordinates
  moved to where the eye's ray meets them, rays followed to ~83 degrees off the normal, 3-10 steps by the wavelengths
  crossed, no depth written; and a few steps towards the sun for the troughs' shadow, on the lightmap's sun share), their
  slope on the lighting pass's normal (through `gLightmapRelief`), and their depth in the base pass's colour (troughs
  darker, crests lighter: lit along their crests, the relief alone barely showed); each size fading out as its
  wavelength nears a few pixels across its crests; none on slip faces past ~32 degrees; the rock grit off on sand (+0.4 ms at
  2560 x 1440 in rvk_demo on Newland Desert). The dunes lit by the sun now (`RVK_DuneLight`, 0.6; terrain.glsl
  `NativeSandLight`): as the rocks, 0.75 of the light where the bake says the sun reached is the sun's, by the smooth
  normal's Lambert over the flat ground's, the lee a little cooler - the bake's flat sky share made the dunes a felt at
  noon. On sand the pebbles are few (0.05) and of the sand's hue, and grains glint in the sun near the eye
  (`RVK_SandGlint`, 0.5; parallax.glsl `SandGlint`: cells from 3 cm, doubling with distance, a hashed facet each, near
  the half-way direction; added past the stages' clip, divided by the sand's colour). The
  deserts' pale tile (their paths and flats) is packed ground, not sand. Over the deserts the air is dusty and the
  far land shimmers (`RVK_DesertAir`, `RVK_HeatShimmer`: `Device::DesertAir`, our sky's horizon band, the land haze,
  tonemap.frag `HeatShimmer`; docs/desert-plan.md phase 4). Characters leave footprints in the sand
  (`RVK_SandPrints`, 1; sand_prints.cpp, sand_prints.frag): a print a step along the grass's trail of each character
  (`UpdatePushTrail`), stamped into a 2048-texel map at 2 cm round the eye (toroidal, two images by turns, relaxing
  over minutes), read on sand (parallax.glsl `SandPrintAt`) in the march (the ripples pressed to its floor), the
  lighting pass's normal and the base pass's tone. A relief's own shadow in the march takes the sun's 0.75 of the
  light, not the sky's. A cell's tile with a side-loaded height (`_h.png`, docs/materials.md) is marched the same way, in both passes
  (the maps texture bound in the base pass too then: `m_native.anyHeight`, FL.terrain3.w). Not yet: level of detail. `rvk_demo --native-ground-test [--native-on]` (a synthetic playfield drawn the game's way and natively -
  the same picture), `--native-ground-file F` (a real one: `tools/ground-data.py 790 --export F`; `--eye`,
  `--view-yaw`, `--view-pitch`).
  *One pass* (`RVK_GroundOne`, hidden, on): the base pass is deferred to the lighting pass, which draws the ground once
  (`DrawNativeGroundOne`): its own state - lights, lightmap, white fog, `IsMultiplyPass` - drawn as the base pass is
  (opaque, writing depth, in the depth pre-pass, covering the glow), with the base pass's stand-in texture, stage-0
  sampler and fog (its colour in a storm's air, or our sky's horizon) in the draw record's spare words. ffp_main lays
  the base's colour in the same pixel (on a copy of the terrain map, once for both where the tiles have material maps)
  and multiplies it in at the fog, writing the base's albedo; the image matches the two passes (the half-float rounding
  of the base colour in between gone). The base pass's state is kept, and the base pass drawn late as it was should
  something else come before the lighting pass, or the scene end (`DrawPendingGroundBase`). Only while the ground had a
  lighting pass the frame before, per-pixel lit, into the HDR scene. ~0.35-0.4 ms of ~4 at 2560 x 1440 with the ground
  filling the view (Stret West Bank, Newland Desert; demo `--ground-one 0|1`).
- **Ground range** (setting `RVK_GroundRange`, Terrain; 1 = the game's own, 1.5 the default, up to 2.5):
  DisplaySystem.dll's AnarchyGround_t draws the heightmap as a split tree of triangles; within its full-quality radius
  (+0x1A8: entering a playfield the game sets 8, 44 - the option `DisplayGroundFullQualityRadius`, its slider 7..44 -
  then 150, the one it draws with) the leaves are drawn with each 4 x 4 cell's own tile
  (64 texels a unit), beyond it each 256-unit patch is one baked texture of 8 texels a cell (`CreateHiTexture`: 512 x
  512, 2 texels a unit - the blurry land past the near ring). The radius also lifts the split thresholds
  (`FUN_1003216f`: (i x quality / 100)^2.25 + radius^2). `rnative::ground` (proxy/native/ground.cpp) wraps
  `SetHQRadius` (+0x3255B) to scale the game's value, and `Tesselate` (+0x35348, called on the live ground as the
  camera moves) to take up a changed scale - no pointer to the ground is kept; both entries' bytes checked. The
  ground's vertex buffers are made as needed (`FUN_10034658`), so the cost is the ring's area: about 120 full-quality
  ground draws a pass and 24k vertices at the game's own, the square of the scale times that - and the game's own
  rebuilding of the ground as the camera moves. At x4 (600) that was 100-230 ms frames moving and ~900 MB less address
  space in the 32-bit process; x2 had occasional 50-60 ms ones. Hence 2.5 at most.
  **Far ground** (setting `RVK_GroundFar`, Terrain; 8 = the game's, 16 the default, or 32): past the radius,
  `CreateHiTexture` bakes each patch of n cells into one texture of n x 8 texels a side (`mov ecx, eax; shl ecx, 3`,
  DisplaySystem.dll+0x37BDF; capped at the device's largest texture, 8192), each cell's tile shrunk to 8 x 8. The
  setting writes that shift's count (bytes around it checked): 16 texels a cell makes a 64-cell patch 1024 x 1024.
  Each baked patch costs its texture and the CPU copy rvk keeps of a texture the game locked: 0.5 MB each at 8, 2 MB
  at 16, 8 MB at 32 - in the 32-bit address space the log's `address space` line shows. Patches baked after a change
  take it. Decompiles:
  `re/lod_ds.c`, `re/ds_tess.c`, `re/ds_ground_hq.c`, `re/ds_hitex.c`, `re/n3_lod.c`, `re/fq_N3.dll.c`.
- **Detail range** (setting `RVK_DetailRange`, General; 1 = the game's own, 2 the default): N3.dll shows a
  playfield's placed objects zone by zone. When the game's camera moves onto another tile, each loaded zone gets a
  ring by how far its middle is from the camera (radii of the view distance x 0.5 x 0.10, 0.15, 0.30, 0.40, 0.55,
  none under 40 m), and the ring switches the zone's lists of statels on or off. Rocks and shrubs go at the second
  ring, about 150 m with the view distance near 2000, a whole zone at a time, while the statel shadows
  (`RVK_StaticShad`) still cast them. `rnative::rings` (proxy/native/rings.cpp) wraps the ring function
  (`N3.dll+0x28AB7`, its first 21 bytes checked): while it runs the two inner factors are scaled (the outer ones, trees
  and buildings, stay the game's, moved out only to stay in order: scaling all five cost a lot of frame rate), each
  kept within the zones N3 keeps loaded (the view distance x the grid's +0x28), then put back. The game's factors
  aren't always the constants above: with a high detail option they ran 0.2, 0.3, 0.6, 0.8, 1.1. A change is taken up when the
  camera next moves onto another tile. Decompiles: `re/n3_ring.c`, `re/n3_popin*.c`, `re/n3_statel_controller.c`.
  Measured (a 245 fps spot, 60): the interface's game-thread time 1.5 -> 0.34 ms, its GPU time 0.3 -> 0.07 ms;
  cursor, interface look and input unchanged in play. 60 is the recommended value.
- **Dawn, dusk and the night's coming** (`rvk/sky.cpp`, `shaders/sky_common.glsl`; docs/twilight-plan.md,
  docs/dusk-plan.md): one *sun clock* (`UpdateSunClock`) eases the true sun's direction (the game's brightest
  directional light, which by night points up from under the ground) over a second of real time and derives the
  dusk's phases from its height - golden hour, afterglow, blue hour, night (`FL.phase`). The night grade
  (`NightAmount`), the moon's light and shadows, the low sun's light and tint, the stars and the sky all read it, so day
  turns to night over game minutes, not at once; the sun's light ends at the horizon and the moon's starts ~2 degrees
  under it, so the shadow map changes hands with both out. The sky pass (`sky_glow.frag`) lays a twilight palette over
  the game's dome (`RVK_Twilight`, up to 1.5 for more vivid; `RVK_BlueHour`; `RVK_SkyGlow`), handing the sky back to the
  game ~12-16 degrees under; the far land, haze, mist and low clouds fade into the same colours (`SkyHorizonDelta`); the
  clouds take the sunset's colours and stay lit from below after the land's sunset (`RVK_Afterglow`,
  `cloud_light.glsl`); the land's shade takes the sky's hue (`SkyTwilightCpu`, the model on the CPU - keep the two
  alike); the game's stars fade in by the sun's height (`RVK_StarFade`); sunset rays over the hills and through the
  clouds' gaps (`RVK_DuskRays`, `volume.frag`). With the sun near the horizon the log gets a `dusk:` line every ~2 s
  (and `time:` with the hour while `RVK_TimeOfDay` / `RVK_TimeSpeed` run the clock). The demo's `--sun-sweep A B N`
  renders a dusk and `tools/sun-sweep.py` makes a contact sheet and flags jumps between shots.
- **Our sky's colours, its night, rainbows** (`rvk/atmosphere.cpp`, `rvk/sky.cpp`, `shaders/atmos_view.frag`,
  `sky_glow.frag`, `tonemap.frag` Rainbow; docs/sky-plan.md): the physical sky is recoloured by nine RRGGBB keys
  (`RVK_SkyDayTop` ... `RVK_SkyNightLow`, read as on screen: gamma 2.2) as ratios to their references
  (`kSkyKeyRef`, measured with the `sky: keys` log), blended by the sun's height into four ratios a frame -
  overhead, 35 degrees up, low towards and away from the sun. The CPU reads the sky back with them taken out
  (`m_skyPhys`). (A mode following the game's own sky colours was tried and taken out: docs/sky-plan.md.) The sky pass
  draws our night (`RVK_Stars` - the game's points left out -, `RVK_Galaxy`, `RVK_Meteors`, `RVK_Aurora`;
  `UpdateNightSky`), the tone map a rainbow after rain (`RVK_Rainbow`, `UpdateRainbow`, `FL.neon.yzw`). The console's
  `skypreset` sets the nine colours. Demo: `--sky-key I RRGGBB`, `--stars`, `--aurora X`,
  `--rainbow X`, `--profile-window`.
- **Ground texturing** (`rvk/terrain.cpp`, `shaders/terrain.glsl`; settings `RVK_CliffTex`, `RVK_TileHide`,
  `RVK_GroundVary`, Terrain; docs/terrain-plan.md): the game lays its ground texture from above, so down a cliff it
  stretches into streaks. Its near ground gives each 4 x 4 unit cell its own four vertices and its own copy of a tile
  (0..1 over the cell, turned or flipped one of eight ways, read clamped); its far ground is one plane, a map of the
  zone. Each chunk's triangles' mappings are solved once (`FitTerrain`; kept by `TerrainChunkKey`): a chunk whose
  triangles are nine in ten the same number of repeats per unit of a turned or flipped tile, at most 64 units a
  repeat, is "tiled" (the log counts them), and its draw record carries that scale (`DrawTransform::terrain`). The
  shader keeps the game's coordinates and sampler on flat ground - its tiles are placed by hand (transitions, edges,
  patches of another ground in a tile), so nothing there is moved. Where a triangle is steep (its own slope, from the
  position's derivatives - a cliff is one or two cells high and its corners' normals are far flatter; from ~32
  degrees, all of it past ~57) the face gets its own lay along x or z and down, in world space, one lay down the whole
  cliff: rock (`RVK_CliffRock`; Poly Haven's "Cliff Side", CC0, embedded from `rvk/assets/terrain` - albedo, normal
  map, ORM - decoded off the render thread and uploaded inside a frame once in, 24 units a repeat) tinted to the cell's
  tile's mean colour (`Texture::m_mean`, from the grass's colour grid) over the rock's own, its contrast softened a
  quarter towards its mean; or without it the cell's own tile at the tile's scale. Faces read tiled (the normal maps'
  sampler - clamped, the game's way, smears the tile's edge into stripes) through a hexagon grid with an offset per cell
  weighted by the texture's brightness (Mikkelsen's hex-tiling), so a cliff isn't one picture in rows; the rock's
  offsets are only sideways (offset up or down too, its layers were chopped into dashes) and its layers bend in a slow
  wave along the cliff, so they run on across it without stacking into the same rows. Which way a face lies (the lay
  along x or z) follows the interpolated normal, smooth across triangles; how steep it is, the triangle's own. The ground's lighting pass is laid too wherever
  the rock is on, for the rock's relief (its normal map on the faces; a base without one of its own gets none from
  above). The colour varies in patches over the land, laid like the texture (from above on flat ground, on the face where it is
  steep - from above only, they ran down cliffs in streaks).
  Both of the ground's passes lay it the same way: the lighting pass reads the base texture's normal and PBR maps
  through the same lays and weights (`TerrainNormalMap`, `TerrainTexture`). Not covered: the generated bump maps
  (`RVK_BumpOn`) still read the game's coordinates. The frame dump prints each chunk's fit (its `T:` line) and lists
  the ground textures drawn, their colour grids in `<dump>-ground.bmp`. `rvk_demo --terrain-test` draws a canyon wall
  laid out as the game's cells (`--terrain-off` without, `--terrain-maps` with a normal map and PBR material,
  `--terrain-yaw R` turned).
- **Ground grass** (setting `RVK_GrassOn`, Plants; **off** by default): our own grass over the game's terrain, an
  addition on top of its dated foliage cards (grass.cpp, grass.vert / grass.frag).
  - *Ground*: the terrain's own draws are captured (`Device::CaptureTerrain`, from `Draw`) into world-aligned 8-unit
    tiles of quarter-unit cells around the camera: from the base pass each cell's height and its texture's colour
    (`RVK_GrassTex`: grass only where that texel is green - so not on sand, brick or roads, even in a tile atlas), from
    the light pass its lightmap texel. Small textures (the 64 x 64 lightmaps) are kept whole on the CPU at upload,
    bigger ones on a 16 x 16 grid (`Texture::m_thumb`). A cell keeps the finest terrain triangle that covered it (the
    ground's coarse levels of detail overlap its fine ones); 8 x 8-cell blocks skip triangles that can't refine them,
    and a terrain chunk already captured (known by its contents, pass and texture) is skipped whole unless a tile under
    it is newer - so once the ground is known a frame's capture is a hash per terrain draw (~0.05 ms).
  - *Blades*: once a tile's ground has settled its blades are baked (`BuildGrassTile`, ~0.6 ms a tile, at most ~0.6 ms
    of building a frame, nearest first) as 48-byte records (root, axis, height / width / droop, yaw / wind phase,
    colour, light, the ground texel's colour, kind, fade rank, head colour, canopy density) into one device-local pool
    (a VMA virtual block counted in blades, grown by copying); a tile is rebuilt when its ground changes (a finer level
    of detail, a newly seen part, a re-uploaded lightmap, a setting). A random scatter, clumped by low-frequency noise
    and gathered into tufts (jittered centres; blades thin out between tufts and fan out from their centre), thinned
    and shortened towards the edge of the grass ground (a path, a rock) and gone on steep slopes. `RVK_GrassVary` (0-2)
    scales the tufts, dry straw-coloured patches, taller patches and a per-blade hue jitter - every colour still matched
    to the ground texel's brightness. `RVK_GrassFlower` (0-3) scales the share of other kinds: broad blades, seed stalks
    (a straw head) and flowers (in patches of one colour, a few strays). Each subset near-to-far from the tile's centre
    (early-Z); the first quarter is a sparse subset for the edge.
  - *Drawing* (`DrawGrassTiles`, at the game's first blended draw - which comes before the terrain - or at the end of
    the scene): frustum-culled tiles, nearest first, one indexed draw each through a shared index pattern: vertex v of
    blade b is 8 b + v (cross section v >> 1, edge v & 1); near, 6 triangles a blade (a pointed tip's last one has no
    area), from half the radius 4. The vertex shader expands the record into a strip facing the camera about the
    blade's axis, shaped by its kind (`kSectionT` / `kSectionW`). It bends it -
    keeping its length - with the wind (waves running through the field along it, a cross wave, each blade's flutter),
    the gusts (`RVK_GrassGusts`: bands of stronger wind sweeping along it, the bent blades lighter) and the pushers (the plants' push), and lights it per vertex as the
    terrain's light pass lights the ground: (lightmap + its global ambient + its directional light - none under the
    light override) clamped, darkened by the sun's shadow, plus the frame's local lights (a per-tile light mask) with
    their point shadows. The blade's colour is the grass hue at the ground texel's brightness, with a root-to-tip
    gradient and a darker base in a dense canopy, each of mean 1, its root going over to the ground texel's colour - so
    the field is as bright as its ground at any time of day (measured in the demo: within ~4%, ~7% with flowers and
    everything on). `RVK_GrassGlow` adds the sun on the blade itself (scaled by the shadow-casting sun, none at night):
    a rounded blade's lit and shaded edges (zero-mean), the light through its upper part when the sun is behind it,
    a faint sheen along it; a dense canopy keeps a low sun off the lower blades. Towards the edge the field thins
    rather than ends: the dense three quarters shrink away one by one between half and 0.9 of the radius (each at its
    own rank), the sparse quarter widens to cover and shrinks over the last third, and the colour goes over to the lit
    ground's; a blade thinner than a pixel is drawn a pixel wide, its colour going towards the ground's by its
    coverage. The wind clock is the plants' (wrapped hourly - seconds since boot as a float stepped). It writes all
    five scene targets (its albedo for the GI, the local-light and sun shares for AO and contact shadows, motion),
    since the ground isn't under it yet.
  - *Trails* (`RVK_GrassTrail`, with the push on): a 128 x 128 grid of quarter-unit cells around the camera (world
    anchored, wrapping) where each character standing on the captured ground stamps its push - out from under it and
    the way it walks; a cell recovers over ~5 s. Uploaded with the frame (snorm16 pairs, only while something is
    pushed) and read bilinearly at each blade's root: trodden grass lies further over than the live push bends it.
  - The ground is captured afresh after a camera jump or a gap in the grass's frames (zone changes reuse coordinates).
  - Ground still loading: with `RVK_GrassTex`, a base pass without a readable texture isn't captured (the game draws
    ground whose texture is still loading untextured or with a stand-in; classified, all of it would be grass of one
    colour, for good). A captured base texture whose picture changes on re-upload recaptures its tiles' ground. A
    base-pass cell's rank is the coarser of its triangle's and of the world area a texel covers, so a close draw with a
    finer picture replaces a distant one's. The demo's `--grass-bench-loading N` (`--grass-bench-loading-null`) draws
    the ground with a stand-in (untextured) until frame N.
  - *Shadows* (`RVK_GrassShadow`, with the sun's shadows): the blades go into a layer of the sun shadow map of their
    own (`kGrassShadowLayer`, after the cascades), over the nearest cascade's square, after its casters
    (`DrawGrassShadow`, `grass_shadow.vert`): placed as the visible pass will place them (`grass_common.glsl`: shape,
    lean, field edge, wind, gusts; not the characters' push), their width turned square to the sun and at least half a
    texel, shrinking away towards the square's edge. Every blade within 10 units, the sparse quarter beyond. Receivers
    weigh the layer apart from the other casters: a blade takes it at 0.4 (`grass.vert` `SunShadow`, sampled at the
    blade's axis a little towards the sun so it doesn't shadow itself), the scene's surfaces at 0.45 (`ffp_main.glsl`
    `SunVisibility`; the ground under a field, a character's legs in it) - in full, a sunlit field came out 12-20%
    darker; so 5-8% (the demo, shadow strength 0.65), about what it was brighter than its ground. The layer is
    cleared to lit without grass shadows. GPU: ~0.1 ms in the bench.
  - `RVK_GrassEven` (default 1): the blades' colour from one green rather than the ground texel under each (the
    game's ground textures are patchy); still luminance-matched to that colour, the root still blends to the texel.
  - Demo: `--grass-field` (the shadow test's terrain; `--grass-field-tan` / `--grass-field-any` for the filter),
    `--grass-bench` (a 192 x 192 terrain, the camera walking; prints mean and worst frame times; with
    `--grass-field-dist`, `--grass-bench-yaw 3.14` into the sun, `--grass-bench-walker` a character walking through,
    `--grass-bench-night`, `--grass-bench-pitch R` the camera looking down R radians, `--grass-bench-still` the camera
    standing, `--grass-bench-film N` ~60 frames a second with a screenshot every N: film_NNN.bmp; the bench has no other
    shadow caster: add `--grass-bench-walker` for a sun shadow map), `--grass-shadow 0|1`, `--grass-style V,F,G,U,T` (variety, flowers, glow, gusts, trails). The log's `ground grass:` line every 600 frames: blades drawn, tiles, builds and their cost,
    the share of blades lit by a captured lightmap, the capture's cost, the terrain's ambient.
- **Leaves** (setting `RVK_LeafOn`, Experimental; off by default): leaves on the game's trees, shrubs and palms,
  whose canopies are a few big cards with a painted cluster of leaves and twigs each (a 20-unit jungle crown is 54
  triangles) - an addition on top of them (leaves.cpp, `leaf.vert` = ffp.vert built with `RVK_LEAF`). Needs HDR.
  - *Canopies* (`CanopyKind`): lit, static, upright meshes drawn into the scene with depth writes and a texture that is
    mostly holes (mean alpha 0.05 - 0.65 from the texture's 64 x 64 alpha mask, taken at upload: `Texture::m_alphaMask`),
    not one flat plane. 0.2 - 3 units tall: a shrub; up to 45: a tree; a texture at least 2.5 times as long as wide: a
    palm's (or fern's) fronds. Every AO tree is two meshes - a bark trunk and its canopy - so the canopy is its own draw.
  - *Leaves* (`BakeLeaves`, once per mesh and texture, ~4 a frame at most): sprigs scattered over the canopy's cards
    where its alpha mask is leafy (and leafy around), each a card of its own cut from the same texture - a window an
    eighth of it wide (`RVK_LeafSize`), keeping the painted proportions - turned, tilted up to ~40 degrees, lifted off
    the card and a little out of the crown; `RVK_LeafDensity` (1 = each card's leafy area covered about twice). 32-byte
    records in one pool, shuffled so any first part is an even thinning: up to half of `RVK_LeafDist` all are drawn,
    then fewer, none at it. Palms get none: their fronds stay whole and bend.
  - *Drawing* (`DrawLeaves`): just before the canopy's own draw, with its record (`firstInstance`) - the same world
    matrix, lights, fog, shadows and texture - six vertices a leaf from binding 10, both faces. A sprig fades out
    towards its card's rim (`ffp_main.glsl` `LeafMask`): no square cut through the painted cluster.
  - *The canopy's own cards* become an inner core (`RVK_LeafCore`): drawn in a little towards the crown's centre and
    thinned out in place (blocks of texels dropped by a stable hash) - a wide crown's outer leaves would float if it
    shrank more. Cards and leaves are shaded darker towards the crown's inside (`CrownShade`). Both blend back to the
    game's look with distance.
  - *Wind* (`wind.glsl` `BranchSwayAt`): the canopy's cards and its leaves sway together with the branches - smooth over
    the crown, growing out from its centre, the grass's gusts on top (`RVK_LeafWind`); each leaf flutters
    (`RVK_LeafFlutter`); a palm's fronds flex further and bounce at their tips. The trunk doesn't bend (it would come
    apart from the canopy, a separate mesh); the game's own world-matrix sway (palms) stays.
  - *Shadows* (`RVK_LeafShadow`): each sun cascade draws its visible canopies' leaves after its casters
    (`DrawLeafShadows`, `leaf_shadow.vert` = shadow.vert built with `RVK_LEAF`), the canopy's cards cast as drawn (in
    and thinned), and both sway, so the light under a tree moves with its crown. `ShadowRecord` carries the crown.
  - *Falling leaves* (`RVK_LeafFall`): a few slots a canopy (five a tree, one a shrub); now and then - likelier in a
    gust - a slot takes one of its leaves, which drifts down tumbling and spiralling, lies at the object's origin
    height a few seconds and shrinks away (`leaf.vert` `FallingLeaf`). A function of the time only: no state.
  - Cost (demo, 64 trees, per-pixel lighting, sun shadows, 960 x 960): ~0.2 ms GPU. The log's `leaves:` line every 600
    frames counts canopies by kind, leaf draws, leaves drawn and cast, bakes; frame dumps print a `leaves:` line per
    canopy (its kind, leaves baked and drawn, its crown).
  - Demo: `tools/extract-tree.py <abiff id> build/trees` dumps a game mesh with its textures (needs ao-assets);
    `rvk_demo --leaf-tree a.tree[,b.tree] --hdr` stands them in a row (`--leaf-forest N` an N x N grid seen from eye
    height; `--leaf-off`, `--leaf-shadow-off`, `--leaf-density/size/dist/core/wind/flutter/fall`, `--leaf-cam-dist`,
    `--leaf-cam-height`, `--leaf-cam-yaw`, `--leaf-film N`). Trees: 20842 tree_multiplebranch_big, 13147
    jungle_talltree_20m, 6302 desert_tree_fat, 6257 shrubs_treetype_medium, 31932 tropical_palmtree01.
- **Vertex buffers** keep their contents in CPU memory; every draw copies the range it uses into the ring
  buffer, so rewriting a buffer between draws is safe (the game's CPU skinning reuses one buffer).
- Memory through VMA (from the Vulkan SDK); Vulkan entry points loaded at run time from `vulkan-1.dll`.
- Requirements: Vulkan 1.3 with dynamic rendering, synchronization2, demote-to-helper, push descriptors,
  extended dynamic state 3 (blend enable + equation), vertex input dynamic state. All present on the
  RTX 4070 Ti under Wine 11.18.

## Testing

`tools/rvk-demo.sh` runs `tests/rvk_demo.cpp` headless under Wine with the Khronos validation layer and
writes `build/rvk-demo/rvk_demo.png`: nine tiles covering UI quads, a lit textured cube, additive and
alpha-tested effects, fog, two texture stages with texture matrix and coordinate generation,
lines/fan/culling, render to texture mid-frame, every texture format, mip level selection and vertex
buffer semantics. `rvk_demo.exe --window` shows the same scenes animated.

## Not done yet

- `ProcessVertices` (software vertex transform into a buffer), wireframe fill mode (debug only), W-buffer.
- Texture updates take effect for the whole frame being recorded (uploads run before the draws); a
  texture changed after being drawn in the same frame shows the new contents everywhere. Uploads into
  render targets are ignored.
- Palettised (P8) textures: convert to A8R8G8B8 on the CPU in the integration layer if the game uses them.
- The `IDirectDrawSurface7` shims and everything else that maps Randy onto rvk (integration layer).
- Unbound stages sample (0, 0, 0, 1); check against the game.
