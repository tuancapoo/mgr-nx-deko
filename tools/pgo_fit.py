#!/usr/bin/env python3
"""How much of the committed profile (pgo/) still fits a recompiled file, without building the NRO.

GCC matches a function with its profile by a checksum of its source position and of its control flow. Regenerated
code moves lines and changes branches, and every function that no longer matches is compiled as if it had no profile
(-Wno-coverage-mismatch in the build). This compiles sa2_recomp.<N>.cpp with the Switch build's flags and the
profile, in a scratch folder laid out like the build folder (so GCC finds the file's .gcda under its build name), with
the mismatch warnings on, and counts the functions that lost their profile.

  python tools/pgo_fit.py [--add FLAG ...] <N> [<N> ...]
--add: an extra compiler flag (e.g. -DREX_FLUSH_FLOW_OFF, to see which change costs the fit).
Needs out/nx to be configured with SA2_PGO=use (the release configuration) and devkitA64. Nothing in out/nx is touched.
"""
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''
# The function's name in a GCC warning: between plain or typographic quotes.
QUOTED = re.compile('[\'‘]([^\'’]*)[\'’]')


def main():
    extra = []
    while '--add' in sys.argv:
        i = sys.argv.index('--add')
        extra.append(sys.argv[i + 1])
        del sys.argv[i:i + 2]
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    scratch = os.path.join(ROOT, 'out', 'pgo_fit')
    lines = open(os.path.join(ROOT, 'out', 'nx', 'build.ninja'), encoding='utf-8').read().split('\n')
    totals = [0, 0, 0]
    for n in sys.argv[1:]:
        rel = 'CMakeFiles/sa2_recomp.dir/generated/default/sa2_recomp.%s.cpp.obj' % n
        start = next((i for i, l in enumerate(lines) if l.startswith('build %s:' % rel)), None)
        if start is None:
            sys.exit('no build statement for sa2_recomp.%s.cpp in out/nx/build.ninja' % n)
        parts = {}
        for l in lines[start + 1:start + 8]:
            for key in ('DEFINES', 'FLAGS', 'INCLUDES'):
                if l.startswith('  %s = ' % key):
                    parts[key] = l[len(key) + 5:]
        cmd = [DEVKITA64 + 'aarch64-none-elf-g++' + EXE]
        had_profile = False
        for key in ('DEFINES', 'INCLUDES', 'FLAGS'):
            skip = False
            for c in shlex.split(parts.get(key, '')):
                if skip:
                    skip = False
                elif c == '-include':
                    skip = True
                elif c in ('-flto', '-fno-fat-lto-objects', '-Winvalid-pch', '-Wno-coverage-mismatch',
                           '-Wno-missing-profile'):
                    continue
                elif c.startswith('-fprofile-prefix-path='):
                    had_profile = True
                    cmd.append('-fprofile-prefix-path=' + scratch.replace('/', '\\'))
                else:
                    cmd.append(c)
        if not had_profile:
            sys.exit('out/nx is not configured with SA2_PGO=use: no profile flags in its build statements')
        os.makedirs(os.path.join(scratch, os.path.dirname(rel)), exist_ok=True)
        src = os.path.join(ROOT, 'app', 'generated', 'default', 'sa2_recomp.%s.cpp' % n)
        r = subprocess.run(cmd + extra + ['-Wcoverage-mismatch', '-Wno-error=coverage-mismatch', '-Wmissing-profile',
                                  '-c', src, '-o', rel], cwd=scratch, capture_output=True, text=True)
        if r.returncode != 0:
            print('sa2_recomp.%s.cpp: FAILED\n%s' % (n, r.stderr[-3000:]))
            continue
        functions = sum(1 for l in open(src, encoding='utf-8') if l.startswith('DEFINE_REX_FUNC('))
        moved, lost, absent = set(), set(), set()
        for line in r.stderr.splitlines():
            if 'warning:' not in line:
                continue
            text = line.split('warning:', 1)[1].strip()
            m = re.search(QUOTED, text)
            if not m:
                continue
            if 'source locations' in text:
                moved.add(m.group(1))       # the lines moved: GCC warns and still uses the counts
            elif 'control flow' in text or 'number of counters' in text:
                lost.add(m.group(1))        # the control flow changed: compiled without its profile
            elif 'not found in profile data' in text:
                absent.add(m.group(1))      # not in the profile at all (a function the old code did not have)
        moved -= lost
        print('sa2_recomp.%s.cpp: %d recompiled functions: %d lost their profile (control flow changed), %d have '
              'none (new), %d keep it with moved lines, the others match unchanged' %
              (n, functions, len(lost), len(absent), len(moved)))
        totals[0] += functions
        totals[1] += len(lost)
        totals[2] += len(absent)
    if len(sys.argv) > 2:
        print('total: %d functions, %d lost their profile (%.1f %%), %d have none (%.1f %%)' %
              (totals[0], totals[1], 100.0 * totals[1] / totals[0], totals[2], 100.0 * totals[2] / totals[0]))


if __name__ == '__main__':
    main()
