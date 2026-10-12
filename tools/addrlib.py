#!/usr/bin/env python3
"""Address Library (format 5) reader + static hook-site audit against SkyrimSE.exe.

usage:
  addrlib.py resolve ID [ID...]          print RVA/VA for Address Library IDs
  addrlib.py site ID+OFF [...]           decode the instruction at a hook site (call target -> ID)
  addrlib.py audit [hooks.json]          verify every hook in the spec (default skse/hooks.json)
  addrlib.py disasm ID+OFF [N]           llvm-objdump N bytes around a site
"""
import json, struct, subprocess, sys
from functools import cached_property
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[2]
EXE = PROJECT / 'steamapps/common/Skyrim Special Edition/SkyrimSE.exe'
BIN = PROJECT / 'steamapps/common/Skyrim Special Edition/Data/SKSE/Plugins/versionlib-1-7-104-0.bin'
SPEC = Path(__file__).resolve().parent.parent / 'skse/hooks.json'
IMAGE_BASE = 0x140000000


class AddressLibrary:
    def __init__(self, path: Path = BIN):
        d = path.read_bytes()
        fmt, *ver = struct.unpack_from('<i4I', d, 0)
        if fmt != 5:
            raise ValueError(f'unsupported address library format {fmt}')
        self.version = '.'.join(map(str, ver))
        self.name = d[20:84].split(b'\0', 1)[0].decode()
        ptr_size, data_fmt, count = struct.unpack_from('<3i', d, 84)
        self.offsets = struct.unpack_from(f'<{count}I', d, 96)

    def rva(self, id_: int) -> int:
        if id_ >= len(self.offsets) or not self.offsets[id_]:
            raise KeyError(f'ID {id_} not in address library {self.version}')
        return self.offsets[id_]

    @cached_property
    def by_rva(self) -> dict[int, int]:
        return {o: i for i, o in enumerate(self.offsets) if o}


class PE:
    def __init__(self, path: Path = EXE):
        self.data = path.read_bytes()
        pe = struct.unpack_from('<I', self.data, 0x3C)[0]
        nsec, opt_size = struct.unpack_from('<H12xH', self.data, pe + 6)
        sec = pe + 24 + opt_size
        self.sections = []
        for i in range(nsec):
            name, vsize, va, rawsize, rawptr = struct.unpack_from('<8sIIII', self.data, sec + 40 * i)
            self.sections.append((name.rstrip(b'\0').decode(), va, max(vsize, rawsize), rawptr))

    def section(self, rva: int) -> str | None:
        return next((n for n, va, sz, _ in self.sections if va <= rva < va + sz), None)

    def read(self, rva: int, n: int) -> bytes:
        for _, va, sz, raw in self.sections:
            if va <= rva < va + sz:
                return self.data[raw + rva - va: raw + rva - va + n]
        raise ValueError(f'RVA {rva:#x} not in any section')


def parse_site(s: str) -> tuple[int, int]:
    id_, _, off = s.partition('+')
    return int(id_, 0), int(off or '0', 0)


def decode_site(al: AddressLibrary, pe: PE, id_: int, off: int) -> dict:
    rva = al.rva(id_) + off
    b = pe.read(rva, 6)
    out = {'site': f'{id_}+{off:#x}', 'rva': f'{rva:#x}', 'bytes': b.hex(' ')}
    if b[0] == 0xE8:                                   # call rel32
        tgt = rva + 5 + struct.unpack_from('<i', b, 1)[0]
        out.update(kind='call', target_rva=f'{tgt:#x}', target_id=al.by_rva.get(tgt))
    elif b[0] == 0xE9:                                 # jmp rel32
        tgt = rva + 5 + struct.unpack_from('<i', b, 1)[0]
        out.update(kind='jmp', target_rva=f'{tgt:#x}', target_id=al.by_rva.get(tgt))
    elif b[:2] == b'\xff\x15':                         # call [rip+disp32]
        out.update(kind='call_indirect')
    else:
        out.update(kind='other')
    return out


def audit(spec_path: Path) -> int:
    al, pe = AddressLibrary(), PE()
    spec = json.loads(spec_path.read_text())
    bad = 0
    for h in spec['hooks']:
        try:
            if h['kind'] == 'call':
                r = decode_site(al, pe, *parse_site(h['site']))
                ok = r['kind'] == 'call' and (h.get('target') is None or r['target_id'] == h['target'])
                detail = f"call -> id {r.get('target_id')} ({r.get('target_rva')}) bytes {r['bytes']}"
            elif h['kind'] == 'function':
                rva = al.rva(h['id'])
                ok = pe.section(rva) == '.text'
                detail = f'rva {rva:#x} in {pe.section(rva)}'
            elif h['kind'] == 'vtable':
                rva = al.rva(h['id'])
                slot = struct.unpack_from('<Q', pe.read(rva + 8 * h['index'], 8))[0] - IMAGE_BASE
                ok = pe.section(rva) in ('.rdata', '.data') and pe.section(slot) == '.text'
                detail = f"vtbl {rva:#x}[{h['index']:#x}] -> {slot:#x} ({pe.section(slot)})"
            elif h['kind'] == 'data':
                rva = al.rva(h['id'])
                ok = pe.section(rva) in ('.data', '.rdata', '.bss')
                detail = f'rva {rva:#x} in {pe.section(rva)}'
            else:
                ok, detail = False, f"unknown kind {h['kind']}"
        except (KeyError, ValueError) as e:
            ok, detail = False, str(e)
        bad += not ok
        print(f"{'OK ' if ok else 'BAD'} {h['name']:<34} {detail}")
    print(f'{len(spec["hooks"]) - bad}/{len(spec["hooks"])} ok (address library {al.version})')
    return 1 if bad else 0


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    cmd, args = argv[0], argv[1:]
    if cmd == 'audit':
        return audit(Path(args[0]) if args else SPEC)
    al, pe = AddressLibrary(), PE()
    if cmd == 'resolve':
        for a in args:
            rva = al.rva(int(a, 0))
            print(f'{a}: rva {rva:#x} va {IMAGE_BASE + rva:#x} ({pe.section(rva)})')
    elif cmd == 'site':
        for a in args:
            print(json.dumps(decode_site(al, pe, *parse_site(a))))
    elif cmd == 'disasm':
        id_, off = parse_site(args[0])
        n = int(args[1]) if len(args) > 1 else 32
        va = IMAGE_BASE + al.rva(id_) + off
        subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', f'--start-address={va - 16:#x}',
                        f'--stop-address={va + n:#x}', str(EXE)])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
