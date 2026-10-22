#!/usr/bin/env python3
"""A/B of a compiler flag or macro on ONE recompiled file with the Switch build's own flags, in seconds instead of a
full build: each variant is compiled to an object with machine code (LTO off) and its instructions, loads and stores
are counted. Static counts only: they say what the compiler does with a variant, not how often the code runs.

  python tools/one_file_ab.py <N> name=flags [name=flags ...]      (several flags separated by '|')
  python tools/one_file_ab.py 117 base= "nofp=-fomit-frame-pointer"
  python tools/one_file_ab.py 117 base= "barrier=-DREX_LOOP_BARRIER()=((void)0)"

The flags come from the configured Switch tree (out/nx/build.ninja, the file's own build statement), without -flto and
without the precompiled header (the file includes the generated pch itself). Objects go to out/one_file_ab/.
Needs out/nx to be configured (tools/win/build_switch.ps1 once) and devkitA64. The prefix of the generated files
(sa2_recomp.N.cpp) is this port's: change PREFIX for another port.
"""
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREFIX = 'sa2_recomp'
DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''
LOAD = re.compile(r'^(ldr|ldp|ldur|ldrb|ldrh|ldrsw|ldrsb|ldrsh|ldurb|ldurh|ldursw|ld1)')
STORE = re.compile(r'^(str|stp|stur|strb|strh|sturb|sturh|st1)')
INS = re.compile(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    n = sys.argv[1]
    out = os.path.join(ROOT, 'out', 'one_file_ab')
    os.makedirs(out, exist_ok=True)
    lines = open(os.path.join(ROOT, 'out', 'nx', 'build.ninja'), encoding='utf-8').read().split('\n')
    head = 'build CMakeFiles/%s.dir/generated/default/%s.%s.cpp.obj:' % (PREFIX, PREFIX, n)
    start = next((i for i, l in enumerate(lines) if l.startswith(head)), None)
    if start is None:
        sys.exit('no build statement for %s.%s.cpp in out/nx/build.ninja' % (PREFIX, n))
    parts = {}
    for l in lines[start + 1:start + 8]:
        for key in ('DEFINES', 'FLAGS', 'INCLUDES'):
            if l.startswith('  %s = ' % key):
                parts[key] = l[len(key) + 5:]
    base = [DEVKITA64 + 'aarch64-none-elf-g++' + EXE]
    for key in ('DEFINES', 'INCLUDES', 'FLAGS'):
        skip = False
        for c in shlex.split(parts.get(key, '')):
            if skip:
                skip = False
            elif c == '-include':
                skip = True
            elif c not in ('-flto', '-fno-fat-lto-objects', '-Winvalid-pch') and not c.startswith('-fprofile'):
                base.append(c)
    src = os.path.join(ROOT, 'app', 'generated', 'default', '%s.%s.cpp' % (PREFIX, n))
    for variant in sys.argv[2:]:
        name, _, flags = variant.partition('=')
        obj = os.path.join(out, 'r%s_%s.o' % (n, name))
        r = subprocess.run(base + [f for f in flags.split('|') if f] + ['-c', src, '-o', obj],
                           cwd=os.path.join(ROOT, 'out', 'nx'), capture_output=True, text=True)
        if r.returncode != 0:
            print(name, 'FAILED', r.stderr[-1500:])
            continue
        d = subprocess.run([DEVKITA64 + 'aarch64-none-elf-objdump' + EXE, '-d', '--no-show-raw-insn', obj],
                           capture_output=True, text=True).stdout
        ins = loads = stores = msr = dmb = tbl = frames = 0
        for line in d.split('\n'):
            m = INS.match(line)
            if not m:
                continue
            ins += 1
            op = m.group(2)
            if LOAD.match(op):
                loads += 1
            elif STORE.match(op):
                stores += 1
            elif op == 'msr' and 'fpcr' in m.group(3):
                msr += 1
            elif op == 'dmb':
                dmb += 1
            elif op == 'tbl':
                tbl += 1
            elif op == 'mov' and m.group(3).replace(' ', '').startswith('x29,sp'):
                frames += 1
        print('%-14s %9d instructions %8d loads %8d stores %6d msr fpcr %5d dmb %6d tbl %6d frame set-ups' %
              (name, ins, loads, stores, msr, dmb, tbl, frames))


if __name__ == '__main__':
    main()
