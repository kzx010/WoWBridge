import json
import unittest
from pathlib import Path
from data.build_mounts import build, REPO


class MountCatalogTest(unittest.TestCase):
    def test_export_matches_pinned_client_and_creature_models(self):
        catalog = json.loads((REPO / 'data/mounts.json').read_text())
        self.assertEqual(build(), catalog)
        self.assertEqual(len(catalog['mounts']), 21)
        self.assertEqual(len({r['key'] for r in catalog['mounts']}), 21)
        saber = next(r for r in catalog['mounts'] if r['spell'] == 23221)
        self.assertEqual(saber['display'], 14331)  # spell misc is creature 14556, not display 14556
        self.assertEqual(saber['creature'], 14556)
