#!/usr/bin/env python3
"""Export the installed Skyrim HUD's compass frames into a local minimap badge atlas."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from data.skyrim_magic import Bsa


def blp(image):
    levels, offsets, sizes, at = [], [], [], 148
    while True:
        raw = image.tobytes('raw', 'BGRA')
        levels.append(raw); offsets.append(at); sizes.append(len(raw)); at += len(raw)
        if image.width == 1: break
        image = image.resize((image.width // 2, image.height // 2), Image.Resampling.LANCZOS)
    return (struct.pack('<4sI4BII', b'BLP2', 1, 3, 8, 0, 1, 512, 512) +
            struct.pack('<16I', *(offsets + [0] * (16-len(offsets)))) +
            struct.pack('<16I', *(sizes + [0] * (16-len(sizes)))) + b''.join(levels))


def atlas(frames, output, source_hash, labels):
    canvas = Image.new('RGBA', (512, 512)); nonempty = []
    for frame in range(1, 153):
        source = Image.open(frames / f'{frame}.png').convert('RGBA')
        bounds = source.getchannel('A').getbbox()
        if not bounds: continue
        nonempty.append(frame)
        source = source.crop(bounds); source.thumbnail((22, 22), Image.Resampling.LANCZOS)
        badge = Image.new('RGBA', (32, 32)); d = ImageDraw.Draw(badge)
        d.ellipse((1, 1, 30, 30), fill=(18, 17, 13, 230), outline=(169, 141, 67, 255), width=2)
        badge.alpha_composite(source, ((32-source.width)//2, (32-source.height)//2))
        canvas.alpha_composite(badge, (((frame-1)%16)*32, ((frame-1)//16)*32))
    output.mkdir(parents=True, exist_ok=True)
    (output/'WB_Compass.blp').write_bytes(blp(canvas)); canvas.save(output/'atlas.png')
    manifest = {'version': 1, 'source_sha256': source_hash, 'frame_count': 152,
                'labels': labels, 'nonempty_frames': nonempty, 'atlas': 'WB_Compass.blp'}
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(f'{len(nonempty)} native compass frames -> {output}')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data', type=Path, default=ROOT.parent/'steamapps/common/Skyrim Special Edition/Data')
    p.add_argument('--ffdec', type=Path, default=ROOT.parent/'toolchain/ffdec/ffdec.jar')
    p.add_argument('--out', type=Path, default=ROOT/'build/compass')
    args = p.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    # Prefer the installed loose HUD override; the base game's SWF contains the
    # source vectors corresponding to its optimized GFX movie.
    loose = args.data/'Interface/hudmenu.swf'
    source = loose.read_bytes() if loose.exists() else Bsa(args.data/'Skyrim - Interface.bsa').read('interface/hudmenu.swf')
    digest = hashlib.sha256(source).hexdigest()
    swf = args.out/'hudmenu.swf'; swf.write_bytes(source)
    home = args.out/'ffdec-home'; home.mkdir(exist_ok=True)
    command = ['java', '-Djava.awt.headless=true', f'-Duser.home={home.resolve()}', '-jar', str(args.ffdec)]
    xml = args.out/'hud.xml'
    subprocess.run(command+['-swf2xml', str(swf), str(xml)], check=True, stdout=subprocess.DEVNULL)
    sprites = []
    for item in ET.parse(xml).iter():
        if item.get('type') != 'DefineSpriteTag': continue
        tags = item.find('subTags')
        if tags is None: continue
        labels, frame = {}, 1
        for tag in tags:
            if tag.get('type') == 'FrameLabelTag': labels[tag.get('name')] = frame
            if tag.get('type') == 'ShowFrameTag': frame += 1
        if 'LocationMarkers' in labels: sprites.append((item, labels))
    if len(sprites) != 1 or sprites[0][0].get('frameCount') != '152':
        raise ValueError('unsupported HUD compass frame layout; keep the native compass visible')
    sprite, labels = sprites[0]
    subprocess.run(command+['-selectid', sprite.get('spriteId'), '-export', 'sprite', str(args.out/'frames'), str(swf)],
                   check=True, stdout=subprocess.DEVNULL)
    frames = next(path.parent for path in (args.out/'frames').rglob('152.png'))
    atlas(frames, args.out, digest, labels)


if __name__ == '__main__': main()
