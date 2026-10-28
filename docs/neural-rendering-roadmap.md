# UI stabilization and DLSS 5 roadmap

2026-10-28. Source reviewed at `54b9dc9` on `ui-separation-scaling`, with the latest 4K gameplay report.
This is a stocktake and proposed sequence. The terrain-quality and remaining font corrections below are open;
this document does not claim they have been implemented.

## What has landed

| Work | Evidence and remaining acceptance |
| --- | --- |
| UI capture independent of refresh rate and Native Direct | `d710252`; zero refresh means redraw every frame. Native and renderer checks cover the boundary and fallback modes. |
| Overlays separated from world processing | `cf37f87`; labels, floating text, windows/HUD, console and cursor have ordered layers, independent of frame generation. World-only screenshots, composed readback and threaded tests exist. Complete gameplay coverage is still required. |
| Independent window and HUD scaling | `abf8b5c`, followed by clipping, layout, font and interaction corrections. Gameplay confirms usable scaling, retained positions across round trips, stable release-to-apply Preferences and restored icon quality. |
| Higher-resolution GUI text | Outline rasterization into larger private caches is implemented. Baselines, bottom clipping and spacing remain visibly wrong in some controls. This part is not accepted yet. |
| Native terrain fallback | `54b9dc9` preserves original draws for unrecognised or undrawable replacement passes, and adds bounded diagnostics. Gameplay confirms terrain is visible again. Game-side quality ownership is incomplete. |
| Existing temporal rendering | TAA, FSR 2.2, FSR 3.1, motion/depth inputs and AMD frame generation were already present before these contributions. They are useful infrastructure, not new NVIDIA integration. |
| DLSS 5 | No runtime integration, cross-process bridge or NVIDIA hardware validation has landed. |

World-label/floating-text sizes, cursor size, and developer-console/readout size remain excluded from the new
window/HUD scale factors. Independent scaling is a usability feature; exclusion of overlays is the part directly
needed for a future world-only neural pass.

## Immediate corrections

### Terrain quality must follow actual replacement

The visibility fix changed renderer ownership in `rvk/native_ground.cpp`, but did not change the game's optimizer
in `proxy/native/ground.cpp`. `Scaled()` forces the original ground radius to 8 while `g_native` is true;
`ApplyFar()` reduces new far-patch baking to two texels per cell. These were intended to save work while native
patches replace the whole surface.

In `proxy/ddraw/rvk_ground.cpp`, `OnGroundFrame()` calls `SetNative(g_setting)` after decoded data and texture
pointers have been handed over. That is not confirmation that the renderer recognised a pass or submitted a
replacement. Original fallback draws can therefore be visible but artificially low quality. The exported native
terrain test bypasses these game-side radius and baking hooks, so its successful pavement image cannot validate
this transition.

Turning `groundon` off does call `SetNative(false)`, and the tessellation hook is intended to restore the saved
game radius. However, `SetNative()` does not immediately restore that live radius, nor does changing the bake
instruction rebuild existing far textures. The reported persistence with native ground off needs a live-state
transition test; it is not evidence that keeping the tiny radius is intended.

Next implementation: retain the requested F10 radius per live ground, trace requested/effective radius and actual
native ownership, and restore ordinary game quality on fallback, disable and zoning. Invalidate or rebuild the
affected cached geometry/textures where required. Only reduce original work after replacement is established;
mixed recognised/unrecognised passes need a conservative policy. Keep callbacks and game-object writes on the
game thread; the threaded renderer cannot directly modify the live ground object.

Acceptance: in Old Athen, test several F10 detail-radius values with native ground off, fallback active and native
replacement active; repeat on/off/on without restarting, after moving, and after zoning. Confirm correct pavement
beyond the former tiny ring, distinguish original/native submissions, and measure CPU and address-space cost.

### Text needs consistent font geometry

`proxy/native/gui_font.cpp::RasterGlyph()` clamps each glyph's vertical position to fit a fixed line cell.
When an outline exceeds that cell, different letters can move by different amounts. That breaks the common
baseline and is a concrete candidate for the raised `p`/`y` appearance. `Plot()` also clips against the allocated
bitmap, and the final text surface/view clip can remove pixels later. The exact source of each NCU, shortcut and
settings artefact still needs measurement.

The replacement draws natural outline widths but retains original font advances and wrapping. Those advances
may belong to a bitmap font that is being replaced with Verdana, so fresh outlines alone do not guarantee correct
spacing. Fractional placement, native text zoom and surface cropping must be checked together.

Next implementation: record the actual font/metrics for the affected controls, use a common baseline and consistent
run positioning, and give raster caches appropriate ascent/descent/overhang space. Map that space through surface
layout and clipping without changing canonical window rectangles, input or native wrapping. Resolve differences
between bitmap-font advances and outline metrics rather than applying per-letter shifts or width squeezing.

The current small-glyph regression compares total coverage with a natural GDI outline. It can pass even when a
complete glyph has moved vertically; it does not verify baseline placement or final displayed clipping. Extend it
with baseline/bounds checks and complete text-surface rendering: `gjpqy`, `AV To Wa`, `NCU`, digits, consecutive
lines and bottom-edge controls at 100/125/150/176/200%. Include Verdana, bitmap-font fallback, chat and the custom
skin. Native placement, selection, wrapping and scale round trips must remain consistent.

## What DLSS 5 changes in the plan

NVIDIA now describes DLSS 5 as a final neural appearance stage driven by rendered frame colour and engine motion
vectors, with temporal state and developer controls. It is distinct from Super Resolution and Frame Generation.
The public description does not make a deferred-renderer rewrite or path tracing a prerequisite. See the
[technical description](https://research.nvidia.com/labs/adlr/DLSS5/) and
[developer overview](https://developer.nvidia.com/blog/whats-new-for-game-developers-dlss-5-with-3d-guided-neural-rendering-nvidia-ace-updates-and-new-rtx-kit-capabilities/).

The supplied logs identify an RTX 5080, in the currently documented RTX 50 hardware family. That removes one
obvious hardware mismatch, but does not establish a supported SDK, driver or Wine integration.

ao-vk is an x86 Windows DLL inside a 32-bit game. The public
[DLSS library tree](https://github.com/NVIDIA/DLSS/tree/9981c6350dc58c0ce21f024fd65e31e851c31689/lib)
contains Windows x86-64 and ARM64 variants, with no Windows x86 runtime. The inspected
[Streamline build instructions](https://github.com/NVIDIA-RTX/Streamline/blob/625db07d4bca5abc67d781c42af7e7fb0761f1b6/README.md)
target amd64, arm64 and arm64ec. A 64-bit DLL cannot load directly into the game process.

The public Streamline checkout inspected here is 2.14.16, commit `625db07`, dated 2026-10-16. Its checked-in
feature guides cover SR, FG and RR, but do not establish a DLSS 5 Neural Rendering API or supported Vulkan/x86
deployment path. Binary artifacts are distributed separately in release packages. Obtain and inspect the actual
authorized Neural Rendering package before deciding its API, formats, platform support or redistribution terms;
absence of a checked-in guide is not proof that no package exists.

This updates the older rendering plan's broad verdict against neural rendering. The architectural constraints
remain real, but availability and a working integration path need current evidence.

## Proposed milestones and exit criteria

1. **Stabilize the current branch.** Complete the terrain-quality and font corrections above. Retest hover,
   title-bar and item anchors, Perk tabs, login, Friends, maps/browser views and restart persistence. Accept the
   original three contributions when layout and input remain reversible, every tested glyph is complete, icons
   are clean, excluded sizes are unchanged and the original terrain settings work on fallback.
2. **Prove the processing input.** Capture world-only and composed frames at 4K across world scales 100/75/50,
   UI refresh 0/60 and cached/redrawn frames. Visualize motion for camera movement, skinning, foliage, water and
   particles, and test cuts, zoning and resolution changes. Specify colour space, motion units/sign/jitter,
   dimensions, reset/frame IDs and image ownership. Existing FSR inputs are evidence to build on, not proof of
   an NVIDIA-compatible contract. The candidate colour boundary is after world processing and before held labels;
   its exact format and position must follow the chosen SDK.
3. **Resolve the NVIDIA integration gate.** Verify the Neural Rendering package, hardware/driver checks,
   supported graphics API and licensing. Prototype the smallest viable 64-bit worker with shared GPU images and
   synchronization while retaining the 32-bit client. First prove a pass-through round trip is pixel-correct,
   bounded in latency and survives resize, timeout and worker loss without freezing the game. Test on the RTX
   machine and under the intended Wine configuration; the software-Vulkan cloud cannot establish this support.
   Existing external-memory validation errors must be resolved for that path. Avoid per-frame CPU readback.
   A full 64-bit render worker is a larger alternative if a small post-processing worker cannot meet the contract.
4. **Add an opt-in neural pass.** Once that gate passes, evaluate it on world colour with the SDK's required motion,
   state and controls, then compose the existing overlays. Start with frame generation disabled, preserve an
   immediate unprocessed-world fallback, and measure stability on characters, signs, weather and camera turns.
   Tune available masks and intensity controls for AO's appearance. Add frame generation and pacing only after
   ordinary neural output is correct. SR integration may be a useful separate experiment; it is not a substitute
   for this milestone and is not assumed to be a mandatory DLSS 5 prerequisite.
5. **Accept performance and compatibility.** Compare identical stationary and moving workloads, warm and cold
   font caches, frame-time distributions, CPU submission, GPU pass times, VRAM and input latency. Neural appearance
   enhancement has its own compute cost; do not treat it as an automatic FPS improvement. Retain working TAA/FSR
   paths for unsupported configurations.

The current screenshot's roughly 83 FPS is encouraging operational evidence, but cannot be compared directly
with the earlier 35–40 FPS capture from another workload. The
[performance audit](ui-scaling-performance-audit.md) records the controlled scale sweep and its limits. Above
100%, measured mapping was about 0.10–0.11 ms/frame in that session; no isolated hook-cost verdict was obtained
because the on/off sessions had substantially different world draw counts.

The next implementation work is terrain quality restoration and font baseline/clipping correctness. SDK and
64-bit-worker feasibility can be investigated alongside stabilization; broader lighting rewrites or a game port
should wait for evidence that the selected NVIDIA path needs them.
