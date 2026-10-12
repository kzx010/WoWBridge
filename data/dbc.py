"""WDBC reader driven by WoWDBDefs (.dbd) layouts for a specific client build (default 1.12.1.5875).

The layout whose BUILD lines cover the build is used; its computed record size must equal the file's
header record size, otherwise the definition does not match the data and we refuse to guess.
"""
import re
import struct
from dataclasses import dataclass
from pathlib import Path

DEFS = Path(__file__).resolve().parents[2] / 'reference/WoWDBDefs/definitions'
BUILD = (1, 12, 1, 5875)
LOCALES_PRE_TBC = 8   # 1.x locstrings: 8 string offsets + 1 flags word


@dataclass
class Field:
    name: str
    kind: str          # int | float | string | locstring
    bits: int          # storage size for ints
    signed: bool
    count: int         # array length (1 = scalar)


def _build_tuple(s: str) -> tuple:
    return tuple(int(x) for x in s.split('.'))


def _covers(spec: str, build: tuple) -> bool:
    for part in (p.strip() for p in spec.split(',')):
        if '-' in part:
            lo, hi = (_build_tuple(x) for x in part.split('-'))
            if lo <= build <= hi:
                return True
        elif part and _build_tuple(part) == build:
            return True
    return False


def load_layout(table: str, build: tuple = BUILD) -> list[Field]:
    text = (DEFS / f'{table}.dbd').read_text()
    blocks = text.split('\n\n')
    kinds = {}
    for line in blocks[0].splitlines()[1:]:                       # COLUMNS
        m = re.match(r'(\w+)(?:<[^>]*>)?\s+(\w+)', line.strip())
        if m:
            kinds[m.group(2)] = m.group(1)
    for block in blocks[1:]:
        lines = [l.strip() for l in block.splitlines() if l.strip() and not l.startswith('COMMENT')]
        specs = [l[6:] for l in lines if l.startswith('BUILD ')]
        if not any(_covers(s, build) for s in specs):
            continue
        fields = []
        for l in lines:
            if l.startswith(('BUILD', 'LAYOUT')):
                continue
            m = re.match(r'(?:\$[^$]*\$)?(\w+)(?:<(u?)(\d+)>)?(?:\[(\d+)\])?', l)
            name, unsigned, bits, count = m.group(1), m.group(2), m.group(3), m.group(4)
            fields.append(Field(name, kinds.get(name, 'int'), int(bits) if bits else 32, not unsigned, int(count) if count else 1))
        return fields
    raise LookupError(f'{table}: no layout for build {".".join(map(str, build))}')


def _size(f: Field) -> int:
    if f.kind == 'locstring':
        return 4 * (LOCALES_PRE_TBC + 1) * f.count
    if f.kind in ('float', 'string'):
        return 4 * f.count
    return f.bits // 8 * f.count


def read(path: Path, table: str | None = None, build: tuple = BUILD) -> list[dict]:
    data = Path(path).read_bytes()
    magic, n, nfields, rsize, ssize = struct.unpack_from('<4s4I', data)
    if magic != b'WDBC':
        raise ValueError(f'{path}: not a WDBC file')
    fields = load_layout(table or Path(path).stem, build)
    if sum(map(_size, fields)) != rsize:
        raise ValueError(f'{path}: layout size {sum(map(_size, fields))} != record size {rsize}')
    strings = data[20 + n * rsize:]

    def s(off: int) -> str:
        return strings[off:strings.index(b'\0', off)].decode('utf-8', 'replace') if off < len(strings) else ''

    rows = []
    for i in range(n):
        o, row = 20 + i * rsize, {}
        for f in fields:
            vals = []
            for _ in range(f.count):
                if f.kind == 'locstring':
                    offs = struct.unpack_from(f'<{LOCALES_PRE_TBC}I', data, o)
                    vals.append(s(offs[0]))                   # enUS
                    o += 4 * (LOCALES_PRE_TBC + 1)
                elif f.kind == 'float':
                    vals.append(struct.unpack_from('<f', data, o)[0]); o += 4
                elif f.kind == 'string':
                    vals.append(s(struct.unpack_from('<I', data, o)[0])); o += 4
                else:
                    fmt = {8: 'b', 16: 'h', 32: 'i', 64: 'q'}[f.bits]
                    vals.append(struct.unpack_from('<' + (fmt if f.signed else fmt.upper()), data, o)[0]); o += f.bits // 8
            row[f.name] = vals if f.count > 1 else vals[0]
        rows.append(row)
    return rows


def by_id(rows: list[dict]) -> dict[int, dict]:
    return {r['ID']: r for r in rows}
