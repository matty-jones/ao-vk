# Zone hand-over: no frozen picture at a zone change

## Why

Walking from one playfield into the next (Newland City into Newland Desert), the picture freezes for 2 to 5 s with
nothing in between, then the new zone appears and fills in over the next couple of seconds. Real streaming - the next
zone loaded while you walk - isn't possible:

- A playfield change is a server teleport (`n3TeleportIIR_t`, `n3EngineClientAnarchy_t::StartTeleportTry` /
  `Confirmed` / `Failed`).
- The client hosts one playfield at a time (`n3EngineClient_t::GetPlayfield`).
- Characters, other live objects and collision come from the server.

The load itself is short:

- The playfield's RDB records are read in about 50 ms.
- The game thread then stalls for 0.2 to 0.6 s.
- Statels and meshes already stream in over the next ~2 s: N3 loads statels per zone cell on an `AsyncRequests`
  worker, and meshes load through `ResourceManager::GetAsync`.

So most of the freeze is waiting on the server. The hand-over makes that wait look intended, and the arrival smooth:

1. **Hold.** The last picture of the world stays up, slowly drifting (a slight push-in, blur and dimming), not a
   frozen frame.
2. **Settle.** The new zone renders hidden under the held picture until it has settled: exposure, darkness, TAA,
   grass, the ground's tiles and the statels in.
3. **Reveal.** It cross-fades in. The interface comes back at once, and moving cuts the wait short.

Left for later: drawing neighbouring playfields past the border (their ground and statels are in `rdb.db`, placed by
`MapCoordinates.xml`), building the destination ahead of time at a border, and taking our own load work
(`LoadStatels`' `WaitIdle`, the mesh-id scan) off the stall.

## What isn't known yet (phase 0)

The game DLLs have a loading screen: GUI.dll's `ServerLogin3DModule_t`, closed by the server's
`START_CLOSING_LOADSCREEN`. No one sees it at a zone change. There are two possible reasons, and they need different
designs:

- **(A) The game thread is blocked for the whole freeze, and nothing is presented.** The worker only replays the game's
  frames (`rvk/threaded.cpp`, `kMaxFramesAhead = 1`), so keeping the picture moving needs the worker to present on
  its own.
- **(B) The game presents frames without 3D, and what they carry doesn't show.** Each of those frames can draw the
  held picture. If the loading screen is drawn and lost, that is a bug of ours.

`proxy/native/zone.cpp` logs a zone change to tell them apart (`[Native] Zone`, on; observers only):

- The teleport calls, with their thread and duration:
  - Gamecode.dll: `StartTeleportTry` / `Confirmed` / `Failed`
  - N3.dll: `n3EngineClient_t::StartTeleport`, which stops the old playfield
  - N3.dll: `NewPlayfield`
- GUI.dll's loading screen:
  - `SetLoadingScreen(kind)` calls
  - its state at each of its own frames and each presented one: kind, window up, closing flag
- The current playfield id.
- In a 20 s window after any of those, the presented frames:
  - every one that took over 40 ms, or whose 3D came or went
  - every 30th otherwise
  - gaps over 250 ms

The lines are `native: zone: +<ms> ...` in `randy-vk.log`. The kinds of zone change to cover are a border crossing,
a whompah, the grid, death and resurrection, a door between indoors and outdoors, and login.

### Found (2026-10-25, in game)

It is case B. The game presents at its full rate through every zone change; its frames just have no 3D. No loading
screen is shown at a zone change; GUI.dll's is only at login (kind 0, about 5 s).

| zone change | frames without 3D | longest frame |
| --- | --- | --- |
| Newland Desert -> 567, border | 0.91 s | 98 ms |
| 567 -> Newland Desert | 0.83 s | 86 ms |
| Newland Desert -> 1187 | 1.25 s | 80 ms |
| 1187 -> Newland Desert | 1.09 s | 166 ms |
| Newland Desert -> 560 | 1.31 s | 179 ms |
| 560 -> Newland Desert | 0.49 s | 83 ms |
| login (the loading screen) | 5.2 s | 419 ms |

The timeline of a zone change:

1. `StartTeleportTry` returns true (a border, a whompah). The grid and some others go straight to `StartTeleport`.
2. Within 10 to 50 ms, the last 3D frame.
3. `StartTeleport` 0 to 400 ms later: the old playfield stops (0 ms).
4. `NewPlayfield` 0.15 to 1 s after that (4 to 60 ms).
5. The first 3D frame 0.3 to 0.7 s later, after a few slow frames (60 to 180 ms: our ground, statels, mesh ids).

All of it runs on the game thread. So the hold needs no presenting of its own: each blank frame shows it.

## Phase 1: hold

- `rvk/handover.cpp` (state in `rvk.h`).
- **Arm:** armed by the teleport (a `ThreadedDevice` record from the hooks).
- **Capture:** while armed, each 3D frame copies `m_main` after the scene's tone mapping and before the interface
  (`InterfaceSceneEnd`, rvk/interface.cpp) into a held image. The last one wins. The hold disarms on
  `StartTeleportFailed`, or when nothing follows.
- **Show it:**
  - Case A: the worker presents the held picture itself once the game hasn't ended a frame for ~50 ms. It stops at
    the game's next frame. Frame generation's presenting between the game's frames (`framegen.cpp`) is the precedent.
  - Case B: each frame without 3D shows it.
- **Look:** `rvk/shaders/handover.frag` gives the slow push-in (~3% over 4 s), a growing blur, and dims to ~85% with a
  little less colour.
- **Never** at login or at character select, or without a held picture.
- **Settings:** `RVK_ZoneFade` (on/off) and `RVK_ZoneHoldMax` (s, hidden), as knobs (`RVK_BOOL_KNOBS` /
  `RVK_FLOAT_KNOBS`).

## Phase 2: settle and reveal

- **Hold on:** after the freeze, the held picture stays in place of the scene, laid in at `InterfaceSceneEnd`. The new
  zone's frames render whole underneath, and the live interface draws over the held picture.
- **Ready** when all of these hold:
  - the native ground is set, or doesn't apply (GNDA playfields, indoors)
  - the statels are loaded (the glue reports both)
  - there have been at least ~20 frames with 3D
  - the last 10 frame times are within 2x their median
- **Fade:** over ~0.5 s once ready, after `RVK_ZoneHoldMax` from the first 3D frame, or as soon as the camera or
  character moves.
- **Frame generation and FSR:** the fade is in `m_main` before the interface, so frame generation sees an ordinary
  scene. The held image is at output size, after upscaling.

## Checks

- `rvk_demo --handover-test`:
  1. a scene
  2. arm
  3. frames without 3D
  4. another scene (a camera jump)
  5. the reveal

  It writes PNGs along the way, with no validation messages.
- `tools/look-check.py`: unchanged (the hand-over is idle without a teleport).
- `tools/crash-check.sh`.
- In game, every kind of zone change, with frame generation on and off, at render scale 75, by night, and at login
  (unchanged).
- Zone loads are already where the freezes and heap corruption happen (docs/stability-plan.md). Nothing here may add
  game-thread work or game-heap allocations at that moment.
