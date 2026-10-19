# Terrain rock (rvk terrain.cpp)

Embedded in the DLL and laid on the faces of cliffs (`RVK_CliffRock`), tinted to each ground cell's own colour.

| File | What |
|---|---|
| `cliff_side_diff_1k.jpg` | albedo |
| `cliff_side_nor_gl_1k.jpg` | normal map (OpenGL convention) |
| `cliff_side_arm_1k.jpg` | occlusion, roughness, metallic (R, G, B) |

"Cliff Side" from Poly Haven (https://polyhaven.com/a/cliff_side): photography James Ray Cock and Dario Barresi,
processing Jenelle van Heerden. Released under CC0 (public domain, https://creativecommons.org/publicdomain/zero/1.0/);
no attribution is required, it is given here anyway. The photo covers 1.83 x 1.83 m; ao-vk lays it larger (one repeat
per `kRockUnits` world units).
