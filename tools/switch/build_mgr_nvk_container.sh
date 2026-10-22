#!/usr/bin/env bash
# Run inside the dedicated mgr-nvk container. Input mounts are read-only;
# /export receives the SDK and /work retains incremental Linux build files.
set -euo pipefail
jobs=${MGR_BUILD_JOBS:-6}
mkdir -p /work/mesa /export/sdk
if [ ! -f /work/.mgr_mesa_source_ready ]; then
    if [ -f /tmp/mgr_mesa.zip ]; then
        echo '[mgr-nvk] Extracting the pinned source archive inside Linux'
        python3 - <<'PY'
import pathlib
import zipfile
root = pathlib.Path('/work/mesa').resolve()
with zipfile.ZipFile('/tmp/mgr_mesa.zip') as archive:
    for item in archive.infolist():
        parts = pathlib.PurePosixPath(item.filename).parts[1:]
        if not parts:
            continue
        target = root.joinpath(*parts)
        if not target.resolve().is_relative_to(root):
            raise SystemExit('Invalid archive path')
        if item.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(item))
PY
    else
        echo '[mgr-nvk] Copying the pinned Mesa source into the Linux filesystem'
        cp -a /input/. /work/mesa/
    fi
    touch /work/.mgr_mesa_source_ready
fi
cd /work/mesa
mkdir -p /usr/local/libexec
cp bindgen-switch-wrapper.sh /usr/local/libexec/bindgen
cp rustc-switch-wrapper.sh /usr/local/libexec/rustc
cp bindgen-atomic-shim.h /usr/local/libexec/bindgen-atomic-shim.h
chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc
cp src/gallium/winsys/nouveau/drm/nouveau.h /opt/devkitpro/portlibs/switch/include/
cp src/nouveau/headers/nv_device_info.h /opt/devkitpro/portlibs/switch/include/
if [ ! -d /usr/lib/llvm-15/lib/clang/15/include ]; then
    ln -sf /usr/lib/llvm-15/lib/clang/15.0.6/include /usr/lib/llvm-15/lib/clang/15/include
fi
for name in dl rt util; do
    if [ ! -f "/opt/devkitpro/portlibs/switch/lib/lib${name}.a" ]; then
        printf 'void __mesa_switch_stub_lib(void) {}\n' > /tmp/mgr_stub_posix.c
        /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc -c /tmp/mgr_stub_posix.c -o /tmp/mgr_stub_posix.o
        /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs "/opt/devkitpro/portlibs/switch/lib/lib${name}.a" /tmp/mgr_stub_posix.o
    fi
done

echo '[mgr-nvk] Installing the ELF AArch64 Rust standard library'
rustup target add aarch64-unknown-linux-gnu

echo '[mgr-nvk] Building native mesa_clc and vtn_bindgen2'
if [ ! -f builddir-native/build.ninja ]; then
    meson setup builddir-native \
        -Dvulkan-drivers= -Dgallium-drivers= -Dshader-cache=true \
        -Dplatforms= -Dglx=disabled -Degl=disabled -Dopengl=false \
        -Dgles1=disabled -Dgles2=disabled '-Dtools=[]' -Dllvm=enabled \
        -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true
fi
ninja -j "$jobs" -C builddir-native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2

echo '[mgr-nvk] Configuring Horizon ARM64 NVK'
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu
# The cross file names Rust/bindgen wrappers by absolute path. Keep them off
# PATH so native Rust proc-macros use the host compiler and standard library.
export PATH="/work/mesa/builddir-native/src/compiler/clc:/work/mesa/builddir-native/src/compiler/spirv:$PATH"
if [ ! -f builddir-switch/build.ninja ]; then
    meson setup builddir-switch --cross-file switch_cross_file.txt \
        --buildtype=release -Doptimization=1 -Db_lto=false -Db_ndebug=true \
        -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau -Dshader-cache=true \
        -Dgallium-rusticl=false -Dplatforms=switch -Dglx=disabled -Degl=disabled \
        -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled \
        -Dshared-glapi=disabled -Dshared-llvm=disabled -Dmesa-clc=system \
        -Dprecomp-compiler=system -Dcpp_rtti=false
fi

if [ ! -f builddir-switch/.mgr_native_rust_env_v2 ]; then
    # Ninja does not track PATH changes. Discard Rust intermediates left by
    # an earlier host build that accidentally selected the cross wrapper.
    python3 - <<'PY'
from pathlib import Path
root = Path('/work/mesa/builddir-switch').resolve()
for item in root.rglob('*'):
    if item.is_file() and item.suffix in ('.rlib', '.so') and item.resolve().is_relative_to(root):
        item.unlink()
PY
    touch builddir-switch/.mgr_native_rust_env_v2
fi

echo '[mgr-nvk] Building the standalone static Vulkan archive'
ninja -j "$jobs" -C builddir-switch src/nouveau/vulkan/libvulkan.a
# The host's /usr/bin/ar may load an older LLVM plugin and omit Rust symbols
# from the merged archive's index despite retaining their native ELF objects.
# Rebuild the index with the target toolchain before exporting for consumers.
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc-ranlib builddir-switch/src/nouveau/vulkan/libvulkan.a

echo '[mgr-nvk] Exporting SDK headers and libraries'
cp -a /opt/devkitpro/portlibs/switch/include /export/sdk/
cp -a /opt/devkitpro/portlibs/switch/lib /export/sdk/
cp -a include/vulkan include/vk_video /export/sdk/include/
cp builddir-switch/src/nouveau/vulkan/libvulkan.a /export/sdk/lib/
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump -f \
    /export/sdk/lib/libvulkan.a > /export/nvk_archive_arch.txt
if grep -q 'file format elf64-x86-64\|file format pe-' /export/nvk_archive_arch.txt; then
    echo '[mgr-nvk] ERROR: non-ARM object detected in Vulkan archive' >&2
    exit 1
fi
sha256sum /export/sdk/lib/libvulkan.a > /export/libvulkan.sha256
printf 'Mesa source: danfromtico/mesa-switch 1a8c1a66d6f\nNo nfsmw driver patch applied\n' > /export/SOURCE.txt
echo '[mgr-nvk] SDK ready'
