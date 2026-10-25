# Shop portals: the inside of a shop seen through its door

## Why

Outside a shop (a Fair Trade store in Newland Desert), the doorway behind the sliding door is solid black. Going in is
a zone change: the game builds the shop's rooms only once you are inside, in a playfield of its own (1187,
`ord_smarket_neut_advanced`). The goal is to see the real interior through the open door, in 3D: the floor, counters
and terminals in their places and depth, shifting correctly as you walk past.

The game can't show it: it hosts one playfield at a time, and the server drives the zone change. The interior has to
come from ao-vk's own record of it.

## What is known (2026-10-25)

- **The black is part of the building.** It is a quad of `1010004:6014` (`blacktexture.png`) at the back of the
  doorway recess, about 1.6 deep, inside the building mesh (`last_ditch_building_medium3`, `1010001:42574`).
  - The rest of the building's black sub-mesh is window strips, drawn in the same draw.
  - In the Fair Trade shop in 565, the doorway's world centre is (2211.98, 22.73, 1566.47) with outward normal
    (-0.891, 0, 0.454). That comes from the statel matrix (ao-assets `statels.py`).
  - The pick (Ctrl+Shift+J) names it as "drawn with RDB texture 1010004:6014, 18 vertices".
- **The door is separate.** It is its own mesh (`..._door0`, `1010001:42632`) in front of the black quad, opened by the
  server (`Door_t`, `DoorStatusUpdateIIR_t`). Closed, it hides the doorway, so a portal needs no door state.
- **Interiors aren't stored as meshes.** N3 builds them on entry from a tilemap (rdb_1000009, a GNDA row) plus tile
  pieces (rdb_1010013). Room shapes and heights aren't decoded, so the interior can't be built offline.
- **Inside, the game draws the rooms as ordinary draws.**
  - Walls and floor are `VisualRoom_t` (fvf 0x152: position, normal, colour, one UV), with their lighting baked into
    the vertex colours (used as emissive).
  - The props are static meshes (`RTriMesh_t`), lit by the room's lamps.
  - ao-vk already classifies them (`VisualKind::Room`), keeps static ones on the GPU (static_gpu.cpp), and knows each
    texture's RDB id (`Texture::m_rdbId`).
- **Shops of a tier share one playfield.** 1187 (neutral advanced) and 1186 (neutral basic) share tilemap 1142 but
  have different props. Omni and Clan shops use other tilemaps. Every door of a tier probably opens the same interior.
- **Where a door leads isn't readable offline.** It is in the zone's surface resource (rdb_1000013, version 5, codec
  unknown). At run time the zone-change hooks (proxy/native/zone.cpp) see the teleport and the playfield arrived in.
- **The depth buffer is D32 with no stencil.** The portal's opening has to be masked another way.

## How it works

1. **Capture, once, inside.** While you are in a shop, every static draw of the interior is recorded: rooms and
   props, but not characters, effects or the interface. Each record holds its geometry, textures, material, render
   states and the lights the game gave it.
   - The record grows as you look around, and is saved to disk keyed by the interior's playfield.
   - It also stores an **anchor**: the interior's entrance door (`door_village_standard2` in a Fair Trade shop), whose
  bottom centre and inward facing go on the outdoor doorway's sill.
2. **Link, once, at the door.** When a teleport starts outdoors, the doorway quad you are standing in is matched with
   the playfield you arrive in. The link (outdoor playfield, doorway corners, interior playfield) is saved.
3. **Draw, every frame, outside.** When a linked doorway is on screen and near, ao-vk draws the interior behind it at
   the end of the scene:
   1. **Mask.** Our own copy of the doorway quad is drawn against the scene's depth into a small mask image: the
      doorway's visible pixels, not those hidden by the door, a character or a wall.
   2. **Open.** Under the mask, the depth is pushed to the far plane, so the black quad no longer hides what is
      behind it.
   3. **Interior.** The captured draws are replayed through the normal draw path. Each is moved by the transform
      that puts the interior's anchor at the doorway, facing in. Each is discarded outside the mask and in front of
      the doorway plane, and lit as indoors: its own lamps, no sun, sky, fog or rain.

   Afterwards the scene goes on as usual. Bloom, TAA and the tone mapping treat the interior as part of the scene.

## Phases

### Phase 0: check the assumptions (diagnostics, in game)

- **Capture coverage.** Log every interior draw for a few seconds after arrival (kind, mesh id, textures, vertex
  count, world position) and see whether the whole shop is drawn from the entrance or only what is in view. N3 culls
  statels by zone cell; the rooms may come as a whole.
- **The anchor.** Log the arrival pose: the camera and the character's draw. Look for the entrance door's mesh inside,
  the interior side of the same door, as a sturdier anchor than the arrival pose.
- **The doorway outdoors.** Make Ctrl+Shift+J on a black texture draw report the picked quad: the hit triangle and
  the one it shares an edge with, as the room pick does (draw.cpp `PickDraw`), with its world corners and normal.
  Check it against the statel-matrix numbers above.
- **Lighting flags.** Check how a draw could be lit as indoors while the frame is outdoors: the sun, sky ambient, fog,
  wetness, cloud and sun shadows are per-frame (FL) today. Find the smallest per-draw switch.
- **Write-up:** the findings here. Decide the anchor and the capture's completion rule.

### Phase 0 results (2026-10-25, Fair Trade in Newland Desert, 565 -> 1187 -> 565)

- **The doorway pick** (Ctrl+Shift+J):
  - 4 triangles of the building's black draw: an arched opening 2.55 wide and 2.86 tall, sill at y 21.30, peak at
    24.16;
  - centre (2211.98, 22.73, 1566.47), normal (-0.891, 0.001, 0.454);
  - this is exactly the statel-matrix numbers. The doorway is the black draw's triangles joined to the hit one, so the
    windows don't come with it.
- **The rooms come whole, at once:**
  - 6 `VisualRoom_t` draws of one 657-vertex buffer, all 125 ms after arrival;
  - their box x 194..216, y 5..13, z 120..146.
- **The props come mostly at once:**
  - 234 of 270 static pieces in the first second, from the arrival view;
  - the other 36 when turning round: mostly round the entrance (z ~120, behind the arrival camera), a few at the
    sides.

  So what a doorway shows is in the capture from arrival. Capturing over the whole visit, and merging visits, fills
  the rest.
- **The arrival is fixed:**
  - both visits put the camera at (205.02, 7.02, 120.35) looking (0, -0.09, 0.996), with the player's head at
    (204.99, 5.08, 124.00);
  - leaving puts the player about 4.8 units out from the doorway.
- **The interior has its own entrance:**
  - `door_village_standard2.abiff` at (205.08, 6.58, 119.95), about 2.4 wide by 3.2 tall, from the floor at y 5.0;
  - over it `door_blocker_flat.abiff`, a 6.1 x 3.8 panel of the same black texture.

  **Anchor: the interior door's bottom centre (205.04, 5.0, 119.95), facing +z.** It goes on the outdoor doorway's
  sill centre (2211.98, 21.30, 1566.47), facing in (0.891, 0, -0.454): a turn about the vertical and a move.
- **Lighting a draw as indoors:**
  - every bit of the draw flag word is taken, but the interior's draws set their own game state: no directional
    light (so no sun), the room's ambient (FF1E1E1E), its fog;
  - what is per frame (sun shadows F_SHADOW, sky light, wetness, cloud shadows, the env probe, GI, the light rig's
    gains) needs a per-draw switch in a spare word of the draw constants.
- **Brightness by day:** to be decided in phase 3. The light rig exposes for daylight, so the shop's lamps at that
  exposure would leave the doorway nearly black by day: true to life, but no portal. The interior gets a gain so it
  looks as it does from inside, times a setting (dimmer than the street by day).

### Phase 1: capture and a viewer

- **`rvk/portal_capture.cpp`** records, while in an indoor playfield:
  - static draws, deduplicated by mesh fingerprint and world matrix;
  - vertex and index data, from the static arena when it holds them;
  - textures by RDB id, with their pixels;
  - render states and material;
  - the frame's lights and the global ambient.
- **Saving.** To `<client>/randy-vk/portals/<playfield>.rvkp` when you leave (the zone hook) or after a quiet spell
  with no new pieces.
  - This file holds the game's meshes and textures, so it is local data: never committed or shipped, made by playing.
  - It carries a format version and a hash of its pieces.
- **`rvk_demo --portal-view <file>`** draws a capture alone with a free camera. This checks the capture without the
  game, and gives the portal's look-check scene.

### Phase 2: links

- At `StartTeleport` outdoors, the doorway is the nearest black-texture quad drawn in the last frames within ~3 units
  of the player. Its quad comes from the draw's triangles.
- At `NewPlayfield`, the link is completed and saved to `portals/links.tsv`: outdoor playfield, doorway corners,
  building mesh id, interior playfield.
- **Shared interiors.** A doorway of the same building mesh in another place, with the same quad in mesh space,
  inherits the link's interior. One visit then covers every Fair Trade door of that tier. Each door's own first use
  confirms or corrects it.
- Log lines for each link learned and each portal found in a frame.

### Phase 3: the portal

- **`rvk/portal.cpp`, at EndScene, before the post passes:**
  1. the mask pass (the stored quad, depth test LEQUAL with a small bias, into an R8 image the size of the scene);
  2. the depth push (gl_FragDepth = 1 under the mask);
  3. the replay, with a portal transform and clip;
  4. the interior's lamps added to the frame lights for its draws, moved by the portal transform.
- **Static buffers.** Upload the capture's buffers once, kept as long as a linked doorway is near.
- **Only when needed.** Run only for a doorway the game drew this frame (its building's black draw), on screen,
  within `RVK_PortalDist`.
- **Settings:** `RVK_Portals` (on/off) and `RVK_PortalDist` (units, hidden), as knobs.
- **Interplay to check:**
  - motion vectors (the interior is static: camera motion only);
  - TAA;
  - the depth pre-pass;
  - ray-traced AO and shadows (the interior isn't in the TLAS: no sun reaches it, AO from the screen only);
  - SSR;
  - the env probe;
  - frame generation;
  - the zone hand-over (walking through, the held picture is the portal's view: no jump).

### Phase 4: polish

- Interior lamps' glow in the air, and the shop's own fog colour.
- Fade the portal in with distance.
- A capture taken again when the game data changes (the hash).
- Windows of the same black texture could later get generic interiors; out of scope here.

## Checks

- **`rvk_demo --portal-view`** against a capture: shots from the anchor and from angles through a doorway quad, with no
  validation messages.
- **`tools/look-check.py`:** unchanged with no portal in view (none of its scenes has one). The portal viewer becomes
  a scene of its own once its look is settled.
- **In game:**
  - the Fair Trade shop in Newland Desert: walk past at angles, with the door opening and closing, by day and night,
    at render scale 75, with frame generation on;
  - then a Fair Trade shop elsewhere (the shared-interior link);
  - time the portal's GPU cost with the shop in view.

## Risks

- **The anchor may be off.** If the arrival pose isn't just inside the entrance, facing in, the interior sits wrong in
  the doorway. Phase 0 decides; the entrance door's own mesh inside is the fallback.
- **The capture may be incomplete.** If statels are culled by cell and you don't look around, pieces are missing. The
  capture can keep growing on every visit.
- **The game's state at the draw.** The interior is replayed through the draw path that the game's draws use. The
  states it sets have to be saved and put back, as for the held labels and the ground's own passes.
- **Zone loads are fragile** (docs/stability-plan.md). Saving the capture happens on the worker after the zone
  change, never in the game's load.
