// N3.dll's statel rings (rings.cpp): how far out the playfield's placed objects - rocks, shrubs, trees - are shown,
// RVK_DetailRange.
#pragma once

namespace rnative::rings {

// Hooks N3.dll once it is loaded (the first presented frames); later calls do nothing.
void Install();
// RVK_DetailRange: the small things' rings (the inner two) times this (1: the game's own); the outer rings stay the
// game's. Each kept within the zones N3 keeps loaded.
// Taken up when the rings are next measured: when the game's camera moves onto another tile.
void SetScale(float scale);

}  // namespace rnative::rings
