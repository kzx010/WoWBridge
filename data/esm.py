"""Skyrim SE plugin files (TES4 format, form version 44): reading the records of chosen types out of a master, and
writing a plugin of new records. Enough of the format for WoWBridge.esp: records, groups, subrecords, compression.

A record is (type, form id, flags, [(subrecord type, bytes)]); its subrecords keep their order, which the game reads.
"""
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

COMPRESSED = 0x00040000
ESL = 0x00000200          # TES4 flag: a light plugin (its records numbered 0x800-0xFFF)
FORM_VERSION = 44


@dataclass
class Record:
    type: str
    form: int
    flags: int = 0
    subs: list = field(default_factory=list)   # [(type, bytes)]

    def get(self, sub: str) -> bytes | None:
        return next((d for t, d in self.subs if t == sub), None)

    def all(self, sub: str) -> list[bytes]:
        return [d for t, d in self.subs if t == sub]

    def set(self, sub: str, data: bytes) -> None:
        """Replace the first such subrecord, or add it at the end."""
        for i, (t, _) in enumerate(self.subs):
            if t == sub:
                self.subs[i] = (sub, data)
                return
        self.subs.append((sub, data))

    def drop(self, *subs: str) -> None:
        self.subs = [(t, d) for t, d in self.subs if t not in subs]

    @property
    def edid(self) -> str:
        e = self.get('EDID')
        return e.rstrip(b'\0').decode('cp1252') if e else ''


def _subrecords(data: bytes) -> list:
    out, i, big = [], 0, None
    while i < len(data):
        t, size = data[i:i + 4].decode('ascii'), struct.unpack_from('<H', data, i + 4)[0]
        i += 6
        if t == 'XXXX':   # the next subrecord's size, past 64 KiB
            big = struct.unpack_from('<I', data, i)[0]
            i += size
            continue
        if big is not None:
            size, big = big, None
        out.append((t, data[i:i + size]))
        i += size
    return out


def read(path: Path, types: set[str]) -> dict[str, list[Record]]:
    """The records of these types in a master (their top groups only), by type."""
    data = path.read_bytes()
    out = {t: [] for t in types}
    i = struct.unpack_from('<I', data, 4)[0] + 24   # past the TES4 header record
    while i < len(data):
        label = data[i + 8:i + 12].decode('ascii', 'replace')
        size = struct.unpack_from('<I', data, i + 4)[0]
        if label in types:
            j, end = i + 24, i + size
            while j < end:
                rtype = data[j:j + 4].decode('ascii')
                rsize, flags, form = struct.unpack_from('<III', data, j + 4)
                if rtype == 'GRUP':   # these types have no subgroups, but step over any
                    j += rsize
                    continue
                body = data[j + 24:j + 24 + rsize]
                if flags & COMPRESSED:
                    body = zlib.decompress(body[4:])
                out[label].append(Record(rtype, form, flags & ~COMPRESSED, _subrecords(body)))
                j += 24 + rsize
        i += size
    return out


def exterior_land(path: Path, references: frozenset[str] = frozenset()):
    """Yield (WRLD form, CELL form, record) from exterior CELL/LAND groups.

    LAND lives in a cell's temporary children, not in a top-level LAND group.
    Placed references of the requested types (e.g. REFR) come from any of a cell's
    children, including the worldspace's persistent cell; exterior DATA positions are world units.
    Skip unrelated record bodies; retain group labels for master remapping.
    """
    with path.open('rb') as source:
        header = source.read(24)
        source.seek(24 + struct.unpack_from('<I', header, 4)[0])

        def walk(end, world=None, cell=None, temporary=False):
            while source.tell() < end:
                at = source.tell()
                header = source.read(24)
                if len(header) != 24:
                    raise ValueError(f'truncated group/record in {path}')
                size = struct.unpack_from('<I', header, 4)[0]
                if header[:4] == b'GRUP':
                    group = struct.unpack_from('<i', header, 12)[0]
                    label = struct.unpack_from('<I', header, 8)[0]
                    if size < 24 or at + size > end:
                        raise ValueError(f'invalid group size in {path}')
                    if group == 0 and header[8:12] != b'WRLD':
                        source.seek(at + size)
                        continue
                    yield from walk(at + size, label if group == 1 else world,
                                    label if group == 6 else cell, group == 9)
                else:
                    rtype = header[:4].decode('ascii')
                    flags, form = struct.unpack_from('<II', header, 8)
                    if at + 24 + size > end:
                        raise ValueError(f'invalid record size in {path}')
                    if world is not None and (rtype == 'CELL' or cell is not None and (
                            rtype == 'LAND' and temporary or rtype in references)):
                        body = source.read(size)
                        if flags & COMPRESSED:
                            expected = struct.unpack_from('<I', body)[0]
                            body = zlib.decompress(body[4:])
                            if len(body) != expected:
                                raise ValueError(f'bad uncompressed size in {path}')
                        yield world, form if rtype == 'CELL' else cell, Record(
                            rtype, form, flags & ~COMPRESSED, _subrecords(body))
                    else:
                        source.seek(at + 24 + size)

        yield from walk(path.stat().st_size)


def _sub(t: str, d: bytes) -> bytes:
    if len(d) > 0xFFFF:
        return b'XXXX' + struct.pack('<HI', 4, len(d)) + t.encode() + struct.pack('<H', 0) + d
    return t.encode() + struct.pack('<H', len(d)) + d


def _record(r: Record) -> bytes:
    body = b''.join(_sub(t, d) for t, d in r.subs)
    return r.type.encode() + struct.pack('<IIIIHH', len(body), r.flags, r.form, 0, FORM_VERSION, 0) + body


def write(path: Path, records: list[Record], masters: list[str], author: str, light: bool = True) -> None:
    """A plugin of these records, grouped by type in the order given."""
    order, groups = [], {}
    for r in records:
        if r.type not in groups:
            order.append(r.type)
            groups[r.type] = []
        groups[r.type].append(r)
    hedr = struct.pack('<fII', 1.71, len(records), max((r.form & 0xFFFFFF for r in records), default=0x7FF) + 1)
    subs = [('HEDR', hedr), ('CNAM', author.encode('cp1252') + b'\0')]
    for m in masters:
        subs += [('MAST', m.encode('cp1252') + b'\0'), ('DATA', struct.pack('<Q', 0))]
    out = _record(Record('TES4', 0, ESL if light else 0, subs))
    for t in order:
        body = b''.join(_record(r) for r in groups[t])
        out += b'GRUP' + struct.pack('<I', 24 + len(body)) + t.encode() + struct.pack('<iHHI', 0, 0, 0, 0) + body
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(out)


def zstring(s: str) -> bytes:
    return s.encode('cp1252', 'replace') + b'\0'
