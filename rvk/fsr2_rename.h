// FSR 2.2's Vulkan backend (third_party/fsr2) and the FidelityFX SDK's (third_party/ffx-sdk, FSR 3.1) define functions
// of the same names: FSR 2's are renamed (its sources through fsr2_vk_shim.h, and rvk/fsr2.cpp).
#pragma once
#define ffxGetDeviceVK fsr2GetDeviceVK
#define ffxGetCommandListVK fsr2GetCommandListVK
#define getVKImageLayoutFromResourceState fsr2GetVKImageLayoutFromResourceState
#define getVKAccessFlagsFromResourceState fsr2GetVKAccessFlagsFromResourceState
#define getVKImageUsageFlagsFromResourceUsage fsr2GetVKImageUsageFlagsFromResourceUsage
#define getVKPipelineStageFlagsFromResourceState fsr2GetVKPipelineStageFlagsFromResourceState
#define findMemoryTypeIndex fsr2FindMemoryTypeIndex
#define addBarrier fsr2AddBarrier
#define flushBarriers fsr2FlushBarriers
#define setVKObjectName fsr2SetVKObjectName
#define BackendContext_VK Fsr2BackendContext_VK
