# Interface separation and scaling

The interface is captured at display resolution, independently of world rendering and refresh rate. Windows and
HUD have independent scales, with matching input and fresh text rasterization. See the
[font metrics](ui-font-metrics.md), [defect investigation](ui-scaling-investigation.md),
[performance audit](ui-scaling-performance-audit.md) and [neural-rendering roadmap](neural-rendering-roadmap.md).

`RVK_UiRate` controls redraw frequency only. At 0 the interface redraws every frame and still uses its own
display-sized image. Capture callbacks work with Native Direct and Native Device disabled. GUI controller work
continues each frame when drawing is cached, including mouse tracking and tooltips.

The renderer finishes world processing before labels and floating text, windows/HUD, console and cursor. These
overlays use separate display-sized images, independently of frame generation. Presentation, screenshots and
main-surface readback compose the same layers; the world target remains available without overlays. Temporary
GUI render targets remain separate, while blits to the main surface follow the active overlay. Opaque texture
formats retain opaque alpha when copied. `RANDYVK_WORLD_SHOT=1` saves the world alone for comparison.

`RVK_WinScale` and `RVK_HudScale` are independent percentages in General (50–200, default 100). Scaling happens
in GUI.dll's native view coordinates, before drawing into the display-sized interface image. Rectangle transforms
include the last pixel, so clipping and input use the same bounds as drawing. Native layout keeps its original
workspace; the controller's physical screen and mouse coordinates stay unchanged. HUD changes take effect at the
next GUI frame. Window changes wait while the primary mouse button is held, keeping the Preferences slider still,
then apply at the first GUI frame after release. Applied changes force a fresh cached image.
Clamping affects only display placement, so returning to 100% restores the unchanged model. Full-screen HUD
containers keep their native dimensions; their widget groups scale around the nearest screen edge or centre.
Their clipping and input bounds include the enlarged children. No scale-driven move/resize calls or separate
preferences file are used. Native frame queries always remain canonical. Only the controller's cursor and tooltip
hit-test call sites request displayed frames, including outside rendering. Border dragging and frame updates use
canonical screen coordinates after input dispatch has inverted the display transform. Tab-driven resizing therefore
retains the window position. Mouse-relative window placement resets any old display-clamp offset. Dragged item
images inherit the source window/HUD scale around the physical cursor, retaining their native hotspot.

The sidebar's native rollup is one HUD widget group, including when it occupies the full screen height. Its panels
share one display transform and retain native stacking, expansion and scroll coordinates. Individual panels are
never independently anchored or clamped to the screen. A full-height rollup keeps its top anchor when shrinking.
The PF map's custom renderer keeps tile clipping, zoom and marker calculations in native units, then transforms
its submitted vertices once. Tiles, outlines and markers therefore match the surrounding frame. Mouse conversion
still uses the inverse display transform. Custom map drawing changes neither texture coordinates nor depth, and
normal GUI/world drawing resumes after the map render call.

Login and character-selection windows retain native scale and placement; saved gameplay percentages remain intact.
At 100%/100%, hierarchy mapping is skipped. `[Native] UiScaling=off` leaves coordinate and font hooks uninstalled
after a restart, retaining interface capture and overlay separation.

Packed UI textures have no gutters between images. Scaled quads retain their UV endpoints and clamp fragment
samples to their source texel centres, preserving interior strokes. Interface sampling uses the top mip level and skips forced
anisotropy so filtering cannot reach neighbouring allocations. World and cursor sampling retain their original state.

HUD classification covers the character bars, compass, shortcut bars, control centre and notifications through
their native window types and names. Other GUI windows use the window factor. World labels and floating text,
cursor, and developer console/performance readout do not use either scale.

The scaling hook group recognises GUI.dll timestamp `0x5CD32BEA`, image size `0x2B0000`, and checks exported
addresses and entry instructions before patching. Unknown builds remain at 100% and log the reason. No client
object layout or allocation size changes. The earlier [source audit](ui-rendering-audit.md) is preserved separately.
The map's private render entries also require their checked native vtable slots and renderer imports.

GUI text above 100% uses a larger RGBA bitmap and new outline glyphs at the requested scale, including any native
text zoom. One physical font cell fits the scaled logical line height, with a common baseline for every glyph.
A single font-wide horizontal transform follows the original font's average cell width, preserving explicit
bitmap-glyph spacing rather than enlarging it into the letter shapes. No glyph is moved or squeezed individually.
The text renderer's
native logical font metrics and line layout stay unchanged; only its private raster cache changes resolution. Texture
sampling sees the complete larger image, while layout sees the original logical dimensions. Selection,
underlines, inline bitmaps and shadows use the same cache mapping. Returning to 100% rebuilds the native cache.
TrueType faces and styles are cloned from the original GUI font; legacy bitmap fonts use Verdana as an outline
fallback, retaining their logical advances. Glyphs with no scalable outline retain their source bitmap. The
separate world-label, floating-text, cursor and console/readout paths do not enter this cache.

Scaled font caches are also released when the owning native font is destroyed, so a reused font address cannot
inherit another face's metrics. Empty outline glyphs retain their native advances without allocating temporary
bitmap fallbacks. The font entry group and GUI import slots are checked before activation. The log reports
`interface fonts: display-resolution text cache hooked` on success. Font/glyph caches are bounded and released
with their owning resources. Embedded browser text and other externally rendered images keep their own raster
paths; this correction covers native GUI text.

## Validation

The Linux cross-build produces the 32-bit proxy and checks. A 64-bit renderer-only build runs headlessly through
Wine and software Vulkan. The prepared PRK 0.7.2 client's actual 32-bit GUI.dll runs through native Wine under
QEMU i386, avoiding the available WoW64 loader's startup failure.

- `native_check.exe --gui-check` checks the capture callback without native optimizations, exact identity at 100%,
  4K at 200%, independent factors, inclusive clips and inverse input transforms.
- `native_check.exe --gui-client-check <client-directory>` invokes the actual checked GUI.dll exports on synthetic
  native-layout view trees. It checks nested conversions and overloads, independent 200% window/150% HUD factors,
  clipping, parent hit tests, window mouse borders, unchanged layout workspace and controller coordinates,
  and deferred activation at frame boundaries/mouse release. Scale round trips preserve native rectangles, and the actual
  native preference writer is checked under non-default scales. Full-width HUD container checks cover
  enlarged child clipping and nested input mapping.
- Sidebar regressions use the client's actual rollup type and a 4K view tree, with independent window/HUD factors.
  They check panel close/reopen, expansion, top/right anchors, inverse input dispatch and unchanged native frames
  at 75/100/150/176/200%. Custom map checks invoke its real ground-render entry and tile/outline drawing helpers,
  substituting engine tile data and capturing renderer submissions. Tile, outline and marker geometry scales once;
  depth, colour, UVs and native source arrays remain unchanged. Subsequent drawing uses the normal conversion path.
- The same native check exercises border mouse-down/move, client resizing, mouse-relative placement, tooltip
  hit detection/timer text outside rendering, and dragged item movement from both windows and HUD. Synthetic
  fixtures isolate native cursor capture, layout callbacks, tooltip content and display invalidation.
- The same check invokes the actual native string renderer and surface-layout methods at 100%, 110%, 150%,
  176% and 200%. It checks fresh antialiased glyph pixels, unchanged advances, logical surface dimensions,
  selection/underline drawing and exact native raster restoration at 100%. `RANDYVK_GUI_FONT_SHOT=<prefix>`
  saves the test cache images for inspection. The check logs the resolved font face, since Wine may substitute
  an unavailable font. These images validate the native hook path rather than a full game session.
- Small-font checks compare complete ink coverage against natural GDI outlines at 9, 12 and 16 pixels, including
  descenders and narrow strokes. Bitmap letters, inline icons, native text zoom, empty-cache retries, fixed login
  geometry and the 100% hierarchy fast path have native regression checks. `--gui-scaling-off-check` verifies
  untouched GUI entries/imports with scaling disabled; `--gui-bench` reports synthetic coordinate/mapping costs.
- `rvk_demo.exe --interface-test` checks no-3D menus, redraw/cached frames, labels and floating overlays, opaque
  black and translucent UI, temporary render-target copies, opaque-format blits, cursor order, hidden UI, resize,
  viewport state, and readback both during and after a frame. HDR/TAA at 50% world resolution and threaded
  rendering exercise the same boundaries. A world-only screenshot checks for overlay contamination.
- `rvk_demo.exe --ui-atlas-test --size 480 240` renders adjacent contrasting atlas allocations at 50%, 75%,
  110%, 150%, 176% and 200%, with fractional placement and forced anisotropy configured. Unguarded quads reproduce
  foreign edge pixels; guarded UI quads must match an isolated patterned texture, including bottom rows and thin
  strokes. A draw outside the interface checks that normal mip
  sampling is restored. Immediate and threaded rendering exercise the sampler cache boundary.
- Atlas protection is checked directly against vertices produced by the client's native `_UpdateVertices` function.
  Those vertices use reciprocal depth 100; the renderer test covers both 1 and 100 without changing their UV mapping.
- The full native comparison against the client's original renderer passed with zero failures. Generated settings
  documentation and whitespace checks passed.

Earlier local 4K gameplay review confirmed clean text, stable performance, working terrain toggles, the requested ground
detail radius, sidebar stacking and PF map scaling. Native checks do not establish the complete behaviour of
every skin, tab, tooltip or item action. The cloud client exits before a rendered GUI frame, so broader browser
and restart-persistence checks require a playable client. The fixed world-look check timed out; its baseline was
not changed. Software Vulkan exposes only one queue, so hardware frame generation remains untested there. It also
reports the existing external-memory extension dependency validation error
`VUID-vkCreateDevice-ppEnabledExtensionNames-01387`. QEMU/Wine helper teardown can report a signal after
successful native checks. Actual DLSS integration remains future work.

## Input follow-up and remaining chat clipping

At 150%/150%, the Programs panel could draw outside its sidebar host but fail the native window search's final
parent-bounds check. That input call site now includes the displayed descendants. Layout still reads native bounds.
Popup menus take physical cursor positions, while their placement and screen clamp use logical units. Their
separate global mouse-down check now compares the displayed frame, preventing cancellation before item activation.
Native checks exercise the actual window search and popup placement, dismissal, mouse-down and mouse-up methods.

An intermittent clipped chat/NPC line remains under investigation. Moving its window preserves the damage;
changing scale clears it. Native checks cover multiline wrapping, glyph extents, surface uploads and cache resizing,
but do not yet reproduce this gameplay case. The font rasterizer is unchanged by this follow-up.

When a line is clipped, use console `dump` or Ctrl+Shift+F9 before changing scale. Alongside the frame screenshot
and draw list, `rvk-frame-HHMMSS-ui.tar` contains enlarged native text-cache BMPs and `caches.txt` with density,
logical/physical dimensions, line descriptors, surface rectangles, cached quad vertices and clipping. It records existing caches without
rendering, invalidating them or changing settings. Visible caches have priority; limits are 128 images and 24 MiB
of image data and approximately 3 MiB of layout records. At 100%, the original bitmap path may leave no enlarged caches to save.
The archive contains the displayed text, including any chat represented by those caches.
