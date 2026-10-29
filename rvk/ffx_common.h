// The FidelityFX SDK's effects beyond FSR (third_party/ffx-sdk, MIT; docs/amd-fx-plan.md): what each needs of the
// SDK's Vulkan backend - its interface over our device, with scratch memory for the contexts the effect makes - and
// our images as its resources. (FSR 3 and frame generation set up their own, fsr3.cpp / framegen.cpp.)
#pragma once

#include "vk.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <cstdint>
#include <vector>

namespace rvk {

struct FfxBackend {
    VkDeviceContext deviceContext{};             // (ffxGetDeviceVK keeps a pointer to it: the struct mustn't move)
    std::vector<uint8_t> scratch;                // the backend's (ffxGetScratchMemorySizeVK)
    FfxInterface iface{};

    FfxErrorCode Init(VkDevice device, VkPhysicalDevice physical, uint32_t contexts)
    {
        deviceContext = {device, physical, vk::vkGetDeviceProcAddr};
        scratch.assign(ffxGetScratchMemorySizeVK(physical, contexts), 0);
        return ffxGetInterfaceVK(&iface, ffxGetDeviceVK(&deviceContext), scratch.data(), scratch.size(), contexts);
    }
};

// An image of ours as the SDK's resource, in its read state - or, `uav`, for its writes (made with storage usage).
inline FfxResource FfxImage(VkImage image, VkFormat format, uint32_t w, uint32_t h, bool uav, const wchar_t* name)
{
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (uav ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
    return ffxGetResourceVK(uint64_t(image), ffxGetImageResourceDescriptionVK(image, ci), name,
                            uav ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ);
}

}  // namespace rvk
