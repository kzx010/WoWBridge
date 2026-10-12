#!/usr/bin/env python3
"""M5 gate: wowcore's HUD (build/wowcore/hud_render) against the real client's screenshots (tools/hudref.py).

usage: hud_diff.py [STATE ...]       (renders first if build/hud/<state>.png is missing)

For each state our transparent render is composited over <state>_world.png (the client's frame with its interface
hidden, taken right after the reference) and compared with <state>.png where interface is: inside the rectangles of
the textures and font strings the client showed (evidence/hudref/frames.lua), and wherever we drew. Parts that are 3D
renders of the world rather than interface art (portraits, the minimap's map) are not compared; they belong to M8.
Writes build/hud/<state>_diff.png (reference | ours | error x4) and build/hud/report.json.
"""
import json, re, subprocess, sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
REF, OUT = ROOT / 'evidence/hudref', ROOT / 'build/hud'
STATES = ['hud_idle', 'hud_target_debuffs', 'hud_casting', 'hud_tooltip', 'spellbook', 'game_menu', 'key_bindings']
WORLD_RENDERED = {'PlayerPortrait', 'TargetPortrait', 'MicroButtonPortrait', 'Minimap'}   # 3D or world renders
TOLERANCE = 24      # max channel difference that still counts as the same pixel


def load(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert('RGBA'), dtype=np.int16)


def client_regions(ref_dir: Path, state: str) -> tuple[list, list]:
    """Pixel rectangles (x0, y0, x1, y1) of the regions the client showed, and of the world-rendered parts."""
    text = (ref_dir / 'frames.lua').read_text(encoding='latin-1')
    at = text.index(f'["frames_{state}"]')
    block = text[at:text.index('\n\t}', at)]
    rows = [r.split('|') for r in re.findall(r'\] = "(.*)",', block)]
    screen_w = float(rows[0][1])
    regions, world = [], []
    for r in rows[1:]:
        if len(r) < 6:
            continue
        l, b, rt, t = map(float, r[2:6])
        k = 1280 / screen_w
        box = (int(l * k), int((768 - t) * k * (720 / 720)), int(np.ceil(rt * k)), int(np.ceil((768 - b) * k)))
        if r[1] in WORLD_RENDERED:
            world.append(box)
        elif r[0] == 'R' and not (len(r) > 7 and r[6] == 'FontString' and not r[-1].strip('~ ')):
            regions.append(box)
    return regions, world


def compare(state: str) -> dict:
    ref, world, ours = load(REF / f'{state}.png'), load(REF / f'{state}_world.png'), load(OUT / f'{state}.png')
    a = ours[..., 3:4] / 255.0
    comp = (ours[..., :3] * a + world[..., :3] * (1 - a)).round().astype(np.int16)
    present = ours[..., 3] > 8
    regions, world_rendered = client_regions(REF, state)
    for x0, y0, x1, y1 in regions:
        present[max(0, y0):y1, max(0, x0):x1] = True
    for x0, y0, x1, y1 in world_rendered:
        present[max(0, y0):y1, max(0, x0):x1] = False
    err = np.abs(comp - ref[..., :3]).max(axis=2)
    n = int(present.sum())
    refui, oursui, bad = np.abs(ref[..., :3] - world[..., :3]).max(axis=2) > TOLERANCE, ours[..., 3] > 8, err > TOLERANCE
    cells = (bad & present).reshape(18, 40, 32, 40).sum(axis=(1, 3))
    worst = [(int(x) * 40, int(y) * 40, int(cells[y, x])) for y, x in zip(*np.unravel_index(np.argsort(-cells.ravel())[:6], cells.shape))]
    same = int(((err <= TOLERANCE) & present).sum())
    heat = np.zeros_like(ref[..., :3])
    heat[..., 0] = np.where(present, np.minimum(err * 4, 255), 0)
    heat[..., 1] = np.where(present & (err <= TOLERANCE), 60, 0)
    strip = np.concatenate([ref[..., :3], comp, heat], axis=1).astype(np.uint8)
    Image.fromarray(strip).save(OUT / f'{state}_diff.png')
    return {'pixels': n, 'match': round(same / n, 4) if n else 1.0,
            'mean_error': round(float(err[present].mean()), 2) if n else 0.0,
            'bad_missing': int((bad & present & refui & ~oursui).sum()),     # the client drew interface, we did not
            'bad_extra': int((bad & present & ~refui & oursui).sum()),       # we drew where the client has none
            'bad_both': int((bad & present & refui & oursui).sum()),         # both drew, differently
            'worst_cells_40px': worst}


def main(states: list[str]) -> None:
    states = states or STATES
    OUT.mkdir(parents=True, exist_ok=True)
    missing = [s for s in states if not (OUT / f'{s}.png').exists()]
    if missing:
        subprocess.run([ROOT / 'build/wowcore/hud_render', OUT, *missing], check=True)
    report = {s: compare(s) for s in states}
    for s, r in report.items():
        print(f'{s:20} {r["match"] * 100:6.2f}% of {r["pixels"]:7} interface pixels within {TOLERANCE}, '
              f'mean error {r["mean_error"]}; off: missing {r["bad_missing"]} extra {r["bad_extra"]} '
              f'both {r["bad_both"]}; worst {r["worst_cells_40px"][:4]}')
    (OUT / 'report.json').write_text(json.dumps(report, indent=1))


if __name__ == '__main__':
    main(sys.argv[1:])
