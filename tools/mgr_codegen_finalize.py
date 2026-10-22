"""Record MGR-specific halt evidence and preserve identical generated file mtimes.

Run only after codegen and all post-codegen passes finish. Never changes code.
The halt list is derived from emitted branch comments AND loaded guest opcodes,
so the Switch validation does not inherit another game's address whitelist.
"""
from pathlib import Path
import bisect
import hashlib
import json
import os
import re

ROOT = Path(__file__).resolve().parents[1]
RUN = ROOT / 'out/experiments/full_codegen_20261016'
GEN = ROOT / 'app/generated'
OLD = RUN / 'generated_pass1'
image = (ROOT / 'out/default_image.bin').read_bytes()
same = changed = 0
for path in GEN.rglob('*'):
    if not path.is_file() or path.suffix not in ('.cpp', '.h', '.cmake'):
        continue
    old = OLD / path.relative_to(GEN)
    if old.is_file() and path.read_bytes() == old.read_bytes():
        stat = old.stat()
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        same += 1
    else:
        changed += 1
halts = {}
oracle = None
for path in (GEN / 'default').glob('*_recomp.*.cpp'):
    text = path.read_text(encoding='utf-8')
    heads = list(re.finditer(r'^DEFINE_REX_FUNC\(([^)]+)\) \{', text, re.M))
    starts = [m.start() for m in heads]
    for branch in re.finditer(r'// b 0x([0-9a-fA-F]{8})\s*\n', text):
        addr = int(branch[1], 16)
        offset = addr - 0x82000000
        if offset < 0 or image[offset:offset+4] != b'\x48\x00\x00\x00':
            continue
        owner = bisect.bisect_right(starts, branch.start()) - 1
        if owner >= 0:
            halts.setdefault(heads[owner][1], []).append(f'0x{addr:08X}')
    if 'DEFINE_REX_FUNC(sub_825B4070) {' in text:
        body = text.split('DEFINE_REX_FUNC(sub_825B4070) {', 1)[1].split('\n}\n', 1)[0]
        oracle = hashlib.sha256(('void OriginalFilter(PPCContext& ctx, uint8_t* base) {' + body + '\n}\n').encode()).hexdigest()
record = dict(identical_mtimes_restored=same, changed_files=changed,
              wwise_oracle_sha256=oracle, guest_halts=halts)
(RUN / 'codegen_finalization.json').write_text(json.dumps(record, indent=2) + '\n')
(RUN / 'mgr_halts.txt').write_text(','.join(sorted(halts)))
print(json.dumps(record, indent=2))
