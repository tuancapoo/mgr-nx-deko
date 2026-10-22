#!/usr/bin/env python3
"""What the Switch compiler makes of the AArch64-only forms of the CPU round of 2026-10-12 (sasrt-nx's): compiles
sdk/tests/arm_forms_check.cpp with devkitA64 and the Switch build's own flags (taken from out/nx/build.ninja, a
recompiled file's build statement, without LTO, the precompiled header and the profile) and prints, per function, its
instruction count and the instructions that matter (exclusive pairs, `dmb`, table lookups, conversions).

It also checks the claims the documentation makes about them and exits 1 when one does not hold:
  - the __atomic lock words use ldaxr / stlxr and run no `dmb` unless g_rex_atomic_legacy_barrier is set (the one
    `dmb` each has is behind that branch); the legacy __sync forms end with an unconditional `dmb`;
  - vperm is one two-register table lookup (tbl with {v, v}); vctsxs is fcvtzs, vctuxs fcvtzu;
  - with -DREX_VMX_HELPERS_X86_FORM the same functions are the longer x86 emulation.

  python tools/arm_forms_check.py [--keep]
Needs out/nx to be configured once (tools\\win\\build_switch.ps1) and devkitA64. Nothing is linked or run.
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
FUNC = re.compile(r'^[0-9a-f]+ <(\w+)>:$')
INS = re.compile(r'^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$')
INTERESTING = ('ldaxr', 'stlxr', 'ldxr', 'stxr', 'dmb', 'tbl', 'fcvtzs', 'fcvtzu', 'msr', 'mrs', 'bl', 'frinti', 'uqadd',
               'srhadd', 'urhadd', 'ucvtf', 'cmhi', 'ushl', 'sshl', 'clz', 'sdiv', 'udiv', 'cbz', 'cbnz', 'smull',
               'csel')


def switch_flags():
    lines = open(os.path.join(ROOT, 'out', 'nx', 'build.ninja'), encoding='utf-8').read().split('\n')
    head = 'build CMakeFiles/sa2_recomp.dir/generated/default/sa2_recomp.0.cpp.obj:'
    start = next((i for i, l in enumerate(lines) if l.startswith(head)), None)
    if start is None:
        sys.exit('no build statement for sa2_recomp.0.cpp in out/nx/build.ninja (configure the Switch tree once)')
    parts = {}
    for l in lines[start + 1:start + 8]:
        for key in ('DEFINES', 'FLAGS', 'INCLUDES'):
            if l.startswith('  %s = ' % key):
                parts[key] = l[len(key) + 5:]
    flags = []
    for key in ('DEFINES', 'INCLUDES', 'FLAGS'):
        skip = False
        for c in shlex.split(parts.get(key, '')):
            if skip:
                skip = False
            elif c == '-include':
                skip = True
            elif c in ('-flto', '-fno-fat-lto-objects', '-Winvalid-pch') or c.startswith('-fprofile'):
                continue
            else:
                flags.append(c)
    return flags


def functions(obj):
    d = subprocess.run([DEVKITA64 + 'aarch64-none-elf-objdump' + EXE, '-d', '--no-show-raw-insn', obj],
                       capture_output=True, text=True).stdout
    out = {}
    name = None
    for line in d.split('\n'):
        m = FUNC.match(line)
        if m:
            name = m.group(1)
            out[name] = []
            continue
        m = INS.match(line)
        if m and name:
            out[name].append((m.group(1), m.group(2)))
    return out


def compile_variant(flags, name, extra):
    out = os.path.join(ROOT, 'out', 'arm_forms_check')
    os.makedirs(out, exist_ok=True)
    obj = os.path.join(out, name + '.o')
    src = os.path.join(ROOT, 'sdk', 'tests', 'arm_forms_check.cpp')
    r = subprocess.run([DEVKITA64 + 'aarch64-none-elf-g++' + EXE] + flags + extra + ['-c', src, '-o', obj],
                       cwd=os.path.join(ROOT, 'out', 'nx'), capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('%s: the Switch compiler failed:\n%s' % (name, r.stderr[-3000:]))
    return functions(obj)


def describe(ins):
    counts = {}
    for op, _ in ins:
        if op in INTERESTING:
            counts[op] = counts.get(op, 0) + 1
    return ', '.join('%d %s' % (n, op) for op, n in sorted(counts.items())) or '-'


def dmb_is_conditional(ins):
    """True when no `dmb` is on the straight path after the exclusive store: from the store on, skipping the retry
    branch that follows it, another conditional branch comes before any `dmb` (the test of the barrier byte)."""
    ops = [op for op, _ in ins]
    store = next((i for i, op in enumerate(ops) if op in ('stlxr', 'stxr')), None)
    if store is None:
        return 'dmb' not in ops
    at = store + 1
    if at < len(ops) and ops[at] in ('cbnz', 'cbz'):
        at += 1  # the retry of the exclusive pair
    for op in ops[at:]:
        if op == 'dmb':
            return False
        if op in ('cbz', 'cbnz', 'tbz', 'tbnz', 'ret', 'b') or op.startswith('b.'):
            return True
    return True


def main():
    flags = switch_flags()
    neon = compile_variant(flags, 'neon', [])
    x86 = compile_variant(flags, 'x86_form', ['-DREX_VMX_HELPERS_X86_FORM'])
    problems = []
    print('%-28s %5s  %s' % ('function', 'insns', 'of interest'))
    for name, ins in neon.items():
        if name.startswith('check_'):
            print('%-28s %5d  %s' % (name, len(ins), describe(ins)))
    print('with -DREX_VMX_HELPERS_X86_FORM:')
    for name in ('check_vperm', 'check_vctsxs', 'check_vctuxs', 'check_adds_epu32', 'check_avg_epi8', 'check_avg_epi16',
                 'check_avg_epi32', 'check_cvtepu32_ps', 'check_cmpgt_epu8', 'check_cmpgt_epu16', 'check_sllv_epi16',
                 'check_srlv_epi16', 'check_srav_epi16', 'check_sllv_epi8'):
        print('%-28s %5d  %s' % (name, len(x86[name]), describe(x86[name])))

    def ops(name, table=neon):
        return [op for op, _ in table[name]]

    for name in ('check_atomic_inc', 'check_atomic_dec', 'check_atomic_cas32', 'check_atomic_cas64',
                 'check_atomic_exchange_add'):
        o = ops(name)
        if 'ldaxr' not in o or 'stlxr' not in o:
            problems.append('%s: no ldaxr / stlxr pair' % name)
        if o.count('dmb') != 1 or not dmb_is_conditional(neon[name]):
            problems.append('%s: its dmb is not only the one behind g_rex_atomic_legacy_barrier' % name)
    for name in ('check_legacy_inc', 'check_legacy_cas32'):
        o = ops(name)
        if 'dmb' not in o or dmb_is_conditional(neon[name]):
            problems.append('%s: expected the legacy unconditional dmb' % name)
    if ops('check_vperm').count('tbl') != 1:
        problems.append('check_vperm: not one tbl')
    if 'fcvtzs' not in ops('check_vctsxs'):
        problems.append('check_vctsxs: no fcvtzs')
    if 'fcvtzu' not in ops('check_vctuxs'):
        problems.append('check_vctuxs: no fcvtzu')
    for name in ('check_vperm', 'check_vctsxs', 'check_vctuxs'):
        if len(x86[name]) <= len(neon[name]):
            problems.append('%s: the x86 form is not longer than the NEON form' % name)
    # The second CPU round (sa2-nx): the conversions to integer and the float copy helpers.
    for name in ('check_fctid', 'check_fctiw'):
        o = ops(name)
        if 'frinti' not in o or 'fcvtzs' not in o or 'bl' in o:
            problems.append('%s: not frinti + fcvtzs without a call' % name)
    if 'fcvtzs' not in ops('check_fctidz') or 'bl' in ops('check_fctidz'):
        problems.append('check_fctidz: not fcvtzs without a call')
    for name in ('check_single_pure', 'check_quiet_single'):
        o = ops(name)
        if 'bl' in o or 'msr' in o or 'mrs' in o:
            problems.append('%s: a call or a mode access' % name)
    if len(neon['check_quiet_single']) > 8:
        problems.append('check_quiet_single: more than 8 instructions')
    # The small VMX helpers as their NEON instruction, and shorter than the x86 emulation.
    for name, wanted in (('check_adds_epu32', 'uqadd'), ('check_avg_epi8', 'srhadd'), ('check_avg_epi16', 'srhadd'),
                         ('check_avg_epi32', 'srhadd'), ('check_cvtepu32_ps', 'ucvtf'), ('check_cmpgt_epu8', 'cmhi'),
                         ('check_cmpgt_epu16', 'cmhi'), ('check_sllv_epi16', 'ushl'), ('check_srlv_epi16', 'ushl'),
                         ('check_srav_epi16', 'sshl'), ('check_sllv_epi8', 'ushl')):
        if wanted not in ops(name):
            problems.append('%s: no %s' % (name, wanted))
        # Not shorter: for avg_epi32 the compiler already makes the x86 form the same two instructions.
        if len(x86[name]) < len(neon[name]):
            problems.append('%s: the x86 form (%d) is shorter than the NEON form (%d)' %
                            (name, len(x86[name]), len(neon[name])))
    # The third CPU round: the divisions are the instruction, with no branch; a constant divisor is no division.
    for name, wanted in (('check_divwu', 'udiv'), ('check_divdu', 'udiv'), ('check_divw', 'sdiv'),
                         ('check_divd', 'sdiv')):
        o = ops(name)
        if wanted not in o or any(op in ('cbz', 'cbnz', 'bl') or op.startswith('b.') for op in o):
            problems.append('%s: not a branch-free %s' % (name, wanted))
    for name, old in (('check_divwu', 'check_old_divwu'), ('check_divw', 'check_old_divw')):
        if len(neon[name]) > len(neon[old]):
            problems.append('%s (%d instructions) is longer than the guarded C form (%d)' %
                            (name, len(neon[name]), len(neon[old])))
    if 'sdiv' in ops('check_divw_by_10'):
        problems.append('check_divw_by_10: a division by a constant kept its sdiv')
    if problems:
        print('PROBLEMS:')
        for p in problems:
            print('  ' + p)
        sys.exit(1)
    print('all claims hold')


if __name__ == '__main__':
    main()
