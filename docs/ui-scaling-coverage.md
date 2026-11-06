# UI scaling coverage

The supported GUI.dll is checked by timestamp, image size, exports and hook entry bytes. The single visible
UI size controls both window and HUD scale, with distinct internal factors at native 1080p: a 75% setting yields
100% windows and 75% HUD. Ordinary windows and HUD use nearest-edge/centre anchoring; HUD classes and the older
timer system have specialised adapters. A control's drawing, clipping and inverse mouse mapping inherit the same
transform. Native frames remain the layout workspace.

| Elements | Scale and path |
| --- | --- |
| Floating windows, including Inventory, Missions, Programs, Friends, Perks, Research, Skills, chat and NPC dialogs | Window size through the common window/view path |
| Pinned sidebar panels | HUD size through the shared rollup-column transform, including parent hit bounds |
| PF map and city/building map | Their window/rollup transform, including custom tiles, outlines, player arrows and other markers |
| Health/XP bars, compass, shortcut bars, control-center panels and notifications | HUD size |
| Mail indicator, help/tip buttons and raid bar | HUD size; these previously inherited Window size |
| Attack, nanoprogram and other temporary timer bars, including logout/camping | HUD size through their common `TimerBarBase_c` constructor and legacy sprite-window methods; screen-centered bars retain their center |
| Popups and context menus | Window size, physical cursor anchors, logical placement/clamping and displayed-frame click checks |
| Login, character selection/management and progress/loading windows | Native screen coordinates and native size |
| Hover tooltips and dragged item images | Their existing source-window mapping and cursor-anchor adapters |
| World labels/floating text, mouse pointer, renderer console and performance readout | Original sizes, outside window/HUD scaling |

The timer adapter is limited to sprite windows created by the timer constructor. It stores the game's logical
position and scale separately, reapplies HUD size when it changes, and refreshes the native mouse regions.
Placement uses the same screen-edge/center anchoring as other HUD elements, rather than multiplying a
screen-centered position about the top-left corner.
Destroying the sprite window removes its record. Other legacy sprite windows keep their original behaviour.
Timer captions remain on the older bitmap text path; browser content also retains its separate rasterizer.
The display-resolution font replacement covers native `TextRenderer_c` controls.

## Custom drawing and transient placement audit

2026-11-02. The audit followed the supported GUI.dll's raw rendering imports, their callers and all direct
legacy sprite-window construction/placement calls. The seven geometry submission sites use three vertex
formats: untextured coloured vertices (`0x44`), textured coloured vertices (`0x144`), and textured vertices
without a diffuse colour (`0x104`). The last format is the actual rotating player arrow; the original adapter
handled only the other two, leaving the arrow in native screen coordinates while its map moved and scaled.

| Submission path | Coordinate handling and check |
| --- | --- |
| Map sprite/tile fans, coloured fan and outlines (five sites, including the player arrow) | Map arithmetic and clipping stay logical; all three vertex formats receive the containing map transform once at submission. Native player-arrow geometry is checked alongside tiles. |
| Building-map render override | Background, nested ground pass and building overlays share the same map scope. Previously only its nested ground pass entered the adapter. Native render checks compare all submissions across scale changes. |
| Browser strip (one site) | Native code converts its complete destination rectangle to screen coordinates while deriving UVs from logical bounds. It needs no map adapter; native regression checks preserve that path and its UVs. |
| ViewSurface batch (one site) | Existing ViewArea conversion handles the ordinary interface, text and bitmap controls. Existing atlas, clipping, input and font regressions cover this shared path. |

The four legacy `RenderWindow_t` construction sites are in login/loading setup (two), the timer base (one),
and a legacy text-window constructor (one). Login/loading retains native screen sizing. Timer construction,
native scale changes, explicit repositioning, hotspot refresh and destruction are covered together. The logout
caller centers a timer from the physical display size and logical bar size; scaling that position from the
screen origin displaced it. The timer adapter now anchors the complete logical bar rectangle before applying
HUD size. Ordinary non-timer sprite windows remain outside this adapter, including cursor-related rendering.
The legacy text-window constructor is not exported, and no direct caller or static pointer reference was found
in GUI.dll. No other client DLL or executable imports the legacy sprite-window methods.

These checks use 75/100/125/150/176/200% factors, independent Window/HUD settings and return trips to 100%.
They exercise the client implementation rather than substituting a synthetic quad for the player arrow.
The audit accounts for the supported binary's shared drawing paths; it does not claim an authenticated
gameplay visit to every control or every third-party skin.

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
play. Local gameplay review should confirm player-arrow alignment while moving, zooming and toggling map panels,
plus logout centering at the user's saved scales. DLSS 5 work has been dropped; these are independent UI fixes.
