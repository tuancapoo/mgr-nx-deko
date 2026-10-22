"""Collect only new, offline-proven MGR vertex declaration variants.

This does not relax runtime shader identity checks or synthesize missing DEFs.
Use the current package's in_ok directory as --base and the runtime diagnostic
shader-misses/variants directory as --variants. Output must be a new folder.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess


def parse(data):
    def word(offset):
        return struct.unpack_from('>I', data, offset)[0]
    header = word(24)
    start, size = word(4) + word(header), word(header + 4)
    decl = header + 36 + word(header + 24) * 4
    if size % 12 or start + size > len(data):
        raise ValueError('invalid sequencer extent')
    entries = tuple(word(decl + i * 4) & 4095 for i in range(word(header + 28)))
    return start, size, entries, struct.unpack_from('>' + str(size // 4) + 'I', data, start)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, type=Path)
    parser.add_argument('--variants', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--tool', type=Path, default=root / 'out/decl_repair_audit/repair_vertex_variant_declarations.exe')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    containers, proofs = args.output / 'containers', args.output / 'proofs'
    containers.mkdir(); proofs.mkdir()
    known = {hashlib.sha256(p.read_bytes()).digest() for p in args.base.glob('*.bin')}
    sources = []
    for path in sorted(args.base.glob('vs_*.bin')):
        data = path.read_bytes()
        try:
            sources.append((path, data, parse(data)))
        except (ValueError, IndexError, struct.error):
            continue
    report = []
    for variant in sorted(args.variants.glob('*.bin')):
        data = variant.read_bytes()
        start, size, entries, code = parse(data)
        declared = set(entries)
        candidates = []
        for path, original, (os, oz, oe, oc) in sources:
            if (start, size, entries) != (os, oz, oe) or len(data) != len(original):
                continue
            if data[:start] != original[:start] or data[start + size:] != original[start + size:]:
                continue
            if any(a != b and i // 3 not in declared for i, (a, b) in enumerate(zip(code, oc))):
                continue
            candidates.append(path)
        entry = {'variant': variant.name, 'input_sha256': hashlib.sha256(data).hexdigest(), 'status': 'NO_SAFE_TEMPLATE'}
        target = proofs / ('vs_proven_' + variant.stem.removeprefix('vs_') + '.bin')
        for index, source in enumerate(candidates):
            result = subprocess.run([args.tool, source, variant, target], capture_output=True, text=True)
            (proofs / (variant.stem + f'_{index}.log')).write_text(result.stdout + result.stderr)
            if result.returncode:
                entry['status'] = 'REJECTED_BY_PROOF'
                continue
            output = target.read_bytes()
            digest = hashlib.sha256(output).digest()
            entry.update(template=source.name, output_sha256=digest.hex(), status='ALREADY_PRESENT' if digest in known else 'NEW_PROVEN')
            if digest not in known:
                shutil.copy2(target, containers / target.name)
                known.add(digest)
            break
        report.append(entry)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    counts = {status: sum(r['status'] == status for r in report) for status in sorted({r['status'] for r in report})}
    print(json.dumps(counts, indent=2))


if __name__ == '__main__':
    main()
