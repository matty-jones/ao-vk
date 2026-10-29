# Light in and through plants (subsurface scattering for foliage)

Status (2026-10-21, branch `amd-fx`): phase 1 built (`RVK_CrownSky`, `RVK_CrownDepth`, both on by default),
demo-tested; phases 2 and 3 to do.

The user asked why trees look flat at noon and whether subsurface scattering could start with foliage or grass. A
frame dump at 12:00 (Newland City) showed the crowns taking about a fifth of their light from the sun on average
(the scene's "sun share", attachment 2's B) against 0.94 on a sunlit wall: the game lights its trees' material with
its whole global ambient (`FF767676`, 0.46, material ambient 1), the same from every side, so a crown's top and its
underside get the same light and the shape goes. Our crown shading (`RVK_CrownShade`, ffp.vert) darkens the inside
and lightens the top by 6 % - of everything, sunlight and ambient alike - and the sun reaches only the crown's outer
shell (its own shadow), which `RVK_SunStrength` lightens. `RVK_LeafLight` (ffp_main.glsl: sunlight through a leaf
seen against the sun, tinted by its texture) does nothing with a high sun and the camera looking across.

What exists: leaves - the sun through a leaf (`RVK_LeafLight`), from the shadow at the leaf's own surface (so only
the crown's outer layer facing the sun glows); grass - per vertex in grass.vert, the sun shining through the blade
looking towards it.

## Phase 1: tree crowns

- **Sky-shaped ambient** (`RVK_CrownSky`, 0..1): on a crown (`D.leaf.w > 0`: a canopy the leaves know), the ambient
  part only, by the leaf's direction from the crown's centre - 1.45 on its top, 0.55 under it, 1 at its middle (the
  mean over the sphere 1: a tree no brighter or darker overall), as the sky lights a round mass from above. Vertex
  shader (ffp.vert), both lighting paths.
- **Light through the crown by its depth** (`RVK_CrownDepth`, 0..1): the sunlight through a leaf (`RVK_LeafLight`'s
  term) taken from how much crown it crossed - the cascades' depths (`shadowDepths`) give the first thing the sun hit
  over this point, `FL.cascadeDepth` its distance in world units - falling off as exp(-depth / 1.5) in place of the
  leaf's own shadow. Backlit crowns glow at their thin rims and stay dark inside; leaves under another tree's crown or
  a wall get none.
- **Its colour**: light through a leaf is deeper than off it - the texture's colour squared over its brightness.
- Checks: `rvk_demo --leaf-tree` (sun behind, overhead, at dusk; `--sun-dir`), look-check unchanged with both at 0,
  validation. One `shadowDepths` tap more per foliage pixel.

As built: the crowns' light also rolls off towards 1 from 0.6 (under `RVK_CrownSky`) - the game's lighting is clamped at
1 as D3D's, and its ambient (0.46) and the sun reached it on every leaf facing the sun more than halfway: the clamp, not
only the even ambient, had made the lit side flat. And `RVK_LeafLight`'s test for leaves (the texture's coarse mip has
holes) passed little of a dense canopy's texture (its mip 4 nearly solid): a crown the leaves know (`D.leaf`) now always
counts - the cause of "leaflight does nothing" (user). Demo (`--leaf-tree 20842,13147 --shadows --pixel-lighting
--leaf 1`; the demo lights per vertex without `--pixel-lighting` and has no leaf light without `--leaf`): at noon the big
crown's top +23 %, its underside -13 %; backlit, the light through it 9.2 (the old test) -> 15.2 (crowns always) -> 18.2
(with the depth) on the big crown, the small dense one 2.8 -> 6.8 -> 4.9 (its middle darker, as meant).

Code: `rvk/shaders/ffp.vert` (`crownSky` before the lighting block; the shoulder in the per-vertex path),
`rvk/shaders/ffp_main.glsl` (`CrownThrough`; the shoulder before the `F_OVERBRIGHT` clamp; the light through leaves
under "Sunlight through leaves"), `FL.foliage` (x `RVK_CrownSky`, y `RVK_CrownDepth`; z, w free) in
`frame_lights.glsl` / `internal.h` `FrameLights`, filled in `draw.cpp` next to `volume2`; knobs in `rvk.h`
`RVK_FLOAT_KNOBS`; settings in `rvk_settings.cpp` (Lighting, the Advanced list, `kVanilla`). Commit `a7e44d7`, staged
`randy31-crowns1.dll`. Awaiting the user's look in game (noon from the side; walking towards a low sun through trees).

## Phase 2: grass

What grass.vert already does (per vertex, `GF.look.z` = `RVK_GrassGlow`, around line 325): a rounded blade (the edge
towards the sun lighter, zero-mean), the sun through the blade (`behind` - the sun on the face away from the camera -
and `bk` - looking into the sun - times `pow(t, 1.3)`, the thin upper blade more), tinted `albedo * vec3(1.2, 1.25,
0.7)`, a sheen along it, and `canopyShade` (in a dense field a low sun reaches only the upper blades). So less is
missing than for the crowns:
- **Colour**: the light through a blade as the leaves' now - `albedo * albedo / luma(albedo)` - in place of the fixed
  yellowish tint (check against the user's fields at noon and dusk before switching).
- **Depth into the field**: the light through a blade fading with the blades between it and the sun - the grass
  shadow layer (`kGrassShadowLayer`, the map's layer 4: the blades cast into it) gives whether a blade is lit; its
  depth (if that layer keeps one) how far. `canopyShade` approximates it from the blade's height alone today.
- Setting: `RVK_GrassDepth` (0..1) or under `RVK_GrassGlow`; names within 15 characters (`RVK_Grass*` - check
  rvk_settings.cpp's prefix routing, as `RVK_Leaf*` routes to the leaf settings and couldn't be used for phase 1).
- Test: `rvk_demo` grass scenes (`--grass-walk X`, `--grass-field`, the native ground with `--grass-field`), sun behind
  (`--sun-dir`) and overhead; `--shot-run K` saves K frames in the statel and native-ground scenes.

## Phase 3: lamps through foliage

- Point and spot lights behind a bush or a crown glowing through it at night: in `AccumulateLights`
  (`lighting.glsl`, line ~117) a term for foliage (`F_FOLIAGE` or `D.leaf.w != 0`) like the sun's through-term in
  ffp_main.glsl - `max(-dot(nf, L), 0)` for the light behind the leaf as the camera sees it - times the light's
  attenuation; through the light's shadow cube (`PointShadow`, ffp_main.glsl line 14) where it has one, for depth.
- Test: the point-shadow scene (`--point-shadow-test`) with trees, or `--leaf-tree` with `--ground-light`-style lamps
  (add a lamp option to the tree scene).

## Testing the trees (what tripped phase 1)

The demo's tree scene (`--leaf-tree build/trees/20842.tree,build/trees/13147.tree`, absolute paths) is not lit as
the game by default:
- `--pixel-lighting`: without it the demo lights per vertex (the game: "pixel lighting 1" in a dump's first line); the
  scene's sun share (dump `-local.bmp` B) is then 0 and per-pixel changes show nothing.
- `--shadows`: without them every leaf is sunlit and the ambient is a small part of the light.
- `--leaf 1`: the demo's `RVK_LeafLight` is 0 by default (the game's 1).
- `--fixed-clock 16.667`: else the leaves' wind and flutter differ between runs and swamp small changes.
- `--sun-dir X Y Z`: the direction the light travels; the camera stands at -z looking +z, so `0.1 -0.35 -1` is behind
  the trees, `0.6 -0.7 -0.4` about the user's noon.
- `--dump d.txt` writes the in-between images (`-scene`, `-albedo`, `-local`: G reflectivity, B sun share; `-surface`,
  `-brixgi`): light = scene (undo x/(1+x), gamma 2.2) / albedo, as measured above.
Everything Wine runs in `WINEPREFIX=$HOME/.wine` while the user's game is open (`~/.wine-prk`); look-check too.

## Not planned: skin

Characters' skin would want a screen-space blur of its diffuse light (a separable SSS pass) on a skin mask. rvk has a
character mask now (the normals' marker, gbuffer.frag) but nothing tells skin from armour and clothing, and the game's
textures carry painted shading: little to gain for the risk.
