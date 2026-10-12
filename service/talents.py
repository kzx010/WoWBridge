"""Stateless Classic talent ranks. The caller owns the returned allocation."""
import json
from functools import lru_cache
from pathlib import Path

from service.progression import ids, number

DEFAULT = Path(__file__).resolve().parents[1] / 'data/talents.json'


def points(level):
    level = number(level, 'caster.level')
    if int(level) != level or level < 1:
        raise ValueError('caster.level must be a positive integer')
    return max(0, int(level) - 9)


@lru_cache(maxsize=1)
def book():
    return Talents()


class Talents:
    def __init__(self, path=DEFAULT, *, data=None):
        self.data = data if data is not None else json.loads(Path(path).read_text())
        if self.data['schema'] != 1:
            raise ValueError('unsupported talents schema')

    def class_definition(self, value):
        for key, definition in self.data['classes'].items():
            if (isinstance(value, str) and value.lower() == key or
                    isinstance(value, int) and not isinstance(value, bool) and value == definition['id']):
                return key, definition
        raise ValueError(f'unknown talent class {value!r}')

    @staticmethod
    def rows(definition):
        return {t['id']: {**t, 'tree': tree['id']} for tree in definition['trees'] for t in tree['talents']}

    @staticmethod
    def reasons(t, ranks, rows, available, known):
        reasons = []
        if ranks.get(t['id'], 0) >= t['max_rank']:
            reasons.append('max_rank')
        if available <= 0:
            reasons.append('no_points')
        spent = sum(rank for tid, rank in ranks.items() if rows[tid]['tree'] == t['tree'])
        if spent < 5 * t['tier']:
            reasons.append('tier_points')
        if any(ranks.get(p['talent'], 0) < p['rank'] for p in t['prerequisites']):
            reasons.append('prerequisite')
        if t['required_spell'] and t['required_spell'] not in known:
            reasons.append('required_spell')
        return reasons

    def allocation(self, definition, values, level):
        if not isinstance(values, dict):
            raise ValueError('caster.talents must be an object of talent IDs to ranks')
        rows = self.rows(definition)
        ranks = {}
        for tid, rank in values.items():
            if isinstance(tid, bool) or not str(tid).isdigit() or int(tid) not in rows:
                raise ValueError(f'talent {tid!r} is outside {definition["name"]}')
            tid = int(tid)
            if tid in ranks or isinstance(rank, bool) or not isinstance(rank, int) or not 0 <= rank <= rows[tid]['max_rank']:
                raise ValueError(f'invalid rank for talent {tid}')
            ranks[tid] = rank
        ranks = {tid: rank for tid, rank in ranks.items() if rank}
        budget = points(level)
        if sum(ranks.values()) > budget:
            raise ValueError('talent allocation exceeds points at caster.level')
        # Replay one rank at a time: a high tier cannot count its own points to
        # unlock itself, and prerequisites must have been earned first.
        earned = {}
        # A saved allocation retains spells earned earlier (e.g. after a Skyrim
        # skill reset). RequiredSpellID is a learn-time gate, not a respec rule.
        known = {t['required_spell'] for t in rows.values() if t['required_spell']}
        while earned != ranks:
            progressed = False
            for tid in sorted(ranks, key=lambda tid: (rows[tid]['tier'], tid)):
                if earned.get(tid, 0) < ranks[tid] and not self.reasons(rows[tid], earned, rows,
                                                                    budget - sum(earned.values()), known):
                    earned[tid] = earned.get(tid, 0) + 1
                    progressed = True
            if not progressed:
                raise ValueError('talent allocation violates tier, prerequisite or required spell gates')
        return ranks

    def learned_spells(self, definition, ranks):
        rows = self.rows(definition)
        learned = {rows[tid]['rank_spells'][rank - 1] for tid, rank in ranks.items()}
        for tid, rank in ranks.items():
            sid = rows[tid]['rank_spells'][rank - 1]
            learned.update(rows[tid].get('granted_spells', {}).get(str(sid), []))
        return learned

    def handle(self, req):
        key, definition = self.class_definition(req.get('class'))
        caster = req.get('caster', {})
        if not isinstance(caster, dict):
            raise ValueError('caster must be an object')
        level = caster.get('level', 60)
        if 'known_spells' in req:
            known = ids(req['known_spells'], 'known_spells')
        else:
            from service.progression import Progression
            known = set(Progression().known_spells({'class': key,
                        'caster': {**caster, 'level': level, 'talents': {}},
                        'acquired_spells': req.get('acquired_spells', [])})['known_spells'])
        ranks = self.allocation(definition, caster.get('talents', {}), level)
        rows = self.rows(definition)
        learned = None
        if 'learn' in req:
            learn = req['learn']
            if (not isinstance(learn, dict) or isinstance(learn.get('talent'), bool) or
                    not isinstance(learn.get('talent'), int) or learn.get('talent') not in rows):
                raise ValueError('learn.talent must be a talent ID of this class')
            tid = learn['talent']
            next_rank = ranks.get(tid, 0) + 1
            if 'rank' in learn and (isinstance(learn['rank'], bool) or not isinstance(learn['rank'], int) or learn['rank'] != next_rank):
                raise ValueError('learn.rank must be exactly the next rank (one point)')
            reasons = self.reasons(rows[tid], ranks, rows, points(level) - sum(ranks.values()), known)
            if reasons:
                raise ValueError('cannot learn talent: ' + ', '.join(reasons))
            ranks[tid] = next_rank
            learned = {'talent': tid, 'rank': next_rank, 'spell': rows[tid]['rank_spells'][next_rank - 1]}
        spent = sum(ranks.values())
        available = points(level) - spent
        trees = []
        for tree in definition['trees']:
            ts = []
            for t in tree['talents']:
                reasons = self.reasons(rows[t['id']], ranks, rows, available, known)
                rank = ranks.get(t['id'], 0)
                ts.append({**t, 'rank': rank, 'next_rank': rank + 1 if rank < t['max_rank'] else None,
                           'learnable': not reasons, 'reasons': reasons,
                           'meets_prerequisites': not any(r in reasons for r in ('prerequisite', 'required_spell'))})
            trees.append({**tree, 'points_spent': sum(ranks.get(t['id'], 0) for t in tree['talents']), 'talents': ts})
        out = {'class': key, 'class_id': definition['id'], 'level': level, 'points_total': points(level),
               'points_spent': spent, 'points_available': available,
               'talents': {str(tid): rank for tid, rank in sorted(ranks.items())},
               'learned_spells': sorted(self.learned_spells(definition, ranks)), 'trees': trees}
        if learned:
            out['learned'] = learned
        return out
