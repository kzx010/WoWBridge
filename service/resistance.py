"""Read a local CMaNGOS resistance table without executing or downloading source."""
import hashlib
import json
from pathlib import Path
import re

PIN = '8ec338a1704e7dcb1c0213eb7ed58f9231ade40f'
SOURCE_SHA256 = '03b67a4b6226f1f463e71be3694d3c950fc9b915a6e921407a98e4ff82fc21d0'


def validate(rows):
    if not isinstance(rows, list) or len(rows) != 101:
        raise ValueError('resistance table must contain 101 rows')
    for row in rows:
        if (not isinstance(row, (list, tuple)) or len(row) != 5 or
                any(type(v) is not int or not 0 <= v <= 10000 for v in row) or sum(row) != 10000):
            raise ValueError('each resistance row must contain five integer weights totaling 10000')
    if tuple(rows[0]) != (10000, 0, 0, 0, 0) or tuple(rows[-1]) != (0, 0, 0, 0, 10000):
        raise ValueError('invalid resistance table endpoints')
    return [tuple(row) for row in rows]


def source_file(path):
    path = Path(path)
    if path.is_dir():
        path = path / 'src/game/Entities/Unit.cpp'
    if not path.is_file() or path.name != 'Unit.cpp':
        raise ValueError('select the extracted mangos-classic source folder or its src/game/Entities/Unit.cpp; a server executable does not contain this source file')
    return path


def rows_digest(rows):
    return hashlib.sha256(json.dumps(rows, separators=(',', ':')).encode()).hexdigest()


def extract(path, *, pinned=False):
    raw = source_file(path).read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    # Git may check out C++ files with CRLF on Windows.
    normalized_digest = hashlib.sha256(raw.replace(b'\r\n', b'\n')).hexdigest()
    if pinned and normalized_digest != SOURCE_SHA256:
        raise ValueError(f'CMaNGOS source does not match the supported commit {PIN}; select that version from the README')
    src = re.sub(r'/\*.*?\*/|//[^\n]*', '', raw.decode('utf-8-sig'), flags=re.S)
    match = re.search(r'InitSpellPartialResistDistribution\s*\(\s*\)\s*\{.*?\bprecalculated\s*=\s*\{(.*?)\}\s*;', src, re.S)
    if not match:
        raise ValueError('CMaNGOS source has no supported precalculated resistance table')
    body = match.group(1)
    row_pattern = r'\{\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}\}'
    rows = [tuple(map(int, r)) for r in re.findall(row_pattern, body)]
    if re.sub(row_pattern, '', body).strip(' \t\r\n,'):
        raise ValueError('unsupported tokens in CMaNGOS resistance table')
    rows = validate(rows)
    return {'schema': 1, 'rows_sha256': rows_digest(rows), 'source_commit': PIN if normalized_digest == SOURCE_SHA256 else None,
            'source_sha256': digest, 'rows': rows}


def load(path):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if not isinstance(data, dict) or data.get('schema') != 1:
        raise ValueError('unsupported local resistance-table format; run setup again')
    rows = validate(data.get('rows'))
    if rows_digest(rows) != data.get('rows_sha256'):
        raise ValueError('local resistance table checksum failed; run setup again')
    return rows
