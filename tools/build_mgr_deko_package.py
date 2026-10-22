"""Build and verify MGR's reNut/deko3d packages from its translated containers.

Run tools/win/build_mgr_deko_shaders.ps1 first. No game-specific GLSL optimisation
passes are applied during bring-up; in particular texture scale and fragment scale
remain dynamic so both 360p and 720p use the same package.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import deko3d_variant_cache


ROOT = Path(__file__).resolve().parent.parent
REFERENCE = Path(os.environ.get('MGR_REFERENCE_ROOT', ROOT.parent / 'mgr-nx'))


def run(args):
    print(' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, cwd=ROOT)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--inputs', type=Path, default=ROOT / 'out/cpk-shader-inputs')
    ap.add_argument('--output', type=Path, default=ROOT / 'out/mgr-deko-shaders')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--spirv-val', type=Path, default=Path(os.environ.get(
        'SPIRV_VAL', REFERENCE / 'out/experiments/chapter_release_20261019/spirv-tools-build/tools/spirv-val.exe')))
    ap.add_argument('--skip-conversion', action='store_true')
    ap.add_argument('--reuse-validation', action='store_true', help='Reuse a complete validation report newer than every SPIR-V input')
    a = ap.parse_args()
    if not a.inputs.is_dir():
        ap.error(f'Missing shader input directory: {a.inputs}')
    if not a.spirv_val.is_file():
        ap.error(f'Missing SPIR-V validator: {a.spirv_val}')
    out = a.output.resolve()
    conversion = out / 'conversion'
    if not a.skip_conversion:
        run([sys.executable, ROOT / 'tools/deko3d_shaders.py', '--spirv', out / 'spirv', '--out', conversion, '--jobs', a.jobs])

    spvs = sorted((out / 'spirv').glob('*.spv'))
    def validate(path):
        p = subprocess.run([str(a.spirv_val), '--target-env', 'vulkan1.0', '--scalar-block-layout', str(path)], capture_output=True, text=True, timeout=120)
        return {'file': path.name, 'ok': p.returncode == 0, 'log': p.stdout + p.stderr}
    validation_path = out / 'spirv-validation.json'
    if a.reuse_validation:
        validation = json.loads(validation_path.read_text())
        assert {v['file'] for v in validation} == {p.name for p in spvs}
        assert validation_path.stat().st_mtime_ns >= max(p.stat().st_mtime_ns for p in spvs)
        print(f'Reusing validation of {len(validation)} unchanged SPIR-V inputs', flush=True)
    else:
        with ThreadPoolExecutor(a.jobs) as pool:
            validation = list(pool.map(validate, spvs))
        validation_path.write_text(json.dumps(validation, indent=2))
    failed = [v for v in validation if not v['ok']]
    if failed:
        raise RuntimeError(f'{len(failed)} SPIR-V validation failures; see spirv-validation.json')

    # Prove every source container is represented, including byte-identical aliases
    # and wrappers whose bytes beyond virtualSize+physicalSize are not part of it.
    groups = {}
    for p in sorted(a.inputs.glob('*.bin')):
        data = p.read_bytes()
        signature, virtual, physical = struct.unpack_from('>III', data)
        assert signature & 0xFFFFFF00 == 0x102A1100
        assert virtual + physical <= len(data)
        digest = hashlib.sha256(data[:virtual + physical]).hexdigest()
        entry = groups.setdefault(digest, {'sha256': digest, 'sources': [], 'stage': 'vs' if signature & 1 else 'ps'})
        entry['sources'].append(p.name)
        hlsl = out / 'hlsl' / (p.stem + '.hlsl')
        if hlsl.is_file():
            found = re.match(r'// (?:pixel|vertex) shader  hash=0x([0-9A-F]{16})', hlsl.read_text())
            if found:
                entry['hash'] = found.group(1)
    coverage = []
    for entry in groups.values():
        assert 'hash' in entry, f'No translated representative of {entry["sources"]}'
        stem = entry['stage'] + '_' + entry['hash']
        glsl = conversion / 'glsl' / (stem + '.glsl')
        dksh = conversion / 'dksh' / (stem + '.dksh')
        assert (out / 'spirv' / (stem + '.spv')).is_file()
        assert glsl.is_file()
        assert dksh.read_bytes()[:4] == b'DKSH'
        entry['glslSha256'] = hashlib.sha256(glsl.read_bytes()).hexdigest()
        entry['dkshSha256'] = hashlib.sha256(dksh.read_bytes()).hexdigest()
        entry['dkshBytes'] = dksh.stat().st_size
        coverage.append(entry)

    run([sys.executable, ROOT / 'tools/renut_switch_shader_cache.py', '--pc', out])
    run([sys.executable, ROOT / 'tools/deko3d_shader_package.py', '--glsl', conversion / 'glsl', '--out', ROOT / 'app/out/renut_switch'])
    # The already checked default variants can be used immediately. Specialised
    # combinations encountered during play are appended by the console compiler.
    warm = {}
    for glsl in sorted((conversion / 'glsl').glob('*.glsl')):
        text = glsl.read_bytes()
        dksh = (conversion / 'dksh' / (glsl.stem + '.dksh')).read_bytes()
        key = deko3d_variant_cache.fnv1a64(text)
        assert key not in warm or warm[key] == dksh
        warm[key] = dksh
    native_cache = bytearray(b'BDKC' + struct.pack('<I', 1) + deko3d_variant_cache.TAG)
    for key, dksh in sorted(warm.items()):
        native_cache += struct.pack('<QI', key, len(dksh)) + dksh
    (ROOT / 'app/out/renut_switch/deko3d_shaders.bin').write_bytes(native_cache)
    report = {'inputFiles': sum(len(e['sources']) for e in coverage), 'uniqueContainers': len(coverage),
              'dekoSpirvValidated': len(validation), 'nativeDkshCompiled': len(coverage),
              'defaultDkshCacheEntries': len(warm), 'glslPasses': [], 'textureScaleCode6': 0.5,
              'shaders': sorted(coverage, key=lambda e: e['hash'])}
    (out / 'coverage.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != 'shaders'}, indent=2))


if __name__ == '__main__':
    main()
