# Old Athen native ground audit

## Quality restoration follow-up

The visibility correction exposed the original terrain, but the game-side optimizer was still forcing radius 8
and baking far textures at two texels per cell after decoding. Successful decoding does not guarantee native
ownership of every later pass. That reduction has been removed; native patch replacement remains enabled.
The game always keeps its requested F10 radius, multiplied by `RVK_GroundRange`, and its requested `RVK_GroundFar`
quality. Switching `groundon` cannot lower either one. This retains extra original tessellation and texture memory;
it avoids a render-thread feedback delay leaving the first fallback frame with reduced-quality geometry.

Requested radius is tracked per ground rather than globally. Range changes are applied on the game's update
thread even when the player has not crossed a terrain cell, and schedule the original tessellator at that ground's
current cell. Zoning clears the saved requests. The log reports requested and effective radius when they change.
No new client object fields or layouts are introduced.

`native_check --ground-client-check <client-directory>` invokes the supported client's actual `SetHQRadius` and
`Tesselate` exports. It checks F10 values 8/16/44/150, the original split thresholds, reversible range changes,
independent grounds, reused ground state and far-bake settings 8/16/32. The previous low-resolution bake override
is absent from the corrected build, so restarting with it discards the old process's reduced-quality cached textures.
The stationary case bypasses N3's engine-dependent update body while exercising the installed frame hook and
the actual DisplaySystem tessellator. It also verifies that a repeated unchanged F10 request does not restart work.
Full Old Athen movement, F10 interaction and CPU/memory measurements remain part of local gameplay review.

## Initial visibility investigation

2026-10-28, `ui-separation-scaling`.

With `groundon 1`, Old Athen pavement disappeared; `groundon 0` restored it, and `groundone 0` did not help.
The supplied frame recorded no FVF `0x212` draws despite successful decoding of 49 patches and 103 textures.

## Findings

`NativeGroundReplaces()` returned true for every terrain draw with the ground vertex format, even when its state
matched neither the base pass nor the multiplying lighting pass. That suppressed the original without drawing a
replacement. It also marked recognised passes complete before attempting native submission. If the frustum test
rejected every patch, all later original ground draws were still discarded.

Suppression happens before `DumpDraw()`. The supplied frame therefore cannot distinguish an unrecognised pass from
a recognised pass with no visible native patches. It contains the camera position, but not the view/projection
matrices or the suppressed draws' state. The new diagnostics capture those decisions before suppression.

An offscreen ground pass could also consume the frame's native base or lighting pass before the primary view.
Native patch draws inherited the caller's retained mesh slot, allowing patch bounds to be cached in the original
mesh's metadata. These are corrected alongside the unconditional suppression.

The old frustum calculation uses the expected row-vector `view * projection`, with Direct3D clip depth `0..w`.
No reversed-plane or matrix-order defect was established. Native visibility now uses the renderer's shared
`BoxInClip()` test with the existing one-unit bounds padding. Visibility is cached per frame and matrix.
An empty result preserves the original terrain rather than consuming its pass.

## Replacement rules

- Only a recognised base or multiplying lighting pass in the primary target can replace the game draw. Other
  targets, cameras and viewports preserve their draws.
- Native buffers, tile textures, the code texture and fallback textures must be ready, with at least one patch
  inside the frustum. Invalid data or a nonfinite camera matrix leaves the original path active.
- A pass is marked complete only when native terrain draw records are appended, after the draw filters. Clutter
  and pebbles do not count as terrain submissions. Lighting cannot take ownership without a native base.
- The one-pass optimization defers a base only after that readiness and visibility check. If the lighting pass
  never arrives, the base is flushed with its captured target, viewport and transforms. Native patch draws save
  and restore the caller's retained geometry state and use their own mesh metadata.

Native ground remains enabled. Fallback applies to the affected passes and frames; a later frame can use native
patches once they are drawable.

## Diagnostics

The normal renderer log records each decision reason once per loaded playfield. A requested frame dump contains
at most eight decision samples and one summary, so large original ground batches do not flood either output.
All-frustum-rejected and invalid-camera samples also include the grid scale and four clip-matrix rows.

Decision samples include `original preserved` or `original suppressed`, the FVF, lighting/blending/depth state,
stage-zero state, ready texture count, decoded and visible patch counts, tests, rejections and submissions.
The summary includes `candidates`, `base`, `light`, `unknown`, `originals`, `replaced`, `tested`,
`frustum-rejected` and `native-submissions`.

For example, `all-frustum-rejected; original preserved` with zero native submissions shows a culling fallback.
`unrecognised-pass; original preserved` identifies a pass the native renderer does not implement. A decoded patch
count alone does not prove that any native terrain reached a draw record.

## Validation

`rvk_demo --ground-fallback-test --size 320 200 --shot ground-fallback` checks rendered pixels and emits frame
dumps. Synthetic original terrain is green and native terrain blue, making accidental suppression detectable.
Cases cover disabled native ground, unlit blended and lit opaque unrecognised passes, complete frustum rejection,
an unavailable tile, successful native drawing, retained meshes after an offscreen pass, a second camera and a
second viewport. HDR frames cover ordinary two-pass drawing, deferred combined drawing, and deferred base flushing
after a target change without a lighting pass.

The 32-bit release DLL and 64-bit renderer harness build successfully. Pixel checks pass for the fallback and
native cases, with four native submissions in the valid synthetic view, eight in the two-pass HDR frame and four
in each deferred frame. Fully rejected and unavailable cases submit zero native patches and retain the original.
The prepared client's native GUI checks also pass, including Verdana text and item dragging.

The validation-layer run reports the existing device-extension dependency error
`VUID-vkCreateDevice-ppEnabledExtensionNames-01387` (`external_memory_dma_buf` without `external_memory_fd`). It
passes the terrain checks and reports no additional validation errors. This does not establish a completely
validation-clean device startup.

The prepared client's Old Athen archive (playfield 540) independently confirms 49 patches of 32 cells and 103 tile
textures, with four-unit cells. Its padded grid is 224 by 224 cells. Terrain height at the captured eye
`(461.92, 19.39, 404.28)` is 15 units. The frustum regression checks the 7-by-7 patch layout at that eye with a
representative forward camera. Client data and captured frames remain local and are not included in the repository.

A local export of that Old Athen data also renders visible pavement in the standalone harness at the captured eye,
with yaw `1.5708`, downward pitch `0.35`, HDR and pixel lighting enabled. Across two frames it exercises the
two-pass and combined paths. The final combined frame tests 49 patches, rejects 28 and submits 21, with all 103
textures ready. The file loader labels its test ground 790; the source archive is playfield 540.

The supplied capture cannot be replayed exactly because its camera matrices and suppressed pass state are absent.
Full Old Athen gameplay confirmation requires a fresh client run; successful renderer checks do not establish
which suppressed state occurred in the original capture.
