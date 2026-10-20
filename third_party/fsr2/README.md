# FidelityFX Super Resolution 2.2 (AMD)

AMD's FSR 2.2.1 (https://github.com/GPUOpen-Effects/FidelityFX-FSR2, tag v2.2.1, commit 1680d1e), MIT licence
(`LICENSE.txt` - it ships with every release, as `LICENSE-FSR2.txt`). ao-vk uses it as the second upscaler for the
render scale (`RVK_Upscaler 1`, `rvk/fsr2.cpp`).

Only what the Vulkan path needs is here: the API (`ffx_fsr2.cpp` and its headers), the Vulkan backend
(`vk/ffx_fsr2_vk.cpp`), and the passes' GLSL (`shaders/`). AMD's shader permutation tool (a Windows program) and its
`vk/shaders/ffx_fsr2_shaders_vk.cpp` are replaced by `tools/gen_fsr2_shaders.py`, run by the build.

Changes from AMD's sources (for a 32-bit build, where Vulkan's non-dispatchable handles are 64-bit integers, not
pointers - each marked "ao-vk" where it isn't a plain `nullptr` -> `VK_NULL_HANDLE`):
- `ffx_types.h`: `FfxResource::resource` and `FfxPipeline` are `uint64_t` (a `void*` truncated the handles).
- `ffx_fsr2.cpp`: its null tests of `resource` compare with 0.
- `vk/ffx_fsr2_vk.cpp`: `nullptr` -> `VK_NULL_HANDLE` for handles; the resource casts to and from `uint64_t`;
  host-visible allocations rounded up to 4 KB (a 32-bit process's memory maps under Wine are placed ones, which
  need it).
- `shaders/ffx_fsr2_callbacks_glsl.h`: two storage images declared in their images' formats - the dilated depth `r32f`
  (it said `r16f`), the prepared input colour `rgba16f` (it said `rgba16`); the writes were undefined.
