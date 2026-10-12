#!/usr/bin/env python3
"""Compare explicit client captures at their stored resolution.

Usage: fidelity_diff.py BEFORE_DIR AFTER_DIR OUT_DIR
Render --states into those directories first; --states lists fixture groups.
Unlike hud_diff, references without world-only pairs use fixed UI ROIs.
These scores are diagnostics, not a claim that the complete frame matches.
"""
import json
import argparse
import re
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
CASES = {}


def case(name, ref, state, regions, size=(1280, 720), crop=None, world=None, frames=None):
    CASES[name] = dict(reference=ref, state=state, regions=regions, render_size=size,
                       crop=crop, world=world, frames=frames)


talent = {'title': (67,111,307,132), 'spent_points': (70,147,310,158),
          'tree': (23,168,300,479), 'scrollbar': (303,167,322,483),
          'points': (23,484,245,501), 'close': (251,484,321,501), 'tabs': (24,507,276,531)}
case('talents', 'talentref/talents_balance_1440.png', 'talents_balance', talent)
for side in ('front', 'back', 'left', 'right'):
    state = 'fidelity_character' + ('' if side == 'front' else '_' + side)
    case('character_' + side, 'charref/char_' + side + '.png', state,
         {'title': (130,215,642,265), 'left_slots': (32,333,110,948),
          'right_slots': (561,333,642,948), 'weapons': (213,913,462,997),
          'paper_doll_model': (128,333,561,736),
          'rotation_buttons': (128,340,246,400)}, (2560,1440))
case('buff_tooltip', 'buffref/motw_buff_tooltip.png', 'buff_motw',
     {'tooltip': (174,86,626,214)}, (2560,1440), (1536,0,2560,432))
case('buff_frames', 'buffref/motw_buff_frames.png', 'fidelity_buff_frames',
     {'player_bars': (166,42,391,128), 'target_bars': (476,42,701,128),
      'buff': (2160,18,2220,82)}, (2560,1440), (0,0,2560,172))
form_art = {'shapeshift': (190,637,339,678), 'main_bar': (38,674,1220,720),
            'player_bars': (82,26,195,64), 'target_bars': (238,26,351,64)}
for name, file, state in (
    ('cat','form_cat_1280','fidelity_cat'), ('bear','form_bear_1280','fidelity_bear'),
    ('prowl_before','prowl_before_1440','fidelity_prowl_before'),
    ('prowl','prowl_1440','fidelity_prowl'),
    ('aquatic_error','aquatic_on_land_1440','fidelity_aquatic_error')):
    regions = dict(form_art)
    if name in ('cat','bear'):
        regions['tooltip'] = (1025,465,1266,635)
    if name == 'aquatic_error':
        regions.pop('target_bars')
        regions['error'] = (520,110,760,136)
    if name in ('prowl_before','prowl'):
        regions.pop('target_bars')
    case(name, 'formref/' + file + '.png', state, regions)
for name, file, state in (
    ('level_start','level_up_start_1440','fidelity_level_start'),
    ('level_1s','level_up_1s_1440','fidelity_level_1s')):
    regions = dict(form_art)
    regions.pop('target_bars')
    case(name, 'levelref/' + file + '.png', state, regions)
for elapsed in ('0.10','0.40','0.75','1.10','1.35','1.50','1.60','1.80'):
    case('gcd_' + elapsed, 'hudref/gcd/gcd_' + elapsed + '.png', 'fidelity_gcd_' + elapsed,
         {'action_cooldowns': (164,681,406,720)})
for scale in ('080','100'):
    for state in ('hud_idle','spellbook','game_menu'):
        base = 'hudref/scale' + scale + '/'
        case(state + '_scale' + scale, base + state + '.png',
             'fidelity_' + state + '_scale' + scale, {},
             world=base + state + '_world.png', frames=(base + 'frames.lua', state))
# New clean client references are additional cases; retain every historical oracle.
for scale in ('080','100'):
    for state in ('hud_idle','spellbook'):
        label=state+'_scale'+scale
        base='clientref-20261010/'+label+'/'
        case('clientref_'+label,base+'ui.png','clientref_'+label,{},
             world=base+'world.png',frames=(base+'state.lua',label))
# Panel-only comparisons avoid claiming uncaptured NPC health, camera or portraits.
PANEL_CASES = set()
for label in ('merchant_page1','merchant_page2','merchant_buyback','loot_page1','loot_page2',
              'trainer_populated','trainer_known_verified','trainer_available_verified','taxi_map_populated'):
    base='clientref-20261010/'+label+'/'
    name='clientref_'+label
    case(name,base+'ui.png',name,{},world=base+'world.png',frames=(base+'state.lua',label))
    CASES[name]['include']=[(0,90,245,345) if label.startswith('loot_') else (0,90,366,590)]
    PANEL_CASES.add(name)
# Genuine native input states, isolated from the nine existing panel baselines.
MERCHANT_INTERACTION_CASES = set()
for label in ('merchant_hover_verified','merchant_pressed_verified',
              'merchant_quantity_min_verified','merchant_quantity_middle_verified','merchant_quantity_max_verified'):
    name='clientref_'+label
    base='clientref-20261010/'+label+'/'
    quantity=label.startswith('merchant_quantity_')
    regions=({'quantity':(22,82,184,173), 'decrement':(32,102,49,120),
              'increment':(162,102,180,120), 'selection':(106,103,140,120)} if quantity else
             {'item_button':(21,170,59,209), 'tooltip':(57,101,180,171)})
    case(name,base+'ui.png',name,regions,world=base+'world.png',frames=(base+'state.lua',label))
    CASES[name]['include']=list(regions.values())
    # These ROIs contain opaque foreground controls covering the NPC portrait.
    CASES[name]['exclude_portraits']=False
    # Gear-level requirements were deliberately removed in this host. The final
    # native requirement row / host durability row is outside this comparison.
    CASES[name]['excluded']=[] if quantity else [(58,150,181,171)]
    CASES[name]['required_frames']=['F|MerchantFrame|']+(['F|StackSplitFrame|'] if quantity else ['F|GameTooltip|'])
    CASES[name]['match_text_frames']=['StackSplitText'] if quantity else ['GameTooltipTextLeft1','GameTooltipTextLeft2','GameTooltipTextRight2','GameTooltipTextLeft3']
    MERCHANT_INTERACTION_CASES.add(name)
FONT_CASES={'clientref_font_preview_native'}
base='clientref-20261010/font_preview_native/'
case('clientref_font_preview_native',base+'ui.png','clientref_font_preview_native',{},
     world=base+'world.png',frames=(base+'state.lua','font_preview_native'))
CASES['clientref_font_preview_native']['include']=[(350,172,955,513)]
CASES['clientref_font_preview_native']['foreground_on_black']=True
CASES['hud_idle_scale080']['excluded'] = [(295,78,550,425), (515,87,820,200), (0,610,550,682)]

# Clean full-frame references; keep the approved host statistics adaptation.
CHARACTER_CASES, BUFF_CASES = set(), set()
character_regions = {
    'title': (44,113,327,127), 'left_slots': (16,165,56,472),
    'right_slots': (285,165,325,472), 'weapons': (112,457,231,495),
    'paper_doll_model': (61,203,280,381), 'rotation_buttons': (60,169,128,204),
}
for side in ('front','back','left','right'):
    label='character_'+side+'_verified'
    name='clientref_'+label
    base='clientref-20261010/'+label+'/'
    case(name,base+'ui.png',name,character_regions,world=base+'world.png',frames=(base+'state.lua',label))
    CASES[name]['include']=list(character_regions.values())
    # The host deliberately replaces these five resistance icons and values.
    CASES[name]['excluded']=[(247,168,281,307)]
    CASES[name]['match_text_frames']=['CharacterNameText']
    CASES[name]['required_frames']=['F|CharacterFrame|','F|CharacterModelFrame|']
    CHARACTER_CASES.add(name)
for label in ('buff_motw_native','buff_motw_tooltip_native'):
    name='clientref_'+label
    base='clientref-20261010/'+label+'/'
    regions={'player_bars':(80,18,202,69),'buff':(1080,9,1115,43)}
    if label=='buff_motw_tooltip_native': regions['tooltip']=(850,38,1084,112)
    case(name,base+'ui.png',name,regions,world=base+'world.png',frames=(base+'state.lua',label))
    CASES[name]['include']=list(regions.values())
    CASES[name]['required_frames']=['R|BuffButton0Icon|']+(['F|GameTooltip|'] if 'tooltip' in regions else [])
    CASES[name]['match_text_frames']=['GameTooltipTextLeft1','GameTooltipTextRight1','GameTooltipTextLeft2','GameTooltipTextLeft3'] if 'tooltip' in regions else []
    BUFF_CASES.add(name)

# These filenames do not represent the requested clean client state. Keep
# their original diagnostics available, but never use them as fidelity gates.
UNRELIABLE = {
    'hud_idle_scale080': 'open spellbook, removed Honor panel, debug/chat and tester target; not clean idle',
    'spellbook_scale080': 'shows GameMenuFrame, not a spellbook',
    'spellbook_scale100': 'shows GameMenuFrame, not a spellbook',
}


def captured_frames(path, state):
    text = path.read_text(encoding='latin-1')
    at = text.index('["frames_' + state + '"]')
    block = text[at:text.index('\n\t}', at)]
    return [r.split('|') for r in re.findall(r'\] = "(.*)",', block)]


def frame_mask(path, state, size):
    rows = captured_frames(path, state)
    sw, sh = map(float, rows[0][1:3])
    regions, excluded = [], []
    for r in rows[1:]:
        if len(r) < 6:
            continue
        left, bottom, right, top = map(float, r[2:6])
        box = (int(left*size[0]/sw), int((sh-top)*size[1]/sh),
               int(np.ceil(right*size[0]/sw)), int(np.ceil((sh-bottom)*size[1]/sh)))
        if r[1] in {'PlayerPortrait','TargetPortrait','MicroButtonPortrait','Minimap',
                    'MerchantFramePortrait','ClassTrainerFramePortrait','TaxiPortrait','LootFramePortrait'}:
            excluded.append(box)
        elif r[0] == 'R' and not (len(r)>7 and r[6]=='FontString' and not r[-1].strip('~ ')):
            regions.append(box)
    return regions, excluded


def rectangle(mask, box, value):
    x0,y0,x1,y1 = box
    mask[max(y0,0):max(y1,0),max(x0,0):max(x1,0)] = value


def compare(spec, directory):
    if spec.get('required_frames') or spec.get('match_text_frames'):
        frame_dump = (directory / (spec['state'] + '_frames.txt')).read_text().splitlines()
        for prefix in spec.get('required_frames',[]):
            if not any(row.startswith(prefix) for row in frame_dump):
                raise ValueError(f"Missing visible frame for {spec['state']}: {prefix}")
        if spec.get('match_text_frames'):
            native = {row[1]:row[-1] for row in captured_frames(ROOT/'evidence'/spec['frames'][0],spec['frames'][1]) if row[0]=='R'}
            rendered = {row.split('|')[1]:row.split('|')[-1] for row in frame_dump if row.startswith('R|')}
            for name in spec['match_text_frames']:
                if name not in native or rendered.get(name) != native[name]:
                    raise ValueError(f"Wrong native text for {spec['state']}: {name}")
    ref = Image.open(ROOT / 'evidence' / spec['reference']).convert('RGB')
    ours = Image.open(directory / (spec['state'] + '.png')).convert('RGBA')
    if ours.size != tuple(spec['render_size']):
        raise ValueError(f"Wrong render resolution for {spec['state']}: {ours.size}")
    if spec['crop']:
        ours = ours.crop(spec['crop'])
    if ref.size != ours.size:
        raise ValueError(f"Reference/render dimensions differ: {ref.size}/{ours.size}")
    a, rgba = np.asarray(ref,dtype=np.int16), np.asarray(ours,dtype=np.int16)
    masks = {}
    if spec['world']:
        world = np.asarray(Image.open(ROOT/'evidence'/spec['world']).convert('RGB'),dtype=np.int16)
        alpha = rgba[...,3:4]/255.
        b = np.round(rgba[...,:3]*alpha+world*(1-alpha)).astype(np.int16)
        mask = rgba[...,3]>8
        regions, excluded = frame_mask(ROOT/'evidence'/spec['frames'][0],spec['frames'][1],ref.size)
        if not spec.get('exclude_portraits', True):
            excluded = []
        for box in regions:
            rectangle(mask,box,True)
        for box in excluded:
            rectangle(mask,box,False)
        for box in spec.get('excluded',[]):
            rectangle(mask,box,False)
        if spec.get('include'):
            included = np.zeros(mask.shape,dtype=bool)
            for box in spec['include']:
                rectangle(included,box,True)
            mask &= included
        if spec.get('foreground_on_black'):
            mask &= (a.max(axis=2)>8) | (b.max(axis=2)>8)
        masks['interface'] = mask
        for name, box in spec['regions'].items():
            region = np.zeros(mask.shape,dtype=bool)
            rectangle(region,box,True)
            masks[name] = mask & region
    else:
        b = rgba[...,:3]
        for name, box in spec['regions'].items():
            mask = np.zeros(a.shape[:2],dtype=bool)
            rectangle(mask,box,True)
            masks[name] = mask
    empty = [name for name,mask in masks.items() if not mask.any()]
    if empty:
        raise ValueError(f"No comparable pixels for {spec['state']}: {', '.join(empty)}")
    err = np.abs(a-b).max(axis=2)
    metrics = {name: {'pixels':int(mask.sum()), 'match':round(float((err[mask]<=24).mean()),4),
                      'mean_error':round(float(err[mask].mean()),2)} for name,mask in masks.items()}
    union = np.logical_or.reduce(list(masks.values()))
    heat = np.zeros_like(a,dtype=np.uint8)
    heat[...,0] = np.where(union,np.minimum(err*4,255),0)
    heat[...,1] = np.where(union & (err<=24),60,0)
    return metrics, np.concatenate([a,b,heat],axis=1).astype(np.uint8)


def main(before, after, out, reliable=False, cases=None):
    out.mkdir(parents=True,exist_ok=True)
    report = {}
    for name,spec in (cases if cases is not None else CASES).items():
        if reliable and name in UNRELIABLE:
            print(f'Skipped {name}: {UNRELIABLE[name]}')
            continue
        prior,_ = compare(spec,before)
        current,strip = compare(spec,after)
        Image.fromarray(strip).save(out/(name+'_comparison.png'))
        report[name] = {'reference':spec['reference'],'state':spec['state'],
                        'before':prior,'after':current,
                        'method':'world-composite' if spec['world'] else 'fixed UI rectangles; raw RGB',
                        'included_rectangles':spec.get('include'),
                        'named_rectangles':spec['regions'],
                        'excluded_rectangles':spec.get('excluded',[]),
                        'exclude_portraits':spec.get('exclude_portraits',True),
                        'matched_text_frames':spec.get('match_text_frames',[]),
                        'required_frames':spec.get('required_frames',[]),
                        'foreground_on_black':spec.get('foreground_on_black',False),
                        'note':'Diagnostics only; world animation can differ and portraits are excluded.'}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    if reliable:
        (out/'skipped.json').write_text(json.dumps(UNRELIABLE,indent=2)+'\n')
    print(f"Compared {len(report)} capture states; report: {out/'report.json'}")


def check_layouts(directory):
    """Check native fixture dimensions against the untouched client XML."""
    layouts = (
        ('character', 'CharacterFrame', 'FrameXML/CharacterFrame.xml'),
        ('quest_log', 'QuestLogFrame', 'FrameXML/QuestLogFrame.xml'),
        ('merchant', 'MerchantFrame', 'FrameXML/MerchantFrame.xml'),
        ('loot', 'LootFrame', 'FrameXML/LootFrame.xml'),
        ('dressing_room', 'DressUpFrame', 'FrameXML/DressUpFrame.xml'),
        ('bags_split', 'StackSplitFrame', 'FrameXML/StackSplitFrame.xml'),
        ('druid_combo_error', 'ComboFrame', 'FrameXML/ComboFrame.xml'),
        ('fidelity_taxi_layout', 'TaxiFrame', 'FrameXML/TaxiFrame.xml'),
        ('trainer', 'ClassTrainerFrame', 'AddOns/Blizzard_TrainerUI/Blizzard_TrainerUI.xml'),
    )
    report = {}
    for state,name,path in layouts:
        root = ET.parse(ROOT/'build/apiref/ui/Interface'/path).getroot()
        node = next(n for n in root.iter() if n.get('name')==name)
        size = next(n for n in node if n.tag.endswith('Size'))
        dims = next(n for n in size if n.tag.endswith('AbsDimension'))
        expected = [float(dims.get('x')),float(dims.get('y'))]
        text = (directory/(state+'_frames.txt')).read_text()
        row = next(line.split('|') for line in text.splitlines() if line.startswith('F|'+name+'|'))
        actual = [float(row[4])-float(row[2]),float(row[5])-float(row[3])]
        if any(abs(a-b)>0.05 for a,b in zip(actual,expected)):
            raise ValueError(f'{name}: {actual}, client XML declares {expected}')
        report[name] = {'xml':path,'expected':expected,'rendered':actual}
    (directory/'layout-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'{len(report)} native frame dimensions match original 1.12 FrameXML')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--states', action='store_true')
    groups = parser.add_mutually_exclusive_group()
    groups.add_argument('--clientref', action='store_true', help='only the four clean native scale references')
    groups.add_argument('--fonts', action='store_true', help='controlled opaque native font preview')
    groups.add_argument('--panels', action='store_true', help='native merchant, loot, trainer and taxi panel references')
    groups.add_argument('--characters', action='store_true', help='four native paper-doll orientations; adapted statistics excluded')
    groups.add_argument('--buffs', action='store_true', help='native Mark of the Wild HUD and tooltip')
    groups.add_argument('--merchant-interactions', action='store_true', help='native hover, pressed button and three quantity selections')
    parser.add_argument('--layouts', type=Path)
    parser.add_argument('--reliable', action='store_true', help='skip mislabelled/unclean scale captures')
    parser.add_argument('directories', nargs='*', type=Path)
    args = parser.parse_args()
    cases = {n:s for n,s in CASES.items() if (not args.reliable or n not in UNRELIABLE) and (not args.clientref or (n.startswith("clientref_") and n not in (PANEL_CASES | FONT_CASES | CHARACTER_CASES | BUFF_CASES | MERCHANT_INTERACTION_CASES))) and (not args.panels or n in PANEL_CASES) and (not args.fonts or n in FONT_CASES) and (not args.characters or n in CHARACTER_CASES) and (not args.buffs or n in BUFF_CASES) and (not args.merchant_interactions or n in MERCHANT_INTERACTION_CASES)}
    if args.states:
        for size in ((1280,720),(2560,1440)):
            states = sorted({s['state'] for s in cases.values() if tuple(s['render_size'])==size})
            print(f"--size {size[0]}x{size[1]} " + ' '.join(states))
    elif args.layouts:
        check_layouts(args.layouts)
    elif len(args.directories)==3:
        main(*args.directories, reliable=args.reliable, cases=cases)
    else:
        parser.error('provide BEFORE_DIR AFTER_DIR OUT_DIR, --states, or --layouts DIR')
