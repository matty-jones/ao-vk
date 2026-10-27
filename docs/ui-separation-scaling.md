# Interface separation and scaling

Local gameplay testing confirmed that the atlas correction restores usable compass/icon sampling. The font and
layout corrections below have native regression coverage and still need a gameplay check. See the
[investigation](ui-scaling-investigation.md) for the original defects and evidence.

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
workspace; the controller's physical screen and mouse coordinates stay unchanged. Changes take effect together
at the next GUI frame and force a fresh cached image. The client's layout remains in its original coordinates.
Clamping affects only display placement, so returning to 100% restores the unchanged model. Full-screen HUD
containers keep their native dimensions; their widget groups scale around the nearest screen edge or centre.
Their clipping and input bounds include the enlarged children. No scale-driven move/resize calls or separate
preferences file are used. Window-frame queries used by the native preference writer remain canonical even
while controller hit testing uses display bounds. Deliberate dragging continues through the inverse display transform.

HUD classification covers the character bars, compass, shortcut bars, control centre and notifications through
their native window types and names. Other GUI windows use the window factor. World labels and floating text,
cursor, and developer console/performance readout do not use either scale.

The scaling hook group recognises GUI.dll timestamp `0x5CD32BEA`, image size `0x2B0000`, and checks exported
addresses and entry instructions before patching. Unknown builds remain at 100% and log the reason. No client
object layout or allocation size changes. The earlier [source audit](ui-rendering-audit.md) is preserved separately.

GUI text above 100% uses a larger RGBA bitmap and new outline glyphs at the requested scale. The text renderer's
native font metrics and line layout stay unchanged; only its private raster cache changes resolution. Texture
sampling sees the complete larger image, while layout sees the original logical dimensions. Selection,
underlines, inline bitmaps and shadows use the same cache mapping. Returning to 100% rebuilds the native cache.
TrueType faces and styles are cloned from the original GUI font; legacy bitmap fonts use Verdana as an outline
fallback, retaining their logical advances. Glyphs with no scalable outline retain their source bitmap. The
separate world-label, floating-text, cursor and console/readout paths do not enter this cache.

The font entry group and GUI import slots are checked before activation. The log reports
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
  and deferred activation at frame boundaries. Scale round trips preserve native rectangles, and the actual
  native preference writer is checked while controller work is active. Full-width HUD container checks cover
  enlarged child clipping and nested input mapping.
- The same check invokes the actual native string renderer and surface-layout methods at 100%, 110%, 150%,
  176% and 200%. It checks fresh antialiased glyph pixels, unchanged advances, logical surface dimensions,
  selection/underline drawing and exact native raster restoration at 100%. `RANDYVK_GUI_FONT_SHOT=<prefix>`
  saves the test cache images for inspection. The Wine fixture uses a registered DejaVu Sans substitute for
  Verdana; these images validate the native hook path, not a full game session or exact Microsoft font pixels.
- `rvk_demo.exe --interface-test` checks no-3D menus, redraw/cached frames, labels and floating overlays, opaque
  black and translucent UI, temporary render-target copies, opaque-format blits, cursor order, hidden UI, resize,
  viewport state, and readback both during and after a frame. HDR/TAA at 50% world resolution and threaded
  rendering exercise the same boundaries. A world-only screenshot checks for overlay contamination.
- The full native comparison against the client's original renderer passed with zero failures. Generated settings
  documentation and whitespace checks passed.

Full game GUI interaction remains unverified: the client exits during startup before a rendered GUI frame in
this environment. Live dragging, maps/browser interaction, all font variants and persistent layout changes need
a playable client session. The fixed world-look check timed out even with an extended limit; its baseline was
not changed. Software Vulkan exposes only one queue, so hardware frame generation remains untested. It also
reports the existing external-memory extension dependency validation error
`VUID-vkCreateDevice-ppEnabledExtensionNames-01387`. QEMU/Wine helper teardown can report a signal after
successful native checks. Actual DLSS integration remains future work.
