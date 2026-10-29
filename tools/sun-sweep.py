#!/usr/bin/env python3
"""The dusk sweep's shots (rvk_demo --sun-sweep A B N: sweep_NNN.bmp) as a contact sheet, and where they jump.

Usage: tools/sun-sweep.py <out-dir> [from-degrees to-degrees] [--csv FILE]

Writes <out-dir>/sweep-sheet.png (the shots in order, each labelled with its sun height when the range is given) and
prints each shot's mean colour of the sky (upper third) and the land (lower third). A step between neighbours bigger
than 3x the median step (in brightness or in colour) is flagged: day to night should go by in even steps
(docs/dusk-plan.md).

With rvk_demo --rig-props (rig-props.txt beside the shots: the screen rects of a static's front face and top and of
the ground beside it, all with the ground's texture), also each shot's statics-to-ground brightness ratio - the
lighting's, the albedo being the same - with their own step flags, and a flag when the ratio's spread over the sweep
(its largest over its smallest) is more than 1.5x: statics and the ground should take day and night alike
(docs/light-rig-plan.md). --csv writes every number per shot.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

LUMA = np.array([0.3, 0.59, 0.11])


def main():
    args = sys.argv[1:]
    csv = None
    if "--csv" in args:
        i = args.index("--csv")
        csv = args[i + 1]
        del args[i:i + 2]
    out = args[0]
    shots = sorted(glob.glob(os.path.join(out, "sweep_*.bmp")))
    if not shots:
        sys.exit("no sweep_*.bmp in " + out)
    span = (float(args[1]), float(args[2])) if len(args) > 2 else None
    rects = {}
    rect_file = os.path.join(out, "rig-props.txt")
    if os.path.exists(rect_file):
        for line in open(rect_file):
            name, *box = line.split()
            rects[name] = [float(v) for v in box]
    images = [Image.open(p).convert("RGB") for p in shots]
    rows, props = [], []
    for im in images:
        a = np.asarray(im, dtype=np.float32) / 255.0
        h, w = a.shape[:2]
        sky, land = a[: h // 3].reshape(-1, 3).mean(0), a[2 * h // 3:].reshape(-1, 3).mean(0)
        rows.append((sky, land))
        if rects:
            mean = {}
            for name, (x0, y0, x1, y1) in rects.items():
                part = a[int(y0 * h):max(int(y1 * h), int(y0 * h) + 1), int(x0 * w):max(int(x1 * w), int(x0 * w) + 1)]
                mean[name] = float(part.reshape(-1, 3).mean(0) @ LUMA)
            props.append(mean)

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
    if props:
        print(" shot  sun   face   top    ground  face/ground top/ground")
        ratios = {"face": [], "top": []}
        for i, m in enumerate(props):
            g = max(m["ground"], 1e-4)
            for k in ratios:
                ratios[k].append(m[k] / g)
            print(f"{i:4d} {height(i):6.1f}  {m['face']:.3f}  {m['top']:.3f}  {m['ground']:.3f}   "
                  f"{ratios['face'][-1]:.2f}        {ratios['top'][-1]:.2f}")
        for name in ("face", "top", "ground"):
            luma = np.array([m[name] for m in props])
            steps = np.abs(np.diff(luma))
            med = max(float(np.median(steps)), 0.002)
            for i, d in enumerate(steps):
                if d > 3.0 * med:
                    flagged += 1
                    print(f"JUMP {name} brightness: shot {i} -> {i + 1} (sun {height(i):.1f} -> {height(i + 1):.1f}): "
                          f"{d:.3f}, {d / med:.1f}x the median step")
        for k, r in ratios.items():
            r = np.array(r)
            spread = float(r.max() / max(r.min(), 1e-4))
            mark = "SPREAD" if spread > 1.5 else "spread"
            if spread > 1.5:
                flagged += 1
            print(f"{mark} {k}/ground: {r.min():.2f} .. {r.max():.2f} ({spread:.2f}x over the sweep)")
    if csv:
        with open(csv, "w") as f:
            f.write("shot,sun,sky_luma,land_luma" + (",face,top,ground" if props else "") + "\n")
            for i, (sky, land) in enumerate(rows):
                line = f"{i},{height(i):.2f},{sky @ LUMA:.4f},{land @ LUMA:.4f}"
                if props:
                    line += f",{props[i]['face']:.4f},{props[i]['top']:.4f},{props[i]['ground']:.4f}"
                f.write(line + "\n")
        print("wrote", csv)
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
