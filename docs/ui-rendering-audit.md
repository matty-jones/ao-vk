
# UI separation audit

Historical source audit, recovered from the prior conversation. Tool and client limitations below describe
that earlier workspace; they are not findings about the prepared implementation environment.

Audited `matty-jones/ao-vk`, `main`, commit `18589675722bfd93b217bb5654c7352360cd0e65` (2026-10-20).
Source review only. No repository files changed; no build or game tests run. This workspace has no clang-cl,
MSVC/Windows SDK, Vulkan SDK, Wine, or client binaries/decompiles.

## Assessment

Separating the world from the UI is feasible and already substantially implemented. Build on the existing layers;
a replacement GUI or renderer rewrite is unnecessary. Making the separation consistent is moderate work.
Independent UI size is a larger task because layout and input still belong to the game's binary GUI.dll.
DLSS 5 integration remains a separate feasibility question.

| Goal | Current state | Remaining work |
| --- | --- | --- |
| Lower world resolution with full-resolution UI | Implemented by `RVK_RenderScale`, TAA/FSR and deferred labels | Regression coverage and edge cases |
| Keep GUI windows in a separate image | Implemented by the GUI hook and `m_uiLayer` | Remove refresh-rate and native-direct dependencies; define fallbacks |
| Keep every overlay outside later world processing | Partial; FSR frame generation has additional layers | Generalize labels, floating text, pointer and console handling |
| Enlarge the UI independently at 4K and above | No independent UI scale found | Logical UI dimensions, drawing/clipping transforms, input mapping and font quality |
| Exclude the UI from future neural rendering | Suitable architecture already exists | Establish a guaranteed world-only processing boundary before all overlays |

## Existing rendering boundaries

- `proxy/native/gui.cpp:30` wraps `GUI.dll!WindowController_c::Render` with `InterfaceBegin`/`InterfaceEnd`.
  It also hooks recursive `View::_CallRender` to skip drawing on cached frames while retaining per-frame GUI work.
  `CheckedEntry` validates the GUI binary before patching it; mismatches are logged and left unhooked.
- `rvk/interface.cpp:32` finishes world rendering, redirects GUI draws to `m_uiLayer`, and clears it to transparent.
  `InterfaceEnd` composites the premultiplied layer over the main image, except when frame generation consumes it
  separately. The layer is currently the same size as the output.
- `rvk/hdr.cpp:542` tone maps at world resolution, resolves/upscales into output resolution, upsamples depth, applies
  motion blur, then replays held labels. UI is drawn afterwards. Scaling currently depends on HDR and TAA
  (`rvk/rvk.h:3066`); it is not a universal scene-size abstraction.
- `rvk/framegen.cpp:226` supplies separate label and top layers. `FgBeforePresent` combines labels, windows, console
  and pointer in that order and registers the overlay with AMD's interpolation swapchain. `FgComposedFrame` restores
  overlays for screenshots/readback. This is useful existing infrastructure for exclusion.
- `rvk/threaded.cpp:1070` queues the overlay boundaries on the existing render thread. Keep this ordering and resource
  ownership when extending the design.

## Gaps and risks

### 1. UI isolation depends on refresh rate and native mode

`proxy/ddraw/rvk_device.cpp:633` returns before opening the UI layer when `RVK_UiRate` is zero. Zero means redraw every
frame, but currently also means draw straight into the frame. The source settings table defaults to 60
(`proxy/ddraw/rvk_settings.cpp:49`), despite older prose in `docs/rvk.md` saying zero is the default.

`proxy/native/device.cpp:612` also bypasses the boundary when `[Native] Direct` is off or the direct callback is absent.
An unrecognized GUI binary cannot provide the hook. Frame generation does not override these conditions.
Consequently its separate-window-overlay path is not guaranteed in every supported configuration.

Separate the decision to isolate GUI rendering from the decision to reuse its previous image. With zero refresh
limit, capture a newly drawn UI every frame. Retain checked hooks and make unsupported isolation visible rather
than claiming a UI-free frame. Exercise native modes and hook failure explicitly.

### 2. Floating text has another route onto the main image

`proxy/ddraw/rvk_particles.cpp:116` holds packed-glyph sprite batches, including damage/XP text.
`DirectInterfaceBegin` flushes these before opening the window layer. `FlushOverlayText` (`proxy/ddraw/rvk_particles.cpp:201`)
ends the scene through `OverlayBegin` and calls the original drawing routine without opening an overlay layer.
This is a concrete path onto the finished main image outside `m_uiLayer`.

Held character labels take a different path: `rvk/draw.cpp:2759` queues recognized quads, and `rvk/hdr.cpp:624` puts
them in a separate layer only when `m_fgUiLayer` is set. The label recognizer (`rvk/shadow.cpp:428`) is a shape,
format and texture heuristic; it is not an exhaustive semantic tag for all text. Do not assume the existing
frame-generation image excludes every kind of floating text. Confirm actual batches with in-game frame captures.

### 3. Exclusion is coupled to frame generation

`rvk/device.cpp:1623` derives `m_fgUiLayer` from the active AMD frame-generation state. Deferred labels and late
pointer draws (`rvk/draw.cpp:2513`) use that flag. Without frame generation, windows are composited at
`InterfaceEnd`, and other overlays can be drawn directly into the main image.

A future world-processing pass needs an explicit boundary before labels and floating text as well as windows.
For a processed LDR world, the useful point is after world post-processing in `EndScene`, before `DrawHeldLabels`;
the existing end-of-frame main image is not generally UI-free. Keep the stage independent of AMD's swapchain.
The final location and color format must follow the future SDK's contract.

### 4. UI scale requires coordinated layout and input

There is no UI logical-size or mouse-coordinate scaling in the reviewed GUI/input paths. Screen-space vertices
are interpreted in viewport coordinates (`rvk/shaders/ffp.vert:333`); viewport/scissor scaling currently applies
only to the world (`rvk/draw.cpp:1964`). The compositor uses `texelFetch` at output pixel coordinates
(`rvk/shaders/ui_composite.frag:8`) and requires matching layer/output sizes.

For 200% UI at 3840x2160, GUI layout should behave approximately as a 1920x1080 logical area, mapped to the full
output. Multiplying existing vertex positions alone would push right/bottom windows offscreen and leave hit tests
behind. Establish the GUI layout/extent and mouse-query paths through binary analysis, then use one transform and
its inverse for rendering, clips, hit tests, dragging, tooltips, menus and browser interaction. Keep world picking
and camera mouse deltas in their existing coordinate space.

Prefer a full-output-size UI target with logical coordinates transformed during rendering. A smaller UI texture
stretched afterwards is a viable interim approach but softens text/icons. Existing bitmap/font textures also limit
sharpness even with full-resolution rasterization; layer separation does not provide higher-resolution glyphs.
World-anchored labels need their screen anchors preserved if their glyph size is changed.

### 5. Preserve compatibility details

- Layer blending already handles coverage and opaque map fills, but destination-dependent multiply blending is
  approximated (`rvk/interface.cpp:5`, `rvk/draw.cpp:2157`). A single ordinary RGBA overlay cannot exactly encode
  arbitrary operations on an unknown world background. Check the blend modes actually used by affected widgets.
- `CopyTexture` resolves a null destination to `m_main` (`rvk/draw.cpp:1868`), independently of UI-layer target
  redirection. `RSurface::DoBlt` can use this route (`proxy/ddraw/rvk_surface.cpp:395`). Determine whether any GUI
  windows use it; audit CPU locks/uploads too before asserting complete capture. Ordinary draw capture alone is
  insufficient evidence.
- Menus/loading screens without 3D have a direct-render fallback when the main target is still floating point
  (`rvk/interface.cpp:38`). Test transitions and cached-layer invalidation on resize/scale changes.
- Check maps, browser views, embedded 3D previews and temporary render targets. A blanket classification of all
  pre-transformed geometry as UI would capture water too (`rvk/draw.cpp:708`). Preserve depth occlusion for labels
  and preview depth/viewport behavior.
- Preserve screenshots and DirectDraw `Lock` readback through the composed-frame path (`rvk/device.cpp:2010`).
  Generalizing the layer path must preserve resource lifetimes and the existing frame-generation buffering.

## Recommended contribution sequence

1. Make window isolation independent of `RVK_UiRate`; preserve 100% appearance and current refresh behavior.
   Establish what happens with native-direct mode off and unrecognized GUI binaries.
2. Generalize existing overlay boundaries outside frame generation, explicitly including packed floating text.
   Expose a world-only stage and retain the current ordering: world, labels/text, windows, console, pointer.
3. Prove the logical GUI dimensions and input mapping, then add an optional `RVK_UiScale` (default 100%). Keep
   the physical output size and world render scale independent. Extend existing settings/threaded interfaces.
4. Evaluate DLSS 5 separately. This project is an x86 Windows DLL using Vulkan 1.3 and Wine. Confirm the SDK's
   architecture, API, runtime, hardware, licensing and input requirements before committing to an integration.
   FSR support and existing depth/motion data do not establish DLSS 5 compatibility or required input quality.

Follow `CLAUDE.md`: explicit CMake source/shader lists, no manual edits to generated files, unchanged binary
layouts/vtables, the existing settings table and generated settings documentation, and short player-facing
changelog entries for visible changes. Keep new code attribution to the user, as requested.

## Validation before implementation is accepted

Extend `tests/rvk_demo.cpp`'s existing `--shadow-test --ui-window` scene for layer boundaries, skipped redraws,
floating text, overlap/coverage and scaling. Run `tools/rvk-demo.sh` with Vulkan validation and `tools/look-check.py`
for unchanged world rendering. Use native/calllog checks where native behavior is changed; synthetic rvk scenes
alone do not exercise GUI.dll hooks or input.

In the actual client, capture both world-only and composed images at 1080p, 1440p and 4K; world scales 100/75/50;
UI refresh 0/60; frame generation off/on; and HDR/TAA fallback configurations. Exercise label occlusion, damage/XP
text, maps, browser, previews, dragging, tooltips, cursor alignment, screenshots, resizing and loading transitions.
At 100% UI scale, the composed frame should match the existing appearance. At larger UI scales, layout, clipping
and hit regions must agree, with all excluded overlays absent from the world-processing input.
