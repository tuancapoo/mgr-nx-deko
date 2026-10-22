"""Filter ABI audit diagnostics to registers the function actually localizes."""
from pathlib import Path
import collections
import re

root = Path(__file__).resolve().parents[1] / 'out/experiments/full_codegen_20261016'
head = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{', re.M)
decl = re.compile(r'^\t(?:PPCRegister|PPCVRegister) (r0|r2|r11|r12|f0|v(?:3[2-9]|4[0-9]|5[0-9]|6[0-3]))\{\};', re.M)
locals_by_fn = {}
for path in (root / 'audit_input').glob('*.cpp'):
    text = path.read_text()
    matches = list(head.finditer(text))
    for i, match in enumerate(matches):
        end = matches[i+1].start() if i+1 < len(matches) else len(text)
        locals_by_fn[match[1]] = set(decl.findall(text[match.end():end]))
findings = []
counts = collections.Counter()
for line in (root / 'non_argument_final.log').read_text().splitlines():
    if not line.startswith(('ENTRY ', 'RET ', 'HANDOVER ')):
        continue
    kind, name, rest = line.split(' ', 2)
    # A caller has to hand over only registers it declared as locals. Registers
    # that remain in ctx are already visible to the callee.
    regs = set(re.findall(r'\b(?:r0|r2|r11|r12|f0|v(?:3[2-9]|4[0-9]|5[0-9]|6[0-3]))\b',
                          rest.split('|', 1)[0]))
    actual = regs & locals_by_fn.get(name, set())
    if actual:
        counts[kind] += 1
        findings.append(line + ' [localized: ' + ','.join(sorted(actual)) + ']')
out = '\n'.join(findings) + '\n'
(root / 'non_argument_localized_final.log').write_text(out)
print(dict(counts))
print(out)
