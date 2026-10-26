# Effects plan: our own particle effects, and an editor for them

The game's effects (nano casts, hits, buffs, fire, teleports, auras) are thin: a few dozen camera-facing sprites
per effect, a handful of fixed behaviours, no light. This plan replaces them with effects of our own, simulated and
drawn by rvk, written in an effect editor in ao-assets, while the game keeps deciding when, where and for how long
an effect plays.

Players without randy-vk see the stock effects; nothing here changes the client's files.

## What the game has

**Templates.** `client/Setupf/gfxtweak.bin` holds every effect as templates: `u32 count`, then per template
`u32 id, u32 type, u32 n, n x u32 params` (Gamecode.dll FUN_10106be2 / FUN_101068fa; params are ints or floats as
the type's reader takes them). 2695 templates, about 150 (type, length) shapes. The type picks a control class
(`_GfxControlStars_t`, `GfxControlTParticle_t`, `_GfxControlSpell1_t`, ... 89 RTTI names in Gamecode.dll). An effect
is a tree: containers (Meta 2007, Sequencer 3004, Delay 3013) list child template ids. Nanos (rdb_1040005) name
their templates in four stats: 361 (600 distinct templates, mostly Meta / Stars: the nano's own effect), 428 (218,
mostly Spell1: the cast projectile), 419 (205, mostly Stars: likely the landing), 366 (69; usually 49999, "none").
Lesser Controlled Rage, nano 43369: 361 -> 47119, 428 -> 46116, 419 -> 17200. `python -m aoassets fx find|tree`
(ao-assets) searches them by id, type or nano name.

Effects are shared heavily: Stars 43029 is the stat-361 effect of 1306 nanos, Stars 17300 the 419 of 939, Sparks
2710 of 871, Meta 17200 of 858, Spell1 46133 and 46123 the projectiles of 715 and 597. Twenty templates or so cover
most of what players see cast. 1827 trees in all, 771 of them played by nanos; the rest are weapons, items,
monsters and the world.

`~/.wine-prk/drive_c/linux/fxmod/gfxtweak.py` (from the first GPU particle round, e85a1a6) knows the type names and
the Stars params. Its `boost` of Lesser Controlled Rage is still in the client's gfxtweak.bin (8 clones,
98700000-98700007); `gfxtweak.py restore` puts the 2013 original back.

**Making an effect.** Everything goes through exported `_EffectHandler_t` methods (Gamecode.dll; decompiles in
`re/fx/`):

- `CreateEffect2(templateId, anchor...) -> handle`: checks the effect category (the player's "show effects"
  options), then `CreateGfxControl(templateId, anchor...)` builds the control and `MakeHandle` files it. Anchors,
  one overload each: nothing, a world position, a matrix, an `RConnector` (a mesh's attach point), an `n3Dynel` +
  body location (a character), and source + target pairs of those (projectiles, beams). `CreateTracer` for shots.
- `UpdatePosition` / `UpdateMatrix`, `SetDuration`, `SetColor`, `SetScale`, `SetSize`, `SetRadius`, `SetSpeed`,
  `NextState`, `TerminateEffectGracefully`, `DeleteEffect`, `IsRunning`, by handle.
- `ReloadGfxTemplates` re-reads gfxtweak.bin.

**A control.** `_GfxControl_t` is 0x2C bytes: vtable, template id (+4), frame time (+8), age (+0xC), duration
(+0x10), done (+0x14), visible (+0x20). `Process` advances the clock; `TerminateGracefully` is vtable slot 6.
Subclasses position themselves through a `_GfxLocator_t` set up by the exported `InitPositionTemplate`,
`InitMatrixTemplate`, `InitConnectorTemplate`, `InitDynelTemplate` (params 0-6 of the template: locator flags and
an offset). Stars draws through a `GfxVisualDiaBill` sprite batch (DisplaySystem.dll), which is what the current
GPU particles (`rvk_particles.cpp`) hook.

## Design

### Effect files

One file per effect, `<client>/randy-vk/fx/<name>.rvkfx` (JSON), loaded and hot-reloaded by randy-vk:

- **binding**: the template ids it stands for, and `replace` (the stock effect is hidden) or `add` (drawn with it).
  Binding a container replaces its whole subtree; binding a leaf only that part.
- **emitters**, each with:
  - *shape*: point, sphere / shell, disc / ring, cone, box, line (source to target), the anchor character's body;
  - *spawn*: rate over the effect's life, bursts, a cap;
  - *particle*: life, start speed and direction, inherit the anchor's velocity;
  - *forces*: gravity, drag, curl noise, vortex around an axis, attraction to the anchor or the target, wind
    (`_EffectHandler_t::GetSmoothWind`);
  - *over life*: colour and alpha gradient, size curve, rotation, sprite atlas frame;
  - *look*: texture (the game's effect textures by id, or our own PNG), blend (add / alpha / premultiplied), HDR
    intensity, soft edges against the depth buffer, render as billboard, velocity-stretched, ribbon trail or mesh;
  - *sub-emitters* on spawn / death, and a *light* (colour and radius curve) for the whole emitter.
- **timing**: start delay per emitter, loop or one-shot, and what `TerminateGracefully` does (stop spawning and
  let particles finish, or fade).

The format is versioned and the editor writes it; randy-vk reads it with defaults for everything missing.

### Runtime (randy-vk)

- **Our control class.** Hook the `CreateGfxControl` overloads (they run for every node of a tree, so containers'
  children come through too). For a template bound by an `.rvkfx`, build `RvkFxControl`, a `_GfxControl_t` of our
  own (the base layout, a `_GfxLocator_t` filled by the matching exported `Init*Template`, our effect instance id)
  with a vtable of our functions. The game then handles it like any control: handle, anchor, duration, terminate,
  delete, effect categories. `add` builds the stock control too, inside a small container of ours. If the class
  turns out not to be self-contained enough, the fallback is shadowing: build the stock control, `DisableVisibility`
  it, and read its locator.
- **Simulation.** Grow `rvk/particles.cpp` into an emitter system: a pool of particles on the GPU, per-frame spawn
  lists from the CPU (emitter positions, counts), one compute pass to spawn and update, indirect draws by blend
  mode. Additive particles unsorted; alpha-blended ones sorted per effect. Lights go into the frame lights; smoke
  emitters can feed the volumetric smoke. Darkness: particles are emissive, but alpha-blended smoke takes `m_dark`.
- **Play from outside.** A console command / hotkey `fx <id or name>` calls `CreateEffect2` on the player or the
  target, so an effect can be seen without casting the nano. The editor writes a play request file the DLL watches
  (like the live meshes).
- **Settings.** `RVK_Fx` (on/off), `RVK_FxScale` (particle budget), `RVK_FxLog` (ini only: logs every effect the game
  creates with its template, anchor and handle, for finding which template a spell uses).
- **Tests.** `rvk_demo --fx <file> [--fx-anchor ...]` renders an effect headless (a few frames along its life to
  PNG), so `look-check` can hold a reference effect. `native-check` is not needed (nothing of the original's is
  replaced), `calllog-ab` checks that `RVK_Fx=0` leaves the frame as it was.

### Editor (ao-assets)

- **gfxtweak reader** (`aoassets/gfxtweak.py`): templates, type names, trees, byte-identical round trip in
  `selftest`; names from the nanos and items that point at roots, where we can read them.
- **Effects panel** in the workbench / desktop app: the stock tree searchable by name, type and id; for each node
  its params and the `.rvkfx` bound to it; "new effect for this template".
- **Editor**: emitters as a list, each a form with curve and gradient widgets, presets to start from (sparks,
  embers, smoke puff, energy ring, shockwave, beam, swirl, heal motes, teleport column). Textures from the game's
  effect textures or a file.
- **Preview**: in the three.js viewport, a JavaScript copy of the simulation on a character or a dummy (a stand-in
  that will differ in details), and "Play in game", which saves and asks the running DLL to play it now.

## Phases

0. **Investigation.** Format, factory, handle API, base class (done, this document). Left: `_GfxLocator_t`'s size
   and how it gives a world matrix; which anchor each common action uses; what Meta does with its children.
1. **ao-assets gfxtweak reader.** Module, `fx` CLI (`fx find`, `fx tree`), selftest, type names, nano names.
2. **Effect log in game.** Hook `CreateGfxControl` (all overloads) and the terminate / delete calls, log with
   `RVK_FxLog`, `fx <id>` command. Play through a session, collect which templates the common actions use.
   *Done (60f17da):* hooked at `MakeHandle` instead (one function; the overload from its return address). A session
   (394 effects): nano casts start a `Spell1` from caster to target (stat 428; anchor dynel+dynel, Gamecode+0x07b18e)
   and the nano's own effect on the target (stat 361; e.g. Meta 47173, dynel, +0x07adf1). Running buffs show as Stars
   (43640/43641) and BuffPlaceHolder pairs (+0x0d3bd8 / +0x0d3c42); the world's lamps and fires as Sprite_t / Fire_t /
   Smoke_t on connectors; every character has WaterRipples 33000 and Shadow 37000.
3. **Runtime.** `RvkFxControl`, `.rvkfx` loader with hot reload, the emitter system, `rvk_demo --fx`. One hand-written
   effect replacing Lesser Controlled Rage, seen in game.
   *First round:* not our own control class yet - the game's cast (`_GfxControlSpell1_t`) keeps running (its flight,
   homing, timing) with its drawing hidden, and announces ours from its update (proxy rvk_fx.cpp). The emitter system
   is rvk fx.cpp / fx.comp (1024 particles an emitter, 256 blocks, 128 drawn), definitions fx_def.cpp, files in
   `fx/` (first: `nano_bolt.rvkfx`, template 46133). Other control classes need an adapter each, or the control class.
4. **Editor.** Effects panel, emitter forms, JS preview, play in game.
5. **Effect set.** Effects for the common actions, in turn: nano cast / buffs, hits and impacts, heals,
   teleports and zoning, fire and ambient, projectiles and beams.
6. **Extras.** Ribbon trails, mesh particles, heat haze (refraction), decals on the ground, depth-buffer collisions.

## Risks

- Building a control the game didn't make: our object must satisfy everything that walks the control list
  (`RunFunction`, category toggles, `GetEffectByID` callers). Phase 2's log and the shadowing fallback cover this.
- Effects of many players in a crowd: a fixed particle budget, nearest effects first, like the current particles.
- Overlap with the GPU particles of `[Particles] Templates`: a template bound by an `.rvkfx` skips them.
