// Picking characters natively (cat_pick.cpp): randy-vk.ini [Native] Scene=on, and Pick:
//   on      ours (the default)
//   off     the original's picking (the rest of Scene stays native)
//   verify  both on every test, the original's answer kept; disagreements and why ours missed go to randy-vk.log
#pragma once

#include "native/native.h"

namespace rnative::catpick {

void Install(HMODULE orig);
// Whether picking takes RCATMesh_t's line and ray tests (Scene=on, Pick on or verify): then it keeps deferred
// skinning exact around them (skin::PickingBegin), not cat_skin's own hooks.
bool Native();

}  // namespace rnative::catpick
