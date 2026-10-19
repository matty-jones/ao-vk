// DisplaySystem.dll's ground (ground.cpp): how far out it is drawn at full quality, RVK_GroundRange.
#pragma once

#include <cstdint>

namespace rnative::ground {

// Hooks DisplaySystem.dll once it is loaded (the first presented frames); later calls do nothing.
void Install();
// RVK_GroundRange: the game's full-quality radius (DisplayGroundFullQualityRadius, its slider at most 44) times this
// (1: the game's own). Taken up the next time the game tessellates the ground (it does as the camera moves).
void SetRange(float scale);
// RVK_GroundFar: the texels a 4 x 4 cell gets in the textures the game bakes for the ground past that radius - 8 (the
// game's), 16 or 32. Patches baked after a change take it.
void SetFar(int texelsPerCell);

// The native ground (proxy rvk_ground.cpp, docs/native-ground-plan.md): told of each playfield's ground the first time
// the game tessellates it (the AnarchyGround_t and its AnarchyGroundData_t: +0x17C), on the game's thread.
// And each frame the game updates it (N3's n3GroundRenderer_t::RunFunction - the ground alive then: what reads the
// game's ground through it, does it there; Tesselate itself runs only when the player crosses into another cell).
void SetNewGround(void (*onNewGround)(void* ground, void* data));
void SetOnGroundFrame(void (*onGroundFrame)(void* ground, void* data));
// How many frames the game has updated a ground (none for a while: it is gone - a loading screen, indoors); and the
// ground forgotten, so the next one is read anew (a playfield loaded again may reuse the same memory).
uint32_t GroundFrames();
void ForgetGround();
// A tile's texture as the game makes it (FUN_10036594: loaded once, kept by the data): an RTexture_t, or null.
void* TileTexture(void* data, uint32_t tile);
// While rvk draws the ground itself: the game's own (unseen) at its least work - the smallest full-quality radius,
// its far patches baked at 2 texels a cell.
void SetNative(bool active);

}  // namespace rnative::ground
