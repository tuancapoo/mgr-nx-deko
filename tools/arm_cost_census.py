#!/usr/bin/env python3
"""Census of the A57-expensive constructs in a Switch ELF, per function (one streaming pass over objdump -d).

What it counts (each is cheap on the build PC and costly on the Cortex-A57 at 1020 MHz, so a PC profile hides it):
  outline   calls to GCC's outline atomics (__aarch64_cas4_acq, __aarch64_ldadd8_relax, ...): a call, a load of
            __aarch64_have_lse_atomics and a branch around every atomic operation, then the LL/SC loop the A57 needs
            anyway (-mno-outline-atomics inlines the loop);
  llsc      inline exclusive loads (ldxr/ldaxr and their byte/half/pair forms): one per atomic read-modify-write;
  tls       calls to __aarch64_read_tp (-mtp=soft: every thread_local access is a call that reads tpidrro_el0 and a
            slot of libnx's thread block);
  fpcr      msr fpcr (a floating-point mode switch: the A57 serialises its FP pipeline);
  dmb       memory barriers;
  svc       supervisor calls (only libnx's wrappers have them: who calls those is what matters);
  frame     functions that set up the frame pointer (mov x29, sp).

  python tools/arm_cost_census.py out/nx/sa2 [--top 25] [--only outline,tls] [--callers NAME ...]
--callers NAME: also list the functions that call NAME (bl/b), with their call counts (e.g. mutexLock, svcSleepThread).
Recompiled game functions (sub_XXXXXXXX, __imp__sub_XXXXXXXX) are totalled apart from the rest.
"""
import argparse
import collections
import os
import re
import subprocess

DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''
KINDS = ['outline', 'llsc', 'tls', 'fpcr', 'dmb', 'svc', 'frame']
OUTLINE = re.compile(r'<__aarch64_(?:cas|swp|ldadd|ldset|ldclr|ldeor)\d+_\w+>')
HEAD = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
INS = re.compile(r'^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$')
TARGET = re.compile(r'<([^>+]+)>')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--only', default=','.join(KINDS))
    ap.add_argument('--callers', nargs='*', default=[])
    a = ap.parse_args()
    proc = subprocess.Popen([DEVKITA64 + 'aarch64-none-elf-objdump' + EXE, '-d', '--no-show-raw-insn', a.elf],
                            stdout=subprocess.PIPE, text=True, errors='replace', bufsize=1 << 20)
    counts = {k: collections.Counter() for k in KINDS}
    outline_kinds = collections.Counter()
    callers = {n: collections.Counter() for n in a.callers}
    functions = 0
    func = None
    for line in proc.stdout:
        m = HEAD.match(line)
        if m:
            func = m.group(1)
            functions += 1
            continue
        m = INS.match(line)
        if not m or not func:
            continue
        op, args = m.groups()
        if op in ('bl', 'b'):
            t = TARGET.search(args)
            if t:
                name = t.group(1)
                if OUTLINE.search(args):
                    counts['outline'][func] += 1
                    outline_kinds[name] += 1
                elif name == '__aarch64_read_tp':
                    counts['tls'][func] += 1
                if name in callers and name != func:
                    callers[name][func] += 1
        elif op in ('ldxr', 'ldaxr', 'ldxrb', 'ldaxrb', 'ldxrh', 'ldaxrh', 'ldxp', 'ldaxp'):
            counts['llsc'][func] += 1
        elif op == 'msr' and args.startswith('fpcr'):
            counts['fpcr'][func] += 1
        elif op == 'dmb':
            counts['dmb'][func] += 1
        elif op == 'svc':
            counts['svc'][func] += 1
        elif op == 'mov' and args.replace(' ', '') == 'x29,sp':
            counts['frame'][func] += 1
    proc.wait()
    guest = lambda n: n.startswith(('sub_8', '__imp__sub_8'))  # noqa: E731
    print(f'{a.elf}: {functions} functions')
    for kind in a.only.split(','):
        c = counts[kind]
        total = sum(c.values())
        in_guest = sum(v for n, v in c.items() if guest(n))
        print(f'\n{kind}: {total} in {len(c)} functions ({in_guest} in {sum(1 for n in c if guest(n))} recompiled game '
              f'functions, {total - in_guest} in {sum(1 for n in c if not guest(n))} others)')
        if kind == 'outline':
            print('  helpers: ' + ', '.join(f'{n} x{v}' for n, v in outline_kinds.most_common()))
        if kind == 'frame':
            continue
        for name, v in c.most_common(a.top):
            print(f'  {v:6d}  {name[:150]}')
    for name, c in callers.items():
        print(f'\ncallers of {name}: {sum(c.values())} calls from {len(c)} functions')
        for n, v in c.most_common(a.top):
            print(f'  {v:6d}  {n[:150]}')


if __name__ == '__main__':
    main()
