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


if __name__ == '__main__':
    unittest.main()
