# Scaling defects found in local gameplay

Historical defects in the initial scaling implementation (`abf8b5c`) included blurred text and icons,
a stretched compass and irreversible layout changes. The 4K client used Verdana and the Notum Dovvetech Edge
skin. The corrections below have native regression coverage and local gameplay confirmation; the
[implementation notes](ui-separation-scaling.md) describe current behaviour and remaining compatibility checks.

## Texture sampling

GUI.dll's `ViewArea_c::_UpdateVertices` (RVA `0x151898`) converts its destination to screen space, clips it,
and derives texture coordinates from the clipped distances. Its texture-to-destination ratio still uses the
logical destination size. The initial `ConvertToScreen` hook enlarged only the screen rectangle, so those
distances no longer had the units expected by the native calculation.

A regression invokes that actual native function with a synthetic 256x256 atlas and a clipped subrectangle.
At 100% the horizontal texture range was `0.156250..0.351563`; at 200% it collapsed to
`0.273438..0.273438`. The test failed on the initial implementation. This defect exists independently of
the skin and can stretch a strip of an image across its visible quad. Local gameplay confirmed restored compass
and icon geometry after the correction.

The correction runs native clipping and texture-coordinate calculation in logical screen coordinates, then
transforms only the final vertex positions, retaining D3D's half-pixel convention. The new entry is checked
with the rest of the hook group before activation. It does not increase source icon resolution.
The corrected native check passed with zero failures; the pre-correction check failed as described above.

## Font resolution

`TextRenderer_c::PreRender` (RVA `0x163320`) allocates a cached bitmap, renders text into it, and uploads that
bitmap to a view surface. `_RenderString` (`0x15FFDD`) gets glyphs from `FontInfo_t` and draws them into the
bitmap with `SpriteInfo_t::RenderGlyph`. The initial implementation changed coordinates without requesting
larger fonts or higher-resolution text bitmaps. A display-sized UI target therefore still enlarges the original
cached pixels, producing blurry enlarged text.

The client has a native TrueType path (`FontSystem_t::CreateTTFont`, `0x12EC5C`) using GDI font creation and
glyph rasterization, alongside bitmap fonts. The correction uses per-GUI-scale font and text caches
rendered at the target pixel size, retaining logical metrics for wrapping, selection and hit testing. Shared
world-label fonts, cursor and the separate developer console/readout must remain unchanged. Rebuilding only
the final UI target, changing filtering, or globally enlarging the game's font is insufficient.

## Layout drift

The initial `BeginScaleFrame` called native `Window::MoveTo` and `Window::ResizeTo`, using the prior frame's
already-adjusted coordinates and clamping to a smaller logical workspace. That discarded position information,
so returning to 100% could not restore it.

For a 3840-pixel-wide workspace and a 300-pixel-wide window starting at x=3500, that calculation produced:

| Scale | Native x | Display x |
| --- | --- | --- |
| 100% initially | 3500 | 3500 |
| 200% | 1620 | 3240 |
| 75% | 4320 | 3240 |
| 100% again | 3240 | 3240 |

Full-screen HUD resizing can also trigger the client's own child reflow. Recomputing each scale from the prior
mutated layout is not an invertible presentation transform. The correction leaves native frames untouched and uses a reversible
display transform. Regression checks cover independent factors, edge clamping, input mapping, scale round trips
and native preference serialization.

## Input and image edges

Further defects included missing tooltips, Perk-tab position jumps, title-bar and lockpick drag offsets, and
fragments around Friends-list text and shortcut icons.

Native border movement converts an already logical input point back to screen coordinates. Applying the display
transform there mixes physical coordinates with the native drag rectangle and grab offset. Border mouse-down,
movement and frame application now keep those calculations in canonical coordinates. `ResizeClientTo` also reads
the original view frame, so tab-driven size changes no longer feed a displayed position back into layout.

The old `GetFrame@View` wrapper supplied displayed bounds broadly during controller work but original bounds
outside it. `UpdateToolTip` also runs from mouse events outside rendering, so it could miss enlarged windows.
The general frame getter is now untouched. Only the checked cursor/tooltip hit-test calls at `0x15731B` and
`0x1578F0` request displayed bounds. Native tooltip hit detection and timer text are exercised outside rendering.
Mouse-relative placement refreshes its display transform instead of retaining a previous edge-clamp offset.

`DragObject_c::SlotMouseMoved` moves its image window using physical cursor coordinates plus a logical hotspot.
Scaling that position around the screen origin displaces the image. The transient image instead inherits the
source window/HUD scale around the physical cursor; the cursor and drop coordinates remain unchanged. Native
drag movement tests cover both source categories and positions near the screen edge.

Packed UI images share texture storage without gutters. Magnification can blend a texel from the adjacent
allocation into an image's edge. Scaled source bounds now stop at their own texel centres, and interface samplers
use the top mip without forced anisotropy. A Vulkan test with neighbouring green/red allocations reproduced
317 contaminated pixels in unguarded draws and zero in guarded draws. A following non-interface draw confirms
that normal mip sampling is restored. The final guard also accepts the native GUI
quad's reciprocal depth of 100; requiring exactly 1 had bypassed protection on actual client draws.

Window-scale activation now waits for the primary mouse button to be released. The slider value can change
during the drag without moving Preferences or changing its input mapping. HUD scaling continues to preview live.

## Font metrics and fixed-screen windows

Higher-resolution caches alone did not fix raised descenders or inconsistent spacing. One fitted physical font
cell and a shared baseline now preserve complete glyphs; a font-wide pitch transform follows native advances.
Bitmap-font letters use outline fallbacks, while inline icons remain bitmaps. See the
[font metrics correction](ui-font-metrics.md). Login and character-selection windows retain native geometry,
independent of saved gameplay scales.

## Sidebar and PF map

A tall rollup container was misclassified as screen layout, causing its panels to receive independent anchors
and clamps. The entire rollup now shares a transform. The PF map bypasses ordinary UI surfaces: its tiles,
markers and outlines now scale at submission after native clipping and zoom calculations.

Local gameplay confirmed the corrected text, icons, scale round trips, sidebar stacking and PF map scaling.
The [performance audit](ui-scaling-performance-audit.md) records measured costs and workload limitations.
The layout path preserves the layout it starts with; it cannot recover positions lost by an earlier build.
