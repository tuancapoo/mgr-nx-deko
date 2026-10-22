#!/usr/bin/env python3
"""What the LTO link left in a Switch ELF (one streaming pass over objdump -d, about 20 s for a 55 MB program):

  - the direct calls between recompiled game functions that were NOT inlined, by the callee's size (is the inliner
    leaving small functions out of line, or only large ones?);
  - the game functions with exactly one direct call site left (in an ordinary program GCC inlines a function that is
    called once and drops its body; here the function table takes every function's address, so it cannot);
  - the calls from game code into code outside the LTO unit (the runtime), by target;
  - callee-saved registers pushed per game function, and the functions that keep a frame pointer;
  - the split-off cold parts (`.cold` symbols) and the clones the interprocedural passes made (`.constprop`, `.isra`,
    `.part`): from the symbol table.

  python tools/lto_census.py out/nx/sa2 [--top 25]

Needs the unstripped ELF (out/nx/sa2, or one archived in out/nx/elf) and devkitA64.
"""
import argparse
import collections
import os
import re
import subprocess

DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''
LABEL = re.compile(r'^[0-9a-f]+ <(.+)>:$')
INSN = re.compile(r'^\s+[0-9a-f]+:\t(\S+)(?:\t(.*))?$')
GAME = re.compile(r'^(?:__imp__)?sub_[0-9A-F]{8}(?:\.\w+)*$')
TARGET = re.compile(r'<([^>+]+)(\+0x[0-9a-f]+)?>')
SAVED = re.compile(r'\bx(19|2[0-8])\b')
BUCKETS = [8, 16, 32, 64, 128, 256, 1024, 1 << 30]


def symbols(elf):
    """Counts and bytes of the text symbols by the suffix an optimisation pass gave them."""
    out = subprocess.run([DEVKITA64 + 'aarch64-none-elf-nm' + EXE, '-S', elf], capture_output=True, text=True).stdout
    kinds = collections.OrderedDict((k, [0, 0]) for k in ('.cold', '.constprop', '.isra', '.part', '.lto_priv'))
    for line in out.split('\n'):
        parts = line.split()
        if len(parts) != 4 or parts[2] not in 'tT':
            continue
        for k in kinds:
            if k + '.' in parts[3] + '.':
                kinds[k][0] += 1
                kinds[k][1] += int(parts[1], 16)
    return kinds


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf')
    ap.add_argument('--top', type=int, default=25, help='how many targets outside the game code to list')
    args = ap.parse_args()

    p = subprocess.Popen([DEVKITA64 + 'aarch64-none-elf-objdump' + EXE, '-d', '--no-show-raw-insn', args.elf],
                         stdout=subprocess.PIPE, text=True, errors='replace')
    size = collections.Counter()          # function -> instructions
    calls = []                            # (caller, callee, is a tail branch)
    indirect = collections.Counter()
    saved = collections.Counter()         # function -> callee-saved x registers pushed in its prologue
    frame = set()
    cur = None
    n_in = 0
    for line in p.stdout:
        m = LABEL.match(line)
        if m:
            cur = m.group(1)
            n_in = 0
            continue
        m = INSN.match(line)
        if not m or cur is None:
            continue
        op, operands = m.group(1), m.group(2) or ''
        size[cur] += 1
        n_in += 1
        if op in ('bl', 'b'):
            t = TARGET.search(operands)
            if t and not t.group(2) and (op == 'bl' or t.group(1) != cur):
                calls.append((cur, t.group(1), op == 'b'))
        elif op in ('blr', 'br'):
            indirect[cur] += 1
        elif op in ('stp', 'str') and n_in <= 12 and 'sp' in operands:
            saved[cur] += len(set(SAVED.findall(operands.split('[')[0])))
        elif op == 'mov' and operands.startswith('x29, sp'):
            frame.add(cur)
    p.wait()

    game = {f for f in size if GAME.match(f)}
    print('functions: %d, of them game: %d (%d instructions of %d)' % (
        len(size), len(game), sum(size[f] for f in game), sum(size.values())))
    gg = [c for c in calls if c[0] in game and c[1] in game]
    gr = [c for c in calls if c[0] in game and c[1] not in game]
    print('direct call sites game -> game: %d (%d of them tail branches), game -> other code: %d' % (
        len(gg), sum(1 for c in gg if c[2]), len(gr)))
    print('indirect branches (blr / br) in game functions: %d' % sum(indirect[f] for f in game))
    sites = collections.Counter(c[1] for c in gg)
    print('\ngame -> game call sites left, by the callee\'s size in instructions:')
    hist = collections.Counter()
    for _, callee, _ in gg:
        hist[next(b for b in BUCKETS if size[callee] <= b)] += 1
    acc = 0
    for b in BUCKETS:
        acc += hist[b]
        print('  %-8s %7d sites  (%5.1f %% so far)' % ('<= %d' % b if b < 1 << 30 else 'larger', hist[b],
                                                      100.0 * acc / max(1, len(gg))))
    once = [f for f, n in sites.items() if n == 1]
    print('\ngame functions with exactly one direct call site left: %d (%d instructions, %.1f %% of the game code)' % (
        len(once), sum(size[f] for f in once), 100.0 * sum(size[f] for f in once) / max(1, sum(size[f] for f in game))))
    print('\ncalls from game code to other code, the %d most frequent targets:' % args.top)
    for name, n in collections.Counter(c[1] for c in gr).most_common(args.top):
        print('  %7d  %s (%d instructions)' % (n, name[:90], size.get(name, 0)))
    pushed = [saved[f] for f in game]
    print('\ncallee-saved x registers pushed in game prologues: %d, %.2f per function; with a frame pointer: %d' % (
        sum(pushed), sum(pushed) / max(1, len(pushed)), len(frame & game)))
    print('\nsymbols the optimiser made (count, bytes):')
    for k, (n, b) in symbols(args.elf).items():
        print('  %-11s %6d  %9d' % (k, n, b))


if __name__ == '__main__':
    main()
