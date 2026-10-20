// Force-included into FSR 2's sources (third_party/fsr2; CMakeLists.txt): its Vulkan backend calls a few instance
// functions directly, which rvk loads at run time (vk.h) - here they are rvk::vk's pointers.
#pragma once
#include "vk.h"
#include "fsr2_rename.h"
using namespace rvk::vk;
