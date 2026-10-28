#version 450
// ffp_lite.frag with no discard (ffp_nocut.frag's reason: early-Z kept).
#define RVK_LITE
#define RVK_NO_CUTOUT
#include "ffp_main.glsl"
