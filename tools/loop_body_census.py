#!/usr/bin/env python3
"""What sits inside loops in AArch64 objects: instructions, loads and stores between each backward branch and its
target (each instruction counted once, however deeply nested), per object and in total.

A change to how loops are compiled (tools/loop_barriers.py) cannot be judged by a file's size: without a barrier the
compiler moves work out of loops and copies loop headers, which makes a function longer and each iteration shorter.
This counts the part that repeats. It is still a static count (no trip counts), so it compares two builds of the same
code, nothing more.

  python tools/loop_body_census.py <object or ELF> [<object> ...]
  python tools/loop_body_census.py --compare before.o after.o     the functions both have, side by side
"""
import re
import subprocess
import sys

OBJDUMP = 'C:/devkitPro/devkitA64/bin/aarch64-none-elf-objdump.exe'
FUNC = re.compile(r'^([0-9a-f]+) <([^>]+)>:$')
INS = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
TARGET = re.compile(r'\b([0-9a-f]+) <')
BRANCH = re.compile(r'^(b|b\.\w+|cbz|cbnz|tbz|tbnz)$')
LOAD = re.compile(r'ld[rp]|ldur|ld1|ldrs?[bhw]')
STORE = re.compile(r'st[rp]|stur|st1|str[bh]')


def functions(path):
    d = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', path], capture_output=True, text=True).stdout
    out, name = {}, None
    for line in d.split('\n'):
        m = FUNC.match(line)
        if m:
            name = m.group(2)
            out[name] = []
            continue
        m = INS.match(line)
        if m and name is not None and m.group(2) != 'nop':
            out[name].append((int(m.group(1), 16), m.group(2), m.group(3)))
    return out


def loop_part(ins):
    """(instructions, loads, stores, loops) inside the backward-branch ranges of one function."""
    if not ins:
        return 0, 0, 0, 0
    lo, hi = ins[0][0], ins[-1][0]
    ranges = []
    for addr, op, args in ins:
        if BRANCH.match(op):
            m = TARGET.search(args)
            if m:
                t = int(m.group(1), 16)
                if lo <= t <= addr <= hi:
                    ranges.append((t, addr))
    if not ranges:
        return 0, 0, 0, 0
    inside = [i for i in ins if any(a <= i[0] <= b for a, b in ranges)]
    return (len(inside), sum(1 for i in inside if LOAD.match(i[1])), sum(1 for i in inside if STORE.match(i[1])),
            len(ranges))


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if args[0] == '--compare':
        a, b = functions(args[1]), functions(args[2])
        ta = tb = (0, 0, 0, 0)
        whole_a = whole_b = 0
        for name in a:
            if name in b:
                ta = tuple(x + y for x, y in zip(ta, loop_part(a[name])))
                tb = tuple(x + y for x, y in zip(tb, loop_part(b[name])))
                whole_a += len(a[name])
                whole_b += len(b[name])
        print('whole functions: %d -> %d instructions (%+.2f %%)' % (whole_a, whole_b,
                                                                      100.0 * (whole_b - whole_a) / max(1, whole_a)))
        for label, x, y in zip(('inside loops: instructions', 'inside loops: loads', 'inside loops: stores',
                                'backward branches'), ta, tb):
            print('%-28s %7d -> %7d (%+.2f %%)' % (label, x, y, 100.0 * (y - x) / max(1, x)))
        return
    total = (0, 0, 0, 0)
    for path in args:
        t = (0, 0, 0, 0)
        for ins in functions(path).values():
            t = tuple(x + y for x, y in zip(t, loop_part(ins)))
        total = tuple(x + y for x, y in zip(total, t))
        print('%s: inside loops %d instructions, %d loads, %d stores; %d backward branches' % ((path,) + t))
    if len(args) > 1:
        print('total: inside loops %d instructions, %d loads, %d stores; %d backward branches' % total)


if __name__ == '__main__':
    main()
