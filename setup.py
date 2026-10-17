#!/usr/bin/env python3
"""Configure and install a local WoWBridge tester bundle. No network operations."""
import argparse
import configparser
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / 'runtime' if (ROOT / 'runtime').is_dir() else ROOT
MOD = ROOT / 'mod' if (ROOT / 'mod').is_dir() else ROOT / 'dist/WoWBridge'
LOCAL = ROOT / 'local'
sys.path.insert(0, str(RUNTIME))

INI = 'SKSE/Plugins/WoWBridge.ini'
PLUGINS = ('WoWBridge.esp', 'WoWBridgeArmory.esp')   # load order: the Armory has WoWBridge.esp as a master
ADDRESS_LIBRARY = 'versionlib-1-7-104-0.bin'
ARCHIVES = ('dbc.MPQ', 'fonts.MPQ', 'interface.MPQ', 'misc.MPQ', 'model.MPQ',
            'sound.MPQ', 'speech.MPQ', 'terrain.MPQ', 'texture.MPQ', 'wmo.MPQ')


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def write(path, raw):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as f:
        tmp = Path(f.name)
        try:
            f.write(raw)
        except BaseException:
            tmp.unlink(missing_ok=True)
            raise
    try:
        tmp.replace(path)
    finally:
        tmp.unlink(missing_ok=True)


def save(path, value):
    write(path, (json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode('utf-8'))


def read(path, default=None):
    return json.loads(Path(path).read_text(encoding='utf-8')) if Path(path).exists() else default


def files(root):
    result = {}
    for path in sorted(Path(root).rglob('*')):
        if path.is_symlink():
            raise ValueError(f'symbolic links are not supported in the mod payload: {path}')
        if path.is_file():
            result[path.relative_to(root).as_posix()] = path
    return result


def safe_relative(value):
    path = Path(value)
    if path.is_absolute() or '..' in path.parts or '\\' in value or ':' in value:
        raise ValueError('invalid relative path in file manifest')
    return path


def verify_bundle():
    manifest = read(ROOT / 'release-manifest.json')
    if manifest is None:
        if RUNTIME != ROOT:
            raise ValueError('release-manifest.json is missing; extract the complete tester bundle again')
        return
    for name, expected in manifest['files'].items():
        path = ROOT / safe_relative(name)
        if not path.is_file() or path.is_symlink() or digest(path) != expected:
            raise ValueError(f'bundle file is missing or changed: {name}; extract a fresh copy')


def need(path, label):
    if not Path(path).is_file():
        raise ValueError(f'{label} was not found: {path}')


def validate_installs(skyrim, client, address):
    for name in ('SkyrimSE.exe', 'Data/Skyrim.esm', 'skse64_loader.exe', 'skse64_1_7_104.dll'):
        need(skyrim / name, 'Skyrim 1.7.104 / SKSE 2.3.1 dependency')
    need(address, 'Address Library for Skyrim 1.7.104')
    if address.name != ADDRESS_LIBRARY:
        raise ValueError(f'select {ADDRESS_LIBRARY} from the installed Address Library mod')
    need(client / 'WoW.exe', 'WoW 1.12.1 client executable')
    for name in ARCHIVES:
        need(client / 'Data' / name, 'WoW 1.12.1 client archive')


def prompt(value, label, default=None):
    if value:
        return str(value)
    answer = input(label + (f' [{default}]' if default else '') + ': ').strip().strip('"')
    if not answer and not default:
        raise ValueError(f'{label} is required')
    return answer or str(default)


def ini_bytes(source, client):
    if any(c in client for c in '\r\n\0'):
        raise ValueError('the client path contains an invalid character')
    ini = configparser.ConfigParser(interpolation=None, strict=False)
    ini.optionxform = str
    ini.read_string(source.decode('utf-8-sig'))
    for section in ('WoW', 'Test'):
        if not ini.has_section(section):
            ini.add_section(section)
    # Match SimpleIni's case-insensitive keys without leaving duplicate settings.
    for section, key in (('WoW', 'client'), ('Test', 'autorun')):
        for old in list(ini[section]):
            if old.lower() == key:
                del ini[section][old]
    ini['WoW']['client'] = client
    ini['Test']['autorun'] = '0'
    out = io.StringIO()
    ini.write(out, space_around_delimiters=False)
    return out.getvalue().encode('utf-8')


def install_tree(source, target, manifest_path, *, overrides=None, allow_edited=()):
    source, target = Path(source).resolve(), Path(target).resolve()
    if source == target or source in target.parents or target in source.parents:
        raise ValueError('source and destination mod folders must be separate')
    old = read(manifest_path, {'target': str(target), 'files': {}})
    if old['target'] != str(target):
        raise ValueError('this install manifest belongs to another folder; uninstall it before changing the destination')
    payload = files(source)
    if not payload:
        raise ValueError('mod payload is empty; extract the tester bundle or run tools/package.py')
    hashes = {name: hashlib.sha256(overrides[name]).hexdigest() if overrides and name in overrides else digest(src)
              for name, src in payload.items()}
    # Complete the conflict check before copying any files.
    actuals, checked = {}, set()
    directories = set(old.get('directories', []))
    for name in payload:
        dst = target / safe_relative(name)
        for parent in dst.parents:
            if parent not in checked:
                if parent.is_symlink():
                    raise ValueError(f'symbolic link in destination: {name}')
                checked.add(parent)
        if dst.is_symlink():
            raise ValueError(f'symbolic link in destination: {name}')
        actual = digest(dst) if dst.exists() else None
        actuals[name] = actual
        expected = old['files'].get(name)
        accepted = {hashes[name]} if expected is None else {expected, old.get('pending_files', {}).get(name)}
        if actual is not None and actual not in accepted and name not in allow_edited:
            raise ValueError(f'refusing to overwrite a modified or untracked file: {name}')
        for parent in (dst.parent, *dst.parent.parents):
            if parent != target and target not in parent.parents:
                break
            if not parent.exists():
                directories.add(parent.relative_to(target).as_posix())
    owned = dict(old['files'])
    for name in payload:
        if actuals[name] is None or name in owned:
            owned[name] = hashes[name]
    pending = dict(old.get('pending_files', {}))
    for name in payload:
        previous = pending.pop(name, None)
        # An interrupted upgrade may leave either version on disk. Remember
        # only a verified installer-owned version, never an allowed user edit.
        if (name in old['files'] and actuals[name] is not None and actuals[name] != hashes[name]
                and actuals[name] in {old['files'][name], previous}):
            pending[name] = actuals[name]
    result = {**old, 'target': str(target), 'files': owned, 'directories': sorted(directories), 'pending_files': pending}
    # Journal both owned versions before writes, including files not copied yet.
    save(manifest_path, result)
    for name, src in payload.items():
        if actuals[name] != hashes[name]:
            write(target / safe_relative(name), overrides[name] if overrides and name in overrides else src.read_bytes())
    # Once copied, an older version becomes an ordinary user modification.
    # Retain pending ownership for files omitted from an interrupted upgrade.
    result['pending_files'] = {name: value for name, value in pending.items() if name not in payload}
    if not result['pending_files']:
        result.pop('pending_files')
    save(manifest_path, result)
    return result


def configure(args):
    from service.resistance import extract
    verify_bundle()
    previous = read(LOCAL / 'settings.json', {})
    skyrim = Path(prompt(args.skyrim, 'Skyrim folder containing SkyrimSE.exe', previous.get('skyrim'))).expanduser().resolve()
    client = Path(prompt(args.wow_client, 'WoW 1.12.1 folder containing WoW.exe', previous.get('wow_client'))).expanduser().resolve()
    cmangos = Path(prompt(args.cmangos, 'Extracted CMaNGOS Classic source folder or Unit.cpp', previous.get('cmangos'))).expanduser().resolve()
    address = args.address_library or previous.get('address_library') or skyrim / 'Data/SKSE/Plugins' / ADDRESS_LIBRARY
    if not Path(address).is_file() and not args.skyrim:
        address = prompt(None, 'Address Library .bin file in your mod manager')
    address = Path(address).expanduser().resolve()
    validate_installs(skyrim, client, address)
    table = extract(cmangos, pinned=True)
    native_client = str(client) if os.name == 'nt' else (args.wow_client_windows or
                     (previous.get('wow_client_windows') if previous.get('wow_client') == str(client) else None))
    if not native_client:
        if args.skyrim:
            raise ValueError('on Linux, supply --wow-client-windows with the path visible inside Wine/Proton, for example Z:/games/WoW')
        native_client = prompt(None, 'WoW client path visible inside Wine/Proton', 'Z:' + client.as_posix())
    mod_dir = Path(args.mod_dir or previous.get('mod_dir') or LOCAL / 'mod/WoWBridge').expanduser().resolve()
    # Keep game installations outside the staging operation.
    for protected in (skyrim, client, cmangos if cmangos.is_dir() else cmangos.parent):
        if mod_dir == protected or protected in mod_dir.parents or mod_dir in protected.parents:
            raise ValueError('the staged mod folder must be separate from the game and CMaNGOS installations')
    existing_ini = mod_dir / INI
    base = existing_ini.read_bytes() if existing_ini.exists() else (MOD / INI).read_bytes()
    install_tree(MOD, mod_dir, LOCAL / 'stage-manifest.json', overrides={INI: ini_bytes(base, native_client)}, allow_edited=(INI,))
    save(LOCAL / 'partial_resists.json', table)
    save(LOCAL / 'settings.json', {'schema': 1, 'skyrim': str(skyrim), 'wow_client': str(client),
                                 'wow_client_windows': native_client, 'cmangos': str(cmangos),
                                 'address_library': str(address), 'mod_dir': str(mod_dir)})
    print(f'Configured mod folder: {mod_dir}')
    print('Enable this folder, WoWBridge.esp and then WoWBridgeArmory.esp in your mod manager, or run: python setup.py install')
    print('Before starting Skyrim through SKSE, run: python setup.py service')


def settings():
    value = read(LOCAL / 'settings.json')
    if not isinstance(value, dict) or value.get('schema') != 1:
        raise ValueError('run setup.py configure first')
    return value


def check():
    from service.resistance import load
    from service.catalog import Catalog
    from service.progression import Progression
    from service.talents import Talents
    from service.perks import Perks
    verify_bundle()
    cfg = settings()
    validate_installs(Path(cfg['skyrim']), Path(cfg['wow_client']), Path(cfg['address_library']))
    load(LOCAL / 'partial_resists.json')
    cat = Catalog()
    Perks(cat, Progression())
    Talents()
    need(Path(cfg['mod_dir']) / PLUGINS[0], 'configured mod')
    need(Path(cfg['mod_dir']) / INI, 'configured mod INI')
    need(Path(cfg['mod_dir']) / 'SKSE/Plugins/WoWBridge.dll', 'configured plugin')
    print('Local dependencies, resistance table and gameplay data passed validation.')
    return cfg


def install(args):
    cfg = check()
    if args.plugins_txt:
        plugins = args.plugins_txt.expanduser().resolve()
    elif os.name == 'nt' and os.environ.get('LOCALAPPDATA'):
        plugins = Path(os.environ['LOCALAPPDATA']) / 'Skyrim Special Edition/Plugins.txt'
    else:
        raise ValueError('direct install needs --plugins-txt pointing to the Plugins.txt used by Skyrim; mod-manager profiles manage their own plugin list')
    old = read(LOCAL / 'install-manifest.json', {})
    if old.get('plugins_path') and old['plugins_path'] != str(plugins):
        raise ValueError('uninstall before changing the tracked Plugins.txt location')
    m = install_tree(Path(cfg['mod_dir']), Path(cfg['skyrim']) / 'Data', LOCAL / 'install-manifest.json')
    m['plugins_path'] = str(plugins)
    before = plugins.read_bytes() if plugins.exists() else b''
    lines = before.decode('utf-8-sig').splitlines()
    listed = {line.lstrip('*').strip().lower() for line in lines}
    shipped = [PLUGINS[0]] + [plugin for plugin in PLUGINS[1:] if (Path(cfg['mod_dir']) / plugin).is_file()]   # an older mod: none
    missing = [plugin for plugin in shipped if plugin.lower() not in listed]   # appended in load order, after the rest
    if missing:
        newline = b'\r\n' if b'\r\n' in before else b'\n'
        appended = before + (newline if before and not before.endswith((b'\n', b'\r')) else b'') + \
            b''.join(b'*' + plugin.encode() + newline for plugin in missing)
        if m.get('plugin_added'):   # an upgrade: entries an earlier install added stay ours, the file before them its original
            m.update(plugins_added=m.get('plugins_added', ['WoWBridge.esp']) + missing,
                     plugins_installed_sha256=hashlib.sha256(appended).hexdigest())
        else:
            m.update(plugin_added=True, plugins_added=missing, plugins_original_hex=before.hex(), plugins_existed=plugins.exists(),
                     plugins_installed_sha256=hashlib.sha256(appended).hexdigest())
        # Record the added entry before the atomic append so a crash cannot
        # make a retry mistake our entry for a pre-existing user's entry.
        save(LOCAL / 'install-manifest.json', m)
        write(plugins, appended)
    for plugin in shipped:
        if plugin.lower() in listed and not any(line.strip().lower() == '*' + plugin.lower() for line in lines):
            print(f'{plugin} is listed but disabled. Enable it in your plugin manager.')
    save(LOCAL / 'install-manifest.json', m)
    print('Installed WoWBridge into Skyrim Data. Launch Skyrim through SKSE after starting the service.')


def remove_tracked(path):
    m = read(path)
    if not m:
        return 0
    target = Path(m['target'])
    kept = {}
    for name, expected in m['files'].items():
        dst = target / safe_relative(name)
        if not dst.exists():
            continue
        if (any(p.is_symlink() for p in [dst, *dst.parents]) or
                digest(dst) not in {expected, m.get('pending_files', {}).get(name)}):
            kept[name] = expected
            print(f'Kept modified file: {name}')
            continue
        dst.unlink()
    directories = []
    for name in sorted(m.get('directories', []), key=lambda value: len(Path(value).parts), reverse=True):
        directory = target / safe_relative(name)
        if directory.is_dir() and not any(p.is_symlink() for p in [directory, *directory.parents]):
            if not any(directory.iterdir()):
                directory.rmdir()
            else:
                directories.append(name)  # a kept file may be removed by a later uninstall
    if m.get('plugin_added'):
        plugins = Path(m['plugins_path'])
        if plugins.exists():
            if digest(plugins) == m.get('plugins_installed_sha256'):
                if m.get('plugins_existed', True):
                    write(plugins, bytes.fromhex(m['plugins_original_hex']))
                else:
                    plugins.unlink()
            else:
                raw = plugins.read_bytes()
                prefix = b'\xef\xbb\xbf' if raw.startswith(b'\xef\xbb\xbf') else b''
                lines = raw[len(prefix):].splitlines(keepends=True)
                added = {plugin.lower() for plugin in m.get('plugins_added', ['WoWBridge.esp'])}
                lines = [line for line in lines if line.decode('utf-8').strip().lstrip('*').lower() not in added]
                write(plugins, prefix + b''.join(lines))
    pending = {name: value for name, value in m.get('pending_files', {}).items() if name in kept}
    save(path, {**m, 'files': kept, 'pending_files': pending, 'directories': directories, 'plugin_added': False})
    return len(kept)


def uninstall():
    kept = remove_tracked(LOCAL / 'install-manifest.json')
    kept += remove_tracked(LOCAL / 'stage-manifest.json')
    print(f'WoWBridge installation removed. {kept} modified files kept. Local settings and logs kept in local/.')


def service(trace=False):
    check()
    with socket.socket() as probe:
        probe.settimeout(0.5)
        if probe.connect_ex(('127.0.0.1', 7878)) == 0:
            raise ValueError('port 7878 is already in use; close the existing service before starting this one')
    print('Starting the gameplay service on 127.0.0.1:7878. Keep this window open while playing.', flush=True)
    command = [sys.executable, '-m', 'service.server', '--resist-table', str(LOCAL / 'partial_resists.json')]
    if trace:
        command += ['--trace', str(LOCAL / 'service_trace.jsonl')]
    return subprocess.call(command, cwd=RUNTIME)


def main():
    if sys.version_info < (3, 10):
        raise ValueError('Python 3.10 or newer is required')
    ap = argparse.ArgumentParser(description=__doc__)
    subs = ap.add_subparsers(dest='command')
    conf = subs.add_parser('configure', help='select existing local dependencies and stage the mod')
    for name in ('skyrim', 'wow-client', 'cmangos', 'address-library', 'mod-dir'):
        conf.add_argument('--' + name, type=Path)
    conf.add_argument('--wow-client-windows', help='client path visible inside Wine/Proton (Linux only)')
    inst = subs.add_parser('install', help='install the staged mod directly into Skyrim Data')
    inst.add_argument('--plugins-txt', type=Path)
    for name in ('check', 'service', 'uninstall'):
        command = subs.add_parser(name)
        if name == 'service':
            command.add_argument('--trace', action='store_true', help='append detailed request/response logs in local/')
    args = ap.parse_args()
    if args.command is None:
        args = conf.parse_args([])
        args.command = 'configure'
    if args.command == 'configure':
        configure(args)
    elif args.command == 'install':
        install(args)
    elif args.command == 'service':
        return service(trace=args.trace)
    elif args.command == 'check':
        check()
    else:
        uninstall()
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, configparser.Error) as exc:
        print(f'Setup: {exc}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print('\nStopped.')
        sys.exit(130)
