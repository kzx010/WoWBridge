# Toolchain environment for WoWBridge builds (bash). Usage: source tools/env.sh
P="${WOWBRIDGE_PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
export VCPKG_ROOT=$P/toolchain/vcpkg
export XWIN_SYSROOT=$P/toolchain/xwin-out
export LLVM_MINGW_BIN=$P/toolchain/llvm-mingw/bin
export COMMONLIB_DIR=$P/reference/CommonLibSSE-NG
export VCPKG_DEFAULT_BINARY_CACHE=$P/toolchain/vcpkg-bincache
export WINEPREFIX=$P/toolchain/wine-build   # isolated prefix for fxc2 (shader compiles)
export WINEDEBUG=-all
mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE"
export WINEDLLOVERRIDES="mscoree=;mshtml="
export FXC2=$P/toolchain/fxc2/fxc2.exe

# Directly invoked Python helpers use the project virtual environment.
export PATH="$P/toolchain/venv/bin:$PATH"
