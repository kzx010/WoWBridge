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
sys.path.insert(0, str(REPO / 'tools'))
from release import DATA as RELEASE_DATA  # noqa: E402


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
        for name in RELEASE_DATA:   # exactly what the tester bundle ships, so setup's check proves the bundle complete
            shutil.copyfile(REPO / 'data' / name, runtime / 'data' / name)
        (runtime / 'build').mkdir()
        shutil.copyfile(REPO / 'build/catalog.json', runtime / 'build/catalog.json')
        self.mod = self.bundle / 'mod'
        self.put(self.mod / 'WoWBridge.esp', b'mod esp')
        self.put(self.mod / 'WoWBridgeArmory.esp', b'armory esp')
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
        self.assertEqual(self.plugins.read_text(), '*OtherMod.esp\n*WoWBridge.esp\n*WoWBridgeArmory.esp\n')   # its master first
        self.assertTrue((self.game / 'Data/SKSE/Plugins/WoWBridge.dll').exists())
        self.assertTrue((self.game / 'Data/WoWBridgeArmory.esp').exists())
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




class InterruptedInstall(unittest.TestCase):
    def setUp(self):
        import importlib.util
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        spec = importlib.util.spec_from_file_location('setup_upgrade_test', REPO / 'setup.py')
        self.setup = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.setup)
        self.source = self.root / 'payload'
        self.target = self.root / 'game' / 'Data'
        self.manifest = self.root / 'local' / 'install.json'
        self.source.mkdir()
        self.old = {'nested/a.bin': b'old a', 'nested/b.bin': b'old b'}
        self.new = {'nested/a.bin': b'new a', 'nested/b.bin': b'new b'}
        self.payload(self.old)
        self.setup.install_tree(self.source, self.target, self.manifest)

    def tearDown(self):
        self.tmp.cleanup()

    def payload(self, values):
        for name, raw in values.items():
            p = self.source / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(raw)

    def interrupted_upgrade(self):
        from unittest.mock import patch
        self.payload(self.new)
        write = self.setup.write
        def disk_full(path, raw):
            if Path(path) == self.target / 'nested/b.bin':
                raise OSError('simulated disk full during upgrade')
            return write(path, raw)
        with patch.object(self.setup, 'write', side_effect=disk_full):
            with self.assertRaisesRegex(OSError, 'simulated disk full'):
                self.setup.install_tree(self.source, self.target, self.manifest)
        self.assertEqual((self.target / 'nested/a.bin').read_bytes(), b'new a')
        self.assertEqual((self.target / 'nested/b.bin').read_bytes(), b'old b')

    def test_retry_after_partial_upgrade_accepts_both_written_and_unwritten_owned_files(self):
        self.interrupted_upgrade()
        self.setup.install_tree(self.source, self.target, self.manifest)
        for name, raw in self.new.items():
            self.assertEqual((self.target / name).read_bytes(), raw)
        self.assertEqual(self.setup.remove_tracked(self.manifest), 0)
        self.assertFalse((self.target / 'nested').exists())

    def test_uninstall_partial_upgrade_removes_owned_versions_and_preserves_user_edit(self):
        self.interrupted_upgrade()
        edited = self.target / 'nested/a.bin'
        edited.write_bytes(b'user changed this file')
        self.assertEqual(self.setup.remove_tracked(self.manifest), 1)
        self.assertEqual(edited.read_bytes(), b'user changed this file')
        self.assertFalse((self.target / 'nested/b.bin').exists())

    def test_user_edit_during_partial_upgrade_refuses_retry_before_any_copy(self):
        self.interrupted_upgrade()
        edited = self.target / 'nested/b.bin'
        edited.write_bytes(b'user changed this file')
        before = (self.target / 'nested/a.bin').read_bytes()
        with self.assertRaisesRegex(ValueError, 'modified or untracked'):
            self.setup.install_tree(self.source, self.target, self.manifest)
        self.assertEqual(edited.read_bytes(), b'user changed this file')
        self.assertEqual((self.target / 'nested/a.bin').read_bytes(), before)


    def test_completed_retry_stops_owning_prior_content_versions(self):
        self.interrupted_upgrade()
        self.setup.install_tree(self.source, self.target, self.manifest)
        edited = self.target / 'nested/a.bin'
        edited.write_bytes(self.old['nested/a.bin'])
        self.assertEqual(self.setup.remove_tracked(self.manifest), 1)
        self.assertEqual(edited.read_bytes(), self.old['nested/a.bin'])

    def test_another_upgrade_can_resume_an_already_interrupted_upgrade(self):
        from unittest.mock import patch
        self.interrupted_upgrade()
        third = {name: raw.replace(b'new', b'third') for name, raw in self.new.items()}
        self.payload(third)
        write = self.setup.write
        def interrupted_again(path, raw):
            if Path(path) == self.target / 'nested/b.bin':
                raise OSError('interrupted again')
            return write(path, raw)
        with patch.object(self.setup, 'write', side_effect=interrupted_again):
            with self.assertRaisesRegex(OSError, 'interrupted again'):
                self.setup.install_tree(self.source, self.target, self.manifest)
        self.setup.install_tree(self.source, self.target, self.manifest)
        for name, raw in third.items():
            self.assertEqual((self.target / name).read_bytes(), raw)

    def test_removed_payload_file_keeps_its_interrupted_ownership_until_uninstall(self):
        self.interrupted_upgrade()
        (self.source / 'nested/b.bin').unlink()
        self.setup.install_tree(self.source, self.target, self.manifest)
        self.assertEqual(self.setup.remove_tracked(self.manifest), 0)
        self.assertFalse((self.target / 'nested').exists())

    def test_failed_final_manifest_write_leaves_recoverable_ownership(self):
        from unittest.mock import patch
        self.payload(self.new)
        save = self.setup.save
        saves = 0
        def fail_commit(path, value):
            nonlocal saves
            saves += 1
            if saves == 2:
                raise OSError('final manifest write failed')
            return save(path, value)
        with patch.object(self.setup, 'save', side_effect=fail_commit):
            with self.assertRaisesRegex(OSError, 'final manifest write failed'):
                self.setup.install_tree(self.source, self.target, self.manifest)
        self.setup.install_tree(self.source, self.target, self.manifest)
        self.assertEqual(self.setup.remove_tracked(self.manifest), 0)
        self.assertFalse((self.target / 'nested').exists())


    def interrupted_plugin_append(self, after_write=True):
        from contextlib import ExitStack
        from types import SimpleNamespace
        from unittest.mock import patch
        self.plugin_local = self.root / 'plugin-local'
        self.plugin_game = self.root / 'plugin-game'
        self.plugin_list = self.root / 'profile' / 'Plugins.txt'
        self.plugin_list.parent.mkdir()
        original = b'\xef\xbb\xbf*OtherMod.esp\r\n'
        self.plugin_list.write_bytes(original)
        stack = ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(patch.object(self.setup, 'LOCAL', self.plugin_local))
        stack.enter_context(patch.object(self.setup, 'check', return_value={
            'skyrim': str(self.plugin_game), 'mod_dir': str(self.source)}))
        write = self.setup.write
        def interrupted(path, raw):
            if Path(path) == self.plugin_list:
                if after_write:
                    write(path, raw)
                raise OSError('plugin append interrupted')
            return write(path, raw)
        with patch.object(self.setup, 'write', side_effect=interrupted):
            with self.assertRaisesRegex(OSError, 'plugin append interrupted'):
                self.setup.install(SimpleNamespace(plugins_txt=self.plugin_list))
        return original

    def test_uninstall_recovers_interruption_after_plugin_entry_was_appended(self):
        original = self.interrupted_plugin_append()
        self.assertIn(b'*WoWBridge.esp', self.plugin_list.read_bytes())
        self.setup.uninstall()
        self.assertEqual(self.plugin_list.read_bytes(), original)

    def test_retry_does_not_adopt_its_interrupted_plugin_entry_as_preexisting(self):
        from types import SimpleNamespace
        original = self.interrupted_plugin_append()
        self.setup.install(SimpleNamespace(plugins_txt=self.plugin_list))
        self.setup.uninstall()
        self.assertEqual(self.plugin_list.read_bytes(), original)

    def test_failed_plugin_append_preserves_existing_plugin_list(self):
        original = self.interrupted_plugin_append(after_write=False)
        self.setup.uninstall()
        self.assertEqual(self.plugin_list.read_bytes(), original)


    def test_later_uninstall_cleans_owned_directories_after_modified_file_is_restored(self):
        changed = self.target / 'nested/a.bin'
        changed.write_bytes(b'user edit')
        self.assertEqual(self.setup.remove_tracked(self.manifest), 1)
        self.assertTrue(changed.exists())
        changed.write_bytes(self.old['nested/a.bin'])
        self.assertEqual(self.setup.remove_tracked(self.manifest), 0)
        self.assertFalse((self.target / 'nested').exists())


if __name__ == '__main__':
    unittest.main()
