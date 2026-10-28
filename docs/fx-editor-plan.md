# Effect editor plan

How effects of ours get made (docs/fx-plan.md is the runtime): in the ao-assets workbench, with the running game as
the preview.

## The workflow

1. **Pick what to replace**: the game's effects searched by nano name, template id or type; or *Recent*, what the
   game just made (do it in game, pick it). Each shows whether ao-vk can stand in for it yet.
2. **Start from a preset**: bolt, burst, aura, heal, smoke, teleport - bound to the template, its phases those of the
   template's kind (cast: charge / flight / impact; play).
3. **Edit**: emitters as tabs (mute, duplicate, reorder), their fields as forms built from a schema (aoassets/fx.py
   `EMITTER_FIELDS`), size and colour over life as a curve and a gradient.
4. **See it in the game**: every change saved into the client's `randy-vk/fx`, which the game reads again each second;
   **Play in game** asks the game to play the template on the player (`_play.txt`). **Record the game's** has it play
   its own and record it (`capture`); the preview's **Compare** shows that beside ours: the view split in two through
   the same camera, both in one scene, or the game's alone.
5. **Keep it**: the workspace's git history (**Save version**); later, packs.

## The pieces

- ao-assets `fx.py` (files, schema, presets, the game link, the API), `viewer/static/fx.html|js|css` (the page, `/fx`),
  the desktop app's View > Effects editor.
- randy-vk proxy `rvk_fx.cpp`: `_play.txt` (`<serial> <template> cast|on`, read every 15 frames; a serial seen at start
  is not played), `_recent.txt` (the last 48 effects made, rewritten at most twice a second).
- The game's own effect, recorded (`<serial> <template> cast|on capture`): the template plays with none of ours in
  its place, and every GfxVisualDiaBill sprite batch that starts drawing after the request within 30 m of it is
  written to `_capture_<template>.txt` - per frame the effect's points (a Spell1's hand, target and bolt head, a Stars'
  locator; else its first sprites' middle) and each live sprite in the world (size, D3DCOLOR, atlas cell), per batch the
  RDB texture and blending its draws used (RDevice::TapDraw). It ends 45 frames after its batches stop drawing (15 s at
  most). fx.py `capture` makes the sprites relative to the point of their phase; the preview puts them on its caster.
  A cast is recorded at yourself, so its flight is short: its charge and impact are the useful parts.

## Next

1. **Preview in the workbench**: a JavaScript copy of fx.comp in the three.js viewport, on a stand-in (a caster and a
   target for casts, a figure for the rest) - looping, scrubbing, slow motion. Approximate; the game stays the judge.
2. **More of the game's effects**: an adapter each (hits, heals, buffs, fire / smoke, teleports) or our own control
   class (docs/fx-plan.md phase 3).
3. **Richer effects**: own sprites and flipbooks, spin, a light the effect casts, ribbons, sub-emitters, spawning over a
   character's body, ground collisions.
4. **Items' effects by name**: items name their effects in their events, not their stats - read those so the search
   finds "Basic Nano Recharger" (today: the nanos, ids, types, and Recent).
5. **Packs**: effects shipped with a pack.
