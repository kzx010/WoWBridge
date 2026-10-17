#!/bin/bash
# Build the plugin, package dist/, deploy the mod. Stops on the first failure.
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/env.sh
cmake --build build/skse -j "${WB_JOBS:-$(( $(nproc) < 18 ? $(nproc) : 18 ))}" > build/last-build.log 2>&1 || { grep -E 'error|FAILED' build/last-build.log | grep -v MESA | head -30; exit 1; }
# Keep each shipped DLL's PDB by its hash: a fault's "WoWBridge.dll return addresses" then resolve with
#   llvm-symbolizer --obj=build/pdb/<hash>.dll --pdb=build/pdb/<hash>.pdb --relative-address 0xOFFSET
h=$(sha256sum build/skse/WoWBridge.dll | cut -c1-16) && mkdir -p build/pdb && cp build/skse/WoWBridge.dll "build/pdb/$h.dll" && cp build/skse/WoWBridge.pdb "build/pdb/$h.pdb"
python3 tools/package.py > /dev/null
python3 tools/deploy.py install --only mod --trust-unchanged
