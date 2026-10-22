#!/usr/bin/env python3
"""Loads of an address from the GOT in a Switch ELF, per function (one streaming pass over objdump -d).

The NRO is position independent. For a global the compiler cannot prove local to the executable (an `extern`
declaration with default visibility: anything defined in another translation unit that is not part of the same LTO
link), GCC does not compute the address (adrp + add) but loads it from the global offset table:
    adrp x0, <page of the GOT slot>
    ldr  x0, [x0, #slot]          <- this load
    ldr  w1, [x0]                 the access itself
One more load on every such access, and one more cache line (the GOT) in the hot set. This counts the `ldr` whose
address (the adrp page plus the offset) falls in .got, totalled for recompiled game functions and for the rest, and
lists the functions with the most.

  python tools/arm_got_census.py out/nx/sa2 [--top 25]
"""
import argparse
import collections
import os
import re
import subprocess

DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''
HEAD = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
ADRP = re.compile(r'^\s+[0-9a-f]+:\s+adrp\s+(x\d+), ([0-9a-f]+)')
LDR = re.compile(r'^\s+[0-9a-f]+:\s+ldr\s+x\d+, \[(x\d+), #(\d+)\]')
ANY = re.compile(r'^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf')
    ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()
    sections = subprocess.run([DEVKITA64 + 'aarch64-none-elf-readelf' + EXE, '-S', '-W', a.elf], capture_output=True,
                              text=True).stdout
    m = re.search(r'\] \.got\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)', sections)
    if not m:
        raise SystemExit('no .got section')
    got, size = int(m.group(1), 16), int(m.group(2), 16)
    p = subprocess.Popen([DEVKITA64 + 'aarch64-none-elf-objdump' + EXE, '-d', '--no-show-raw-insn', a.elf],
                         stdout=subprocess.PIPE, text=True, encoding='utf-8', errors='replace')
    per = collections.Counter()
    slots = collections.Counter()
    pages = {}
    name = None
    for line in p.stdout:
        m = HEAD.match(line)
        if m:
            name = m.group(1)
            pages = {}
            continue
        m = ADRP.match(line)
        if m:
            pages[m.group(1)] = int(m.group(2), 16)
            continue
        m = LDR.match(line)
        if m and m.group(1) in pages:
            address = pages[m.group(1)] + int(m.group(2))
            if got <= address < got + size:
                per[name] += 1
                slots[address] += 1
            continue
        m = ANY.match(line)
        if m and m.group(1) in ('bl', 'blr', 'ret', 'b', 'br'):
            pages = {}  # the page registers do not survive a call; a branch ends the straight line
    game = sum(n for f, n in per.items() if re.match(r'(__imp__)?sub_[0-9A-F]{8}', f))
    total = sum(per.values())
    print('%s: .got has %d slots; %d loads from it in %d functions (%d in recompiled game functions, %d in the rest); '
          '%d slots are loaded somewhere' % (a.elf, size // 8, total, len(per), game, total - game, len(slots)))
    for f, n in per.most_common(a.top):
        print('  %6d  %s' % (n, f[:110]))


if __name__ == '__main__':
    main()
