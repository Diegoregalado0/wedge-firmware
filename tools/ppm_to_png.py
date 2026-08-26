#!/usr/bin/env python3
"""Convert the headless renderer's PPM output to PNG, and tile it into one
contact sheet so a set of hours can be judged side by side."""

import glob
import os
import sys

from PIL import Image

d = sys.argv[1] if len(sys.argv) > 1 else "."
paths = sorted(p for p in glob.glob(os.path.join(d, "*.ppm")) if "motion-" not in p and "boot-" not in p)
images = []
for p in paths:
    im = Image.open(p).convert("RGB")
    im.save(p.replace(".ppm", ".png"))
    os.remove(p)
    images.append((os.path.basename(p)[:-4], im))

if images:
    w, h = images[0][1].size
    gap = 14
    sheet = Image.new("RGB", (w + gap * 2, (h + gap) * len(images) + gap), (16, 16, 18))
    for i, (_, im) in enumerate(images):
        sheet.paste(im, (gap, gap + i * (h + gap)))
    sheet.save(os.path.join(d, "contact-sheet.png"))
    print("  contact sheet: %s" % os.path.join(d, "contact-sheet.png"))


# The motion capture becomes a GIF, which is the only honest way to review a
# spring: a still cannot show whether it overshoots or where it settles.
motion = sorted(glob.glob(os.path.join(d, "motion-*.ppm")))
if motion:
    frames = [Image.open(p).convert("RGB") for p in motion]
    for p in motion:
        os.remove(p)
    out = os.path.join(d, "motion.gif")
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=33, loop=0,
                   optimize=True)
    print("  motion: %s (%d frames)" % (out, len(frames)))


# Boot is a sequence, tiled so the ramp and the caption crossfade can be read.
boots = sorted(glob.glob(os.path.join(d, "boot-*.ppm")))
if boots:
    frames = [Image.open(p).convert("RGB") for p in boots]
    for p in boots:
        os.remove(p)
    w, h = frames[0].size
    sc = 0.62
    w2, h2 = int(w * sc), int(h * sc)
    gap = 6
    rows = (len(frames) + 1) // 2
    sheet = Image.new("RGB", (2 * (w2 + gap) + gap, rows * (h2 + gap) + gap), (14, 14, 16))
    for i, f in enumerate(frames):
        sheet.paste(f.resize((w2, h2), Image.LANCZOS),
                    (gap + (i % 2) * (w2 + gap), gap + (i // 2) * (h2 + gap)))
    sheet.save(os.path.join(d, "boot.png"))
    print("  boot: %s (%d frames)" % (os.path.join(d, "boot.png"), len(frames)))
