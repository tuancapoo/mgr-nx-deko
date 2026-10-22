#!/usr/bin/env bash
# banjo-nx (deko3d renderer, docs/deko3d-renderer.md): the PC tools the deko3d shader chain needs, built into
# out/deko3d_tools: SPIRV-Cross (from UnleashedRecomp-NX's MoltenVK copy, Apache-2.0; a PC tool only) and uam
# (app/thirdparty/uam, SwitchWakerHD's patched 1.1.0) with devkitPro's MSYS2 clang (MinGW).
#   bash tools/deko3d_build_tools.sh [SPIRV-Cross source]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPIRV_CROSS_SRC="${1:-/c/devkitPro/UnleashedRecomp-NX/thirdparty/MoltenVK/SPIRV-Cross}"
OUT="$ROOT/out/deko3d_tools"
export PATH="/c/devkitPro/msys2/clang64/bin:/c/devkitPro/msys2/usr/bin:$PATH"

mkdir -p "$OUT/spirv-cross-build" "$OUT/uam-build"
cmake -S "$SPIRV_CROSS_SRC" -B "$OUT/spirv-cross-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SHARED=OFF \
    -DSPIRV_CROSS_ENABLE_C_API=OFF -DCMAKE_EXE_LINKER_FLAGS=-static
ninja -C "$OUT/spirv-cross-build" spirv-cross
cmake -S "$ROOT/app/thirdparty/uam" -B "$OUT/uam-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS=-static
ninja -C "$OUT/uam-build" uam_tool
"$OUT/uam-build/uam.exe" --version
