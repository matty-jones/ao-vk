# UI stabilization and DLSS 5 roadmap

2026-10-28. Stages 1 (terrain quality, `93592f6`) and 2 (font metrics, `6fe876c`), including sidebar/PF map
scaling (`aa9ee47`), have local gameplay confirmation. Text is clean and unclipped, performance is stable,
terrain detail follows the requested radius, and scaled sidebar panels stack correctly.
Stage 3 (processing inputs) remains paused; this branch adds no NVIDIA runtime or processing-input contract.

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

1. **Restore terrain quality — locally confirmed.** Retain regression coverage for F10 detail changes, native on/off/fallback,
   stationary changes, zoning and moving CPU/memory cost in Old Athen. The game's original detail must be available
   whenever native replacement does not take a pass.
2. **Finish font metrics and sidebar scaling — locally confirmed.** Retain regression coverage for text, panel
   stacking, PF map content, hover, title-bar/item anchors, Perk tabs, login and scale round trips. Broader browser
   and restart-persistence checks remain part of compatibility testing.
3. **Prove the processing input — paused.** Capture world-only and composed frames at 4K across world scales 100/75/50,
   UI refresh 0/60 and cached/redrawn frames. Visualize motion for camera movement, skinning, foliage, water and
   particles, and test cuts, zoning and resolution changes. Specify colour space, motion units/sign/jitter,
   dimensions, reset/frame IDs and image ownership. Existing FSR inputs are evidence to build on, not proof of
   an NVIDIA-compatible contract. The candidate colour boundary is after world processing and before held labels;
   its exact format and position must follow the chosen SDK.
4. **Resolve the NVIDIA integration gate.** Verify the Neural Rendering package, hardware/driver checks,
   supported graphics API and licensing. Prototype the smallest viable 64-bit worker with shared GPU images and
   synchronization while retaining the 32-bit client. First prove a pass-through round trip is pixel-correct,
   bounded in latency and survives resize, timeout and worker loss without freezing the game. Test on the RTX
   machine and under the intended Wine configuration; the software-Vulkan cloud cannot establish this support.
   Existing external-memory validation errors must be resolved for that path. Avoid per-frame CPU readback.
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

Stages 1 and 2 are accepted locally. Processing-input work and NVIDIA feasibility are the next milestones;
implementation remains paused until stage 3 is authorized.
