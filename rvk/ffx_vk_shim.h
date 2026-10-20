// Force-included into the FidelityFX SDK's sources (third_party/ffx-sdk; CMakeLists.txt): rvk's run-time loaded Vulkan
// functions for the ones it calls directly (vk.h), and what its headers use without including (std::popcount).
#pragma once
#include <bit>
#include "vk.h"
using namespace rvk::vk;
