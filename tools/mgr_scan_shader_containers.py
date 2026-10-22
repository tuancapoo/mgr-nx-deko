"""Find complete, uncompressed XDK shader containers in MGR's disc files.

This is a bounded structural scan, not a CPK decompressor or proof of coverage.
Discovered bytes are review inputs; they are never installed as executable shaders.
"""
import argparse
import hashlib
import json
import mmap
from pathlib import Path
import struct

from mgr_chapter_cache import atomic_write, json_bytes, validate_container


def scan(files, output, known_dirs=()):
    output = Path(output)
    if output.exists():
        raise ValueError("Use a new scan output directory")
    known = set()
    for folder in known_dirs:
        known.update(hashlib.sha256(path.read_bytes()).hexdigest() for path in Path(folder).glob('*.bin'))
    output.mkdir(parents=True)
    found = {}
    report = {"scope": "complete uncompressed XDK containers in raw file bytes only",
              "compressed_or_encrypted_data_decoded": False, "compatibility": "unproven",
              "files": [], "containers": []}
    for file in files:
        file = Path(file)
        report['files'].append({'name': file.name, 'bytes': file.stat().st_size})
        if not file.stat().st_size:
            continue
        with file.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            for magic in (b'\x10\x2a\x11\x00', b'\x10\x2a\x11\x01'):
                offset = 0
                while True:
                    offset = data.find(magic, offset)
                    if offset < 0:
                        break
                    start = offset
                    offset += 4
                    if len(data) - start < 28:
                        continue
                    virtual, physical = struct.unpack_from('>II', data, start + 4)
                    size = virtual + physical
                    if size < 28 or size > 8 << 20 or size > len(data) - start:
                        continue
                    container = data[start:start + size]
                    try:
                        stage = validate_container(container, variant=True)
                    except ValueError:
                        continue
                    sha = hashlib.sha256(container).hexdigest()
                    if sha in found:
                        found[sha]['occurrences'] += 1
                        continue
                    item = {'source': file.name, 'offset': start, 'bytes': size, 'sha256': sha,
                            'stage': stage, 'already_known': sha in known, 'occurrences': 1}
                    found[sha] = item
                    if sha not in known:
                        atomic_write(output/'collect/scanned'/f'{stage}_{sha}.bin', container)
    report['containers'] = list(found.values())
    report['unique_containers'] = len(found)
    report['new_review_inputs'] = sum(not value['already_known'] for value in found.values())
    atomic_write(output/'scan.json', json_bytes(report))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--known', nargs='*', type=Path, default=[])
    args = parser.parse_args()
    files = [args.game_root/'default.xex', *sorted((args.game_root/'media').glob('*.cpk'))]
    files = [file for file in files if file.is_file()]
    if not files:
        parser.error('No default.xex or media/*.cpk files found')
    try:
        report = scan(files, args.output, args.known)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')
    print(json.dumps({key: report[key] for key in ('scope', 'unique_containers', 'new_review_inputs')}, indent=2))


if __name__ == '__main__':
    main()
