"""Extract the real generated leaf, compile and run its differential test.

Windows: load tools/win/env.ps1 first, then run with Python. No project build,
code generation, production source rewriting or game execution is performed.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--compiler', default='clang++')
ap.add_argument('--output', type=Path, default=root / 'out/tests/wwise_filter')
ap.add_argument('--negative-control', action='store_true')
args = ap.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
source = root / 'app/generated/default/sa2_recomp.231.cpp'
text = source.read_text(encoding='utf-8')
body = text.split('DEFINE_REX_FUNC(sub_825B4070) {', 1)[1].split('\n}\n', 1)[0]
oracle = 'void OriginalFilter(PPCContext& ctx, uint8_t* base) {' + body + '\n}\n'
(args.output / 'mgr_wwise_filter_oracle.inc').write_text(oracle, encoding='utf-8')
print('Generated oracle SHA256:', hashlib.sha256(oracle.encode()).hexdigest(), flush=True)
exe = args.output / 'test_mgr_wwise_filter.exe'
command = [args.compiler, '-std=c++23', '-O3', '-ffp-contract=off', '-Wno-ignored-attributes',
           '-fno-strict-aliasing',
           '-I' + str(root / 'sdk/include'), '-I' + str(root / 'sdk/thirdparty/simde'),
           '-I' + str(args.output), str(root / 'app/tests/test_mgr_wwise_filter.cpp'),
           '-o', str(exe)]
if args.negative_control:
    command.insert(1, '-DMGR_WWISE_TEST_NEGATIVE_CONTROL')
subprocess.run(command, check=True)
run = subprocess.run([str(exe)])
if args.negative_control:
    if run.returncode != 1:
        raise RuntimeError('Negative control did not detect the deliberate memory corruption')
    print('PASS negative control: deliberate native-result corruption detected')
else:
    run.check_returncode()
