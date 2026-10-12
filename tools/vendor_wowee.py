#!/usr/bin/env python3
"""Copy the WoWee subset WoWBridge builds on into wowcore/third_party, pristine, with provenance.

Run from anywhere; re-running copies upstream sources, then applies the complete
WoWBridge patch bundle. Local runtime fixes must remain reproducible in that
bundle. Upstream: github.com/Kelsidavis/WoWee.
"""
import shutil, subprocess

import patch_wowee
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / 'reference/WoWee'
DST = Path(__file__).resolve().parent.parent / 'wowcore/third_party'

WOWEE = [
    # Retained widget tree, layout, XML -> Lua, text, fonts, drawing
    *[f'include/ui/{n}.hpp' for n in ('widget_tree', 'widget_renderer', 'xml_parser', 'framexml_emitter', 'text_markup',
                                      'text_wrap', 'simple_html', 'link_hit', 'interface_fonts', 'framexml_takeover',
                                      'ui_colors', 'plural_escape')],
    *[f'src/ui/{n}.cpp' for n in ('widget_tree', 'widget_renderer', 'xml_parser', 'framexml_emitter', 'interface_fonts',
                                  'framexml_takeover', 'plural_escape')],
    # Lua engine (widget bindings) and addon manifests
    *[f'include/addons/{n}.hpp' for n in ('lua_engine', 'lua_services', 'lua_handler_globals', 'lua_api_registrations',
                                          'toc_parser')],
    *[f'src/addons/{n}.cpp' for n in ('lua_engine', 'lua_handler_globals', 'toc_parser')],
    # Textures
    'include/pipeline/blp_loader.hpp', 'include/pipeline/dxt_block.hpp', 'src/pipeline/blp_loader.cpp',
    # Models (M8): M2 v256 geometry, skins, bones, animation tracks, attachments, emitters
    'include/pipeline/m2_loader.hpp', 'include/pipeline/m2_color_track.hpp', 'src/pipeline/m2_loader.cpp',
    'include/rendering/m2_track_sampler.hpp',
    # Upstream tests for the subset
    *[f'tests/test_{n}.cpp' for n in ('widget_tree', 'framexml', 'text_markup', 'text_wrap', 'simple_html',
                                      'lua_handler_globals', 'blp_loader')],
    'include/core/env_flag.hpp',
    'LICENSE',
]
EXTERN = ['extern/lua-5.1.5', 'extern/catch2']


def main() -> None:
    commit = subprocess.run(['git', '-C', SRC, 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    out = DST / 'wowee'
    for rel in WOWEE:
        p = out / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(SRC / rel, p)
    for rel in EXTERN:
        d = DST / Path(rel).name
        shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(SRC / rel, d, ignore=shutil.ignore_patterns('doc', 'test', 'etc', 'Makefile'))
    (out / 'PROVENANCE.md').write_text(
        f'# Vendored from WoWee\n\nUpstream: https://github.com/Kelsidavis/WoWee @ `{commit}`\n'
        'License: MIT with an additional non-commercial-game restriction (LICENSE). WoWBridge is a free personal mod.\n\n'
        'Files are copied unmodified, then `tools/patch_wowee.py` applies WoWBridge\'s fences (see its docstring).\n\n' + '\n'.join(f'- {r}' for r in WOWEE) + '\n')
    print(f'vendored {len(WOWEE)} files + {EXTERN} from {commit[:7]}')
    patch_wowee.apply_all(out)  # Refreshes and direct patching must use the same bundle.



if __name__ == '__main__':
    main()
