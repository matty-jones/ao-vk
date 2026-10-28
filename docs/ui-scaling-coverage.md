# UI scaling coverage

The supported GUI.dll is checked by timestamp, image size, exports and hook entry bytes. Ordinary windows use
Window size by default; HUD classes and the older timer system have explicit adapters. A control's drawing,
clipping and inverse mouse mapping inherit the same transform. Native frames remain the layout workspace.

| Elements | Scale and path |
| --- | --- |
| Floating windows, including Inventory, Missions, Programs, Friends, Perks, Research, Skills, chat and NPC dialogs | Window size through the common window/view path |
| Pinned sidebar panels | HUD size through the shared rollup-column transform, including parent hit bounds |
| PF map | Its window/rollup transform, with custom tile, outline and marker submission |
| Health/XP bars, compass, shortcut bars, control-center panels and notifications | HUD size |
| Mail indicator, help/tip buttons and raid bar | HUD size; these previously inherited Window size |
| Attack, nanoprogram and other temporary timer bars, including camping | HUD size through their common `TimerBarBase_c` constructor and legacy sprite-window methods |
| Popups and context menus | Window size, physical cursor anchors, logical placement/clamping and displayed-frame click checks |
| Login, character selection/management and progress/loading windows | Native screen coordinates and native size |
| Hover tooltips and dragged item images | Their existing source-window mapping and cursor-anchor adapters |
| World labels/floating text, mouse pointer, renderer console and performance readout | Original sizes, outside window/HUD scaling |

The timer adapter is limited to sprite windows created by the timer constructor. It stores the game's logical
position and scale separately, reapplies HUD size when it changes, and refreshes the native mouse regions.
Destroying the sprite window removes its record. Other legacy sprite windows keep their original behaviour.
Timer captions remain on the older bitmap text path; browser content also retains its separate rasterizer.
The display-resolution font replacement covers native `TextRenderer_c` controls.

## Text cache storage

The supplied capture contained a complete NPC text bitmap for the reported clipped sentence. The damage therefore
occurred after font rasterization. The native 16-by-16 shared texture allocator can reuse an existing pool without
checking the requested format: an RGBA request can receive a one-bit-alpha texture. That loses antialiased glyph
coverage and also splits enlarged text into many independently filtered atlas tiles.

Enlarged text now uses the native surface's independent RGBA texture path. It preserves the raster's alpha values
and avoids internal atlas-tile boundaries. Logical dimensions, native wrapping and font metrics are unchanged;
100% text and bitmap icons keep their existing paths. The pixel-capture command remains available for gameplay
review of any remaining clipping.

## Positions after reload

Edge clamping is a display offset, so scale changes do not rewrite saved layout. Previously a user could drag a
clamped window while retaining that offset, then save a native position that produced a different display position
on restart. On completion of an actual move or resize, the adapter now incorporates the offset into the new native
position and clears the transient translation. The displayed position stays fixed. Clicking a title bar without
moving, changing scale, and ordinary layout callbacks do not perform this normalization.

Previously displaced saved positions cannot recover their earlier location automatically. Arrange affected
windows once after updating; subsequent reloads should retain the new placement.

## Checks and limits

The actual GUI.dll checks cover the HUD/window class policies with unequal factors, native login-progress centering,
sidebar search, popup selection, title-bar movement, and reconstruction of a moved window after discarding transient
mapping state. Timer checks use the native constructor and sprite/hotspot methods, substituting only the engine
resources; their drawing scale, position, mouse bounds, lifecycle and scale round trips are checked independently.

The native allocator check first creates a one-bit-alpha pool, reproduces format reuse for an RGBA request, and
verifies that an independent allocation retains RGBA. Native text checks verify independent cache allocation,
complete bitmap-to-texture ink coverage, wrapping, resizing and unchanged pixel capture. The GPU atlas check also
compares a low-alpha bottom row in RGBA with the same row lost in a one-bit-alpha texture.

This is coverage of the shared source paths, not a record of opening every skin-specific control in authenticated
play. The prepared client provides native GUI and rendering checks; local gameplay review still verifies the exact
reported NPC line, login transitions, fight-time bars and two consecutive relogs before Stage 3.
