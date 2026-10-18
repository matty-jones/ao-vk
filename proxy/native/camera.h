// RCamera_t and RLight_t natively (camera.cpp): randy-vk.ini [Native] Scene=on.
#pragma once

#include "native/native.h"

namespace rnative::camera {

void Install(HMODULE orig);
// RVK_ViewRange: how far the camera sees, x the game's (its fog's end and back plane; 1: the game's own), the far cull
// then by true distance - a round view range, not the game's flat back plane (distant land popped in and out as the
// camera turned: user, 2026-10-18). The renderer's fog goes out with it (rvk Device::SetViewRange).
void SetViewRange(float range);

}  // namespace rnative::camera
