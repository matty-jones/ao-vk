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

## Remaining acceptance

The user subsequently confirmed that `4b11319` restores usable icons and compass geometry. The layout correction
now leaves native view rectangles untouched, using reversible display placement and anchored HUD subtrees.
Native regressions cover scale round trips, clamping, nested full-width HUD containers, input mapping, and
the actual native preference writer while controller work is active. Static screen queries retain the original
workspace, including when preferences are loaded at a non-default scale. The font correction uses higher-resolution
private GUI bitmaps and fresh outline glyphs with unchanged native metrics. Native string, selection/underline,
surface-layout and 100% restoration checks cover fractional scales including 176%.

The new font/layout paths still need a rendered game session. Confirm 4K text quality, default/custom skin
interaction, and repeated 100% to intermediate/extreme scale to 100% cycles, including dragging and restarting.
The native regressions and cache images cannot establish full gameplay acceptance.

The new layout path preserves the layout it starts with. It cannot recover positions lost by the previous build;
restore an existing preferences backup or arrange the UI at 100% before judging further round trips.
