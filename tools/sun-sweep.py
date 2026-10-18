#!/usr/bin/env python3
"""The dusk sweep's shots (rvk_demo --sun-sweep A B N: sweep_NNN.bmp) as a contact sheet, and where they jump.

Usage: tools/sun-sweep.py <out-dir> [from-degrees to-degrees]

Writes <out-dir>/sweep-sheet.png (the shots in order, each labelled with its sun height when the range is given) and
prints each shot's mean colour of the sky (upper third) and the land (lower third). A step between neighbours bigger
than 3x the median step (in brightness or in colour) is flagged: day to night should go by in even steps
(docs/dusk-plan.md).
"""
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

LUMA = np.array([0.3, 0.59, 0.11])


def main():
    out = sys.argv[1]
    shots = sorted(glob.glob(os.path.join(out, "sweep_*.bmp")))
    if not shots:
        sys.exit("no sweep_*.bmp in " + out)
    span = (float(sys.argv[2]), float(sys.argv[3])) if len(sys.argv) > 3 else None
    images = [Image.open(p).convert("RGB") for p in shots]
    rows = []
    for im in images:
        a = np.asarray(im, dtype=np.float32) / 255.0
        h = a.shape[0]
        sky, land = a[: h // 3].reshape(-1, 3).mean(0), a[2 * h // 3:].reshape(-1, 3).mean(0)
        rows.append((sky, land))

    def height(i):
        if not span:
            return float(i)
        return span[0] + (span[1] - span[0]) * i / max(len(shots) - 1, 1)

    print(" shot  sun   sky rgb (luma)            land rgb (luma)")
    for i, (sky, land) in enumerate(rows):
        print(f"{i:4d} {height(i):6.1f}  {sky[0]:.3f} {sky[1]:.3f} {sky[2]:.3f} ({sky @ LUMA:.3f})"
              f"   {land[0]:.3f} {land[1]:.3f} {land[2]:.3f} ({land @ LUMA:.3f})")
    flagged = 0
    for part, name in ((0, "sky"), (1, "land")):
        cols = np.array([r[part] for r in rows])
        luma = cols @ LUMA
        dl = np.abs(np.diff(luma))
        chroma = cols / np.maximum(luma[:, None], 1e-3)
        dc = np.linalg.norm(np.diff(chroma, axis=0), axis=1)
        for steps, what in ((dl, "brightness"), (dc, "colour")):
            med = max(float(np.median(steps)), 0.002 if what == "brightness" else 0.01)
            for i, d in enumerate(steps):
                if d > 3.0 * med:
                    flagged += 1
                    print(f"JUMP {name} {what}: shot {i} -> {i + 1} (sun {height(i):.1f} -> {height(i + 1):.1f}): "
                          f"{d:.3f}, {d / med:.1f}x the median step")
    if not flagged:
        print("no jumps")

    # The sheet: up to 8 across, each shot at 320 wide.
    w = 320
    scale = w / images[0].width
    th = int(images[0].height * scale)
    across = min(8, len(images))
    down = (len(images) + across - 1) // across
    sheet = Image.new("RGB", (across * w, down * th), (0, 0, 0))
    draw = ImageDraw.Draw(sheet)
    for i, im in enumerate(images):
        x, y = (i % across) * w, (i // across) * th
        sheet.paste(im.resize((w, th)), (x, y))
        draw.text((x + 4, y + 4), f"{height(i):.1f}" if span else str(i), fill=(255, 255, 255))
    sheet.save(os.path.join(out, "sweep-sheet.png"))
    print("wrote", os.path.join(out, "sweep-sheet.png"))


if __name__ == "__main__":
    main()
