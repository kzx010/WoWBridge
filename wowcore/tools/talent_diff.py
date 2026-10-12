#!/usr/bin/env python3
"""Crop and compare the non-reference TalentFrame capture; run with the project Python venv.

Usage: talent_diff.py REF_PNG OUR_2560x1440_PNG OUT_DIR
The supplied '*_1440.png' reference is actually 1280x720. Normalize our render
only for comparison; keep the original required 2560x1440 capture intact.
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# Rectangles in the supplied 1280x720 reference. Portrait/world pixels are excluded.
REGIONS = {
    'title': (67, 111, 307, 132),
    'spent_points': (70, 147, 310, 158),
    'tree_art_icons_branches': (23, 168, 300, 479),
    'scrollbar': (303, 167, 322, 483),
    'talent_points': (23, 484, 245, 501),
    'close_button': (251, 484, 321, 501),
    'tabs': (24, 507, 276, 531),
}
CROP = (0, 96, 337, 537)


def compare(reference: Path, rendered: Path, out: Path) -> dict:
    ref = Image.open(reference).convert('RGB')
    if ref.size != (1280, 720):
        raise ValueError('Expected the supplied 1280x720 talent reference')
    ours = Image.open(rendered).convert('RGBA')
    if ours.size != (2560, 1440):
        raise ValueError('Expected a 2560x1440 hud_render capture')
    ours = ours.resize(ref.size, Image.Resampling.LANCZOS).convert('RGB')
    a, b = np.asarray(ref, dtype=np.int16), np.asarray(ours, dtype=np.int16)
    error = np.abs(a-b).max(axis=2)
    report = {}
    for name, (x0, y0, x1, y1) in REGIONS.items():
        e = error[y0:y1, x0:x1]
        report[name] = {'match_within_24': round(float((e <= 24).mean()), 4),
                        'mean_max_channel_error': round(float(e.mean()), 2)}
    out.mkdir(parents=True, exist_ok=True)
    ref.crop(CROP).save(out / 'balance-reference-crop.png')
    ours.crop(CROP).save(out / 'balance-ours-crop.png')
    heat = np.zeros_like(a, dtype=np.uint8)
    heat[..., 0] = np.minimum(error * 4, 255)
    heat[..., 1] = np.where(error <= 24, 60, 0)
    images = [ref.crop(CROP), ours.crop(CROP), Image.fromarray(heat).crop(CROP)]
    strip = Image.new('RGB', (images[0].width * 3, images[0].height))
    for i, im in enumerate(images):
        strip.paste(im, (i * im.width, 0))
    strip.save(out / 'balance-comparison.png')
    (out / 'balance-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    print(json.dumps(compare(*map(Path, sys.argv[1:])), indent=2))
