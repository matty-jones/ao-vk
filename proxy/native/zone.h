// A zone change as the game goes through it (zone.cpp): the teleport, the loading screen's state and the presented
// frames around it, logged with their times - the research for the zone hand-over (docs/zone-handover-plan.md).
#pragma once

#include <cstdint>

namespace rnative::zone {

// Hooks GUI.dll, Gamecode.dll and N3.dll once each is loaded (the first presented frames); later calls do nothing.
// [Native] Zone=off: none of it.
void Install();
// A presented frame (the game's thread), with the renderer's count of frames in a row without 3D (it lags a frame).
void Frame(uint32_t framesWithout3D);

}  // namespace rnative::zone
