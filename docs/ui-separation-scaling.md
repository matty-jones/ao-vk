# Interface separation and scaling

Local gameplay testing confirmed that the atlas correction restores usable compass/icon sampling. Font resolution
is still under correction. Scale changes now use display transforms without changing native rectangles; the new
layout path has native regression coverage and still needs a gameplay check. See the [investigation](ui-scaling-investigation.md).

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

## Validation

The Linux cross-build produces the 32-bit proxy and checks. A 64-bit renderer-only build runs headlessly through
Wine and software Vulkan. The prepared PRK 0.7.2 client's actual 32-bit GUI.dll runs through native Wine under
QEMU i386, avoiding the available WoW64 loader's startup failure.

- `native_check.exe --gui-check` checks the capture callback without native optimizations, exact identity at 100%,
  4K at 200%, independent factors, inclusive clips and inverse input transforms.
- `native_check.exe --gui-client-check <client-directory>` invokes the actual checked GUI.dll exports on synthetic
  native-layout view trees. It checks nested conversions and overloads, independent 200% window/150% HUD factors,
  clipping, parent hit tests, window mouse borders, logical workspace, unchanged physical controller coordinates,
  and deferred activation at frame boundaries. The final run passed with zero failures.
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
