#!/usr/bin/env python3
"""Tracked deployment of SKSE, Address Library and WoWBridge into the Skyrim install.

Every file written is recorded with its SHA-256 in a manifest. Existing files that were not
written by this tool are never overwritten (unless byte-identical), and uninstall only removes
files whose content still matches the manifest. Plugins.txt edits are tracked the same way.

usage: deploy.py install [--only skse,addrlib,mod] | uninstall | status
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, tempfile, zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import winhost  # noqa: E402

PROJECT = Path(__file__).resolve().parents[2]
GAME = Path(os.environ.get('WB_GAME', PROJECT / 'steamapps/common/Skyrim Special Edition'))
PREFIX_USER = PROJECT / 'steamapps/compatdata/489830/pfx/drive_c/users/steamuser'
PLUGINS_TXT = (winhost.plugins_txt() if winhost.IS_WSL else   # native Windows Skyrim, or Proton on Linux
               Path(os.environ.get('WB_PLUGINS_TXT', PREFIX_USER / 'AppData/Local/Skyrim Special Edition/Plugins.txt')))
REPO = Path(__file__).resolve().parent.parent
MANIFEST = Path(os.environ.get('WB_MANIFEST', REPO / 'tools/.deploy-manifest.json'))
DOWNLOADS = PROJECT / 'downloads'
DIST = REPO / 'dist/WoWBridge'           # MO2-style mod root (Data-relative)
RUNTIME = '1_7_104'
# The plugins in their load order: the Armory has WoWBridge.esp as a master, so it must come after it.
PLUGINS = ('WoWBridge.esp', 'WoWBridgeArmory.esp')


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def stamp(p: Path) -> list:
    st = p.stat()
    return [st.st_size, st.st_mtime_ns]


def installed_as(m: dict, rel: Path, dst: Path, digest: str) -> bool:
    """dst still holds what deploy.py installed there. Always read: an edit can keep its size and mtime, and an
    overwrite or uninstall must not take it (the stamps only mark what --trust-unchanged may leave unread)."""
    return m['files'].get(str(rel)) == digest and sha256(dst) == digest


def load_manifest() -> dict:
    return json.loads(MANIFEST.read_text()) if MANIFEST.exists() else {'files': {}, 'plugins': []}


def save_manifest(m: dict) -> None:
    MANIFEST.write_text(json.dumps(m, indent=1, sort_keys=True))


def one(pattern: str) -> Path:
    hits = sorted(DOWNLOADS.glob(pattern))
    if len(hits) != 1:
        sys.exit(f'expected exactly one download matching {pattern!r}, found {len(hits)}')
    return hits[0]


def stage_skse(tmp: Path) -> list[tuple[Path, Path]]:
    """Return (source, game-relative destination) pairs for SKSE runtime files."""
    archive = one('Skyrim Script Extender*2.3.1*.7z')
    subprocess.run(['7z', 'x', '-y', f'-o{tmp}', str(archive)], check=True, stdout=subprocess.DEVNULL)
    root = next(tmp.glob('skse64_*'))
    pairs = [(root / 'skse64_loader.exe', Path('skse64_loader.exe')),
             (root / f'skse64_{RUNTIME}.dll', Path(f'skse64_{RUNTIME}.dll'))]
    pairs += [(f, Path('Data/Scripts') / f.name) for f in sorted((root / 'Data/Scripts').glob('*.pex'))]
    return pairs


def stage_addrlib(tmp: Path) -> list[tuple[Path, Path]]:
    name = f'versionlib-{RUNTIME.replace("_", "-")}-0.bin'
    with zipfile.ZipFile(one('Address Library*1.7.104*.zip')) as z:
        z.extract(f'SKSE/Plugins/{name}', tmp)
    return [(tmp / 'SKSE/Plugins' / name, Path('Data/SKSE/Plugins') / name)]


def seed_ini(source: Path, tmp: Path) -> Path:
    """The shipped INI leaves client= for setup.py to fill. This developer deploy knows where the project keeps
    the 1.12 client, so a first install points at it (as Windows, or Wine, sees it) instead of the absent Skyrim/WoW."""
    client = PROJECT / 'WoW/client'
    text = source.read_text()
    if not (client / 'Data').is_dir() or '\nclient=\n' not in text:
        return source
    tmp.mkdir(parents=True, exist_ok=True)
    seeded = tmp / source.name
    where = winhost.win_path(client.resolve()) if winhost.IS_WSL else 'Z:' + str(client).replace('/', '\\')
    seeded.write_text(text.replace('\nclient=\n', '\nclient=' + where + '\n', 1))
    return seeded


def stage_mod(tmp: Path) -> list[tuple[Path, Path]]:
    if not DIST.exists():
        sys.exit(f'{DIST} missing: run tools/package.py first')
    pairs = []
    for f in sorted(DIST.rglob('*')):
        if not f.is_file():
            continue
        rel = Path('Data') / f.relative_to(DIST)
        # Seed configuration on first install; subsequent upgrades retain the
        # user's client path and preferences, including a pre-existing INI.
        if rel == Path('Data/SKSE/Plugins/WoWBridge.ini'):
            if (GAME / rel).exists():
                continue
            f = seed_ini(f, tmp)
        pairs.append((f, rel))
    return pairs


def install(only: set[str], trust: bool = False) -> None:
    m = load_manifest()
    shipped = {p.name for p in DIST.glob('*.es[pml]')} if 'mod' in only else set()
    plugins = [p for p in PLUGINS if p in shipped] + sorted(shipped - set(PLUGINS))
    for esp in plugins:
        add_plugin(m, esp, check_only=True)
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        pairs = []
        if 'skse' in only:
            pairs += stage_skse(tmp / 'skse')
        if 'addrlib' in only:
            pairs += stage_addrlib(tmp / 'addrlib')
        if 'mod' in only:
            pairs += stage_mod(tmp / 'mod')
        # A developer deploy (--trust-unchanged) leaves alone, unread, what it installed before from the same bytes:
        # over WSL's 9p every stat costs milliseconds, and most of 11,000 files never change.
        if trust:
            pairs = [(src, rel) for src, rel in pairs
                     if not (str(rel) in m.get('stamps', {}) and m['files'].get(str(rel)) == sha256(src))]
        # Validate everything before touching the game folder.
        for src, rel in pairs:
            dst, key = GAME / rel, str(rel)
            if dst.exists() and key not in m['files'] and sha256(dst) != sha256(src):
                sys.exit(f'refusing to overwrite untracked file: {rel}')
            if dst.exists() and key in m['files'] and not installed_as(m, rel, dst, m['files'][key]):
                sys.exit(f'tracked file was modified outside deploy.py: {rel}')
        for src, rel in pairs:
            dst = GAME / rel
            if dst.exists() and m['files'].get(str(rel)) == sha256(src):
                m.setdefault('stamps', {})[str(rel)] = stamp(dst)   # validated above
                continue   # already installed as it is (the minimap's thousands of tiles, mostly)
            dst.parent.mkdir(parents=True, exist_ok=True)
            pre_existing = dst.exists() and str(rel) not in m['files']
            shutil.copyfile(src, dst)
            if not pre_existing:            # identical pre-existing files stay untracked (we did not create them)
                m['files'][str(rel)] = sha256(dst)
                m.setdefault('stamps', {})[str(rel)] = stamp(dst)
    for esp in plugins:
        add_plugin(m, esp)
    save_manifest(m)
    print(f'installed {len(pairs)} files; manifest tracks {len(m["files"])}')


def add_plugin(m: dict, esp: str, *, check_only: bool = False) -> None:
    """Enable a plugin in Plugins.txt (tracked for uninstall): after the plugin it follows in PLUGINS, else at the end.
    One of ours already listed before its master is moved after it."""
    lines = PLUGINS_TXT.read_text(encoding='utf-8').splitlines() if PLUGINS_TXT.exists() else []
    name = lambda line: line.lstrip('*').strip().lower()
    master = PLUGINS[PLUGINS.index(esp) - 1].lower() if esp in PLUGINS[1:] else None
    at = next((i for i, l in enumerate(lines) if name(l) == esp.lower()), None)
    after = next((i for i, l in enumerate(lines) if name(l) == master), None) if master else None
    tracked = {p.lower() for p in m['plugins']}
    # Adding a missing master at the end would also put a user's Armory entry before it.
    if esp == PLUGINS[0] and at is None:
        dependent = next((i for i, l in enumerate(lines) if name(l) == PLUGINS[1].lower()), None)
        if dependent is not None and PLUGINS[1].lower() not in tracked:
            sys.exit(f'{PLUGINS_TXT}: user-owned {PLUGINS[1]} entry precedes its master; '
                     f'place {PLUGINS[0]} before {PLUGINS[1]} in Plugins.txt and retry')
    if at is not None and after is not None and at < after and esp.lower() not in tracked:
        sys.exit(f'{PLUGINS_TXT}: user-owned {esp} entry precedes its master; '
                 f'move {esp} after {PLUGINS[PLUGINS.index(esp) - 1]} in Plugins.txt and retry')
    if check_only or at is not None and (after is None or at > after):
        return   # validate before copying files; preserve an entry already in order
    if at is not None:
        lines.pop(at)
        after -= 1
    lines.insert(after + 1 if after is not None else len(lines), f'*{esp}')
    PLUGINS_TXT.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    if esp not in m['plugins']:
        m['plugins'].append(esp)


def uninstall() -> None:
    m = load_manifest()
    kept = {}
    for rel, digest in sorted(m['files'].items()):
        dst = GAME / rel
        if not dst.exists():
            continue
        if not installed_as(m, Path(rel), dst, digest):
            print(f'kept (modified since deploy): {rel}')
            kept[rel] = digest
            continue
        dst.unlink()
        for parent in dst.parents:            # prune directories we emptied, never the game root
            if parent == GAME or any(parent.iterdir()):
                break
            parent.rmdir()
    if PLUGINS_TXT.exists() and m['plugins']:
        drop = {p.lower() for p in m['plugins']}
        lines = [l for l in PLUGINS_TXT.read_text(encoding='utf-8').splitlines() if l.lstrip('*').lower() not in drop]
        PLUGINS_TXT.write_text('\n'.join(lines) + ('\n' if lines else ''), encoding='utf-8')
    save_manifest({'files': kept, 'plugins': []})
    print(f'uninstalled; {len(kept)} modified files left in place')


def status() -> None:
    m = load_manifest()
    for rel, digest in sorted(m['files'].items()):
        dst = GAME / rel
        state = 'missing' if not dst.exists() else ('ok' if installed_as(m, Path(rel), dst, digest) else 'MODIFIED')
        print(f'{state:8} {rel}')
    print('plugins:', ', '.join(m['plugins']) or '-')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['install', 'uninstall', 'status'])
    ap.add_argument('--only', default='skse,addrlib,mod')
    ap.add_argument('--trust-unchanged', action='store_true', help='skip files installed before from identical sources')
    a = ap.parse_args()
    {'install': lambda: install(set(a.only.split(',')), a.trust_unchanged), 'uninstall': uninstall, 'status': status}[a.cmd]()
