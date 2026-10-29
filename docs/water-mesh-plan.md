# Water from the game's own triangles

## Why

The game draws all its water through `VisualLiquid_t`: one world-space triangle mesh per playfield, handed to us every
frame (rvk/water.cpp `DrawWater`). Finding the water was never the problem. Rebuilding it was: we rasterise those
triangles top-down into a water map (one height per texel) and drape a camera-centred polar grid over it, so the
Gerstner waves have vertices to move. A height map cannot hold what the game's mesh holds, and each new place found
another way it breaks:

| Symptom | Rule added to the map |
|---|---|
| "mountains of water" (shore aprons hung below the surface, the coarse far grid built terraces of them) | each body flattened to one level |
| a pond put up a mountain under the ground | no flattening for bodies spanning more than 4 units |
| walls of water at the subway's pipes | small flat patches at a fall's top go with the fall |
| a small pool flooded everything round it | the map's outermost texels kept dry |
| Borealis's rivers missing (2026-10-21): a sheet at y 10 under the whole land, written over the rivers | highest surface wins |

The game itself just draws the triangles, and the depth test sorts out overlaps, steps and hidden sheets. Drawing its
triangles ourselves puts the water exactly where the game has it, everywhere, with no rule to get wrong.

## Facts about the game's meshes

From the 16 distinct water meshes in the dumps (`logs/*-water.bin`, 2026-10):

- Triangles are small in practice: edges up to ~155 units (the 10000-unit split the game does never triggers). Every
  edge is split into at most ~5000 patches of 64 units or less.
- Welded by position, the mesh is a manifold (an edge has one or two triangles), but coarse triangles meet fine ones
  with T-junctions (Borealis: 214 vertices lying inside a neighbour's 110-unit edge). Displaced, those would crack.
- Flat triangles carry a vertex alpha of 0..204 (the game's shore fade); steep ones are falls and shore steps.
- One mesh may hold several layers: Borealis has a sheet at y 10 under its rivers at 53..90.

## The design

**Once per mesh (CPU, cached by its hash like the maps):** the flat triangles (the falls' and lips' split stays as it
is) welded, the T-junctions stitched (a triangle with points on its edges is fanned from its centroid), edges over
32 units split at their midpoints (per edge, so both triangles beside it agree), until none is longer. Per vertex:

- `calm`: 0 on the mesh's open edges and where it meets a steep triangle, rising to 1 over ~6 units inward (a shortest
  path over the mesh's edges). The waves fade out to the shore and to the falls, so nothing parts at those seams.
- `body`: the size of its connected body (0 pond .. 1 sea) - what the whole map's extent set before, now each body's.

**Each frame (GPU):** the surface vertices and indices go into the frame's ring (as the mesh-mode vertices do now) and
are drawn as 3-point patches: `water_surface.vert` passes them on, `water_surface.tesc` sets each edge's level from its
length and distance alone (the native ground's scheme: no cracks) and drops patches out of view, `water_surface.tese`
lifts the points by the same `Waves()` the grid used, scaled by `calm`, `body`, the storm and the shallows. `water.frag`
shades it as it shaded the grid (mode `W.time.z` 2: no map mask, waves' normal, ripples, wakes, rain, under-water).

**Kept:** the falls' sheets, the shore steps (mesh mode), the spray, the reflections, refraction, caustics, foam.
The disturbance field (wakes) keeps reading the water map for where the water is - the map's only job from now on.

**Switch:** `RVK_WaterMesh` (Water, on): off draws the old grid, for comparison while this settles. Without GPU
tessellation the grid is drawn.

## Phases

1. **The surface** (this branch): the preprocessing, the tessellated draw, `RVK_WaterMesh`. Checks: the dumped meshes
   preprocess to crack-free surfaces (no T-junctions, manifold); `rvk_demo --water-mesh` with the Borealis and subway
   dumps, both settings; `look-check` (its lake scenes will move a little: the waves now ride the lake's own triangles);
   the validation layer.
2. **The wake field's map**: no flattening, no lips - the plain highest-wins mask it needs. Then the map code shrinks to
   that.
3. **The grid goes**: once the surface has been played on in several zones, `RVK_WaterMesh` and the polar grid are
   removed (with a `kMigrations` entry dropping the setting).
4. **Cost**: GPU time against the grid's in the profiling sweep (the grid was ~45k triangles at quality 2).
