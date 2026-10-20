#!/usr/bin/env python3
"""The look check: a few fixed rvk_demo scenes, each shot's brightness by region compared with stored numbers.

Usage: tools/look-check.py [--update] [scene ...]      (default: every scene; output in build/look-check/<scene>)

Each shot is reduced to the mean colour of its top, middle and bottom thirds. A region whose luma or colour moved
more than the tolerance from tests/look-baseline.tsv is reported, and the exit code is 1. Single pixels differ run
to run by up to ~60/255 (the water, dithering), but these means repeat to about 0.002, so a change of 0.006 is a
real one. --update writes the current numbers as the baseline (after a change to the look that was meant).
"""
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tests", "look-baseline.tsv")
TOLERANCE = 0.006
GROUND = os.path.join(ROOT, "build", "pf790.gnd")

GRADE = ["--grade", "1", "1", "0.75", "0.25", "0"]
# The sky's clock pinned: it runs on the wall clock (rvk sky.cpp SkyClock) and drives the cloud cover, the stars and
# the auroras over hours - unpinned, the night scenes drifted with the time of day.
CLOCK = ["--sky-time", "1000"]
SCENES = {
    # Day to night over a lake, our own sky (the darkness, the dusk's phases, the sky's adaptation).
    "dusk": ["--water-lake", "--sky-dome", "8FB0CC", "--proc-sky", "1", "--atmos", "1", "--fixed-clock", "33",
             "--sun-sweep", "6", "-22", "15", "--sweep-hold", "100", "--darkness", "0.5", *GRADE],
    # The same lake at night, at the darkness's ends and default (the floor's gain).
    "night0": ["--water-lake", "--water-night", "--sky-dome", "101828", "--proc-sky", "1", "--atmos", "1",
               "--fixed-clock", "33", "--sun-dir", "0.3", "0.6", "0.7", "--frames", "400", "--darkness", "0", *GRADE],
    "night05": ["--water-lake", "--water-night", "--sky-dome", "101828", "--proc-sky", "1", "--atmos", "1",
                "--fixed-clock", "33", "--sun-dir", "0.3", "0.6", "0.7", "--frames", "400", "--darkness", "0.5", *GRADE],
    "night2": ["--water-lake", "--water-night", "--sky-dome", "101828", "--proc-sky", "1", "--atmos", "1",
               "--fixed-clock", "33", "--sun-dir", "0.3", "0.6", "0.7", "--frames", "400", "--darkness", "2", *GRADE],
    # A thunderstorm by day under the cloud layer.
    "storm": ["--water-lake", "--sky-dome", "8FB0CC", "--proc-sky", "1", "--atmos", "1", "--clouds", "--storm", "1",
              "--fixed-clock", "33", "--frames", "400", *GRADE],
    # The native ground at Stret West Bank (build/pf790.gnd: tools/ground-data.py 790 --export), gravel and a cliff.
    "ground": ["--native-ground-file", GROUND, "--pixel-lighting", "--eye", "1320.5", "34", "2913", "--view-yaw", "1.0",
               "--view-pitch", "0.45", "--fixed-clock", "33", "--frames", "800"],
}
LUMA = np.array([0.3, 0.59, 0.11])


def measure(out):
    """Each shot's regions: {(shot, region): (r, g, b)}."""
    rows = {}
    for path in sorted(glob.glob(os.path.join(out, "*.bmp"))):
        a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
        h = a.shape[0]
        for name, part in (("top", a[: h // 3]), ("mid", a[h // 3: 2 * h // 3]), ("low", a[2 * h // 3:])):
            rows[(os.path.basename(path), name)] = tuple(float(x) for x in part.reshape(-1, 3).mean(0))
    return rows


def run(scene):
    out = os.path.join(ROOT, "build", "look-check", scene)
    os.makedirs(out, exist_ok=True)
    for old in glob.glob(os.path.join(out, "*.bmp")):
        os.remove(old)
    if scene == "ground" and not os.path.exists(GROUND):
        print(f"{scene}: skipped ({GROUND} missing: tools/ground-data.py 790 --export {GROUND})")
        return None
    subprocess.run([os.path.join(ROOT, "tools", "rvk-demo.sh"), out, *SCENES[scene], *CLOCK], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=False)
    log = os.path.join(out, "validation.log")
    if os.path.exists(log):
        bad = sorted({l.split("|")[0].strip() for l in open(log, errors="ignore")
                      if l.startswith("Validation ") and "vkDestroyDevice" not in l})   # (the demo's known leaks)
        if bad:
            print(f"{scene}: validation: {'; '.join(bad)} (see {log})")
    return measure(out)


def load():
    base = {}
    if os.path.exists(BASELINE):
        for line in open(BASELINE):
            if line.startswith("#") or not line.strip():
                continue
            scene, shot, region, r, g, b = line.split("\t")
            base[(scene, shot, region)] = (float(r), float(g), float(b))
    return base


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    update = "--update" in sys.argv
    scenes = args or list(SCENES)
    base = load()
    failed = False
    for scene in scenes:
        rows = run(scene)
        if rows is None:
            continue
        if not rows:
            print(f"{scene}: no shots")
            failed = True
            continue
        moved = []
        for (shot, region), rgb in rows.items():
            key = (scene, shot, region)
            if update:
                base[key] = rgb
                continue
            if key not in base:
                moved.append(f"  {shot} {region}: no baseline")
                continue
            was = np.array(base[key])
            now = np.array(rgb)
            dl = float(now @ LUMA - was @ LUMA)
            dc = float(np.abs(now - was).max())
            if abs(dl) > TOLERANCE or dc > 2 * TOLERANCE:
                moved.append(f"  {shot} {region}: luma {was @ LUMA:.3f} -> {now @ LUMA:.3f} ({dl:+.3f}), "
                             f"colour {dc:.3f} off")
        if update:
            print(f"{scene}: {len(rows)} regions stored")
        elif moved:
            failed = True
            print(f"{scene}: {len(moved)} of {len(rows)} regions moved")
            print("\n".join(moved[:20]))
        else:
            print(f"{scene}: as before ({len(rows)} regions)")
    if update:
        with open(BASELINE, "w") as f:
            f.write("# tools/look-check.py --update: scene, shot, region, mean r g b (0..1)\n")
            for (scene, shot, region), (r, g, b) in sorted(base.items()):
                f.write(f"{scene}\t{shot}\t{region}\t{r:.4f}\t{g:.4f}\t{b:.4f}\n")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
