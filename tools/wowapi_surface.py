#!/usr/bin/env python3
"""The WoW API surface the 1.12 interface calls: global functions it calls but never defines itself.

usage: wowapi_surface.py DUMP_DIR GLOBALS_FILE   (both from `framexml_probe --dump DIR --globals FILE`)
Prints `name<TAB>calls<TAB>files` for every called name that neither the interface nor the engine defines,
FrameXML-only names first. Static and approximate (regex, not a Lua parser): a guide for the API shim, not its test.
"""
import re, sys
from collections import defaultdict
from pathlib import Path

LUA = set('''and break do else elseif end false for function if in local nil not or repeat return then true until while
assert collectgarbage dofile error getfenv getmetatable ipairs load loadfile loadstring module next pairs pcall print
rawequal rawget rawset require select setfenv setmetatable tonumber tostring type unpack xpcall gcinfo newproxy'''.split())

CALL = re.compile(r'(?<![.:\w])([A-Za-z_]\w*)\s*\(')
DEFS = [re.compile(r'function\s+([A-Za-z_]\w*)\s*\('), re.compile(r'^\s*([A-Za-z_]\w*)\s*=(?!=)', re.M),
        re.compile(r'\bname\s*=\s*"([A-Za-z_]\w*)"')]
LOCAL = re.compile(r'\blocal\s+(?:function\s+)?([A-Za-z_][\w\s,]*)')
PARAMS = re.compile(r'function\s*[\w.:]*\s*\(([^)]*)\)')


def strip(text: str) -> str:
    text = re.sub(r'--\[\[.*?\]\]', '', text, flags=re.S)
    text = re.sub(r'--[^\n]*', '', text)
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', '""', text)


def main(dump: Path, globals_file: Path) -> None:
    known = LUA | set(globals_file.read_text().split())
    defined, calls, where = set(), defaultdict(int), defaultdict(set)
    for f in sorted(p for p in dump.rglob('*') if p.suffix in ('.lua', '.xml')):
        raw = f.read_text(errors='replace')
        for rx in DEFS[2:]:
            defined.update(rx.findall(raw))           # frame names, before strings are blanked
        text = strip(raw)
        for rx in DEFS[:2]:
            defined.update(rx.findall(text))
        local = {n.strip() for m in LOCAL.findall(text) for n in m.split(',')}
        local |= {n.strip() for m in PARAMS.findall(text) for n in m.split(',')}
        for name in CALL.findall(text):
            if name not in local:
                calls[name] += 1
                where[name].add(f.relative_to(dump).parts[1] if f.parts[-3] == 'AddOns' else 'FrameXML')
    missing = [n for n in calls if n not in known and n not in defined]
    missing.sort(key=lambda n: ('FrameXML' not in where[n], n.lower()))
    for n in missing:
        print(f'{n}\t{calls[n]}\t{",".join(sorted(where[n]))}')
    print(f'{len(missing)} names ({sum("FrameXML" in where[n] for n in missing)} from FrameXML)', file=sys.stderr)


if __name__ == '__main__':
    main(Path(sys.argv[1]), Path(sys.argv[2]))
