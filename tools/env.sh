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
# WSL: no system LLVM; clang-cl/lld-link/llvm-rc/llvm-lib come from llvm-mingw (toolchain/wsl-bin links), llvm-mt
# from Ubuntu's llvm-18 package.
# WSL as tools/winhost.py tells it (the kernel's release), even in an environment without WSL_DISTRO_NAME.
if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then   # safe under set -eu (ship.sh)
    export WB_WSL=1 PATH="$P/toolchain/wsl-bin:$PATH"
fi
