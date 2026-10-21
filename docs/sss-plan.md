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

## Phase 2: grass

- The same colour and a softer terminator in grass.vert; the light through a blade fading with how deep in the field
  it stands (the blades' density round it).

## Phase 3: lamps through foliage

- Point and spot lights behind a bush or a crown glowing through it at night (the frame lights' loop, lighting.glsl;
  their shadow cubes for depth where they have one).

## Not planned: skin

Characters' skin would want a screen-space blur of its diffuse light (a separable SSS pass) on a skin mask. rvk has a
character mask now (the normals' marker, gbuffer.frag) but nothing tells skin from armour and clothing, and the game's
textures carry painted shading: little to gain for the risk.
