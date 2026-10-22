#!/usr/bin/env python3
"""Compiles chosen source files with the Switch compiler and the Switch build's own flags, without building the NRO:
each file's build statement is taken from the configured Switch tree (out/nx/build.ninja) and run with its object
written to out/switch_compile_check/ instead. LTO and the profile are left out (machine code is wanted, and a profile
of older code says nothing here), and the precompiled header is included as a plain header (a .gch from before a
header change would be stale). Nothing in out/nx is touched.

  python tools/switch_compile_check.py [--add FLAG ...] [--warnings] <source path, or the end of one> [...]
  python tools/switch_compile_check.py --like app/src/cpu/sa2_enhancements.cpp app/src/cpu/new_file.cpp
      a source the configured tree does not know yet, compiled with the flags of one it does
  python tools/switch_compile_check.py sdk/src/core/threading_posix.cpp app/src/cpu/sa2_hot_guest.cpp

Use: to check that code only the Switch build compiles (POSIX threading, `#if defined(__aarch64__)` paths) still
builds after a change, on the PC, in a minute. Prints each object's instruction count and exits 1 on an error.
Needs out/nx to be configured once (tools\\win\\build_switch.ps1) and devkitA64.
"""
import os
import re
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''
BUILD = re.compile(r'^build (\S+): (C|CXX)_COMPILER__\S+ (.+?) \|')


def main():
    args = sys.argv[1:]
    extra = []
    while '--add' in args:
        i = args.index('--add')
        extra.append(args[i + 1])
        del args[i:i + 2]
    like = None
    if '--like' in args:
        i = args.index('--like')
        like = args[i + 1].replace('\\', '/')
        del args[i:i + 2]
    tally = '--warnings' in args
    args = [a for a in args if a != '--warnings']
    if not args:
        sys.exit(__doc__)
    out = os.path.join(ROOT, 'out', 'switch_compile_check')
    os.makedirs(out, exist_ok=True)
    lines = open(os.path.join(ROOT, 'out', 'nx', 'build.ninja'), encoding='utf-8').read().split('\n')
    statements = []
    for i, l in enumerate(lines):
        m = BUILD.match(l)
        if m:
            statements.append((m.group(3).replace('$ ', ' ').replace('$:', ':').replace('\\', '/'), m.group(2), i))
    failed = 0
    for want in args:
        want_n = want.replace('\\', '/')
        found = [s for s in statements if s[0].endswith(want_n)]
        if not found and like and os.path.exists(os.path.join(ROOT, want_n)):
            # Not in the configured tree: the build statement of the --like source, with this file in its place.
            found = [(os.path.join(ROOT, want_n).replace('\\', '/'), s[1], s[2]) for s in statements
                     if s[0].endswith(like)][:1]
        if not found:
            print('%s: no build statement in out/nx/build.ninja' % want)
            failed += 1
            continue
        source, lang, at = found[0]
        parts = {}
        for l in lines[at + 1:at + 10]:
            for key in ('DEFINES', 'FLAGS', 'INCLUDES'):
                if l.startswith('  %s = ' % key):
                    parts[key] = l[len(key) + 5:]
        cmd = [DEVKITA64 + ('aarch64-none-elf-g++' if lang == 'CXX' else 'aarch64-none-elf-gcc') + EXE]
        for key in ('DEFINES', 'INCLUDES', 'FLAGS'):
            tokens = shlex.split(parts.get(key, ''))
            k = 0
            while k < len(tokens):
                c = tokens[k]
                if c == '-include' and k + 1 < len(tokens):
                    # The precompiled header as a plain header: a copy with no .gch next to it.
                    pch = tokens[k + 1]
                    copy = os.path.join(out, 'pch_' + re.sub(r'\W', '_', os.path.dirname(pch))[-60:] + '.hxx')
                    if os.path.exists(pch):
                        shutil.copyfile(pch, copy)
                        cmd += ['-include', copy]
                    k += 2
                    continue
                if c not in ('-flto', '-fno-fat-lto-objects', '-Winvalid-pch') and not c.startswith('-fprofile'):
                    cmd.append(c)
                k += 1
        obj = os.path.join(out, re.sub(r'\W', '_', want_n) + '.o')
        r = subprocess.run(cmd + extra + ['-c', source, '-o', obj], cwd=os.path.join(ROOT, 'out', 'nx'),
                           capture_output=True, text=True)
        if r.returncode != 0:
            print('%s: FAILED\n%s' % (want, r.stderr[-4000:]))
            failed += 1
            continue
        d = subprocess.run([DEVKITA64 + 'aarch64-none-elf-objdump' + EXE, '-d', '--no-show-raw-insn', obj],
                           capture_output=True, text=True).stdout
        ins = sum(1 for line in d.split('\n') if re.match(r'^\s+[0-9a-f]+:\s+\S', line))
        dmb = len(re.findall(r'\tdmb\t', d))
        warnings = r.stderr.count('warning:')
        print('%s: ok (%d instructions, %d dmb, %d warnings)' % (want, ins, dmb, warnings))
        if tally:
            # Warnings by kind, the function's name taken out (a profile check: --add -Wcoverage-mismatch ...).
            kinds = {}
            for line in r.stderr.split('\n'):
                if 'warning:' in line:
                    text = re.sub(r"['‘][^'’]*['’]", "'...'", line.split('warning:', 1)[1].strip())
                    kinds[text] = kinds.get(text, 0) + 1
            for text, n in sorted(kinds.items(), key=lambda kv: -kv[1]):
                print('    %6d  %s' % (n, text[:150]))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
