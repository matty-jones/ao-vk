# Scaled font baseline and line-height correction

2026-10-28, `ui-separation-scaling`.

The remaining 4K report described raised `p`/`y`, sliced glyph bottoms, and inconsistent spacing in settings,
Friends, chat, NCU and shortcut numbers. Icons and scale round trips were reported working.

## Cause and correction

The previous rasterizer cloned the font at an independently rounded character height, then moved each glyph to
fit the old line cell. Physical font metrics could exceed the available cell. Letters with descenders therefore
moved differently from capitals. Removing that clamp alone still lost final antialiased rows at fractional scales.

The corrected rasterizer chooses the physical font's cell height from the scaled logical cell, rounding down to
fit the displayed line. It uses that font's shared ascent as the baseline and retains the glyph's actual bearings.
All letters use the same metrics; no individual vertical clamp remains. The private bitmap's existing rounded-up
dimensions provide the remaining fractional space. Logical line height, wrapping, selection and view rectangles
stay unchanged.

One horizontal outline transform matches the original font's average cell width. TrueType measurement follows
the original font's character extents, overhang and native glyph-width limit. Bitmap measurement reads existing
glyph cells from the checked client's VC10 tree, without creating missing native glyphs. Inline bitmap padding
is excluded from the letter-width fit, so that padding remains spacing rather than stretching the letters.
The native renderer still chooses each character position and advance. This preserves its spacing rules;
it does not introduce a different shaping or wrapping engine.

The font cache includes its native owner and overhang. A checked `FontInfo_t` destructor hook releases its scaled
caches before the owner is destroyed. Spaces and other empty outlines are cached without allocating temporary
bitmap fallbacks. Measurement runs on cache creation, not for every glyph or every frame. Font and glyph cache
limits remain unchanged. The log records at most 16 cache creations per process, with face, source type, logical
cell, scale, raster metrics and horizontal pitch.

The original path is retained at 100% and with `[Native] UiScaling=off`. World labels, floating text, cursor and
developer console/readout do not enter this window/HUD font cache. There are no changes to their dimensions,
the atlas-edge shader, or client object layouts.

## Validation

The prepared client's real GUI.dll runs `native_check --gui-client-check <client-directory>`. The extended checks
cover 6/8/9/12/16-pixel fonts, positive cell heights and negative character heights, regular/bold/italic faces,
and 125/150/176/200% raster density. They verify glyph bearings and common baselines against independent GDI
outline measurements, complete coverage on consecutive lines, and full-string pixel placement on a tight line.
Strings include `NCU`, digits, `AV To Wa` and `gjpqy`. Bitmap-font fixtures with a complete Latin/digit width table
exercise font-wide fitting in tight HUD cells; inline TrueType icons retain their source pixels.

These placement tests strengthen the previous total-coverage-only check, which could pass after a glyph moved.
The existing exact 100% round trip, native advances, source/destination surface dimensions, selection/underline,
layout, hover, dragging and tooltip checks remain in the same suite. The scaling-disabled client check and the
pure coordinate checks remain separate regressions. `rvk_demo --ui-atlas-test` checks the icon sampling path;
the software-Vulkan validation run retains the known device-extension dependency error documented in the
[interface validation notes](ui-separation-scaling.md).

Local 4K gameplay review confirmed clean text without clipping after this correction. Native and renderer
fixtures do not establish the result of every skin/control combination. Browser-rendered text and other
external images continue to use their own rasterizers.

## Texture storage follow-up

A later NPC capture showed the complete clipped sentence in its CPU bitmap. The native shared atlas can reuse a
one-bit-alpha pool for an RGBA request, discarding antialiased coverage after rasterization. Enlarged text now uses
an independent RGBA surface, retaining its alpha and avoiding internal tile seams. Font metrics, character
placement and 100% rendering are unchanged. The native allocator reproducer, cache/upload checks and GPU alpha-row
probe are described in the [scaling coverage audit](ui-scaling-coverage.md).
