#!/bin/bash
# Build the plugin, package dist/, deploy the mod. Stops on the first failure.
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/env.sh
cmake --build build/skse -j 18 > build/last-build.log 2>&1 || { grep -E 'error|FAILED' build/last-build.log | grep -v MESA | head -30; exit 1; }
python3 tools/package.py > /dev/null
python3 tools/deploy.py install --only mod
