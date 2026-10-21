# AMD's FidelityFX effects beyond FSR 3.1

Status (2026-10-20, branch `amd-fx`): phases 0-4 built and run in the demo (validation clean but the known shutdown
leaks; look-check as before with every mode 0); staged `randy31-amdfx1.dll` for trying in game. Phase 5 not started;
phase 6 optional.

Found on the way:
- SSSR's raw output is clean (the boxes' mirror images); what it showed of the sky was the problem - its environment
  was the probe alone (black where unseen). `sssr_env.frag` now gives it the probe where sure, else the frame's own sky
  where that direction is on screen (as ours takes the sky), else the scene shader's guess. Our sky's readback colours
  (`m_skyRing`, `m_skyUp`) are not in the scene's units (overhead 0.1 against 0.7 on screen): not used.
- `RVK_SssrThick` (ini): its hit thickness in world units (AMD's 0.015 is metres in their samples); 0.5.
- CACAO's shaders gather with offsets: the device enables `shaderImageGatherExtended` where the GPU has it (CACAO off
  otherwise). At its defaults (multiplier 1, power 1.5) it blackened the foot of every wall against ours at the same
  `RVK_Ao`: half the multiplier, power 1.
- VRS: winevulkan (32-bit) has `VK_KHR_fragment_shading_rate` on the RTX 4070 Ti (16 x 16 tiles). At the default cutoff
  (0.05) the demo's checker scene stays nearly all 1 x 1; at 1.0 the 2 x 2 blocks show on the checker's edges. Its
  gain wants measuring in game scenes.
- Demo runs: `zsh` doesn't split `$S` - `${=S}` (a whole argument list as one unknown flag ran the test grid).

FSR 3.1 (upscaler + frame generation) is in (`rvk/fsr3.cpp`, `rvk/framegen.cpp`, `docs/fsr3-plan.md`). The user wanted
to try more of the FidelityFX SDK 1.1.4 (MIT, the same `third_party/ffx-sdk`): the stochastic reflections and their
denoiser (SSSR), AMD's ambient occlusion (CACAO) against ours, variable rate shading (VRS), Brixelizer GI, and SPD /
Parallel Sort. Every one comes in beside ours behind a setting (ours the default) until it proves better.

Not taken: CAS - `rvk/shaders/sharpen.frag` is our CAS already, and FSR 3.1 has RCAS. Blur, DoF and Lens - ours exist.
LPM (HDR displays) - only with an HDR swapchain, which Wine's present path makes a project of its own.

## 0. The SDK

- The components vendored from v1.1.4 into `third_party/ffx-sdk` (`sssr`, `denoiser`, `spd`, `cacao`, `vrs`,
  `brixelizer`, `brixelizergi`, `parallelsort`): GPU headers, components, blob accessors, Vulkan GLSL. The host headers
  were there already.
- `tools/gen_ffx_shaders.py` compiles them (`COMPONENTS`); options with more than two values (VRS's tile size 8 / 16
  / 32, SPD's filter) take as many bits in the permutation key as their value list needs (`VALUES`).
- `rvk/ffx_common.h`: the backend interface over our device per effect (`FfxBackend`) and our images as its resources.
- 32-bit: Brixelizer's baked update description is an opaque array sized for 64-bit pointers - patched
  (`FFX_BRIXELIZER_UPDATE_DESCRIPTION_SIZE`, `third_party/ffx-sdk/README.md`).

## 1. The scene's normals and roughness

No new attachment (every scene pipeline would have needed one more):
- **Normals**: the motion vectors' zw (RGBA16F; written by depth-writing draws only, overwritten, never blended) held a
  PBR surface's view-space normal for our SSR (octahedral, w + 3). While an AMD effect wants them (`FL.screen.z`,
  `SceneNormalsWanted`), every other surface with a normal writes its own there too (w + 7: the per-pixel normal with
  its normal or bump map, else the vertices'). `ssr.frag` reads only the PBR range, so ours looks as it did.
- **Roughness**: the local-fraction and albedo attachments' alphas are blend factors, so there is no free channel.
  `gbuffer.frag` makes it from the reflectivity the scene keeps (attachment 2's G: the more a surface reflects, the
  smoother; 1 where it doesn't reflect) - our SSR's own idea of where reflections go.
- `gbuffer.frag` -> `m_sceneSurface` (RGBA16F): world normal (the depth buffer's where none was written), roughness.

## 2. SSSR + denoiser (`RVK_SsrMode` 1)

`rvk/sssr.cpp`, where `ssr.frag` runs (ours where it can't). Inputs: the HDR scene, depth, the motion vectors filled
(`motion_fill.frag`, as FSR has them), `m_sceneSurface`, the environment probe's atlas - SSSR samples a cube map; its
GLSL callback is patched to read our octahedral atlas (`env_common.glsl`'s layout). Matrices camera-relative (in
double): the world's positions are thousands of units. `sssr_resolve.frag` turns its output into what ours hands the
tone mapping (colour, reflectivity x Fresnel x `RVK_Ssr`).

To check: water, wet ground (`RVK_SsrWet`), glossy floors; ours vs AMD's; validation; GPU time; the depth thickness
(0.015) and roughness threshold (0.5) in our units.

## 3. CACAO (`RVK_AoMode` 1)

`rvk/cacao.cpp`, where `ao.frag` runs. Depth only (it makes its normals from it, as ours), half resolution, bilateral
upscale; `RVK_Ao` its shadow multiplier, `RVK_AoRadius` its radius; the kernel rotated over three frames for the TAA.
`cacao_pack.frag` adds the view depth; the tone mapping reads it at full resolution (`occlusion.glsl` bit 32).

To check: forest, outpost, the native ground; side by side; GPU time; that its output is 1 = open.

## 4. Variable rate shading (`RVK_Vrs` 1, `RVK_VrsCutoff`)

`rvk/vrs.cpp`. Device: `VK_KHR_fragment_shading_rate` with an attachment tile of 16, 8 or 32 the GPU takes. Every
graphics pipeline is made with the shading-rate-attachment flag when the GPU has it (`m_sceneRateFlag`) - switching
makes no pipeline. The scene shader's pipelines take the image's rate (combiner REPLACE); the rest keep 1 x 1.
The image is made before the scene's first rendering of a frame (`BeginRenderingOn`) from last frame's tone mapped
picture and motion; AMD's encoding is Vulkan's.

To check: winevulkan (32-bit) exposes the extension; the overlay of rates (not built yet: a debug view would help);
fps at the outpost and in the forest with the GPU otherwise idle; dithered foliage and text under 2 x 2.

## 5. Brixelizer GI (`RVK_GiMode` 1) - a spike first

Brixelizer builds a sparse distance field of the scene from triangle meshes (static instances once, dynamic ones each
frame) in cascades round the camera; Brixelizer GI traces it for diffuse and specular light. What it needs here:
- geometry as persistent GPU vertex + index buffers: the native ground's patches (`native_ground.cpp`) and the statics'
  vertex arena (`static_gpu.cpp`) - whose indices live in the frame's ring, so they would need a persistent copy, and
  whose snapshots come and go (instances deleted with them, or the field reads reused memory);
- the ground alone would let light into every building: statics are needed before it looks right anywhere but the
  open land.
Spike: the context, the ground and the statics as instances, its debug view in the demo; memory (the context struct is
~24 MB, the baked update ~8 MB, both on the heap) and GPU time. Stop and report if it doesn't fit.

## 6. SPD / Parallel Sort (optional)

SPD comes inside SSSR (its depth hierarchy). On its own only `scene_luma.comp`'s reduction would use it. Parallel Sort
sorts on the GPU; ours sorts small lists on the CPU (`smoke.cpp`). Decide after phase 2.

## Checks (each phase)

`tools/rvk-demo.sh` (validation clean but the known shutdown leaks), `tools/look-check.py` unchanged with every mode
0, A/B images per mode (`rvk_demo --knob RVK_SsrMode=1` etc.), GPU timings with the user's other GPU work paused; then
stage.
