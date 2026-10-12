"""World-paired named comparisons retain masks and reject missing UI states."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import fidelity_diff


class WorldPairedRegions(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='fidelity-mask-', dir=ROOT / 'build')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        evidence = self.root / 'evidence'
        evidence.mkdir()
        self.output = self.root / 'render'
        self.output.mkdir()
        Image.new('RGB', (8, 8), (20, 40, 60)).save(evidence / 'world.png')
        Image.new('RGBA', (8, 8), (100, 140, 180, 128)).save(self.output / 'probe.png')
        expected = np.full((8, 8, 3), (60, 90, 120), dtype=np.uint8)
        expected[0, 0] = 255  # Explicitly excluded; it must not affect any score.
        Image.fromarray(expected).save(evidence / 'ui.png')
        (evidence / 'state.lua').write_text('''WBOracleAPI = {
\t["frames_probe"] = {
\t\t[1] = "S|8|8",
\t\t[2] = "R|Observed|0|0|8|8|Texture|test",
\t},
}
''')
        self.spec = dict(reference='ui.png', state='probe', render_size=(8, 8), crop=None,
                         world='world.png', frames=('state.lua', 'probe'),
                         regions={'left': (0, 0, 4, 8), 'right': (4, 0, 8, 8)},
                         include=[(0, 0, 8, 8)], excluded=[(0, 0, 1, 1)])

    def compare(self):
        with patch.object(fidelity_diff, 'ROOT', self.root):
            return fidelity_diff.compare(self.spec, self.output)[0]

    def test_named_regions_composite_world_and_keep_exclusions(self):
        metrics = self.compare()
        self.assertEqual({k: v['pixels'] for k, v in metrics.items()},
                         {'interface': 63, 'left': 31, 'right': 32})
        for value in metrics.values():
            self.assertEqual(value['match'], 1.0)
            self.assertEqual(value['mean_error'], 0.0)

    def test_existing_world_cases_still_have_one_aggregate_score(self):
        self.spec['regions'] = {}
        self.assertEqual(set(self.compare()), {'interface'})

    def test_missing_visible_frame_is_rejected_before_scoring(self):
        self.spec['required_frames'] = ['F|GameTooltip|']
        dump = self.output / 'probe_frames.txt'
        dump.write_text('F|BuffButton0|0|0|8|8\n')
        with self.assertRaisesRegex(ValueError, 'Missing visible frame.*GameTooltip'):
            self.compare()
        dump.write_text(dump.read_text() + 'F|GameTooltip|0|0|8|8\n')
        self.assertEqual(self.compare()['interface']['match'], 1.0)

    def test_empty_named_region_is_rejected(self):
        self.spec['regions'] = {'outside': (9, 9, 10, 10)}
        with self.assertRaisesRegex(ValueError, 'No comparable pixels.*outside'):
            self.compare()

    def test_opaque_overlay_keeps_controls_over_a_portrait(self):
        self.spec['regions'] = {}
        state = self.root / 'evidence/state.lua'
        state.write_text(state.read_text().replace('\n\t},',
            '\n\t\t[3] = "R|MerchantFramePortrait|0|0|4|8|Texture|portrait",\n\t},'))
        self.assertEqual(self.compare()['interface']['pixels'], 32)
        self.spec['exclude_portraits'] = False
        self.assertEqual(self.compare()['interface']['pixels'], 63)

    def test_wrong_native_tooltip_caption_is_rejected(self):
        self.spec['match_text_frames'] = ['TooltipText']
        state = self.root / 'evidence/state.lua'
        state.write_text(state.read_text().replace('\n\t},',
            '\n\t\t[3] = "R|TooltipText|0|0|8|8|FontString|Fonts|12||0|native caption",\n\t},'))
        dump = self.output / 'probe_frames.txt'
        row = 'R|TooltipText|0|0|8|8|FontString|Fonts|12||0|'
        dump.write_text(row + 'wrong caption\n')
        with self.assertRaisesRegex(ValueError, 'Wrong native text.*TooltipText'):
            self.compare()
        dump.write_text(row + 'native caption\n')
        self.assertEqual(self.compare()['interface']['match'], 1.0)


class ReferenceGroups(unittest.TestCase):
    def states(self, group):
        result = subprocess.run([sys.executable, str(ROOT / 'tools/fidelity_diff.py'),
                                 group, '--states'], capture_output=True, text=True, check=True)
        return {name for line in result.stdout.splitlines() for name in line.split()[2:]}

    def test_clean_scale_group_keeps_exactly_four_original_states(self):
        self.assertEqual(self.states('--clientref'),
                         {'clientref_' + state + '_scale' + scale
                          for state in ('hud_idle', 'spellbook') for scale in ('080', '100')})

    def test_merchant_interaction_group_is_isolated_from_panel_baselines(self):
        self.assertEqual(self.states('--merchant-interactions'),
                         {'clientref_merchant_' + state + '_verified' for state in
                          ('hover','pressed','quantity_min','quantity_middle','quantity_max')})
        self.assertEqual(len(self.states('--panels')), 9)

    def test_character_and_buff_groups_are_separate(self):
        self.assertEqual(self.states('--characters'),
                         {'clientref_character_' + side + '_verified'
                          for side in ('front', 'back', 'left', 'right')})
        self.assertEqual(self.states('--buffs'),
                         {'clientref_buff_motw_native', 'clientref_buff_motw_tooltip_native'})


if __name__ == '__main__':
    unittest.main()
