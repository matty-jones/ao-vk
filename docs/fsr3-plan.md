# FSR 3.1: the 3.1 upscaler, then frame generation

Status: Phase A built 2026-10-20 (`RVK_Upscaler 2`, staged `randy31-fsr3a.dll`). Demo, mean difference from 100%
(forest 1440p): at 75% ours 4.95, FSR 2.2 5.05, **FSR 3.1 4.52**; at 50% ours 6.33, **FSR 2.2 4.96**, FSR 3.1 5.77.
Walker at 50%: FSR 2.2 1.89, FSR 3.1 1.87; storm at 50%: ours 5.31, FSR 2.2 5.81, **FSR 3.1 5.20** (keeps the rain).
GPU cost: to measure with the GPU idle.

Phase B built 2026-10-20 (`RVK_FrameGen`, `rvk/framegen.cpp`, staged `randy31-framegen1.dll`): the demo windowed shows
~1195 frames for 600 rendered, with each upscaler, and with frame generation switched off and on mid-run
(`--frame-gen-toggle`); validation clean. AMD bugs found on the way are listed in `third_party/ffx-sdk/README.md`; two
storage-image format mismatches of the same kind in FSR 2.2 too (`third_party/fsr2/README.md`). After those, at 75%
(forest 1440p): FSR 2.2 4.40, FSR 3.1 4.86, ours 4.95; at 50%: FSR 2.2 5.74, FSR 3.1 5.76, ours 6.33; storm 50%: FSR
3.1 5.14, ours 5.31, FSR 2.2 5.35. To try in game: pacing, latency, the interface over generated frames.
First in-game try: no smoother - the configuration's interpolation rectangle was left empty, and every pixel outside
it is the real frame's, so each generated frame was a copy of the next real one (a probe of the frames: generated vs
next real 0.000). With the whole frame as the rectangle (staged `randy31-framegen2.dll`) the generated frame lies
halfway: 0.13 from the previous real frame, 0.12 from the next, 0.25 between the two. With the screen at 144 Hz it
can only help where the game renders well under 144 fps; above about 72 the present mode (mailbox) drops some.
Second try: name labels smeared in fast turns (interpolated - their pixels take the motion of what is behind them),
the console and readout went under the game's interface and were interpolated too (drawn into the frame at
present). Now one overlay goes to the swapchain: the labels (a layer of their own, drawn as the interface is), the
interface, the console - with the SDK's double buffering, as it lays the overlay over a generated frame while the
next is being drawn. Screenshots and the game's Lock read the frame with the labels and interface laid back over
(`FgComposedFrame`). Staged `randy31-framegen3.dll`. The overlay first kept the interface pass's alpha (0): the
interface came out brighter and see-through - its own pass (blend 4) since (`randy31-framegen4.dll`). The mouse
pointer is drawn on the frame after the interface: it went under the overlay. 2D draws on the frame after the
interface go into a top layer, over the console (`FgTopBegin`; demo `--shadow-test --ui-window`; staged
`randy31-framegen5.dll`).

The user asked for FSR 3.1 after FSR 2.2 went in (`RVK_Upscaler` 1, `rvk/fsr2.cpp`, `third_party/fsr2`). FSR 3.1 is
two things:
- the 3.1 upscaler (`fsr3upscaler`): FSR 2's successor, less ghosting and steadier thin detail;
- frame generation: a frame interpolated between each two rendered ones from optical flow and the upscaler's
  inputs, presented between them by AMD's frame-interpolation swapchain, which paces them and lays the interface over
  both.

AMD's Vulkan source for it is the FidelityFX SDK 1.1.x (MIT; v1.1.4 used). Later SDKs ship DirectX 12 DLLs only.

## How the SDK fits

- Components: `fsr3` (ties the others together), `fsr3upscaler`, `opticalflow`, `frameinterpolation`; the shared
  Vulkan backend `backends/vk/ffx_vk.cpp`; the swapchain `backends/vk/FrameInterpolationSwapchain/*`.
- Shaders: GLSL under `include/FidelityFX/gpu/<component>`, compiled by AMD's Windows `FidelityFX_SC.exe` into
  `*_permutations.h` headers that AMD's blob accessors (`backends/shared/blob_accessors`) include. We generate those
  headers with glslc + spirv-cross (as `tools/gen_fsr2_shaders.py` does for FSR 2): only the variants our inputs
  need; the SDK's wave64 and 16-bit names point at the same tables.
- 32-bit: the same fixes as FSR 2 will be needed (64-bit Vulkan handles held in `void*`, `nullptr` for handles,
  Wine's placed memory maps). Each patch listed in `third_party/ffx-sdk/README.md`.

## Phases

### A. The SDK in the build, the 3.1 upscaler (`RVK_Upscaler` 2)

- Vendor what the three components and the Vulkan backend need into `third_party/ffx-sdk`.
- Generator for their permutation headers; build it into the `rvk` library.
- `rvk/fsr3.cpp`: the upscaler alone (the `fsr3` context with `FFX_FSR3_ENABLE_UPSCALING_ONLY`), fed as FSR 2 is
  (`rvk/fsr2.cpp`: motion fill, reactive view, half-pixel shift).
- Compare 2.2 and 3.1 in the demo (forest 75/50, walker, storm); validation; stage.
- If 3.1 is as good or better, FSR 2.2 can go later (one AMD library less).

### B. Frame generation (`RVK_FrameGen`, off by default)

- Device: more queues in the graphics family - a present queue the engine never uses, an image-acquire queue
  (NVIDIA's family 0 has 16); check what Wine's winevulkan gives a 32-bit process.
- Swapchain: `ffxReplaceSwapchainForFrameinterpolationVK` in place of our `vkCreateSwapchainKHR`; its replacement
  functions for acquire / present / images / destroy (`ffxGetSwapchainReplacementFunctionsVK`).
- The interface: our interface layer (`m_uiLayer`, premultiplied) registered as the UI texture
  (`ffxRegisterFrameinterpolationUiResourceVK`), so the interface isn't interpolated (no warped text); the console
  and the performance readout with it. Name labels stay in the scene (interpolated, as 3D text in the world).
- Frame generation configured each frame (`ffxFsr3ConfigureFrameGeneration`: the swapchain, enabled, the
  HUD-less colour = our frame before the interface).
- Pacing and latency: a frame of latency; the readout shows the displayed rate.
- Risks: Wine's present path and the swapchain's own present thread; a 32-bit process's queues; frame pacing under
  Hyprland / X11. A setting to switch it off, off by default until the user has tried it.

### C. Clean-up

- Drop FSR 2.2 if 3.1 wins; the changelog, `LICENSE-FSR2.txt` -> the SDK's licence in the release list.
