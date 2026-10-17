#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

CXX="${CXX:-i686-w64-mingw32-g++}"

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "[-] Error: $CXX not found in PATH."
    echo "    macOS: brew install mingw-w64"
    echo "    Linux: sudo apt-get install g++-mingw-w64-i686"
    exit 1
fi

mkdir -p "${ROOT_DIR}/build"
echo "==> Building d3d9.dll proxy with ${CXX}..."

${CXX} -shared -O3 -msse2 -mfpmath=sse -static-libgcc -static-libstdc++ \
    -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src" \
    "${ROOT_DIR}/src/d3d9_proxy.cpp" \
    "${ROOT_DIR}/src/d3d9.def" \
    -o "${ROOT_DIR}/build/d3d9.dll" \
    -lkernel32 -luser32

echo "==> Build succeeded: ${ROOT_DIR}/build/d3d9.dll"
