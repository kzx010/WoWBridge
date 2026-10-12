#!/usr/bin/env python3
"""Render selected Skyrim->WoW mappings with wowcore; PNG bulk stays in build/."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import textwrap

ROOT = Path(__file__).resolve().parent.parent
FAMILIES = ('iron', 'steel', 'leather', 'fur', 'elven', 'dwarven', 'orcish', 'glass',
            'ebony', 'daedric', 'nordic', 'stahlrim', 'bonemold', 'chitin', 'robes', 'nightingale')
WORN_SLOTS = {'head': 0, 'shoulders': 2, 'chest': 4, 'feet': 7, 'wrists': 8, 'hands': 9, 'back': 14}


def representative(rows):
    """Prefer actual named base gear to enchanted copies, skins, tests or summons."""
    def rank(row):
        edid = row['edid'].casefold()
        unusual = sum(word in edid for word in ('ench', 'skin', 'test', 'dummy', 'dremora', 'summon', 'nonplayable'))
        return unusual, len(row['edid']), row['plugin'].casefold(), row['local_form_id']
    return min(rows, key=rank)


def samples(catalog, families):
    rows = [dict(row, key=key) for key, row in catalog['items'].items()]
    result = []
    for family in families:
        armor = [r for r in rows if r['record_type'] == 'ARMO' and r['family'] == family]
        if not any(r['kind'] == 'chest' for r in armor):
            continue
        outfit = [representative([r for r in armor if r['kind'] == kind])
                  for kind in ('head', 'chest', 'hands', 'feet') if any(r['kind'] == kind for r in armor)]
        for yaw, view in ((25, 'front'), (180, 'back')):
            result.append({'label': f'{family} / {view}', 'rows': outfit, 'mode': 'character', 'yaw': yaw})
    weapons = [r for r in rows if r['record_type'] == 'WEAP']
    # One sword per family plus each weapon kind: material comparison and silhouettes.
    selected = {r['key']: r for family in families
                if (pool := [r for r in weapons if r['family'] == family and r['kind'] == 'sword'])
                for r in [representative(pool)]}
    for kind in ('axe', 'mace', 'dagger', 'greatsword', 'battleaxe', 'warhammer', 'bow', 'staff', 'crossbow'):
        pool = [r for r in weapons if r['kind'] == kind and r['family'] in ('iron', 'steel', 'wood', 'generic')]
        pool = pool or [r for r in weapons if r['kind'] == kind]
        if pool:
            r = representative(pool)
            selected[r['key']] = r
    for r in selected.values():
        result.append({'label': f"{r['family']} / {r['kind']}", 'rows': [r], 'mode': 'item', 'yaw': 90})
    for family in ('iron', 'elven', 'dwarven', 'glass', 'ebony', 'daedric'):
        pool = [r for r in rows if r['family'] == family and r['kind'] == 'shield']
        if pool:
            result.append({'label': f'{family} / shield', 'rows': [representative(pool)], 'mode': 'item', 'yaw': 25})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, default=ROOT / 'data/skyrim_gear_looks.json')
    parser.add_argument('--renderer', type=Path, default=ROOT / 'build/wowcore/model_render')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/skyrim_gear_looks')
    parser.add_argument('--families', nargs='+', help='Default: the represented families from FAMILIES.')
    parser.add_argument('--size', type=int, default=320)
    args = parser.parse_args()
    if args.size < 64:
        parser.error('--size must be at least 64')
    if not args.renderer.is_file():
        parser.error(f'missing renderer {args.renderer}; build wowcore model_render first')
    from PIL import Image, ImageDraw, ImageFont
    catalog = json.loads(args.catalog.read_text())
    known = catalog['counts']['families']
    if args.families is None:
        args.families = [f for f in FAMILIES if f in known]
    unknown = set(args.families) - set(known)
    if unknown:
        parser.error(f'unknown/unrepresented families: {sorted(unknown)}')
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = []
    for index, sample in enumerate(samples(catalog, args.families)):
        out = args.out / f'{index:03d}.png'
        command = [str(args.renderer.resolve()), str(out.resolve()), '--size', str(args.size),
                   '--yaw', str(sample['yaw']), '--light', '0.65,0.65']
        if sample['mode'] == 'item':
            row = sample['rows'][0]
            command += ['--item', str(row['wow_entry']), '--item-display', str(row['display_id']),
                        '--item-type', str(row['inventory_type'])]
        else:
            for row in sample['rows']:
                command += ['--worn', f"{WORN_SLOTS[row['kind']]}:{row['display_id']}:{row['sheath']}"]
        run = subprocess.run(command, capture_output=True, text=True)
        entry = dict(label=sample['label'], mode=sample['mode'], image=out.name,
                     forms=[f"{r['plugin']}:{r['local_form_id']:06X}" for r in sample['rows']],
                     displays=[r['display_id'] for r in sample['rows']],
                     wow_names=[catalog['wow_items'][str(r['wow_entry'])]['name'] for r in sample['rows']],
                     command=command, returncode=run.returncode, log=run.stdout + run.stderr)
        manifest.append(entry)
        print(f"{index:03d} {sample['label']}: {'OK' if run.returncode == 0 else 'FAILED'}", flush=True)
    # Per-page labels preserve the identity and display IDs a reviewer needs.
    font = ImageFont.load_default(size=14)
    columns, per_page, label_height = 4, 16, 128
    sheets = []
    for mode in ('character', 'item'):
        entries = [e for e in manifest if e['mode'] == mode]
        for page, start in enumerate(range(0, len(entries), per_page), 1):
            batch = entries[start:start + per_page]
            height = ((len(batch) + columns - 1) // columns) * (args.size + label_height)
            sheet = Image.new('RGB', (columns * args.size, height), (28, 30, 36))
            draw = ImageDraw.Draw(sheet)
            for i, entry in enumerate(batch):
                x, y = (i % columns) * args.size, (i // columns) * (args.size + label_height)
                if entry['returncode'] == 0:
                    with Image.open(args.out / entry['image']) as tile:
                        sheet.paste(tile.convert('RGB'), (x, y))
                else:
                    draw.text((x + 8, y + 8), 'RENDER FAILED; see manifest', fill='red', font=font)
                label = [entry['label'], 'displays ' + ', '.join(map(str, entry['displays']))]
                label += textwrap.wrap('; '.join(entry['forms']), width=38)
                draw.multiline_text((x + 6, y + args.size + 4), '\n'.join(label), fill='white', font=font, spacing=3)
            filename = f'{mode}-{page:02d}.png'
            sheet.save(args.out / filename)
            sheets.append(filename)
    (args.out / 'manifest.json').write_text(json.dumps({'sheets': sheets, 'samples': manifest}, indent=2) + '\n')
    print(f'{len(manifest)} renders; {len(sheets)} sheets in {args.out}')
    return 1 if any(e['returncode'] for e in manifest) else 0


if __name__ == '__main__':
    sys.exit(main())
