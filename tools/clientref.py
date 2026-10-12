#!/usr/bin/env python3
"""Capture the real local 1.12 client without desktop upscaling.

install                 install the read-only recorder into the existing reference client
record LABEL KIND PANEL capture the currently open panel, then a hidden-UI world-only pair
                        (PANEL=none for idle); save state/geometry and reject mislabeled panels
Output: evidence/clientref-20261010/LABEL/. Historical evidence is never overwritten.
Prepare the panel through wowref.py first. This tool never fakes client API results.
"""
import argparse
import json
import re
import shutil
import time
from pathlib import Path

import apiref
import wowref
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
CLIENT = ROOT.parent / 'WoW/client'
OUT = ROOT / 'evidence/clientref-20261010'
PANELS = ('SpellBookFrame', 'GameMenuFrame', 'CharacterFrame', 'MerchantFrame', 'LootFrame',
          'ClassTrainerFrame', 'TaxiFrame', 'QuestLogFrame', 'DressUpFrame', 'GossipFrame', 'ScriptErrors', 'StaticPopup1', 'StackSplitFrame')


def signatures():
    return {p: (p.stat().st_mtime_ns, p.stat().st_size) for p in (CLIENT / 'Screenshots').glob('*.tga')}


def save_native(before, destination):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        changed = [p for p, sig in signatures().items() if before.get(p) != sig]
        if len(changed) > 1:
            raise RuntimeError('multiple native screenshots arrived; preserve and inspect the client output')
        if changed:
            source = changed[0]
            try:
                with Image.open(source) as image:
                    image.load()
                    size = image.size
                    image.convert('RGB').save(destination.with_suffix('.png'))
                return {'source': str(source.relative_to(ROOT.parent)), 'size': size}
            except (OSError, ValueError):
                pass  # The client may still be writing the TGA.
        time.sleep(0.2)
    raise RuntimeError('no complete native screenshot arrived')


def validate(data, panel, kind, expected=None):
    issues = []
    if data.get('error'):
        issues.append('client recorder error: ' + data['error'])
    if data.get('UIParent:IsVisible()') not in ('1', 'true'):
        issues.append('client interface was hidden')
    if data.get('kind') != kind:
        issues.append('missing or mismatched capture metadata')
    if panel != 'none' and data.get('visible:' + panel) != 'true':
        issues.append('expected panel was not visible: ' + panel)
    for other in PANELS:
        quantity = (other == 'StackSplitFrame' and kind == 'merchant' and
                    (expected or {}).get('visible:StackSplitFrame') == 'true')
        if other != panel and not quantity and data.get('visible:' + other) == 'true':
            issues.append('unexpected visible panel: ' + other)
    count = {'merchant': 'GetMerchantNumItems()', 'buyback': 'GetNumBuybackItems()', 'trainer': 'GetNumTrainerServices()',
             'taxi': 'NumTaxiNodes()', 'loot': 'GetNumLootItems()', 'party': 'GetNumPartyMembers()'}.get(kind)
    if count:
        try:
            if float(data[count]) <= 0:
                issues.append('empty client content: ' + count)
        except (KeyError, ValueError):
            issues.append('missing client content: ' + count)
    if data.get('visible:StackSplitFrame') == 'true':
        try:
            split, maximum = int(data['StackSplitFrame.split']), int(data['StackSplitFrame.maxStack'])
            if not 1 <= split <= maximum or maximum < 2:
                issues.append('invalid native quantity selection')
        except (KeyError, ValueError):
            issues.append('missing native quantity selection')
        if not re.fullmatch(r'"MerchantItem[1-9][0-9]*ItemButton"', data.get('StackSplitFrame.owner:GetName()', '')):
            issues.append('quantity selection is not owned by a merchant item')
    if kind == 'character':
        try:
            float(data['CharacterModelFrame.rotation'])
        except (KeyError, ValueError):
            issues.append('missing native FrameXML model rotation')
    if kind == 'buff':
        try:
            if int(data['GetPlayerBuff(0,"HELPFUL")'].split(',')[0]) < 0:
                issues.append('no helpful buff was recorded')
        except (KeyError, ValueError):
            issues.append('missing native helpful-buff metadata')
    if kind == 'trainer':
        try:
            if int(data.get('GetTrainerSelectionIndex()', '0')) > int(data['GetNumTrainerServices()']):
                issues.append('selected trainer index is outside the recorded service list')
        except (KeyError, ValueError):
            issues.append('missing trainer selection/list metadata')
    for key, wanted in (expected or {}).items():
        if data.get(key) != wanted:
            issues.append(f'{key}: expected {wanted}, recorded {data.get(key)}')
    return issues


def record(label, kind, panel, expected=None, *, prepare=None, cleanup=None):
    # Arm the read-only recorder BEFORE opening a keyboard-consuming dialog or
    # holding a mouse button. Cleanup runs before any further chat commands.
    if (prepare is None) != (cleanup is None):
        raise ValueError('prepared captures require both prepare and cleanup')
    if not label.replace('_', '').isalnum():
        raise ValueError('label must contain only letters, digits and underscores')
    destination = OUT / label
    destination.mkdir(parents=True, exist_ok=False)
    # Metadata and native screenshot are taken by the same deferred client callback.
    before = signatures()
    delay = 5 if prepare else 0.5
    wowref.main([f'slash:/script WBFidelityCapture("{label}","{kind}","{panel}",{delay})'])
    try:
        if prepare:
            prepare()
        ui = save_native(before, destination / 'ui')
    finally:
        if cleanup:
            cleanup()
    time.sleep(1.1)  # 1.12 screenshot filenames have one-second resolution.
    before = signatures()
    # A client callback hides/restores UIParent automatically; no desktop screenshot shortcut.
    wowref.main([f'slash:/script WBFidelityWorld("{label}")', 'wait:2.5'])
    world = save_native(before, destination / 'world')
    wowref.main(['slash:/run ReloadUI()', 'wait:8'])
    # Retain only this capture's API/world/frame blocks, not accumulated unrelated experiments.
    raw = apiref.SV.read_bytes().replace(b'\r\n', b'\n').replace(b'\r', b'\\r').decode('latin-1')
    blocks = []
    for prefix in ('capture_', 'world_', 'frames_'):
        key = prefix + label
        match = re.search(r'^\t\["' + re.escape(key) + r'"\] = \{\n.*?^\t\},?$', raw, re.M | re.S)
        if match:
            blocks.append(match.group(0))
    (destination / 'state.lua').write_text('WBOracleAPI = {\n' + '\n'.join(blocks) + '\n}\n', encoding='latin-1')
    data = apiref.load(destination / 'state.lua').get('capture_' + label, {})
    issues = validate(data, panel, kind, expected)
    world_data = apiref.load(destination / 'state.lua').get('world_' + label, {})
    if world_data.get('UIParent:IsVisible()') != 'false':
        issues.append('world capture did not confirm hidden UI')
    if ui['size'] != world['size']:
        issues.append('UI/world dimensions differ')
    report = {'label': label, 'kind': kind, 'panel': panel, 'ui': ui, 'world': world,
              'metadata': data, 'expected': expected or {}, 'issues': issues,
              'note': 'Native client TGA converted losslessly to PNG; no resize or visual edits. World pair follows UI capture; live world animation can differ.'}
    (destination / 'capture.json').write_text(json.dumps(report, indent=2) + '\n')
    if issues:
        raise RuntimeError('; '.join(issues) + f'; evidence preserved in {destination}')
    print(f'{label}: real client {ui["size"]}, expected panel/content verified, paired world and frame/API state saved')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('install')
    rec = sub.add_parser('record')
    rec.add_argument('label')
    rec.add_argument('kind', choices=('idle', 'spellbook', 'merchant', 'buyback', 'trainer', 'taxi', 'loot', 'character', 'buff', 'party', 'nameplates'))
    rec.add_argument('panel', choices=(*PANELS, 'none'))
    rec.add_argument('--expect', nargs=2, action='append', default=[], metavar=('API_EXPRESSION', 'SERIALIZED_VALUE'), help='require an exact recorded API value, e.g. Train-button enable or page number')
    args = parser.parse_args()
    if args.command == 'install':
        shutil.copytree(ROOT / 'tools/addons/WBOracle', CLIENT / 'Interface/AddOns/WBOracle', dirs_exist_ok=True)
    else:
        record(args.label, args.kind, args.panel, dict(args.expect))


if __name__ == '__main__':
    main()
