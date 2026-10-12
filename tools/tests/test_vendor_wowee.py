"""Refresh the real upstream subset in isolation; never overwrite the checkout."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import patch_wowee
import vendor_wowee


class VendorRefreshTests(unittest.TestCase):
    @unittest.skipUnless(vendor_wowee.SRC.is_dir(), 'project upstream checkout is required')
    def test_real_refresh_reproduces_committed_runtime_and_is_idempotent(self):
        # The already deployed tree is the oracle, including earlier runtime
        # fixes that used to be lost even when all old patch functions ran.
        with tempfile.TemporaryDirectory(prefix='vendor-refresh-', dir=ROOT / 'build') as tmp:
            destination = Path(tmp)
            with patch.object(vendor_wowee, 'DST', destination), contextlib.redirect_stdout(io.StringIO()):
                vendor_wowee.main()
            generated = destination / 'wowee'
            expected = ROOT / 'wowcore/third_party/wowee'
            for relative in vendor_wowee.WOWEE:
                with self.subTest(file=relative):
                    self.assertEqual((generated / relative).read_bytes(), (expected / relative).read_bytes())
            before = {relative: (generated / relative).read_bytes() for relative in vendor_wowee.WOWEE}
            with contextlib.redirect_stdout(io.StringIO()):
                patch_wowee.apply_all(generated)
            self.assertEqual(before, {relative: (generated / relative).read_bytes() for relative in before})

    def test_isolated_patch_root_is_restored_after_failure(self):
        before = patch_wowee.WW, patch_wowee.ENGINE
        with patch.object(patch_wowee, 'patch_engine', side_effect=RuntimeError('upstream anchor changed')):
            with self.assertRaisesRegex(RuntimeError, 'upstream anchor changed'):
                patch_wowee.apply_all(ROOT / 'build/unused-upstream')
        self.assertEqual((patch_wowee.WW, patch_wowee.ENGINE), before)


if __name__ == '__main__':
    unittest.main()
