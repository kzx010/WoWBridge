#!/usr/bin/env python3
"""Assemble a local tester bundle from existing build outputs. Never downloads."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / 'dist/WoWBridge-Tester'
DATA = ('progression.json', 'talents.json', 'perks.json', 'talent_catalog.json', 'perk_catalog.json')
SCREENSHOTS = tuple('docs/screenshots/' + name for name in (
    'wowbridge_toggle.png', 'wowbridge_appearance.png', 'wowbridge_name.png', 'wowbridge_configured.png'))


def copy(src, dst):
    if not src.is_file():
        raise ValueError(f'missing build input: {src.relative_to(REPO) if src.is_relative_to(REPO) else src.name}')
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)


def check_privacy(path, root):
    raw = path.read_bytes()
    for data in (raw, raw.decode('utf-16le', errors='ignore').encode('utf-8')):
        if re.search(rb'/home/|/run/media/|\\run\\media\\|[A-Za-z]:\\Users\\', data):
            raise ValueError(f'build-machine path in release file: {path.relative_to(root)}; rebuild with path mapping')


def build():
    if (OUT / "local").exists():
        raise ValueError("the output folder contains tester settings; move that configured bundle before rebuilding")
    # Stage separately so a failed validation cannot replace the previous bundle.
    stage = REPO / 'build/tester-stage'
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    required = ('WoWBridge.esp', 'SKSE/Plugins/WoWBridge.dll', 'SKSE/Plugins/WoWBridge.ini',
                'SKSE/Plugins/WoWBridge/character.json', 'SKSE/Plugins/WoWBridge/gear.json')
    mod = REPO / 'dist/WoWBridge'
    for name in required:
        if not (mod / name).is_file():
            raise ValueError(f'missing mod input: {name}; run tools/package.py')
    shutil.copytree(mod, stage / 'mod')
    for name in ('README.md', 'LICENSE', 'setup.py', 'Setup.cmd', 'Start Service.cmd', 'Uninstall.cmd', 'start-service.sh'):
        copy(REPO / name, stage / name)
    for name in SCREENSHOTS:
        copy(REPO / name, stage / name)
    for path in sorted((REPO / 'service').glob('*.py')):
        copy(path, stage / 'runtime/service' / path.name)
    for name in DATA:
        copy(REPO / 'data' / name, stage / 'runtime/data' / name)
    copy(REPO / 'build/catalog.json', stage / 'runtime/build/catalog.json')
    # Preserve upstream notices already present in the local build dependencies.
    license_sources = {
        'WoWee.txt': REPO / 'wowcore/third_party/wowee/LICENSE',
        'CMaNGOS-Classic.txt': REPO.parent / 'WoW/cmangos/mangos-classic/LICENSE',
        'StormLib.txt': REPO.parent / 'reference/StormLib/LICENSE',
        'CommonLibSSE-NG.txt': REPO.parent / 'reference/CommonLibSSE-NG/licenses/LICENSE-MIT.txt',
        'hde64.txt': REPO.parent / 'reference/CommonLibSSE-NG/licenses/LICENSE-hde64.txt',
    }
    for path in (REPO / 'build/skse/vcpkg_installed').glob('*/share/*/copyright'):
        license_sources['vcpkg-' + path.parent.name + '.txt'] = path
    for name, path in license_sources.items():
        copy(path, stage / 'licenses' / name)
    # Lua's full permission notice is in lua.h, rather than its README.
    lua = (REPO / 'wowcore/third_party/lua-5.1.5/src/lua.h').read_text()
    notice = re.search(r'/\*{5,}\n\* Copyright.*?\*/', lua, re.S)
    if not notice:
        raise ValueError('missing Lua copyright notice')
    (stage / 'licenses/Lua.txt').write_text(notice.group() + '\n', encoding='utf-8')
    final = OUT
    entries = {}
    for path in sorted(stage.rglob('*')):
        if path.is_file():
            check_privacy(path, stage)
            entries[path.relative_to(stage).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()
    (stage / 'release-manifest.json').write_text(json.dumps({'schema': 1, 'build_commit': commit, 'files': entries}, indent=2) + '\n', encoding='utf-8')
    shutil.rmtree(final, ignore_errors=True)
    shutil.move(stage, final)
    archive = final.with_suffix('.zip')
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted(final.rglob('*')):
            if path.is_file():
                z.write(path, 'WoWBridge-Tester/' + path.relative_to(final).as_posix())
    print(f'Tester bundle: {archive}')
    print(f'{len(entries)} payload files; no local settings, logs or extracted resistance table included.')


def source_snapshot():
    """Export current source files without private Git history or developer binaries."""
    result = subprocess.run(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'],
                            cwd=REPO, capture_output=True, check=True)
    excluded = {'STATUS.md', 'docs/privacy.md', 'docs/next-playtest.md',
                'tools/native/mpqcat', 'tools/native/mpqpack', 'tools/native/mpqverify'}
    roots = {'data', 'service', 'skse', 'tools', 'wowcore', 'docs', 'licenses'}
    public_docs = {'docs/console-commands.md': '## Shell helpers',
                   'docs/perks.md': '## Rebuilding and checks'}
    top = {'README.md', 'LICENSE', 'setup.py', 'Setup.cmd', 'Start Service.cmd', 'Uninstall.cmd', 'start-service.sh', '.gitignore'}
    selected = []
    for name in sorted(set(result.stdout.decode().strip('\0').split('\0'))):
        path = REPO / name
        if (name in excluded or name.startswith('docs/playtest-') or not path.is_file() or
                path.is_symlink() or (Path(name).parts[0] not in roots and name not in top)):
            continue
        if Path(name).parts[0] == 'docs' and name not in public_docs and name not in SCREENSHOTS:
            continue
        raw = path.read_bytes()
        if name in public_docs:
            # Publish the user reference, keeping local verification history private.
            doc = raw.decode('utf-8').split(public_docs[name], 1)[0].rstrip() + '\n'
            # Internal feature notes and generated build files are not in this export.
            doc = re.sub(r'\[([^\]]+)\]\((?:[^:/)]*\.md(?:#[^)]*)?|\.\./build/[^)]*)\)', r'\1', doc)
            raw = doc.encode('utf-8')
        if re.search(rb'/(?:home|run/media)/[A-Za-z0-9_.-]+/', raw):
            raise ValueError(f'local filesystem path in source snapshot: {name}')
        selected.append((name, raw))
    output = REPO / 'dist/WoWBridge-Source.zip'
    output.parent.mkdir(parents=True, exist_ok=True)
    license_root = REPO / 'licenses'
    if not license_root.is_dir():
        license_root = OUT / 'licenses'
    if not license_root.is_dir():
        raise ValueError('build the tester bundle first to collect dependency notices')
    for path in sorted(license_root.glob('*.txt')):
        name = 'licenses/' + path.name
        if not any(existing == name for existing, _ in selected):
            selected.append((name, path.read_bytes()))
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, raw in selected:
            info = zipfile.ZipInfo('WoWBridge/' + name)
            info.external_attr = (0o100755 if raw.startswith(b'#!') else 0o100644) << 16
            z.writestr(info, raw, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    print(f'Source snapshot: {output} ({len(selected)} files, no Git history or internal playtest/handoff documents)')


if __name__ == '__main__':
    try:
        ap = argparse.ArgumentParser(description=__doc__)
        ap.add_argument("--source-snapshot", action="store_true", help="export source without existing Git history")
        args = ap.parse_args()
        source_snapshot() if args.source_snapshot else build()
    except (OSError, ValueError) as exc:
        print(f'Release: {exc}', file=sys.stderr)
        sys.exit(1)
