import json
from pathlib import Path
import tempfile
import unittest

from service import formulas as F, resistance


class ResistanceInputs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.rows = [(10000 - i * 100, 0, 0, 0, i * 100) for i in range(101)]

    def tearDown(self):
        self.tmp.cleanup()

    def source(self, rows=None):
        text = ',\n'.join('{{' + ', '.join(map(str, r)) + '}}' for r in (rows or self.rows))
        p = self.root / 'src/game/Entities/Unit.cpp'
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('static inline SpellPartialResistDistribution InitSpellPartialResistDistribution() {\n'
                     '/* comment */ const SpellPartialResistDistribution precalculated = {\n' + text + '\n};\n}', encoding='utf-8')
        return p

    def test_source_and_local_cache_produce_identical_interpolation(self):
        source = self.source()
        cache = self.root / 'table.json'
        cache.write_text(json.dumps(resistance.extract(self.root)))
        old_rows, old_distribution = F._RESIST_ROWS, F._DISTRIBUTION
        try:
            F.configure_resists(source=source)
            direct = F.partial_resist_distribution()
            source.unlink()
            F.configure_resists(table=cache)
            self.assertEqual(F.partial_resist_distribution(), direct)
            self.assertEqual(len(direct), 10001)
            self.assertEqual(direct[1250], (8750, 0, 0, 0, 1250))
            self.assertEqual(direct[-1], (0, 0, 0, 0, 10000))
        finally:
            F._RESIST_ROWS, F._DISTRIBUTION = old_rows, old_distribution

    def test_rejects_wrong_commit_when_setup_requests_pinned_source(self):
        with self.assertRaisesRegex(ValueError, 'supported commit'):
            resistance.extract(self.source(), pinned=True)

    def test_rejects_unsupported_source_tokens(self):
        p = self.source()
        p.write_text(p.read_text().replace('{{10000,', '{{10000 + 1,'))
        with self.assertRaisesRegex(ValueError, 'unsupported tokens'):
            resistance.extract(p)

    def test_rejects_malformed_rows(self):
        for bad in [[self.rows[0]], [[True, 0, 0, 0, 9999]] * 101,
                    [[10001, 0, 0, 0, -1]] * 101, [[9999, 0, 0, 0, 0]] * 101,
                    [[10000, 0, 0, 0]] * 101]:
            with self.subTest(bad=bad[0]), self.assertRaises(ValueError):
                resistance.validate(bad)

    def test_executable_is_not_accepted_as_source(self):
        p = self.root / 'mangosd.exe'
        p.write_bytes(b'compiled server')
        with self.assertRaisesRegex(ValueError, 'server executable'):
            resistance.extract(p)


if __name__ == '__main__':
    unittest.main()
