# Ray-traced sun shadows and ambient occlusion

Status (2026-10-20, branch `amd-fx`): phases 0-2 built and run in the demo. `RVK_RtDebug 1`: the native ground and
its rocks traced in place (39 meshes, 327k triangles, 22 MB uncompacted). `RVK_AoMode 2`: ray-traced AO, 2 rays a
pixel, 16 frames accumulated - 0.23 ms + 0.07 ms for the structures at 960 x 960 on the RTX 4070 Ti (ours 0.08);
validation clean; look-check as before with the defaults. Staged `randy31-rtao1.dll`.

Phase 3 built (2026-10-21): `RVK_RtShadow 1`, staged `randy31-rtshadow2.dll` (rtshadow1 lacked the setting: built before its table entry). As built, it differs from the plan below:
- The placed objects (statels.cpp: the playfield's own list, what most buildings' and rocks' shadows came from) are
  instances too - their solid pieces, a BLAS per piece built once a place of it is within range (`RtSyncStatels`).
  Without them their map shadows would have stayed on the traced pixels.
- The map isn't thinned: grass (per vertex), water, glass and the light shafts read it too. It gets the cascades again
  (`kUntracedLayer + c`, layers only while wanted: 9 instead of 5, +256 MB at 4096) with only the casters the rays
  lack (`ShadowItem::traced` from `RtTraced`: the draw's instance, or the placed piece's). A pixel the mask covers
  takes mask x those; every other pixel the whole map, as before.
- The trace runs in its own command buffer (`Frame::rays`) recorded at `PrepassEnd`, submitted after the pre-pass
  (and the clouds') and before mainB - whose draws, already recorded, read the mask through `FL.rtShadow`. The TLAS
  is built there (RtUpdate once a frame). The scene shader (HDR variants only) matches its fragment's view depth
  against the mask's.
- The ground casts no shadow rays (instance mask 2; the map's ground shading stands), which also leaves out the
  displaced-ground problem the AO has. Shadow rays terminate on the first hit.
- Denoising: 16 frames through the camera's reprojection (`CameraReprojection`, no motion vectors yet at that
  point), then a depth-aware blur across and down whose reach follows the penumbra (the mean hit distance x the sun
  disc's radius, 0.01 x `RVK_SunSoft` as the map's).
Demo: the statel scene (pack 790) and the native ground: validation clean, look-check as before. Timing still to do
with the GPU idle.

Found on the way: the static-mesh filter (draw.cpp) dropped every draw while `RVK_GroundOne` was on (`IsMultiplyPass`
is true for all of them then) - the native ground never reached Brixelizer either; it takes the native ground's
patches now (`m_native.drawing`). Brixelizer's shaders want `shaderFloat16` and
`shaderStorageBufferArrayNonUniformIndexing` (enabled where present), every slot of its 24-long cascade arrays
bound, and its buffers copyable; the SDK's bindless pool lacked `FREE_DESCRIPTOR_SET_BIT` (patched). The game's
triangles face the rays the right way (no back faces seen from the camera); a ray whose first hit is a back face
started under a surface - the native ground near the camera is drawn tessellated and displaced, its traced copy flat
(the AO went black on a cliff) - and passes through it (rt_ao.comp; the shadow rays will need the same).

The user wants more expensive rendering features after AMD's GI (Brixelizer, `docs/amd-fx-plan.md` phase 5). Ray
queries (`VK_KHR_ray_query` + `VK_KHR_acceleration_structure`) work from our 32-bit DLL under Wine: a probe (a
one-triangle scene with 16-bit indices, a ray query from a compute shader) gave the right hits under Wine 11.19 on the
RTX 4070 Ti, with every ray tracing extension exposed. Wine's 32-bit translation is the same for every GPU, so AMD
cards with ray tracing hardware should work too (RX 6000 and later, Ryzen 6000+ graphics, the Steam Deck); RX 5000 and
older have none. Untested on AMD so far.

Everything here is optional, as the AMD effects are: the device asks for ray queries where the GPU has them (as for
variable rate shading), and every setting falls back to what we have now where it can't run.

## 0. The device

- `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations` and the
  `bufferDeviceAddress` feature, where present (`device.cpp`); `m_rayQuery`; the log says whether.
- VMA made with `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`; the static arena's chunks get
  `SHADER_DEVICE_ADDRESS` and `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY` usage (they are the geometry's source).
- Ray query shaders compiled for Vulkan 1.2 SPIR-V (`GL_EXT_ray_query`).

## 1. The acceleration structures (`rvk/raytrace.cpp`)

- **Meshes**: the draws Brixelizer takes (`BrixNoteDraw`: opaque draws whose vertices and indices the static arena
  keeps - the statics' retained meshes, the native ground's patches). One filter for both (a shared "solid static
  draw" hook in `draw.cpp`).
- **BLAS** per mesh (its vertex slot, index slot, counts, stride), built once (fast trace, compactable), within a
  budget of triangles a frame so a zone's arrival doesn't stall; freed with its arena slot (`ArenaFree`, deferred until
  the GPU is done).
- **Instances** per mesh and place, as Brixelizer's: kept while their slots live, so casters behind the camera (which
  the game doesn't draw) still cast; a mesh drawn somewhere new each frame is left out (doors, lifts).
- **TLAS** made each frame from the instances within range of the camera, their transforms relative to the camera
  (in double): rays start camera-relative, exact at the world's thousands of units.
- Debug (`RVK_RtDebug 1`, ini): the camera's own rays into the TLAS, their hits' normals - against the scene, to see
  what is in it.
- Log: BLAS memory (a city's statics may want a few hundred MB before compaction), instances, build time.

## 2. Ray-traced ambient occlusion (`RVK_AoMode 2`)

After the scene, where CACAO runs (it slots in the same way: occlusion and view depth for the tone mapping).
- Per pixel, two cosine-weighted rays over the normal's hemisphere (`m_sceneSurface`), from the depth's point, out to a
  reach of a few units (`RVK_AoRadius` x k); a hit nearer than the reach occludes, weighted by its distance.
- Temporal accumulation (the camera's reprojection, depth rejection), then the bilateral blur ours has (`ao_blur.frag`).
- What it gains over screen space: occluders off screen or behind others (under bridges, inside doorways, under
  overhangs - wide occlusion ours can't see). Characters and plants aren't in the TLAS: there, ours is kept (the
  smaller of the two where the screen's AO sees something the rays can't).

## 3. Ray-traced sun shadows (`RVK_RtShadow 1`)

The scene is forward shaded: the sun's visibility has to be known while the scene shader runs. The depth pre-pass
(`prepass.cpp`) has the scene's opaque depth before the main commands shade it, so:
- At `PrepassEnd`: a compute pass over the pre-pass depth - per pixel, the point (camera-relative), its normal from
  the depth, one ray towards the sun jittered over the sun's disc (blue noise, a new offset each frame) - into
  `m_rtShadow` (visibility and the depth it was made for).
- Denoising: temporal accumulation (camera reprojection, depth rejection), then a small depth-aware spatial filter,
  wider where the blocker is far (a penumbra that widens away from the blocker, as `CascadeVisibility` does now).
- The scene shader (`SunVisibilityCasters`): a pixel the pre-pass covered (its depth equals the mask's) takes the
  mask; others (blended, cut-out, not pre-passed) the shadow map as now.
- The shadow map, while the rays run, draws only the casters the TLAS lacks (characters, swaying plants, canopies'
  leaves, cut-outs, animated meshes: `ShadowCaster.kind / animated / sway / leafKey`); the mask is multiplied by it.
  Fewer casters: the map gets cheaper too. The rays reach the TLAS's range (beyond the map's distance).
- Same frame, where the map lags one frame.

## 4. AMD's denoiser for shadows (A/B, optional)

The SDK's Denoiser has a shadows mode (the hybrid shadows sample: ray hits as 32-bit masks of 8 x 4 pixel tiles,
denoised with depth, normals and motion). The trace pass can write that format; try it against ours. AMD's Classifier
(which tiles need rays, from the shadow map) doesn't fit a map that holds only characters and plants: not planned.

## 5. Characters and plants in the rays (optional)

- GPU-skinned characters (`skin_gpu.cpp`): a BLAS per piece from the skinned vertices, rebuilt or refitted each frame.
- Cut-out plants: a ray query's candidate loop reading the texture's alpha at the hit (per-instance geometry info:
  the vertices' addresses, their texture coordinates, the texture's bindless slot).
- Swaying plants move in the vertex shader: they stay in the map.

## Checks (each phase)

`tools/rvk-demo.sh` (validation clean but the known shutdown leaks), `tools/look-check.py` unchanged with the new
settings at 0, A/B images (`--knob`), GPU time per pass (the user's other GPU work paused), memory. Wine tests only
while the game is closed (or in a separate prefix for standalone probes). Stage each phase.
