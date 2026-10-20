# FidelityFX SDK 1.1.4 - the FSR 3.1 parts (AMD)

From https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (tag v1.1.4, commit c6efa6b), MIT licence
(`LICENSE.txt` - it ships with every release, as `LICENSE-FidelityFX-SDK.txt`). ao-vk uses it for FSR 3.1
(`RVK_Upscaler 2`, `rvk/fsr3.cpp`; `docs/fsr3-plan.md`).

Here: the host headers, the GPU shaders of the 3.1 upscaler, optical flow and frame interpolation (and the shared
ones they include), their components (`src/components/fsr3`, `fsr3upscaler`, `opticalflow`, `frameinterpolation`),
the shared sources, the Vulkan backend and its frame interpolation swapchain, AMD's blob accessors for the three, and
the passes' Vulkan GLSL. Not here: AMD's shader tool and its `*_permutations.h` - `tools/gen_ffx_shaders.py` makes
those at build time (the variants rvk uses; the wave64 and 16-bit names point at the same tables).

Changes from AMD's sources, for a 32-bit build (where Vulkan's non-dispatchable handles are 64-bit integers, not
pointers) and Wine; each marked "ao-vk" where it is more than `nullptr` -> `VK_NULL_HANDLE` or `0`:
- `include/FidelityFX/host/ffx_types.h`: `FfxSwapchain`, `FfxPipeline` and `FfxResource::resource` are `uint64_t`
  (a `void*` truncated the handles).
- `include/FidelityFX/host/backends/vk/ffx_vk.h`, `src/backends/vk/ffx_vk.cpp`: `ffxGetResourceVK` takes the handle as
  `uint64_t`; casts to and from it; `vkGetBufferMemoryRequirements2KHR` falls back to the core function (the KHR name
  is null unless the extension is enabled - a call through it crashed); host-visible memory rounded up to 4 KB
  (Wine's placed maps for a 32-bit process need it).
- `src/backends/vk/FrameInterpolationSwapchain/*`: the replacements of `vkQueuePresentKHR`, `vkAcquireNextImageKHR`,
  `vkGetSwapchainImagesKHR`, `vkDestroySwapchainKHR`, `vkSetHdrMetadataEXT` declared `VKAPI_CALL` (stored in Vulkan's
  function pointer types: `__stdcall` on 32-bit); the debug name helper takes a `uint64_t`; two `submit` calls get a
  named `SubmissionSemaphores` (clang-cl binds no temporary to a non-const reference).
- `src/components/fsr3upscaler/ffx_fsr3upscaler.cpp`: its null tests of `resource` compare with 0.

AMD bugs fixed (frame generation, `RVK_FrameGen`, `rvk/framegen.cpp`; each found by the validation layer):
- `src/components/fsr3/ffx_fsr3.cpp`: an interpolation-only context never set its `interpolationOnly` field, and made
  the shared dilated depth / motion / previous depth images from the upscaler it hadn't made (0x0): they are made
  from the frame interpolation's descriptions.
- `src/backends/vk/ffx_vk.cpp`: the backend's descriptor pool listed 6 sizes and said 5 (no storage buffers).
- `include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h`: the luma history (an RGBA16F image)
  declared `rgba8`, so its loads and stores were undefined (the 3.1 upscaler too).
- `src/backends/vk/FrameInterpolationSwapchain/`: line width 1 (not 0) in the UI composition's and pacing debug's
  pipelines; the presenter thread's present semaphore the acquired image's own (one for every present was reused
  while an earlier present could still wait on it); the per-image semaphores made once (made twice, the first lost);
  the UI composition's pipelines, render pass, views and descriptors freed with the swapchain (never freed; the "new
  device" branch freed through the old device and never kept the new one); every submit, present and queue wait under
  one lock (`ffxQueueMutex`: the game's thread and the presenter thread used the present and acquire queues at once);
  a second wait for presents once the threads have ended (the present semaphore is signalled before the present).
