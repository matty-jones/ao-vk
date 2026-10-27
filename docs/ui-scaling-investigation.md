# Scaling defects found in local gameplay

The initial scaling implementation at `abf8b5c` is not usable. The user's 4K test found blurred enlarged text
and icons, a stretched compass, and layout changes that did not reverse at 100%/100%. Health/XP bars scaled
well and world rendering retained its appearance. The client uses Verdana and the Notum Dovvetech Edge skin.
These findings invalidate the initial scaling acceptance; coordinate conversion tests alone were insufficient.

## Texture sampling

GUI.dll's `ViewArea_c::_UpdateVertices` (RVA `0x151898`) converts its destination to screen space, clips it,
and derives texture coordinates from the clipped distances. Its texture-to-destination ratio still uses the
logical destination size. The existing `ConvertToScreen` hook enlarged only the screen rectangle, so those
distances no longer had the units expected by the native calculation.

A regression invokes that actual native function with a synthetic 256x256 atlas and a clipped subrectangle.
At 100% the horizontal texture range was `0.156250..0.351563`; at 200% it collapsed to
`0.273438..0.273438`. The test failed on the initial implementation. This defect exists independently of
the skin and can stretch a strip of an image across its visible quad. It is a plausible contributor to the
compass report; the compass widget still needs direct visual validation.

The correction runs native clipping and texture-coordinate calculation in logical screen coordinates, then
transforms only the final vertex positions, retaining D3D's half-pixel convention. The new entry is checked
with the rest of the hook group before activation. It does not increase source icon resolution.
The corrected native check passed with zero failures; the pre-correction check failed as described above.

## Font resolution

`TextRenderer_c::PreRender` (RVA `0x163320`) allocates a cached bitmap, renders text into it, and uploads that
bitmap to a view surface. `_RenderString` (`0x15FFDD`) gets glyphs from `FontInfo_t` and draws them into the
bitmap with `SpriteInfo_t::RenderGlyph`. The scaling implementation changes coordinates but does not request
larger fonts or higher-resolution text bitmaps. A display-sized UI target therefore still enlarges the original
cached pixels. This matches the user's uniformly blurry Verdana text at 4K.

The client has a native TrueType path (`FontSystem_t::CreateTTFont`, `0x12EC5C`) using GDI font creation and
glyph rasterization, alongside bitmap fonts. A complete correction needs per-GUI-scale font and text caches
rendered at the target pixel size, with logical metrics retained for wrapping, selection and hit testing. Shared
world-label fonts, cursor and the separate developer console/readout must remain unchanged. Rebuilding only
the final UI target, changing filtering, or globally enlarging the game's font is insufficient.

## Layout drift

`BeginScaleFrame` calls native `Window::MoveTo` and `Window::ResizeTo`. Ordinary windows use the previous
frame's already-adjusted coordinates, then clamp to the smaller logical workspace. That clamp discards position
information. No canonical unscaled layout is retained, so returning to 100% cannot restore it.

For a 3840-pixel-wide workspace and a 300-pixel-wide window starting at x=3500, the current calculation produces:

| Scale | Native x | Display x |
| --- | --- | --- |
| 100% initially | 3500 | 3500 |
| 200% | 1620 | 3240 |
| 75% | 4320 | 3240 |
| 100% again | 3240 | 3240 |

Full-screen HUD resizing can also trigger the client's own child reflow. Recomputing each scale from the prior
mutated layout is not an invertible presentation transform. The corrective implementation must preserve a
canonical layout separately from temporary display placement, distinguish deliberate dragging from scale-driven
movement, and keep preference saving/restoration stable across restart. It must cover independent window/HUD
changes, edge clamping, screen resize, and complete round trips without drift.

## Subsequent gameplay and interaction fixes

The user confirmed that the higher-resolution font cache fixes enlarged text, and that expanding, shrinking and
expanding again retains window positions. Further gameplay found missing hover tooltips, repeated Perk-tab
position jumps, a title-bar drag offset, a displaced lockpick drag image, and narrow fragments around pinned
Friends-list text and shortcut icons.

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
that normal mip sampling is restored. This establishes the sampling defect; the specific Friends-list and skin
artefacts still require gameplay confirmation.

Window-scale activation now waits for the primary mouse button to be released. The slider value can change
during the drag without moving Preferences or changing its input mapping. HUD scaling continues to preview live.

## Remaining acceptance

The user subsequently confirmed that `4b11319` restores usable icons and compass geometry. The layout correction
now leaves native view rectangles untouched, using reversible display placement and anchored HUD subtrees.
Native regressions cover scale round trips, clamping, nested full-width HUD containers, input mapping, and
the actual native preference writer while controller work is active. Static screen queries retain the original
workspace, including when preferences are loaded at a non-default scale. The font correction uses higher-resolution
private GUI bitmaps and fresh outline glyphs with unchanged native metrics. Native string, selection/underline,
surface-layout and 100% restoration checks cover fractional scales including 176%.

The font quality and scale round trips have local confirmation. Confirm the newer fixes in the same 4K client:
title-bar attachment, Perk tab changes, hover tooltips, lockpick/image hotspot, pinned Friends-list text, shortcut
icon edges and stable Preferences slider dragging. Repeat the layout checks after restarting. Native regressions
and synthetic Vulkan images cannot establish full gameplay acceptance.

The new layout path preserves the layout it starts with. It cannot recover positions lost by the previous build;
restore an existing preferences backup or arrange the UI at 100% before judging further round trips.
