// DisplaySystem.dll's ground (ground.cpp): how far out it is drawn at full quality, RVK_GroundRange.
#pragma once

namespace rnative::ground {

// Hooks DisplaySystem.dll once it is loaded (the first presented frames); later calls do nothing.
void Install();
// RVK_GroundRange: the game's full-quality radius (DisplayGroundFullQualityRadius, its slider at most 44) times this
// (1: the game's own). Taken up the next time the game tessellates the ground (it does as the camera moves).
void SetRange(float scale);
// RVK_GroundFar: the texels a 4 x 4 cell gets in the textures the game bakes for the ground past that radius - 8 (the
// game's), 16 or 32. Patches baked after a change take it.
void SetFar(int texelsPerCell);

}  // namespace rnative::ground
