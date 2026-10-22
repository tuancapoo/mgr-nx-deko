"""MGR's shader containers from the player's own media/data000.cpk, for tools/win/build_mgr_deko_shaders.ps1.

    python tools/extract_mgr_cpk_shaders.py <game folder> [--out out/cpk-shader-inputs] [--scanner <cpk_shader_scan>]

Every model and effect of the game is in media/data000.cpk (CRI CPK, most entries CRILAYLA-compressed Platinum .dat
archives) with its shaders: tools/installer_probe/cpk_shader_scan.cpp decompresses each entry and writes the Xenos
shader containers it holds (magic 0x102A1100/01). This keeps one copy of each (by SHA-256) as vs_/ps_<sha256 prefix>.bin.
reNut keys a shader by the container the game registers (XGRegisterVertexShader, before any vertex fetch is bound),
so these are all it needs (tools/installer_probe/README.md). The output is game data: never commit or distribute it.
"""
import argparse
import glob
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'installer_probe'))
import cpk_toc  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game', help='the game folder (default.xex and media/)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'cpk-shader-inputs'))
    ap.add_argument('--scanner', default=os.path.join(ROOT, 'out', 'tools', 'cpk_shader_scan.exe'),
                    help='tools/installer_probe/cpk_shader_scan.cpp, built')
    a = ap.parse_args()
    cpk = os.path.join(a.game, 'media', 'data000.cpk')
    if not os.path.isfile(cpk):
        sys.exit(f'extract_mgr_cpk_shaders: no {cpk}')
    if not os.path.isfile(a.scanner):
        sys.exit(f'extract_mgr_cpk_shaders: build the scanner first: clang++ -O2 -std=c++17 '
                 f'tools/installer_probe/cpk_shader_scan.cpp -o {a.scanner}')
    _, files = cpk_toc.toc(cpk)
    with tempfile.TemporaryDirectory() as tmp:
        listing = os.path.join(tmp, 'list.txt')
        with open(listing, 'w') as f:
            f.writelines(f'{o} {s} {e} {n}\n' for n, o, s, e in files)
        scanned = os.path.join(tmp, 'containers')
        os.makedirs(scanned)
        subprocess.run([a.scanner, cpk, listing, scanned], check=True)
        os.makedirs(a.out, exist_ok=True)
        seen = set()
        for path in sorted(glob.glob(os.path.join(scanned, 'c*.bin'))):
            data = open(path, 'rb').read()
            digest = hashlib.sha256(data).hexdigest()
            if digest in seen:
                continue
            seen.add(digest)
            stage = 'vs' if struct.unpack('>I', data[:4])[0] & 1 else 'ps'
            shutil.copyfile(path, os.path.join(a.out, f'{stage}_{digest[:16]}.bin'))
    print(f'extract_mgr_cpk_shaders: {len(seen)} containers -> {a.out}')


if __name__ == '__main__':
    main()
