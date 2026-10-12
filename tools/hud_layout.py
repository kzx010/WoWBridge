#!/usr/bin/env python3
"""Layout check: wowcore's frame rectangles (hud_render's <state>_frames.txt) against the real client's
(/wbo frames -> evidence/hudref/frames.lua), by name, in UI units (left, bottom, right, top), and what each region
shows: a font string's font, height and text, a texture's file.

usage: hud_layout.py [STATE ...] [--all]   (--all lists every difference, not just the worst 25 per state)
"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FRAMES, OUT = ROOT / 'evidence/hudref/frames.lua', ROOT / 'build/hud'
STATES = ['hud_idle', 'hud_target_debuffs', 'hud_casting', 'hud_tooltip', 'spellbook', 'game_menu', 'key_bindings']


def client() -> dict[str, dict[str, tuple]]:
    """{state: {name: (kind, left, bottom, right, top, looks)}} for named frames and regions the client showed."""
    text = FRAMES.read_text(encoding='latin-1')
    out, cur = {}, None
    for line in text.splitlines():
        if m := re.match(r'\t\["frames_(\w+)"\] = \{', line):
            cur = out.setdefault(m[1], {})
        elif cur is not None and (m := re.match(r'\t\t\[\d+\] = "(.*)",$', line)):
            f = m[1].replace('\\\\', '\\').split('|')
            if f[0] in 'FR' and len(f) >= 6 and '?' not in f[1] and ':' not in f[1]:
                cur[f[1]] = (f[0], *map(float, f[2:6]), '|'.join(f[6:]) if f[0] == 'R' else '')
    return out


def ours(state: str) -> dict[str, tuple]:
    out = {}
    for line in (OUT / f'{state}_frames.txt').read_text().splitlines():
        f = line.split('|')
        out[f[0 + 1]] = (f[0], *map(float, f[2:6]), '|'.join(f[6:]) if f[0] == 'R' else '')
    return out




def main(argv: list[str]) -> None:
    show_all = '--all' in argv
    states = [a for a in argv if not a.startswith('--')] or STATES
    ref = client()
    for state in states:
        mine, theirs = ours(state), ref.get(state, {})
        missing = sorted(n for n in theirs if n not in mine)
        extra = sorted(n for n in mine if n not in theirs)
        diffs = []
        looks = []
        for n in theirs.keys() & mine.keys():
            d = max(abs(a - b) for a, b in zip(theirs[n][1:5], mine[n][1:5]))
            if d > 0.5:
                diffs.append((d, n, theirs[n][1:5], mine[n][1:5]))
            t, o = theirs[n][5].split('|'), mine[n][5].split('|')
            if t[0] == 'FontString' and len(o) >= 6 and (t[1].lower() != o[1].lower() or abs(float(t[2]) - float(o[2])) > 0.01 or t[5] != o[5]):
                looks.append(f'{n}: client {t[1]} {t[2]} {t[3]!r} {t[5]!r} / ours {o[1]} {float(o[2]):.2f} {o[5]!r}')
            elif t[0] == 'Texture' and len(o) >= 2 and t[1].lower().replace('.blp', '') != o[1].lower().replace('.blp', ''):
                looks.append(f'{n}: client {t[1]!r} / ours {o[1]!r}')
        diffs.sort(reverse=True)
        same = len(theirs.keys() & mine.keys()) - len(diffs)
        print(f'{state}: {same} of {len(theirs)} client rectangles match within 0.5; {len(diffs)} differ, '
              f'{len(missing)} not shown by us, {len(extra)} shown only by us')
        for d, n, t, m in diffs if show_all else diffs[:25]:
            print(f'   {d:7.2f} {n:40} client {t}  ours {m}')
        for line in looks if show_all else looks[:25]:
            print('   looks', line)
        if missing:
            print('   not shown by us:', ' '.join(missing if show_all else missing[:40]))
        if extra:
            print('   shown only by us:', ' '.join(extra if show_all else extra[:40]))


if __name__ == '__main__':
    main(sys.argv[1:])
