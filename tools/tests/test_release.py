"""The tester bundle carries every data file the service reads."""
import re
import unittest
from pathlib import Path

from tools import release

REPO = Path(__file__).resolve().parents[2]


class ReleaseData(unittest.TestCase):
    def test_bundle_ships_every_service_data_file(self):
        read = {m for path in (REPO / 'service').glob('*.py')
                for m in re.findall(r"data/([a-z_]+\.json)", path.read_text())}
        self.assertTrue(read)
        self.assertEqual(read - set(release.DATA), set())


if __name__ == '__main__':
    unittest.main()
