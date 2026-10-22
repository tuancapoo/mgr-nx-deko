"""Package a validated local MGR deko3d build without modifying the reference port."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(4 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--include-game', action='store_true')
    ap.add_argument('--game-root', type=Path, default=ROOT / 'assets/game_root')
    args = ap.parse_args()
    nx = ROOT / 'out/nx'
    nro = nx / 'sa2.nro'
    elf = nx / 'sa2'
    header = elf.read_bytes()[:64]
    assert header[:6] == b'\x7fELF\x02\x01', 'Expected little-endian ELF64'
    assert struct.unpack_from('<H', header, 18)[0] == 183, 'Expected AArch64 ELF'
    with nro.open('rb') as f:
        assert f.read(20)[16:20] == b'NRO0', 'Invalid NRO signature'
    shader_dir = ROOT / 'app/out/renut_switch'
    coverage = json.loads((ROOT / 'out/mgr-deko-shaders/coverage.json').read_text())
    assert coverage['uniqueContainers'] == coverage['dekoSpirvValidated'] == coverage['nativeDkshCompiled']
    smoke_path = ROOT / 'out/validation/smoke-result.json'
    smoke = json.loads(smoke_path.read_text())
    assert smoke.get('verified_frame_count', 0) > 0, 'No verified MGR frame from the new renderer'
    assert smoke.get('renderer') == 'renut', 'Validation used the wrong renderer'

    product = ROOT / 'out/product'
    stage = product / 'switch/mgr-nx-deko'
    stage.mkdir(parents=True, exist_ok=True)
    files = {
        'mgr.nro': nro,
        'mgr.toml': ROOT / 'app/mgr_deko_switch.toml',
        'deko3d_glsl.bin': shader_dir / 'deko3d_glsl.bin',
        'deko3d_shaders.bin': shader_dir / 'deko3d_shaders.bin',
    }
    manifest = {
        'name': 'MGR reNut/plume/deko3d local test build',
        'upstream_sa2_revision': 'adf3a4eee524dcca5cd7c1b42454ddb8a09124a1',
        'shader_containers': coverage['uniqueContainers'],
        'default_dksh_cache_entries': coverage['defaultDkshCacheEntries'],
        'render_height': 360,
        'elf_machine': 'AArch64',
        'physical_switch_tested': False,
        'pc_validation': smoke,
        'files': {},
    }
    for name, source in files.items():
        destination = stage / name
        shutil.copy2(source, destination)
        manifest['files'][f'switch/mgr-nx-deko/{name}'] = {
            'bytes': destination.stat().st_size, 'sha256': digest(destination),
        }
    readme = '''MGR reNut/plume/deko3d — bản thử cục bộ, 360p

1. Giải nén trên PC, chép folder switch/mgr-nx-deko vào thẻ SD.
2. Nếu dùng gói không kèm game, chép dữ liệu Xbox 360 của bạn vào
   switch/mgr-nx-deko/game_root/ (phải có default.xex và media/).
3. Mở Homebrew Menu bằng title takeover rồi chạy Metal Gear Rising deko3d.

Folder này có cấu hình và cache riêng. Giữ nguyên bản mgr-nx đang dùng.
Cache Vulkan/.mesp cũ không dùng cho renderer mới. Cache DKSH ban đầu đã kèm;
các biến thể mới gặp khi chơi vẫn có thể cần biên dịch và được lưu trên máy.
Muốn thử 720p: đổi cả hai sa2_render_height_* trong mgr.toml thành 720.

Bản này đã được cross-build ARM64 và kiểm tra hình ảnh trên backend reNut PC.
Chưa có số đo trên Switch thật; không bảo đảm 30 FPS hay toàn bộ game hết lỗi.
Nếu gặp lỗi, gửi logs/ trong folder mới cùng cảnh gặp lỗi và mức xung đang dùng.
Không ghi đè save bằng dữ liệu kiểm thử. Nếu muốn mang save cũ sang, sao chép
riêng userdata của bạn sau khi giữ một bản sao an toàn.
'''
    (product / 'README.txt').write_text(readme, encoding='utf-8')
    shutil.copy2(ROOT / 'THIRD_PARTY_NOTICES.md', product / 'THIRD_PARTY_NOTICES.md')
    shutil.copy2(ROOT / 'docs/upstream-sa2-deko-notices.md', product / 'UPSTREAM-NOTICES.md')
    shutil.copy2(ROOT / 'LICENSE', product / 'LICENSE')

    game_files = []
    if args.include_game:
        assert (args.game_root / 'default.xex').is_file(), 'Missing game data'
        for p in sorted(args.game_root.rglob('*')):
            if p.is_file():
                relative = p.relative_to(args.game_root).as_posix()
                assert p.stat().st_size < 4 << 30, f'File too large for FAT32: {relative}'
                game_files.append((p, f'switch/mgr-nx-deko/game_root/{relative}'))
        manifest['game_data'] = {'files': len(game_files), 'bytes': sum(p.stat().st_size for p, _ in game_files)}
    manifest_path = product / 'manifest.json'
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding='utf-8')
    archive = product / ('MGR-deko3d-full-local.zip' if args.include_game else 'MGR-deko3d-test.zip')
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
        for path in sorted(stage.iterdir()):
            if path.is_file():
                z.write(path, path.relative_to(product).as_posix())
        for name in ('README.txt', 'THIRD_PARTY_NOTICES.md', 'UPSTREAM-NOTICES.md', 'LICENSE', 'manifest.json'):
            z.write(product / name, name)
        for index, (path, name) in enumerate(game_files, 1):
            z.write(path, name, compress_type=zipfile.ZIP_STORED)
            if index % 10 == 0 or index == len(game_files):
                print(f'Game data {index}/{len(game_files)}', flush=True)
    result = {'archive': str(archive), 'bytes': archive.stat().st_size, 'sha256': digest(archive)}
    (product / 'package-result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
