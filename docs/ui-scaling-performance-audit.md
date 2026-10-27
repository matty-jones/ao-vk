# UI scaling performance audit — 2026-10-27

## Evidence and limits

The supplied `ao-vk-profile-analysis.txt` contains 56 enhancement sweep steps, each with GPU and CPU summaries
averaged over 299 frames. It was treated as measurement data. The same-zone user test also reports roughly
35–40 FPS at 100%/100%. That compares enlargement cost, not installed-hook cost: the previous implementation
still traversed the window hierarchy and rebuilt three maps every frame at 100%.

| Sweep step | FPS | Render CPU frame (ms) | GPU total (ms) | Broad game interface interval (ms) |
| --- | ---: | ---: | ---: | ---: |
| Baseline | 34.2 | 29.30 | 9.88 | 2.20 |
| Point shadows off | 41.5 | 24.16 | 9.71 | 2.08 |
| Light override off | 41.8 | 24.01 | 9.45 | 2.25 |
| Per-pixel lighting off | 44.0 | 22.79 | 8.50 | 2.36 |
| Baseline again | 35.0 | 28.65 | 9.62 | 2.22 |

The baseline's point scan costs 5.06 ms; point shadows including that scan cost 5.17 ms. Sun shadow collection
costs 3.84 ms within the 3.94 ms sun-shadow interval. These are inclusive measurements: do not add the parent and
child times. Likewise, overlapping game, renderer and GPU times must not be added into a frame budget.

This capture is primarily CPU limited. Point-shadow collection is a substantial bottleneck independent of
font rasterization. The roughly 5 ms reduction when point shadows are disabled agrees with the measured scan.
It does not establish that UI scaling has zero overhead or explain every part of the reported regression.

In `rvk/pointshadow.cpp`, `RenderPointShadowMaps` scans caster bounds per selected light, computes cube-face
coverage and hashes stationary caster state before deciding whether to reuse a cached shadow cube. Consequently,
cheap GPU shadow draws can coexist with expensive CPU selection even in a stationary scene. Spatial filtering
and cache invalidation are separate optimization candidates; this change does not alter that world pipeline.

The broad `game: interface (after the lists, to EndScene)` interval is not a precise UI-hook timer. Baseline GPU
interface time is about 0.43 ms (0.41 ms at the final baseline). With HDR off, that GPU label grows to 8.03 ms out
of 8.06 ms total: its boundaries absorb world work in that configuration. It is not evidence of an 8 ms UI pass.
The uploaded sweep contains no hook-disabled or scaling comparison, so a numerical scaling-regression verdict
requires another controlled capture.

## Pipeline findings and changes

- `proxy/native/gui_scale.cpp`: 100%/100% now clears old maps once and skips screen lookup, hierarchy traversal
  and map construction. Coordinate hooks take a direct original-function path at 100% and during nested native
  coordinate conversions. Above 100%, hierarchy mapping still runs every frame; it must track moving/new windows.
- `proxy/native/gui_font.cpp`: empty text that cannot allocate a bitmap no longer requests a density rebuild on
  every frame solely because its bitmap remains null. The font cache evicts its oldest entry rather than clearing
  every cached font when full. Font rasterization remains demand-driven; changing text or scale can incur cold-cache
  work. Larger RGBA text surfaces grow approximately with the square of raster density.
- Native text scale is included in raster density. Legacy bitmap-font letters receive an outline fallback while
  inline bitmap symbols retain their images. Native advances and wrapping are preserved. The former per-character
  horizontal squeeze has been removed; small glyph tests check complete coverage against natural GDI outlines.
- Atlas protection now clamps fragment samples instead of changing quad UV endpoints. This preserves thin strokes
  and bottom rows. Eligible scaled UI quads receive extra per-quad bounds; this adds a CPU vertex conversion and
  larger UI vertices, confined to scaled interface drawing. World/cursor draws and the 100% path skip it.
- UI capture still runs independently of scaling and `RVK_UiRate`. Cached frames skip the window draw trees while
  retaining controller input and tooltip work. The scaling bypass below intentionally retains this capture and
  overlay separation, so it isolates scaling/font work rather than reverting the whole feature branch.

New CPU labels are `game: UI scaling`, `game: UI drawing`, `game: UI cached` and `game: UI font raster`.
Font raster time is nested inside UI drawing; do not add both. Drawing includes native controller work and
renderer submission, not just our hooks. Averages include frames where a named section does no work.

## Controlled local comparison

1. Build the branch, keep resolution, camera, zone, open windows, skin, graphics settings and `RVK_UiRate` fixed.
   Stop dragging/moving windows. Wait for loading and font caches to settle. Keep frame generation disabled for
   interpretable base FPS and record whether a frame cap/vsync is active.
2. Open the renderer console (the key left of 1, normally backtick) and run `profileui`. It measures the saved scales,
   100%/100%, 150%/150%, 200%/200%, then the saved scales again, restoring saved settings between steps. It uses
   the existing sweep's 120 settling frames and 300 measured frames per step. Do not change settings during it.
   The log has the new UI CPU labels alongside game/renderer/GPU totals. This compares warm steady-state drawing;
   it intentionally excludes the initial rasterization spike after a scale change.
3. Exit the game. In `randy-vk.ini`, add `UiScaling=off` under the existing `[Native]` section. Restart, reproduce
   the scene, and run `profileui` again. The log must say `interface scaling: disabled ... coordinate and font hooks
   not installed`. All displayed scale steps remain visually native-sized in this run.
4. Restore `UiScaling=on` (or remove that key) and restart. Repeat the first capture to check scene/time drift.
   Compare bypass against enabled 100% to measure installed scaling/font-hook overhead; compare enabled 100% against
   enabled larger values for enlargement cost. If baseline drift is comparable to the difference, repeat the runs.

The startup switch is deliberately not hot-reloaded: removing installed native hooks during gameplay would not
be a controlled experiment. It does not overwrite saved window/HUD percentages or layout. Comparing this switch
cannot isolate the earlier overlay-separation contributions; that needs a matching pre-branch build and scene.

## Automated validation

`native_check --gui-scaling-off-check <client>` verifies that bypass leaves the supported GUI.dll image, entry
points, call sites and font imports unchanged and never reads a window hierarchy. `--gui-client-check` tests the
100% fast path with an intentionally invalid hierarchy pointer, as well as login/character-select native geometry,
font strokes/descenders and the existing input/layout regressions. `--gui-bench <client>` compares unhooked and
installed coordinate pairs and two-window mapping. Wine/QEMU microbenchmarks verify relative code paths; they
are not hardware FPS predictions or substitutes for the local game capture.

The synthetic run measured 6.30 µs per unhooked coordinate pair, 6.73 µs at installed 100%, 6.57 µs at 150% and
6.88 µs at 200%. The scale-frame path took 0.11 µs at 100%, versus 18.07/27.89 µs at 150%/200% for just two windows.
The emulator and timing noise limit comparisons; the invalid-hierarchy regression independently verifies that
100% does not traverse the tree. The complete native comparison and GUI checks passed with zero failures.

`rvk_demo --ui-atlas-test` compares patterned packed allocations against isolated textures at six factors. It
requires matching interior strokes and bottom rows, rejects neighbour bleeding, and checks normal mip sampling
outside the UI. Indexed lists, non-indexed lists and strips exercise the conversion. A valid caller-supplied
two-texture vertex format must retain its native sampling; only internally tagged UI quads receive bounds.
Immediate and threaded HDR atlas runs passed, with 91 contaminated pixels reproduced in each unguarded row and no protected
image differences. Vulkan validation reports the environment's existing external-memory extension dependency
error (`VUID-vkCreateDevice-ppEnabledExtensionNames-01387`); those runs produced no other validation errors.
The final serial threaded HDR/TAA tests at 50% world resolution passed both atlas sampling and overlay composition,
including cached frames, readback, labels and cursor order. Concurrent software-Vulkan runs exceeded the environment's
memory limit and were discarded. The standard `night0` world-look baseline produced no screenshot within its 600-second
software-rendering limit, so no full world-appearance baseline comparison is claimed.

The environment runs the actual supported GUI.dll on synthetic native objects and the Vulkan renderer through
software Vulkan. It does not provide a playable authenticated session or the user's exact Verdana/skin pixels.
Final chat/HUD typography, login placement and local CPU overhead therefore still need the controlled game check.
