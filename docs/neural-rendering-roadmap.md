# UI stabilization and DLSS 5 roadmap

2026-10-29. The baseline is fork `main` at `ab587fd` (`known-working-0.8`), combining ao-vk 0.8.0 with
the UI and terrain fixes. Stages 1 and 2 and their follow-up corrections are included. The current
[feasibility study](neural-rendering-feasibility.md) assesses NVIDIA runtime access, third-party reuse,
architecture, visual acceptance and performance measurement. No neural runtime or bridge is integrated.

## What has landed

| Work | Evidence and remaining acceptance |
| --- | --- |
| UI capture independent of refresh rate and Native Direct | `d710252`; zero refresh means redraw every frame. Native and renderer checks cover the boundary and fallback modes. |
| Overlays separated from world processing | `cf37f87`; labels, floating text, windows/HUD, console and cursor have ordered layers, independent of frame generation. World-only screenshots, composed readback and threaded tests exist. Complete gameplay coverage is still required. |
| Independent window and HUD scaling | `abf8b5c`, followed by clipping, layout, font and interaction corrections. Gameplay confirms usable scaling, retained positions across round trips, stable release-to-apply Preferences and restored icon quality. |
| Higher-resolution GUI text | `6fe876c` uses a common baseline, fitted line height and font-wide pitch. Extended native checks pass; gameplay confirms clean text without clipping. |
| Native terrain fallback | `54b9dc9` preserves original draws for unrecognised or undrawable replacement passes, with bounded diagnostics. `93592f6` retains requested F10/range/far-texture quality on fallback. Gameplay confirms the requested radius and smooth on/off transitions. |
| Sidebar and PF map scaling | `aa9ee47` keeps the native rollup under one display transform and scales custom map submissions once. Native regressions pass; local gameplay confirms the result. |
| Existing temporal rendering | TAA, FSR 2.2, FSR 3.1, motion/depth inputs and AMD frame generation were already present before these contributions. They are useful infrastructure, not new NVIDIA integration. |
| DLSS 5 | No runtime integration, cross-process bridge or NVIDIA hardware validation has landed. |

World-label/floating-text sizes, cursor size, and developer-console/readout size remain excluded from the new
window/HUD scale factors. Independent scaling is a usability feature; exclusion of overlays is the part directly
needed for a future world-only neural pass.

## Stage 1 and 2 corrections

The original terrain retains its requested F10 radius, `RVK_GroundRange` and `RVK_GroundFar` quality while
native replacement is enabled. The earlier optimizer reduced original geometry and texture quality before
knowing whether native submission would succeed. Removing that reduction keeps the first fallback frame ready.
Range changes schedule tessellation even while stationary; unchanged requests do not restart it. Client object
writes remain on the game thread. See the [terrain audit](native-ground-fallback.md).

GUI fonts use a fitted physical cell, common baseline and font-wide pitch while preserving native advances,
wrapping and input geometry. The earlier per-glyph vertical clamp moved descenders independently. Cache ownership
now follows the native font's lifetime. Native checks cover full text surfaces, consecutive lines, tight cells
and 125/150/176/200% scaling. See the [font metrics](ui-font-metrics.md).

The sidebar rollup shares one display transform, preserving native panel stacking and input. PF map tiles,
markers and outlines are transformed once after native zoom and clipping calculations. See the
[UI implementation](ui-separation-scaling.md) and [performance audit](ui-scaling-performance-audit.md).

## Feasibility findings

The official Remix NR dependency package was obtained and inspected: it contains runtime 310.9.16, Vulkan
helpers and an NVIDIA SDK licence, but only Windows x64/ARM64 runtimes. Streamline 2.14.16's inspected x64 release
ZIP names NR in its feature definitions/changelog without supplying the NR plugin or model. Remix's source
provides a concrete Vulkan integration reference. The study records download hashes and source revisions.

The deciding gates are supported invocation with ao-vk's own application identity, and an x86-to-x64 transport
that works under the intended Wine build. ReShade, OptiScaler NR forks, DLSS5-Feeder, Linux Vulkan layers and
Remix were assessed before proposing custom infrastructure. Feeder's current x86 Vulkan path blocks a known
Wine cross-process D3D12 fence failure; a generic present-time injector also misses our world/overlay boundary.

The recommendation is to proceed only with bounded probes before a production integration or renderer
rewrite. AO-specific visual benefit, RTX 5080 GPU time, VRAM and latency remain unmeasured. Preserve the
current UI compositor and ordinary-world fallback regardless of the selected neural route.

## Proposed milestones and exit criteria

1. **Restore terrain quality — locally confirmed.** Retain regression coverage for F10 detail changes, native on/off/fallback,
   stationary changes, zoning and moving CPU/memory cost in Old Athen. The game's original detail must be available
   whenever native replacement does not take a pass.
2. **Finish font metrics and sidebar scaling — locally confirmed.** Retain regression coverage for text, panel
   stacking, PF map content, hover, title-bar/item anchors, Perk tabs, login and scale round trips. Broader browser
   and restart-persistence checks remain part of compatibility testing.
3. **Prove the processing input — pending feasibility review.** Capture world-only and composed frames at 4K across
   world scales 100/75/50, UI refresh 0/60 and cached/redrawn frames. Visualize motion for camera movement, skinning, foliage, water and
   particles, and test cuts, zoning and resolution changes. Specify colour space, motion units/sign/jitter,
   dimensions, reset/frame IDs and image ownership. Existing FSR inputs are evidence to build on, not proof of
   an NVIDIA-compatible contract. The candidate colour boundary is after world processing and before held labels;
   its exact format and position must follow the chosen SDK.
4. **Resolve the NVIDIA integration gate.** First prove official NR creation/evaluation in a standalone x64 Vulkan
   test with a compatible base NGX SDK and own project identity. Validate hardware/driver support, Wine and
   licensing. Prefer an existing helper/transport before a custom worker while retaining the 32-bit client.
   Prove a pass-through round trip is pixel-correct, bounded in latency and survives resize, timeout and worker
   loss without freezing the game. Test on the RTX
   machine and under the intended Wine configuration; the software-Vulkan cloud cannot establish this support.
   Validate external-memory/semaphore support and ownership for that path. Avoid per-frame CPU readback.
   A full 64-bit render worker is a larger alternative if a small post-processing worker cannot meet the contract.
5. **Add an opt-in neural pass.** Once that gate passes, evaluate it on world colour with the SDK's required motion,
   state and controls, then compose the existing overlays. Start with frame generation disabled, preserve an
   immediate unprocessed-world fallback, and measure stability on characters, signs, weather and camera turns.
   Tune available masks and intensity controls for AO's appearance. Add frame generation and pacing only after
   ordinary neural output is correct. SR integration may be a useful separate experiment; it is not a substitute
   for this milestone and is not assumed to be a mandatory DLSS 5 prerequisite.
6. **Accept performance and compatibility.** Compare identical stationary and moving workloads, warm and cold
   font caches, frame-time distributions, CPU submission, GPU pass times, VRAM and input latency. Neural appearance
   enhancement has its own compute cost; do not treat it as an automatic FPS improvement. Retain working TAA/FSR
   paths for unsupported configurations.

Stages 1 and 2 form the accepted 0.8 baseline. The feasibility study is ready for review; runtime, transport
and processing-input experiments are the next decisions before implementation.
