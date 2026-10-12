#!/usr/bin/env python3
"""Compose 64x64 vanilla-style Skyrim BLP2 icons from extracted WoW 1.12 art.

Extract sources with build/wowcore/mpq_extract DATA 'Interface\\Icons\\*.blp' /tmp/wow-icons.
No original paintings or frames: source brushwork, semantic crops, school/effect
colour, composited effect motifs, and a small deterministic rune distinguish forms.
"""
import argparse
from functools import lru_cache
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
import textwrap

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

COLOURS = {'fire': (255, 105, 27), 'frost': (83, 195, 255), 'shock': (172, 130, 255),
           'Restoration': (255, 215, 99), 'Alteration': (219, 168, 73), 'Illusion': (94, 207, 176),
           'Conjuration': (185, 112, 240), 'Destruction': (225, 94, 148), 'health': (226, 61, 58), 'magicka': (71, 132, 255),
           'stamina': (89, 212, 88), 'combat': (228, 133, 70), 'poison': (118, 205, 51), 'disease': (171, 163, 58),
           'None': (172, 186, 203)}


def effect_rows(row, forms):
    return [forms[e['form']] for e in row['effects']] if row['record_type'] != 'MGEF' else [row]


def classify(row, effects):
    text = ' '.join([row['display_name'], row['edid']] + [e['edid'] + ' ' + e['name'] for e in effects]).lower()
    visible = [e for e in effects if not e['hidden_in_ui']] or effects
    primary = visible[0] if visible else {}
    school = next((e['school'] for e in visible if e['school'] != 'None'), 'None')
    av = primary.get('actor_value_id', -1)
    # Fortify-skill alchemy effects use modifier/power actor values, without an associated school.
    skill_av = next((av - offset + 6 for offset in (96, 114, 135) if offset <= av < offset + 18), av)
    if school == 'None':
        school = {18: 'Alteration', 19: 'Conjuration', 20: 'Destruction', 21: 'Illusion', 22: 'Restoration'}.get(skill_av, school)
    archetype = primary.get('archetype', '')
    # Resist variables, unlike name guesses, also identify nameless elemental damage.
    resists = {e['resist_value_id'] for e in visible} | {e['actor_value_id'] for e in visible}
    element = next((kind for kind, value in [('fire', 41), ('frost', 43), ('shock', 42)] if value in resists), None)
    if element is None:
        element = next((kind for kind, words in [('fire', ('flame', 'fire', 'burn')), ('frost', ('frost', 'blizzard')),
                                                ('shock', ('shock', 'lightning', 'thunder'))]
                        if any(w in text for w in words)), None)
        if element is None and re.search(r'\bice(?:\b|spike|spear|storm|form|bolt|damage)', text):
            element = 'frost'
    colour = element or school
    if row.get('type') == 'Disease' or 'disease' in text or any(w in text for w in ('rockjoint', 'brain rot', 'bone break', 'witbane', 'rattles', 'sanguinare')):
        colour = 'disease'
    elif row.get('type') == 'Poison':
        colour = 'poison'
    elif row.get('type') == 'Potion':
        colour = {24: 'health', 25: 'magicka', 26: 'stamina', 27: 'health', 28: 'magicka', 29: 'stamina',
                  155: 'health', 156: 'magicka', 157: 'stamina'}.get(av, colour)
        if colour == 'None':
            colour = ('combat' if skill_av in (6, 7, 8, 9, 11, 12) else 'Illusion' if skill_av in (13, 14, 15, 17) else
                      'Alteration' if skill_av in (10, 16, 23, 32, 39) else 'poison' if skill_av in (40, 45) else 'magicka')
    return text, primary, school, element, colour, archetype, skill_av


def semantic_art(text, primary, school, element, archetype, av):
    """Meaning determines the composition; form identity only affects the small rune/crop."""
    if 'vampire' in text and ('lord' in text or archetype == 'VampireLord'):
        return 'Spell_Shadow_Metamorphosis', 'transformation'
    if archetype == 'Werewolf' or 'werewolf' in text:
        return 'Ability_Druid_DemoralizingRoar', 'transformation'
    if archetype == 'SoulTrap' or 'soultrap' in text or 'soul trap' in text:
        return 'Spell_Shadow_SoulGem', 'soul'
    if archetype == 'Reanimate' or any(w in text for w in ('reanimate', 'raise zombie', 'dead thrall')):
        return 'Spell_Shadow_RaiseDead', 'reanimate'
    if archetype == 'SummonCreature':
        if element == 'fire': return 'Spell_Shadow_SummonInfernal', 'summon'
        if element == 'frost': return 'Spell_Frost_SummonWaterElemental', 'summon'
        if element == 'shock': return 'Spell_Nature_EarthBind', 'summon'
        return 'Spell_Shadow_SummonVoidWalker', 'summon'
    if archetype == 'BoundWeapon':
        return ('INV_Weapon_Bow_07' if 'bow' in text else 'INV_Weapon_Halberd_04' if 'axe' in text else
                'INV_Weapon_ShortBlade_05' if 'dagger' in text else 'INV_Sword_04'), 'bound'
    if 'whirlwind sprint' in text or 'voicesprint' in text: return 'Spell_Nature_Cyclone', 'sprint'
    if archetype == 'Stagger' or 'unrelenting force' in text: return 'Ability_Warrior_WarCry', 'force'
    if element:
        variant = ('cloak' if archetype == 'Cloak' or 'cloak' in text else 'rune' if 'rune' in text else
                   'wall' if 'wall' in text else 'storm' if any(w in text for w in ('storm', 'blizzard')) else
                   'burst' if any(w in text for w in ('fireball', 'incinerate', 'icy spear', 'thunderbolt', 'chain')) else
                   'bolt' if 'bolt' in text or 'spike' in text else 'stream')
        sources = {'fire': {'stream': 'Spell_Fire_Fire', 'bolt': 'Spell_Fire_FireBolt02', 'burst': 'Spell_Fire_Fireball02',
                            'cloak': 'Spell_Fire_Immolation', 'wall': 'Spell_Fire_FlameShock',
                            'rune': 'Spell_Fire_SealOfFire', 'storm': 'Spell_Fire_MeteorStorm'},
                   'frost': {'stream': 'Spell_Frost_Frost', 'bolt': 'Spell_Frost_FrostBolt02', 'burst': 'Spell_Frost_FrostBolt',
                             'cloak': 'Spell_Frost_FrostArmor02', 'wall': 'Spell_Frost_FrostNova',
                             'rune': 'Spell_Frost_Glacier', 'storm': 'Spell_Frost_IceStorm'},
                   'shock': {'stream': 'Spell_Nature_Lightning', 'bolt': 'Spell_Nature_Lightning',
                             'burst': 'Spell_Nature_ChainLightning', 'cloak': 'Spell_Nature_LightningShield',
                             'wall': 'Spell_Nature_CallStorm', 'rune': 'Spell_Nature_LightningShield',
                             'storm': 'Spell_Nature_StormReach'}}
        return sources[element][variant], variant
    if archetype in ('Calm', 'Disguise') or 'calm' in text: return 'Spell_Shadow_Charm', 'calm'
    if archetype == 'Demoralize' or 'fear' in text: return 'Spell_Shadow_PsychicScream', 'fear'
    if archetype == 'Frenzy' or 'frenzy' in text: return 'Spell_Shadow_UnholyFrenzy', 'frenzy'
    if archetype == 'Rally': return 'Spell_Holy_InnerFire', 'rally'
    if archetype == 'Invisibility' or av == 54: return 'Spell_Nature_Invisibilty', 'invisibility'
    if archetype == 'Paralysis' or av == 53: return 'Ability_Rogue_KidneyShot', 'paralysis'
    if archetype == 'Light': return 'Spell_Holy_MagicalSentry', 'light'
    if archetype in ('DetectLife', 'NightEye'): return 'Spell_Shadow_DetectInvisibility', 'sight'
    if archetype == 'Telekinesis': return 'Spell_Arcane_Blink', 'telekinesis'
    if archetype == 'SlowTime': return 'Spell_Nature_TimeStop', 'time'
    if archetype == 'Etherealize': return 'Spell_Shadow_GhostKey', 'ethereal'
    if archetype == 'TurnUndead': return 'Spell_Holy_TurnUndead', 'turn'
    if archetype == 'Banish': return 'Spell_Shadow_Cripple', 'banish'
    if archetype in ('CureDisease', 'CurePoison'): return 'Spell_Nature_NullifyDisease', 'cure'
    if 'ward' in text or av in (63, 67): return 'Spell_Holy_PowerWordShield', 'ward'
    if 'disease' in text or primary.get('detrimental') and school == 'None' and av not in (24, 25, 26):
        return 'Spell_Shadow_Contagion', 'disease'
    if av in (24, 27, 155):
        return ('Spell_Shadow_LifeDrain02' if primary.get('detrimental') else
                'Spell_Holy_GreaterHeal' if any(w in text for w in ('grand', 'fast', 'close', 'ultimate')) else
                'Spell_Nature_Regenerate' if av != 24 else 'Spell_Holy_FlashHeal'), 'health'
    if av in (25, 28, 156): return 'Spell_Shadow_ManaBurn' if primary.get('detrimental') else 'Spell_Nature_MagicImmunity', 'magicka'
    if av in (26, 29, 157): return 'Spell_Nature_Strength', 'stamina'
    if av == 39 or 'flesh' in text: return 'Spell_Arcane_ArcaneResilience', 'armor'
    if av in (57, 58) or 'waterbreathing' in text: return 'Spell_Shadow_DemonBreath', 'water'
    if av == 32: return 'INV_Misc_Bag_08', 'carry'
    if 'muffle' in text or av == 15: return 'Ability_Stealth', 'stealth'
    if av in (6, 7, 8, 34, 35): return 'Ability_Warrior_SavageBlow', 'weapon'
    if av in (9, 11, 12): return 'Spell_Holy_DevotionAura', 'armor'
    if av in (13, 14): return 'Spell_Shadow_GhostKey', 'lock'
    if av == 16: return 'INV_Potion_93', 'alchemy'
    if av == 17: return 'Spell_Shadow_SoothingKiss', 'speech'
    return {'Restoration': 'Spell_Holy_SealOfSacrifice', 'Alteration': 'Spell_Arcane_ArcaneResilience',
            'Illusion': 'Spell_Shadow_MindSteal', 'Conjuration': 'Spell_Shadow_DarkSummoning',
            'Destruction': 'Spell_Shadow_ShadowBolt'}.get(school, 'Spell_Holy_GreaterBlessingofKings'), 'magic'


def recipe(row, forms):
    effects = effect_rows(row, forms)
    text, primary, school, element, colour, archetype, av = classify(row, effects)
    art, variant = semantic_art(text, primary, school, element, archetype, av)
    material = next((name for name in ('oak', 'stone', 'iron', 'ebony', 'dragon') if name + 'flesh' in text), None)
    if material:
        variant = 'armor-' + material
    kind = row['record_type']
    base, motif = art, None
    if row.get('type') in ('Potion', 'Poison'):
        base = {'health': 'INV_Potion_51', 'magicka': 'INV_Potion_72', 'stamina': 'INV_Potion_64',
                'poison': 'INV_Potion_19'}.get(colour, 'INV_Potion_93')
        motif = art
    elif row.get('type') == 'Food':
        base = ('INV_Drink_05' if any(w in text for w in ('mead', 'wine', 'ale', 'drink')) else
                'INV_Misc_Food_19' if any(w in text for w in ('apple', 'tomato')) else
                'INV_Misc_Food_11' if any(w in text for w in ('bread', 'roll', 'pie')) else
                'INV_Misc_Food_03' if 'cheese' in text else 'INV_Misc_Fish_02' if 'fish' in text or 'salmon' in text else
                'INV_Misc_Food_09' if 'soup' in text or 'stew' in text else 'INV_Misc_Food_15')
        motif = art
    elif kind == 'INGR':
        base = ('INV_Misc_Fish_02' if any(w in text for w in ('fish', 'salmon', 'carp')) else
                'INV_Misc_Dust_02' if any(w in text for w in ('salt', 'dust', 'ash')) else
                'INV_Misc_Bone_01' if any(w in text for w in ('bone', 'tooth', 'antler')) else
                'INV_Mushroom_01' if any(w in text for w in ('mushroom', 'fungus', 'mora', 'namira', 'toadstool')) else
                'INV_Misc_MonsterSpiderCarapace_01' if any(w in text for w in ('larva', 'butterfly', 'moth', 'crab')) else
                'INV_Misc_Organ_01' if 'heart' in text else 'INV_Misc_Eye_01' if 'eye' in text else
                'INV_Egg_01' if 'egg' in text else 'INV_Misc_Herb_01')
        motif = art
    elif kind == 'SCRL':
        base, motif = 'INV_Scroll_03', art
    elif kind == 'SHOU' or row.get('type') == 'Voice':
        base, motif = 'INV_Misc_Head_Dragon_01', art
        variant = 'shout-' + variant
    elif row.get('type') == 'Disease':
        base = 'Spell_Shadow_Contagion'
        motif = ('Spell_Shadow_ManaBurn' if av in (25, 28, 156) else
                 'Spell_Nature_Strength' if av in (26, 29, 157) else
                 'Spell_Shadow_LifeDrain02' if av in (24, 27, 155) else
                 'Spell_Shadow_GhostKey' if av in (13, 14) else
                 'Ability_Warrior_SavageBlow' if av in (6, 7, 8, 34, 35) else
                 'Ability_Stealth' if av == 15 else 'Spell_Shadow_MindRot')
    magnitudes = [abs(e['magnitude']) for e in row['effects']]
    intensity = min(4, int(math.log2(1 + max(magnitudes, default=0)) / 2))
    intensity = max(intensity, 3 if variant in ('burst', 'storm') else 1 if variant == 'bolt' else 0)
    seed = hashlib.sha256((row['key'] + ':' + row['edid']).encode()).digest()
    return {'base': base, 'motif': motif, 'effect_art': art, 'school': school, 'colour': colour,
            'variant': variant, 'intensity': intensity, 'crop': [seed[0] % 4, seed[1] % 4],
            'rune': seed[2:6].hex(), 'material': material,
            'enchantment': kind == 'ENCH', 'hostile': any(e['hostile'] for e in effects)}


class Composer:
    def __init__(self, source):
        self.paths = {p.stem.casefold(): p for p in source.glob('*.blp')}
        self.provenance = {}

    @lru_cache(maxsize=None)
    def source_art(self, name):
        path = self.paths.get(name.casefold())
        if path is None:
            raise FileNotFoundError(f'missing vanilla source art: {name}')
        self.provenance[name] = {'wow_path': 'Interface\\Icons\\' + path.name,
                                 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        with Image.open(path) as im:
            rgba = im.convert('RGBA')
            # Several vanilla icons encode a black matte as transparency. Full bleed
            # icon art needs the matte, never the world's colour showing through it.
            backing = Image.new('RGBA', rgba.size, (8, 7, 12, 255))
            backing.alpha_composite(rgba)
            return backing.convert('RGB').resize((64, 64), Image.Resampling.LANCZOS)

    @lru_cache(maxsize=None)
    def art(self, name):
        # Compose only the painted interior. Restore the client's common bevel
        # afterward: helpful BuffButtonTemplate has no separate border texture.
        return self.source_art(name).crop((4, 4, 60, 60)).resize((64, 64), Image.Resampling.LANCZOS)

    def bevel(self, image):
        frame = self.source_art('Spell_Nature_HealingTouch').copy()
        mask = Image.new('L', (64, 64))
        ImageDraw.Draw(mask).rounded_rectangle((4, 4, 59, 59), radius=3, fill=255)
        interior = Image.new('RGB', (64, 64))
        interior.paste(image.resize((56, 56), Image.Resampling.LANCZOS), (4, 4))
        frame.paste(interior, (0, 0), mask)
        return frame

    def compose(self, spec):
        image = self.art(spec['base']).copy()
        x, y = spec['crop']
        inset = spec['intensity']
        image = image.crop((x, y, 64 - inset, 64 - inset)).resize((64, 64), Image.Resampling.LANCZOS)
        if spec['variant'] == 'bolt' and spec['colour'] == 'shock':
            image = image.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
        colour = COLOURS[spec['colour']]
        if spec['material']:
            colour = {'oak': (190, 132, 65), 'stone': (153, 164, 151), 'iron': (142, 178, 197),
                      'ebony': (112, 88, 132), 'dragon': (221, 208, 154)}[spec['material']]
        shade = ImageOps.colorize(ImageOps.grayscale(image), (6, 5, 12), tuple(min(255, c + 65) for c in colour),
                                 mid=colour)
        item = spec['base'].startswith('INV_Potion')
        image = Image.blend(image, shade, 0.78 if spec['material'] else 0.65 if item else
                           0.32 if spec['school'] == 'Alteration' else 0.18)
        image = ImageEnhance.Contrast(image).enhance(1.06 + spec['intensity'] * 0.015)
        if spec['motif']:
            size = 29 if spec['variant'].startswith('shout') else 25
            motif = self.art(spec['motif']).resize((size, size), Image.Resampling.LANCZOS)
            mask = Image.new('L', (size, size))
            draw = ImageDraw.Draw(mask)
            draw.ellipse((1, 1, size - 2, size - 2), fill=238)
            mask = mask.filter(ImageFilter.GaussianBlur(2.2))
            image.paste(motif, (64 - size - 3, 64 - size - 3), mask)
        if spec['enchantment']:
            # Same enchanted-weapon motif on enchantments, with school-coloured light.
            blade = self.art('INV_Sword_04').resize((22, 22), Image.Resampling.LANCZOS)
            mask = ImageOps.grayscale(blade).point(lambda v: min(230, max(0, v - 24) * 2))
            image.paste(blade, (3, 38), mask)
        # A tiny broken rune in the art, not a frame. Subpixel glow preserves the
        # painted source; its strokes identify otherwise similar rank/quest variants.
        colour = COLOURS[spec['school']] if spec['school'] in ('Alteration', 'Illusion', 'Restoration', 'Conjuration') else colour
        glow = Image.new('RGBA', (128, 128))
        ink = ImageDraw.Draw(glow)
        seed = bytes.fromhex(spec['rune'])
        ox, oy = (18, 21) if spec['motif'] else (101, 102)
        ink.line([(ox - 7, oy + 8), (ox, oy - 8), (ox + 6, oy + 6)], fill=(*colour, 195), width=3)
        for i in range(8):
            yy = oy - 7 + i * 2
            nibble = (seed[i // 2] >> ((i % 2) * 4)) & 15
            if nibble & 1:
                ink.line([(ox, yy), (ox - 7 - (nibble >> 2), yy - 3)], fill=(*colour, 175), width=2)
            if nibble & 2:
                ink.line([(ox, yy), (ox + 7 + (nibble >> 2), yy + 3)], fill=(*colour, 175), width=2)
        halo = glow.filter(ImageFilter.GaussianBlur(3.2))
        halo.alpha_composite(glow)
        rgba = image.convert('RGBA')
        rgba.alpha_composite(halo.resize((64, 64), Image.Resampling.LANCZOS))
        return self.bevel(rgba.convert('RGB'))


def blp2(image):
    """Vanilla-supported palette BLP2 with a full 64->1 mip chain and opaque texels."""
    indexed = image.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    palette = indexed.getpalette()
    palette += [0] * (768 - len(palette))
    bgra = bytes(v for i in range(256) for v in (palette[i * 3 + 2], palette[i * 3 + 1], palette[i * 3], 0))
    levels = [indexed.tobytes()]
    for size in (32, 16, 8, 4, 2, 1):
        mip = image.resize((size, size), Image.Resampling.LANCZOS).quantize(palette=indexed, dither=Image.Dither.NONE)
        levels.append(mip.tobytes())
    offsets, sizes, at = [], [], 148 + 1024
    for level in levels:
        offsets.append(at)
        sizes.append(len(level))
        at += len(level)
    return (struct.pack('<4sI4BII', b'BLP2', 1, 1, 0, 0, 1, 64, 64) +
            struct.pack('<16I', *(offsets + [0] * (16 - len(offsets)))) +
            struct.pack('<16I', *(sizes + [0] * (16 - len(sizes)))) + bgra + b''.join(levels))


def contact_sheet(rows, images, output, title, columns=8):
    width, height = 208, 104
    sheet = Image.new('RGB', (columns * width, 36 + math.ceil(len(rows) / columns) * height), '#17151d')
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=12)
    small = ImageFont.load_default(size=10)
    draw.text((12, 10), title, fill='#eee2bc', font=font)
    for i, row in enumerate(rows):
        x, y = (i % columns) * width + 6, 38 + (i // columns) * height
        sheet.paste(images[row['key']], (x, y))
        name = textwrap.wrap(row['display_name'], width=19, break_long_words=True)[:3]
        draw.multiline_text((x + 69, y + 2), '\n'.join(name), fill='#ece8db', font=small, spacing=2)
        draw.text((x, y + 67), row['record_type'] + ' ' + row['plugin'], fill='#ac9dcc', font=small)
        draw.text((x, y + 81), row['form_id_hex'] + ' ' + row['edid'][:24], fill='#9d9da7', font=small)
    sheet.save(output)


def generate(inventory, source, output):
    rows, forms = inventory['forms'], {r['key']: r for r in inventory['forms']}
    composer, images, mapping = Composer(source), {}, {}
    output.mkdir(parents=True, exist_ok=True)
    specs = {r['key']: recipe(r, forms) for r in rows}
    # Validate all source inputs before writing any icons.
    for spec in specs.values():
        for name in (spec['base'], spec['motif'], 'INV_Sword_04' if spec['enchantment'] else None):
            if name:
                composer.art(name)
    composer.source_art('Spell_Nature_HealingTouch')
    hashes = set()
    for row in rows:
        key, spec = row['key'], specs[row['key']]
        name = 'WB_' + re.sub('[^A-Za-z0-9_]', '_', Path(row['plugin']).stem) + '_' + row['form_id_hex']
        image = composer.compose(spec)
        encoded = blp2(image)
        # Quantization can erase a small glyph difference. Resolve those actual
        # encoded collisions deterministically rather than assuming different
        # recipes always produce different visible pixels.
        attempt = 0
        while hashlib.sha256(encoded).hexdigest() in hashes:
            attempt += 1
            spec['rune'] = hashlib.sha256((key + ':' + str(attempt)).encode()).digest()[:4].hex()
            image = composer.compose(spec)
            encoded = blp2(image)
        hashes.add(hashlib.sha256(encoded).hexdigest())
        (output / (name + '.blp')).write_bytes(encoded)
        # Review the quantized image the engine will display, not the pre-encoding image.
        import io
        with Image.open(io.BytesIO(encoded)) as decoded:
            images[key] = decoded.convert('RGB')
        mapping[key] = {'plugin': row['plugin'], 'local_form_id': row['local_form_id'], 'record_type': row['record_type'],
                        'edid': row['edid'], 'name': row['display_name'], 'icon': name,
                        'wow_path': 'Interface\\Icons\\' + name,
                        'effects': [e['form'] for e in row['effects']], 'recipe': spec,
                        'sha256': hashlib.sha256(encoded).hexdigest()}
    sheets = []
    # All forms appear with names at their actual 64px size in paginated sheets.
    for i in range(0, len(rows), 240):
        name = f'contact_sheet_{i // 240 + 1:02}.png'
        contact_sheet(rows[i:i + 240], images, output / name, f'Skyrim magic {i + 1}-{min(i + 240, len(rows))} of {len(rows)}')
        sheets.append(name)
    # A compact overview of player-facing spells plus the remaining types.
    wanted = ['Flames', 'Firebolt', 'Fireball', 'Fire Storm', 'Frostbite', 'Ice Spike', 'Icy Spear', 'Blizzard',
              'Sparks', 'Lightning Bolt', 'Chain Lightning', 'Lightning Storm', 'Healing', 'Fast Healing',
              'Close Wounds', 'Greater Ward', 'Oakflesh', 'Stoneflesh', 'Ironflesh', 'Ebonyflesh', 'Calm', 'Frenzy',
              'Invisibility', 'Muffle', 'Conjure Flame Atronach', 'Conjure Frost Atronach', 'Bound Sword', 'Soul Trap',
              'Unrelenting Force', 'Whirlwind Sprint', 'Slow Time', 'Become Ethereal']
    featured = []
    for name in wanted:
        row = next((r for r in rows if r['name'] == name and r['record_type'] in ('SPEL', 'SHOU') and r['plugin'] == 'Skyrim.esm'), None)
        if row:
            featured.append(row)
    names_by_kind = {'ALCH': ['Potion of Minor Healing', 'Potion of Minor Magicka', 'Potion of Minor Stamina',
                            'Potion of Cure Disease', 'Weak Poison', 'Potion of Resist Fire', 'Apple Cabbage Stew', 'Sweet Roll'],
                     'INGR': ['Nightshade', 'Bone Meal', 'Salt Pile', 'Fire Salts', 'Frost Salts', 'Daedra Heart', 'Fly Amanita', 'Snowberries'],
                     'ENCH': ['Resist Magic', 'Paralysis', 'Banish Daedra', 'Fireball', 'Frostbite', 'Chain Lightning', 'Soul Trap', 'Turn Undead'],
                     'MGEF': ['Flames', 'Frostbite', 'Sparks', 'Restore Health', 'Telekinesis Effect', 'Bound Sword', 'Reanimate', 'Fortify Armor Rating']}
    for kind in ('ALCH', 'INGR', 'SCRL', 'ENCH', 'MGEF', 'SPEL'):
        candidates = [r for r in rows if r['record_type'] == kind and r['visible_effect'] and r['plugin'] == 'Skyrim.esm']
        if kind == 'SPEL':
            candidates = [r for r in candidates if r.get('type') == 'Disease']
        selected = []
        for name in names_by_kind.get(kind, []):
            match = next((r for r in candidates if r['name'] == name), None)
            if match:
                selected.append(match)
        selected += [r for r in candidates if r not in selected]
        featured.extend(selected[:8])
    contact_sheet(featured, images, output / 'contact_sheet.png', 'Skyrim magic: spell families and representative applied effects')
    manifest = {'schema_version': 1, 'generator': 'tools/skyrim_icons.py', 'size': [64, 64],
                'bevel': {'source': 'Spell_Nature_HealingTouch', 'inset': 4},
                'format': 'BLP2 palette, opaque, seven mip levels', 'plugins': inventory['plugins'],
                'counts': inventory['counts'], 'icon_count': len(mapping), 'contact_sheets': sheets,
                'source_art': dict(sorted(composer.provenance.items())), 'forms': mapping}
    (output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    print(f'{len(mapping)} BLP2 icons, {len(sheets)} full contact sheets, manifest and preview written to {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory', type=Path, default=ROOT / 'build/skyrim_icons/inventory.json')
    parser.add_argument('--source', type=Path, required=True, help='extracted Interface/Icons directory')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/skyrim_icons')
    args = parser.parse_args()
    generate(json.loads(args.inventory.read_text()), args.source, args.out)


if __name__ == '__main__':
    main()
