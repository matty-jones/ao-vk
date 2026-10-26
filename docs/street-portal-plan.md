# Street portals: the street seen from inside a shop

## Why

Shop portals (docs/shop-portal-plan.md) show a shop's room through its door from the street. This is the reverse:
standing in the shop, its open entrance door shows the street outside, in 3D, lit for the hour, instead of whatever
the game puts behind it.

It is harder than the room:

- **Size.** A room is ~300 pieces and 4.5 MB. A street is open-ended, so only what the doorway can see is kept: a cone
  out of the door, ~100 units deep.
- **Light and sky.** A room is lit by its own lamps, the same at any hour. The street changes with the sun, the sky and
  the weather. Its pieces are replayed under the current sun, with ao-vk's own sky behind them, not frozen at the
  capture's hour.
- **The ground isn't the game's draws.** With `RVK_GroundOn` the street's ground is ao-vk's native ground: the game's
  terrain draws are dropped (`NativeGroundReplaces`), and the patches (`DrawNativeGround`) only render correctly with
  their per-cell tile-code, map and mean textures and the frame's terrain fields. Recording the patches' draws won't
  replay; a cut-out of the ground's data in front of the door will (below).
- **Live things.** Characters, mobs and effects come from the server and aren't drawn while you're inside. As for the
  rooms, they aren't shown.

## What is known (2026-10-26)

- **Links exist** (portal_links.cpp, `links.tsv`): which street doorway leads to which room, with the doorway's centre,
  outward facing, sill and size. From inside a room the link names the street and the doorway to show.
- **The room's entrance** (portal_capture.cpp): the door nearest the arrival, its bottom centre and the way in. A
  Fair Trade's is `door_village_standard2` at (205.01, 4.97, 119.98), into the room +z. Its size is now logged.
- **Behind the inside door:** not known yet. A black panel (`door_blocker_flat`, the black texture) sits above the
  door, not behind it. The game probably shows nothing (the clear colour) through the open door. Phase 0.
- **The native ground** (rvk.h `NativeGround`): heights, tile codes (14 bits plus a turn), tile textures and surface
  types for the whole playfield, 4-unit cells, held in one `m_native` (two can't exist at once). The hour's lightmap is
  kept across playfields.
- **The sky** (`DrawSkyGlow`, sky.cpp): a full-screen pass that rebuilds each pixel's view direction from the camera,
  from the atmosphere tables (`UpdateAtmosphere`, which runs indoors too). It can be drawn a second time into a
  doorway's box once its once-a-frame guard and full-screen scissor allow it. Clouds and suns are passes of their own,
  tied to the frame's puffs and sprites: not in a first version.
- **Capture today refuses outdoors.** The room capture drops everything once it sees terrain (portal_capture.cpp). A
  street capture is a second recorder, not that one.

## How it works

1. **Capture, outside.** When a walked-through doorway is near (within ~30), the static draws in front of it are
   recorded: those whose box lies within ~100 of the doorway and inside ~60 degrees of its outward facing. Each keeps
   its geometry, textures, material, states and lights, as a room's piece does. Characters, effects, the sky and
   anything carried are not kept. The ground under the cone is cut out of the native ground's data: heights, codes and
   the tiles' textures, read back once. It is saved with the doorway's link as `randy-vk\portals\street-<link>.rvkp`.
2. **Draw, inside.** When the room's entrance door is in view, open, and near:
   1. The door's opening is found as the doorway was outside: a rectangle at the entrance from the door's size.
   2. The depth is pushed to the far plane where it is the surface seen (the same `portal_open.frag`).
   3. ao-vk's sky is drawn into the opening's box.
   4. The ground cut-out is drawn through the native ground's shader path, which needs `DrawNativeGround` to take the
      ground it draws instead of only `m_native`.
   5. The street's pieces are replayed through the draw path, moved so the doorway stands on the room's entrance,
      facing out.
   6. They are lit by the current sun and sky (the frame's conditions, as outdoors) and their own lamps, and clipped in
      front of the entrance's plane.
   7. The opening is marked so the post passes treat it as the street's.

## Phases

### Phase 0: research (diagnostics, in game)

Built, staged with this plan:

- **`street diag:`** one line per walked-through doorway each arrival (portal_diag.cpp `PortalStreetDiag*`): within
  100 and 60 degrees in front of it, the static pieces, their geometry's MB, their textures and MB, and the ground's
  draws and whether the native ground is drawing. How big a capture would be.
- **The entrance's size:** the `portal capture: ... entrance:` line now gives the door's width and height.

To do in game:

- Stand at a few shop doors you've walked through, in Borealis and Newland Desert, and read the `street diag:` lines.
- Inside a shop, open the entrance door (walk up to it without leaving) and press Ctrl+Shift+J on the opening: what
  is drawn there (nothing, the clear colour, a black panel).
- Check whether the game draws a sky inside a room (the `portal diag:` lines list each piece's kind on arrival).

### Phase 1: street capture

A recorder of its own (portal_street.cpp), keyed by link, with the cone test. The room capture's file format holds it
(pieces, textures), with a new section for the ground cut-out. Then `rvk_demo --portal-view` for a street file.

**Built (2026-10-26):** `rvk/portal_street.cpp`.
- It reuses the room capture's piece recording (`PortalRecordPiece`) and saving (`PortalCaptureSave`, with a path of
  its own and no entrance search).
- The black doorway quads and doors are left out.
- `rvk_demo --portal-capture-test` records its street (`street-4100-0-3.rvkp`), and `--portal-view` draws it from
  the doorway looking out.
- Not yet seen in game.

### Phase 2: ground cut-out

Not started.
- **Why not yet:** `m_native` is used ~200 times over nine files, and `DrawNativeGround` runs on the game's own
  terrain draws, with the game's states.
- **Likely route:** a second `NativeState` for the street, swapped into `m_native` around a ground draw inside
  `DrawStreetPortal`, with the base and lighting passes' states set by hand. This avoids reworking every use.
- **Data:** the cut-out (heights, codes, the tiles read back, the street playfield's lightmap) recorded into the
  street's file as a section of its own.
- **First:** see phases 1 and 3 in game.

`NativeGround` cut to the cone's cells. The draw path takes a ground to draw (`DrawNativeGround(const NativeState&)`),
and the tile textures are kept by the cut-out, as copies read back, not the game's.

### Phase 3: drawing inside

The opening at the room's entrance, the sky into its box (`DrawSkyGlow` for a box, a second time a frame), the
ground, the pieces lit by the current sun, clip and mark. Settings: `RVK_StreetView` (on/off),
`RVK_StreetDist` (hidden).

**Built ahead of phase 2 (2026-10-26):** `DrawStreetPortal` (portal.cpp), at the scene's end, indoors.
- **Which street:** the doorway walked in through (`m_enteredFrom`, set when a teleport's link is made).
- **Opening and place:** the room's entrance comes from its capture's header (`portal::LoadHeader`, read again each
  arrival and when the capture is saved). The door's size comes from the room capture's doors this visit (else 2.4 x
  3.2, 5% larger).
- **Passes:**
  1. the open pass, shared with the shop portals (`PortalOpenPass`);
  2. ao-vk's sky into the box at the far plane (`DrawSkyGlow(&box)`: `fullscreen_far.vert`, a depth-tested pipeline);
  3. the street's pieces (`PortalReplay`), kept from 5 cm out of the entrance's plane.
- **Lit as recorded:** the hour of the capture, the rig off indoors.
- **Check:** `rvk_demo --portal-capture-test` walks street, shop, street, shop: the last shot (-inside) shows the sky
  and the street's box through the entrance.

### Phase 4: polish

Clouds (the layer, at least), the suns, fog and the hour's haze in the opening, rain seen through the door.

## Risks

- **The opening from inside may not be clear.** If the game draws something behind the open door (a black panel, a
  corridor), the opening has to be found differently. Phase 0.
- **Capture size on busy streets.** Borealis's doors face a plaza with many buildings. If the cone holds tens of MB,
  the range comes down, or far pieces are dropped by size.
- **Light.** The street's pieces replayed indoors need the outdoor light rig's gains and the sun's direction for the
  hour. The rig is off indoors (`m_cond.sunKnown` false). The sun's direction and the rig's numbers would have to be
  kept from outdoors or worked out from the hour.
