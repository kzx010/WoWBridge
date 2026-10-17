import os, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / 'deploy.py'


class DeployRoundTrip(unittest.TestCase):
    def run_tool(self, *args, ok=True):
        r = subprocess.run([sys.executable, TOOL, *args], env=self.env, capture_output=True, text=True)
        self.assertEqual(r.returncode == 0, ok, r.stdout + r.stderr)
        return r.stdout + r.stderr

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.game, self.plugins = t / 'game', t / 'Plugins.txt'
        (self.game / 'Data').mkdir(parents=True)
        (self.game / 'Data/Skyrim.esm').write_bytes(b'vanilla')
        self.plugins.write_text('*ccBGSSSE001-Fish.esm\n')
        self.env = dict(os.environ, WB_GAME=str(self.game), WB_PLUGINS_TXT=str(self.plugins), WB_MANIFEST=str(t / 'm.json'))

    def tearDown(self):
        self.tmp.cleanup()

    def test_install_uninstall_restores_tree(self):
        self.run_tool('install', '--only', 'skse,addrlib,mod')
        self.assertTrue((self.game / 'skse64_loader.exe').exists())
        self.assertTrue((self.game / 'Data/SKSE/Plugins/versionlib-1-7-104-0.bin').exists())
        self.assertTrue((self.game / 'Data/SKSE/Plugins/WoWBridge.dll').exists())
        ini = (self.game / 'Data/SKSE/Plugins/WoWBridge.ini').read_text()
        client = TOOL.parents[2] / 'WoW/client'
        if (client / 'Data').is_dir():   # this project's own client, as Windows (from WSL) or Wine sees it
            sys.path.insert(0, str(TOOL.parent))
            import winhost
            where = winhost.win_path(client.resolve()) if winhost.IS_WSL else 'Z:' + str(client).replace('/', '\\')
            self.assertIn('\nclient=' + where + '\n', ini)
        self.assertIn('ok', self.run_tool('status'))
        self.run_tool('uninstall')
        left = sorted(str(p.relative_to(self.game)) for p in self.game.rglob('*'))
        self.assertEqual(left, ['Data', 'Data/Skyrim.esm'])
        self.assertEqual(self.plugins.read_text(), '*ccBGSSSE001-Fish.esm\n')

    def test_preserves_pre_existing_user_configuration(self):
        ini = self.game / 'Data/SKSE/Plugins/WoWBridge.ini'
        ini.parent.mkdir(parents=True)
        ini.write_text('[WoW]\nroot=C:/Tester/WoW\n')
        original = ini.read_bytes()
        self.run_tool('install', '--only', 'mod')
        self.assertEqual(ini.read_bytes(), original)
        self.run_tool('uninstall')
        self.assertEqual(ini.read_bytes(), original)

    def test_upgrade_keeps_edited_seed_configuration(self):
        self.run_tool('install', '--only', 'mod')
        ini = self.game / 'Data/SKSE/Plugins/WoWBridge.ini'
        ini.write_text('[WoW]\nroot=C:/Tester/ExistingClientRoot\n')
        original = ini.read_bytes()
        self.run_tool('install', '--only', 'mod')
        self.assertEqual(ini.read_bytes(), original)
        self.assertIn('kept (modified', self.run_tool('uninstall'))
        self.assertEqual(ini.read_bytes(), original)

    def test_refuses_untracked_overwrite(self):
        (self.game / 'skse64_loader.exe').write_bytes(b'someone else')
        self.assertIn('refusing to overwrite', self.run_tool('install', '--only', 'skse', ok=False))
        self.assertEqual((self.game / 'skse64_loader.exe').read_bytes(), b'someone else')

    def test_keeps_modified_files_on_uninstall(self):
        self.run_tool('install', '--only', 'addrlib')
        f = self.game / 'Data/SKSE/Plugins/versionlib-1-7-104-0.bin'
        f.write_bytes(b'edited')
        self.assertIn('kept (modified', self.run_tool('uninstall'))
        self.assertEqual(f.read_bytes(), b'edited')


    def test_reinstall_notices_a_same_size_edit(self):
        # The manifest's size+mtime stamp skips rehashing unchanged files; an edit still changes the mtime.
        self.run_tool('install', '--only', 'addrlib')
        f = self.game / 'Data/SKSE/Plugins/versionlib-1-7-104-0.bin'
        data = bytearray(f.read_bytes()); data[0] ^= 1
        st = f.stat(); f.write_bytes(bytes(data)); os.utime(f, ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000_000))
        self.assertIn('modified outside deploy.py', self.run_tool('install', '--only', 'addrlib', ok=False))


    def test_trusting_deploy_skips_unchanged_installs(self):
        self.run_tool('install', '--only', 'addrlib')
        f = self.game / 'Data/SKSE/Plugins/versionlib-1-7-104-0.bin'
        f.write_bytes(b'edited outside')   # a developer deploy does not look again at what it installed unchanged
        self.assertIn('installed 0 files', self.run_tool('install', '--only', 'addrlib', '--trust-unchanged'))
        self.assertIn('modified outside deploy.py', self.run_tool('install', '--only', 'addrlib', ok=False))


class PluginOrder(unittest.TestCase):
    """WoWBridgeArmory.esp has WoWBridge.esp as a master: deploy enables it after that, tracked for uninstall."""
    def setUp(self):
        import importlib.util
        from unittest.mock import patch
        self.tmp = tempfile.TemporaryDirectory()
        self.plugins = Path(self.tmp.name) / 'Plugins.txt'
        spec = importlib.util.spec_from_file_location('deploy_under_test', TOOL)
        self.deploy = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, WB_PLUGINS_TXT=str(self.plugins)):
            spec.loader.exec_module(self.deploy)
        self.deploy.PLUGINS_TXT = self.plugins

    def tearDown(self):
        self.tmp.cleanup()

    def add(self, m, *names):
        for name in names:
            self.deploy.add_plugin(m, name)
        return self.plugins.read_text()

    def test_fresh_and_beside_an_existing_master(self):
        m = {'files': {}, 'plugins': []}
        self.assertEqual(self.add(m, 'WoWBridge.esp', 'WoWBridgeArmory.esp'), '*WoWBridge.esp\n*WoWBridgeArmory.esp\n')
        self.assertEqual(m['plugins'], ['WoWBridge.esp', 'WoWBridgeArmory.esp'])
        self.plugins.write_text('*Other.esp\n*WoWBridge.esp\n*Later.esp\n')
        m = {'files': {}, 'plugins': []}
        self.assertEqual(self.add(m, 'WoWBridge.esp', 'WoWBridgeArmory.esp'), '*Other.esp\n*WoWBridge.esp\n*WoWBridgeArmory.esp\n*Later.esp\n')
        self.assertEqual(m['plugins'], ['WoWBridgeArmory.esp'])   # the user's own WoWBridge.esp line stays theirs

    def test_ours_before_its_master_moves_after_it(self):
        self.plugins.write_text('*WoWBridgeArmory.esp\n*Other.esp\n*WoWBridge.esp\n')
        m = {'files': {}, 'plugins': ['WoWBridgeArmory.esp']}
        self.assertEqual(self.add(m, 'WoWBridge.esp', 'WoWBridgeArmory.esp'), '*Other.esp\n*WoWBridge.esp\n*WoWBridgeArmory.esp\n')
    def test_users_invalid_order_is_rejected_without_edits(self):
        original = '*wowbridgearmory.ESP\n*WoWBridge.esp\n'
        self.plugins.write_text(original)
        m = {'files': {}, 'plugins': []}
        with self.assertRaisesRegex(SystemExit, 'move WoWBridgeArmory.esp after WoWBridge.esp.*retry'):
            self.add(m, 'WoWBridge.esp', 'WoWBridgeArmory.esp')
        self.assertEqual(self.plugins.read_text(), original)
        self.assertEqual(m['plugins'], [])

    def test_missing_master_rejects_users_entry_but_moves_ours(self):
        original = '*WoWBridgeArmory.esp\n*Other.esp\n'
        self.plugins.write_text(original)
        m = {'files': {}, 'plugins': []}
        with self.assertRaisesRegex(SystemExit, 'place WoWBridge.esp before WoWBridgeArmory.esp.*retry'):
            self.add(m, 'WoWBridge.esp', 'WoWBridgeArmory.esp')
        self.assertEqual(self.plugins.read_text(), original)
        self.assertEqual(m['plugins'], [])
        m['plugins'] = ['WoWBridgeArmory.esp']
        self.assertEqual(self.add(m, 'WoWBridge.esp', 'WoWBridgeArmory.esp'),
                         '*Other.esp\n*WoWBridge.esp\n*WoWBridgeArmory.esp\n')

    def test_invalid_order_is_rejected_before_install_copies_files(self):
        from unittest.mock import patch
        original = '*WoWBridgeArmory.esp\n*WoWBridge.esp\n'
        self.plugins.write_text(original)
        with patch.object(self.deploy, 'load_manifest', return_value={'files': {}, 'plugins': []}), \
                patch.object(self.deploy, 'DIST', Path(self.tmp.name)), \
                patch.object(self.deploy, 'stage_mod') as stage:
            for esp in self.deploy.PLUGINS:
                (Path(self.tmp.name) / esp).touch()
            with self.assertRaisesRegex(SystemExit, 'user-owned.*precedes its master'):
                self.deploy.install({'mod'})
            stage.assert_not_called()
        self.assertEqual(self.plugins.read_text(), original)


if __name__ == '__main__':
    unittest.main()
