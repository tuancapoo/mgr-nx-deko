#!/usr/bin/env python3
"""Develops a post-codegen step on a scratch copy of the generated code and measures it with the Switch compiler,
without touching app/generated or building anything else.

  python tools/scratch_ab.py copy                      app/generated/default -> out/gen_scratch/default (once, or
                                                       again to start over from the tree's current text)
  python tools/scratch_ab.py count N [N ...] [-- extra compiler flags]
                                                       compiles out/gen_scratch/default/<prefix>_recomp.N.cpp with the
                                                       Switch build's flags (from out/nx/build.ninja, without LTO and
                                                       the profile, plus -fomit-frame-pointer) and prints its
                                                       instructions, loads, stores and a few instruction kinds
  python tools/scratch_ab.py save NAME / compare NAME  remembers the last counts under NAME / prints the difference

Typical use: copy; count 16 64 35; save before; run a tool with --gen out/gen_scratch/default; count ...; compare before.
Needs out/nx configured once and devkitA64.
"""
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'app', 'generated', 'default')
SCRATCH = os.path.join(ROOT, 'out', 'gen_scratch', 'default')
STATE = os.path.join(ROOT, 'out', 'gen_scratch', 'counts.json')
DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''
INS = re.compile(r'^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$')
KINDS = ('fmov', 'rev', 'fcvt', 'bl', 'blr', 'msr', 'dmb', 'tbl')


def prefix():
    return next(f.split('_recomp.')[0] for f in os.listdir(GEN) if re.match(r'\w+_recomp\.\d+\.cpp$', f))


def flags():
    lines = open(os.path.join(ROOT, 'out', 'nx', 'build.ninja'), encoding='utf-8').read().split('\n')
    p = prefix()
    head = 'build CMakeFiles/%s_recomp.dir/generated/default/%s_recomp.0.cpp.obj:' % (p, p)
    start = next((i for i, l in enumerate(lines) if l.startswith(head)), None)
    if start is None:
        sys.exit('no build statement for %s_recomp.0.cpp in out/nx/build.ninja (configure the Switch tree once)' % p)
    parts = {}
    for l in lines[start + 1:start + 8]:
        for key in ('DEFINES', 'FLAGS', 'INCLUDES'):
            if l.startswith('  %s = ' % key):
                parts[key] = l[len(key) + 5:]
    out = ['-I' + SCRATCH]
    for key in ('DEFINES', 'INCLUDES', 'FLAGS'):
        skip = False
        for c in shlex.split(parts.get(key, '')):
            if skip:
                skip = False
            elif c == '-include':
                skip = True
            elif c in ('-flto', '-fno-fat-lto-objects', '-Winvalid-pch') or c.startswith('-fprofile'):
                continue
            elif c.startswith('-I') and os.path.normcase(os.path.normpath(c[2:])) == os.path.normcase(GEN):
                continue  # the scratch copy stands for it
            else:
                out.append(c)
    return out + ['-fomit-frame-pointer']


def count(numbers, extra):
    base = flags() + extra
    p = prefix()
    result = {}
    for n in numbers:
        src = os.path.join(SCRATCH, '%s_recomp.%s.cpp' % (p, n))
        obj = os.path.join(ROOT, 'out', 'gen_scratch', 'r%s.o' % n)
        r = subprocess.run([DEVKITA64 + 'aarch64-none-elf-g++' + EXE] + base + ['-c', src, '-o', obj],
                           cwd=os.path.join(ROOT, 'out', 'nx'), capture_output=True, text=True)
        if r.returncode != 0:
            print('%s FAILED\n%s' % (n, r.stderr[-3000:]))
            sys.exit(1)
        d = subprocess.run([DEVKITA64 + 'aarch64-none-elf-objdump' + EXE, '-d', '--no-show-raw-insn', obj],
                           capture_output=True, text=True).stdout
        c = {'instructions': 0, 'loads': 0, 'stores': 0}
        for k in KINDS:
            c[k] = 0
        for line in d.split('\n'):
            m = INS.match(line)
            if not m or m.group(1) == 'nop':
                continue
            op = m.group(1)
            c['instructions'] += 1
            if re.match(r'ld[rp]|ldur|ld1', op):
                c['loads'] += 1
            elif re.match(r'st[rp]|stur|st1', op):
                c['stores'] += 1
            if op in KINDS:
                c[op] += 1
        result[n] = c
        print('%-4s %s' % (n, '  '.join('%s %d' % (k, v) for k, v in c.items())))
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    state = json.load(open(STATE)) if os.path.exists(STATE) else {}
    state['_last'] = result
    json.dump(state, open(STATE, 'w'))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == 'copy':
        os.makedirs(SCRATCH, exist_ok=True)
        n = 0
        for f in os.listdir(GEN):
            src = os.path.join(GEN, f)
            if os.path.isfile(src):
                shutil.copyfile(src, os.path.join(SCRATCH, f))
                n += 1
        print('%d files copied to %s' % (n, SCRATCH))
    elif cmd == 'count':
        args = sys.argv[2:]
        extra = []
        if '--' in args:
            extra = args[args.index('--') + 1:]
            args = args[:args.index('--')]
        count(args, extra)
    elif cmd in ('save', 'compare'):
        state = json.load(open(STATE))
        if cmd == 'save':
            state[sys.argv[2]] = state['_last']
            json.dump(state, open(STATE, 'w'))
            print('saved as', sys.argv[2])
        else:
            before, after = state[sys.argv[2]], state['_last']
            for n in after:
                if n not in before:
                    continue
                print('%-4s %s' % (n, '  '.join(
                    '%s %+d (%+.2f %%)' % (k, after[n][k] - before[n][k],
                                           100.0 * (after[n][k] - before[n][k]) / max(1, before[n][k]))
                    for k in ('instructions', 'loads', 'stores')) + '  ' + '  '.join(
                    '%s %+d' % (k, after[n][k] - before[n][k]) for k in KINDS if after[n][k] != before[n][k])))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
