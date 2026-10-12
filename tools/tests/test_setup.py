import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class LocalSetup(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.bundle = self.root / 'tester bundle'
        self.bundle.mkdir()
        shutil.copyfile(REPO / 'setup.py', self.bundle / 'setup.py')
        runtime = self.bundle / 'runtime'
        for name in ('service', 'data'):
            (runtime / name).mkdir(parents=True)
        for p in (REPO / 'service').glob('*.py'):
            shutil.copyfile(p, runtime / 'service' / p.name)
        for name in ('progression', 'talents', 'perks', 'talent_catalog', 'perk_catalog'):
            shutil.copyfile(REPO / f'data/{name}.json', runtime / f'data/{name}.json')
        (runtime / 'build').mkdir()
        shutil.copyfile(REPO / 'build/catalog.json', runtime / 'build/catalog.json')
        self.mod = self.bundle / 'mod'
        self.put(self.mod / 'WoWBridge.esp', b'mod esp')
        self.put(self.mod / 'SKSE/Plugins/WoWBridge.dll', b'plugin fixture')
        self.put(self.mod / 'SKSE/Plugins/WoWBridge.ini', b'[WoW]\nroot=WoW\ncursor=1\n[Test]\nautorun=0\n')
        manifest = {p.relative_to(self.bundle).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in self.bundle.rglob('*') if p.is_file()}
        self.put(self.bundle / 'release-manifest.json', json.dumps({'schema': 1, 'files': manifest}).encode())
        self.game = self.root / 'Skyrim Install'
        for name in ('SkyrimSE.exe', 'Data/Skyrim.esm', 'skse64_loader.exe', 'skse64_1_7_104.dll',
                     'Data/SKSE/Plugins/versionlib-1-7-104-0.bin'):
            self.put(self.game / name, b'installed dependency')
        self.client = self.root / 'WoW Client'
        for name in ('WoW.exe', 'Data/dbc.MPQ', 'Data/fonts.MPQ', 'Data/interface.MPQ', 'Data/misc.MPQ',
                     'Data/model.MPQ', 'Data/sound.MPQ', 'Data/speech.MPQ', 'Data/terrain.MPQ', 'Data/texture.MPQ', 'Data/wmo.MPQ'):
            self.put(self.client / name, b'existing client')
        self.source = self.root / 'CMaNGOS Source'
        self.put(self.source / 'src/game/Entities/Unit.cpp',
                 (REPO.parent / 'WoW/cmangos/mangos-classic/src/game/Entities/Unit.cpp').read_bytes())
        self.plugins = self.root / 'Profile/Plugins.txt'
        self.put(self.plugins, b'*OtherMod.esp\n')
        self.local = self.bundle / 'local'
        self.before = {p.relative_to(self.game): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def put(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def run_tool(self, *args, ok=True):
        out = subprocess.run([sys.executable, str(self.bundle / 'setup.py'), *map(str, args)],
                             cwd=self.root, capture_output=True, text=True, timeout=30)
        self.assertEqual(out.returncode == 0, ok, out.stdout + out.stderr)
        return out.stdout + out.stderr

    def configure(self, *args, ok=True):
        self.run_tool('configure', '--skyrim', self.game, '--wow-client', self.client, '--cmangos', self.source,
                      '--wow-client-windows', 'Z:/Games/WoW Client', *args, ok=ok)

    def test_configure_from_other_working_directory_and_check_without_source(self):
        self.configure()
        ini = (self.local / 'mod/WoWBridge/SKSE/Plugins/WoWBridge.ini').read_text()
        expected = str(self.client) if os.name == 'nt' else 'Z:/Games/WoW Client'
        self.assertIn('client=' + expected, ini)
        self.assertIn('autorun=0', ini)
        table = (self.local / 'partial_resists.json').read_text()
        self.assertNotIn(str(self.source), table)
        self.assertEqual(len(json.loads(table)['rows']), 101)
        shutil.rmtree(self.source)
        self.assertIn('passed validation', self.run_tool('check'))

    def test_install_and_uninstall_restore_only_owned_game_files(self):
        self.configure()
        self.run_tool('install', '--plugins-txt', self.plugins)
        self.assertIn('*WoWBridge.esp', self.plugins.read_text())
        self.assertTrue((self.game / 'Data/SKSE/Plugins/WoWBridge.dll').exists())
        self.run_tool('uninstall')
        after = {p.relative_to(self.game): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        self.assertEqual(after, self.before)
        self.assertEqual(self.plugins.read_bytes(), b'*OtherMod.esp\n')
        self.assertFalse((self.local / 'mod/WoWBridge/WoWBridge.esp').exists())
        self.assertTrue(self.client.exists())
        self.assertTrue(self.source.exists())

    def test_uninstall_preserves_changed_and_unrelated_files(self):
        self.configure()
        self.run_tool('install', '--plugins-txt', self.plugins)
        changed = self.game / 'Data/SKSE/Plugins/WoWBridge.ini'
        changed.write_bytes(b'user settings')
        other = self.game / 'Data/another mod.txt'
        other.write_bytes(b'unrelated')
        self.assertIn('Kept modified file', self.run_tool('uninstall'))
        self.assertEqual(changed.read_bytes(), b'user settings')
        self.assertEqual(other.read_bytes(), b'unrelated')

    def test_uninstall_preserves_identical_preexisting_file_and_plugin_entry(self):
        self.configure()
        esp = self.game / 'Data/WoWBridge.esp'
        esp.write_bytes(b'mod esp')
        self.plugins.write_bytes(b'*OtherMod.esp\n*WoWBridge.esp\n')
        self.run_tool('install', '--plugins-txt', self.plugins)
        self.run_tool('uninstall')
        self.assertEqual(esp.read_bytes(), b'mod esp')
        self.assertIn('*WoWBridge.esp', self.plugins.read_text())

    def test_conflict_is_reported_before_any_game_copy(self):
        self.configure()
        dll = self.game / 'Data/SKSE/Plugins/WoWBridge.dll'
        dll.write_bytes(b'untracked plugin')
        out = self.run_tool('install', '--plugins-txt', self.plugins, ok=False)
        self.assertIn('refusing to overwrite', out)
        self.assertFalse((self.game / 'Data/WoWBridge.esp').exists())
        self.assertEqual(dll.read_bytes(), b'untracked plugin')
        self.assertEqual(self.plugins.read_bytes(), b'*OtherMod.esp\n')

    def test_uninstall_restores_plugin_file_encoding_and_missing_final_newline(self):
        self.configure()
        before = b'\xef\xbb\xbf*OtherMod.esp\r\n*SecondMod.esp'
        self.plugins.write_bytes(before)
        self.run_tool('install', '--plugins-txt', self.plugins)
        self.run_tool('uninstall')
        self.assertEqual(self.plugins.read_bytes(), before)

    def test_uninstall_preserves_preexisting_empty_directories(self):
        self.configure()
        existing = self.game / 'Data/SKSE/Plugins/WoWBridge'
        existing.mkdir()
        self.run_tool('install', '--plugins-txt', self.plugins)
        self.run_tool('uninstall')
        self.assertTrue(existing.is_dir())

    def test_uninstall_preserves_plugin_entries_added_after_install(self):
        self.configure()
        self.run_tool('install', '--plugins-txt', self.plugins)
        self.plugins.write_bytes(self.plugins.read_bytes() + b'*LaterMod.esp\n')
        self.run_tool('uninstall')
        self.assertEqual(self.plugins.read_bytes(), b'*OtherMod.esp\n*LaterMod.esp\n')

    def test_uninstall_removes_plugin_file_created_by_setup(self):
        self.configure()
        self.plugins.unlink()
        self.run_tool('install', '--plugins-txt', self.plugins)
        self.run_tool('uninstall')
        self.assertFalse(self.plugins.exists())

    def test_repeated_uninstall_keeps_later_created_empty_directory(self):
        self.configure()
        stage = self.local / 'mod/WoWBridge'
        self.run_tool('uninstall')
        stage.mkdir(parents=True)
        self.run_tool('uninstall')
        self.assertTrue(stage.is_dir())

    def test_configure_preserves_other_ini_preferences(self):
        self.configure()
        ini = self.local / 'mod/WoWBridge/SKSE/Plugins/WoWBridge.ini'
        ini.write_text(ini.read_text().replace('cursor=1', 'cursor=0') + '\n[Custom]\nvalue=100%\n')
        self.configure()
        self.assertIn('cursor=0', ini.read_text())
        self.assertIn('value=100%', ini.read_text())

    def test_missing_dependency_changes_no_game_files(self):
        (self.client / 'Data/fonts.MPQ').unlink()
        self.configure(ok=False)
        self.assertFalse(self.local.exists())
        after = {p.relative_to(self.game): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        self.assertEqual(after, self.before)

    def test_wrong_source_commit_is_rejected_before_staging(self):
        (self.source / 'src/game/Entities/Unit.cpp').write_text('wrong version')
        self.configure(ok=False)
        self.assertFalse(self.local.exists())

    def test_bundle_corruption_is_rejected(self):
        (self.mod / 'WoWBridge.esp').write_bytes(b'corrupted')
        self.configure(ok=False)
        self.assertFalse(self.local.exists())

    def test_windows_crlf_source_is_accepted(self):
        path = self.source / 'src/game/Entities/Unit.cpp'
        path.write_bytes(path.read_bytes().replace(b'\n', b'\r\n'))
        self.configure()


if __name__ == '__main__':
    unittest.main()
