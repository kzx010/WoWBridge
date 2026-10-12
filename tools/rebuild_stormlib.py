#!/usr/bin/env python3
"""Rebuild the existing local StormLib release cache with portable source paths.

Requires the developer toolchain and cached source. Performs no downloads.
Run after sourcing tools/env.sh, before packaging a public DLL.
"""
from pathlib import Path
import shutil
import subprocess

REPO = Path(__file__).resolve().parents[1]
WORKSPACE = REPO.parent
BUILD = WORKSPACE / 'toolchain/vcpkg/buildtrees/stormlib/x64-windows-clangcl-rel'
TARGET = REPO / 'build/skse/vcpkg_installed/x64-windows-clangcl/lib/StormLib.lib'


def main():
    cache = BUILD / 'CMakeCache.txt'
    if not cache.is_file() or not TARGET.is_file():
        raise SystemExit('The existing StormLib build cache and installed release library are required. No source will be downloaded.')
    values = {}
    for line in cache.read_text().splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line:
            key, value = line.split('=', 1)
            values[key.split(':')[0]] = value
    flag = f'/clang:-ffile-prefix-map={WORKSPACE}=/wowbridge'
    cmd = ['cmake', '-S', values['CMAKE_HOME_DIRECTORY'], '-B', str(BUILD),
           '-DVCPKG_MANIFEST_MODE=OFF', '-DVCPKG_MANIFEST_INSTALL=OFF']
    for key in ('CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS'):
        current = values[key]
        cmd.append(f'-D{key}:STRING=' + current + (' ' + flag if flag not in current else ''))
    subprocess.run(cmd, check=True)
    subprocess.run(['cmake', '--build', str(BUILD), '--target', 'storm', '-j', '8'], check=True)
    shutil.copyfile(BUILD / 'StormLib.lib', TARGET)
    print('Installed path-mapped StormLib. Rebuild the plugin to relink it.')


if __name__ == '__main__':
    main()
