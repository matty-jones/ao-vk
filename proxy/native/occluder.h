// HMOccluder_t natively (occluder.cpp): randy-vk.ini [Native] Scene=on.
#pragma once

#include "native/native.h"

namespace rnative::occluder {

void Install(HMODULE orig);
// RVK_Occlusion: the terrain horizon's culling (off: everything in view is drawn - the renderer then knows it, and
// remembers it as a shadow caster when it's out of view; with it, a wall's top hidden behind a hill from the camera
// was never drawn, never known, its shadow missing - user, 2026-10-13).
void SetHorizonCulling(bool on);
// The free camera (Ctrl+Shift+C) flies: no horizon culling meanwhile, whatever RVK_Occlusion says. The horizons are
// built from the ground under the camera, right for the game's camera beside the character but not for one high above
// the hills, which sees what is behind them.
void SetFreeCamera(bool flying);

}  // namespace rnative::occluder
