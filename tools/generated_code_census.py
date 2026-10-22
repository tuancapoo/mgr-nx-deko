#!/usr/bin/env python3
"""Static census of a ReXGlue port's generated code: the counts behind the CPU candidates of docs/enhancements.md
("Fifteen CPU candidates for any ReXGlue port"). Read only; works on any port's generated folder.

  calls      by form: direct (`__imp__sub_X`), through the hookable name (`sub_X`), indirect, kernel imports; the
             functions called through the hookable name, and how many of those really have a hook macro in the
             sources (the others are only mentioned there, which tools/direct_calls.py takes for "hooked")
  leaves     functions without any call, and where the uses of the registers still kept in ctx (r3-r10, f1-f13,
             v0-v13) are: in functions with loop barriers, in leaves
  barriers   loops with a barrier by what they contain: a call the compiler cannot see through, only direct calls,
             no call; of the last, how many read or store no guest memory
  idioms     sqrt calls, float-to-integer conversions by form, single loads and stores, load -> store pairs of one
             register (the float copy idiom), vector intrinsics by use

  python tools/generated_code_census.py [--gen app/generated/default] [--sources app/src sdk/src sdk/include]
Do not run it while the code generator is writing the folder.
"""
import argparse
import collections
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUNC = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{\n(.*?)^\}\n', re.S | re.M)
ANY_CALL = re.compile(r'\(ctx, base[,)]|REX_CALL_INDIRECT_FUNC|ppc_trap\(')
OPAQUE = re.compile(r'^\s*(?:if \(.*\) )?(?:sub_[0-9A-F]{8}|__imp__(?!sub_)\w+)\(ctx, base\);|REX_CALL_INDIRECT_FUNC|ppc_trap\(')
DIRECT = re.compile(r'__imp__sub_[0-9A-F]{8}\(ctx, base\);')
GUEST_STORE = re.compile(r'REX_STORE_|REX_MM_STORE|simde_mm_store\w*\(\(simde__m128i?\*\)REX_RAW_ADDR')
GUEST_LOAD = re.compile(r'REX_LOAD_|REX_RAW_ADDR')
CTXREG = re.compile(r'\bctx\.(?:r(?:[3-9]|10)|f(?:[1-9]|1[0-3])|v(?:[0-9]|1[0-3]))\.')
HOOK_MACRO = re.compile(r'\b(?:REX_HOOK(?:_RAW|_EX)?|\w+_HOT_HOOK\w*|\w+_COPY_FOR_HOOK|REX_PPC_HOOK|PPC_FUNC)\s*\(\s*'
                        r'(?:sub_)?([0-9A-Fa-f]{8})\b')
PAIR = re.compile(r'^\ttemp\.u32 = REX_LOAD(?:_D)?_U32\(.*\);\n\t(\w+)\.f64 = (?:double\(temp\.f32\)|'
                  r'rex::ppc::SingleToDoubleAnyMode\(temp\.u32\));\n(?:\t.*\n){0,2}?\ttemp\.f32 = '
                  r'(?:float\(\1\.f64\)|rex::ppc::DoubleToSingleAnyMode\(\1\.f64\));', re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated', 'default'))
    ap.add_argument('--sources', nargs='*', default=[os.path.join(ROOT, 'app', 'src'), os.path.join(ROOT, 'sdk', 'src'),
                                                     os.path.join(ROOT, 'sdk', 'include')])
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    calls = collections.Counter()
    hooked_targets = collections.Counter()
    intrinsics = collections.Counter()
    idioms = collections.Counter()
    fn = collections.Counter()
    loops = collections.Counter()
    for f in files:
        text = open(os.path.join(a.gen, f), encoding='utf-8').read()
        for m in re.finditer(r'^\s*(?:if \(.*\) )?(__imp__)?(sub_[0-9A-F]{8})\(ctx, base\);', text, re.M):
            if m.group(1):
                calls['direct'] += 1
            else:
                calls['hookable'] += 1
                hooked_targets[m.group(2)[4:]] += 1
        calls['indirect'] += len(re.findall(r'REX_CALL_INDIRECT_FUNC(?:_AT)?\(', text))
        calls['imports'] += len(re.findall(r'^\s*__imp__(?!sub_)\w+\(ctx, base\);', text, re.M))
        idioms['sqrt calls'] += len(re.findall(r'\bsqrtf?\(', text))
        idioms['single loads'] += len(re.findall(r'double\(temp\.f32\)|SingleToDoubleAnyMode\(', text))
        idioms['single stores'] += len(re.findall(r'temp\.f32 = float\(|DoubleToSingleAnyMode\(', text))
        idioms['load -> store pairs of one register'] += len(PAIR.findall(text))
        for name in ('simde_mm_cvtsd_si64', 'simde_mm_cvtsd_si32', 'simde_mm_cvttsd_si64', 'simde_mm_cvttsd_si32',
                     'rex::ppc::fctiwz'):
            idioms[name] += text.count(name + '(')
        intrinsics.update(re.findall(r'(?:rex::ppc::)?simde_(?:x_)?mm_[a-z0-9_]+', text))
        for m in FUNC.finditer(text):
            body = m.group(2)
            n_ctx = len(CTXREG.findall(body))
            n_bar = body.count('REX_LOOP_BARRIER()')
            fn['functions'] += 1
            fn['ctx uses'] += n_ctx
            if n_bar:
                fn['functions with barriers'] += 1
                fn['ctx uses in functions with barriers'] += n_ctx
            if not ANY_CALL.search(body):
                fn['leaves'] += 1
                fn['ctx uses in leaves'] += n_ctx
                if n_bar:
                    fn['leaves with barriers'] += 1
            if not n_bar:
                continue
            lines = body.split('\n')
            label_at = {}
            for i, line in enumerate(lines):
                lm = re.match(r'^(loc_[0-9A-Fa-f]+):$', line.strip())
                if lm:
                    label_at[lm.group(1)] = i
            last_back = {}
            for i, line in enumerate(lines):
                for g in re.findall(r'goto (loc_[0-9A-Fa-f]+);', line):
                    if g in label_at and label_at[g] <= i:
                        last_back[g] = i
            for i, line in enumerate(lines):
                if 'REX_LOOP_BARRIER()' not in line:
                    continue
                j = i - 1
                while j >= 0 and not re.match(r'^loc_[0-9A-Fa-f]+:$', lines[j].strip()):
                    j -= 1
                end = last_back.get(lines[j].strip()[:-1]) if j >= 0 else None
                if end is None:
                    continue
                inner = lines[i + 1:end + 1]
                loops['loops'] += 1
                if any(OPAQUE.search(b) for b in inner):
                    loops['with a call the compiler cannot see through'] += 1
                elif any(DIRECT.search(b) for b in inner):
                    loops['with direct calls only'] += 1
                else:
                    loops['without a call'] += 1
                    if not any(GUEST_LOAD.search(b) for b in inner):
                        loops['without a call, reading no guest memory'] += 1
                    if not any(GUEST_STORE.search(b) for b in inner):
                        loops['without a call, storing no guest memory'] += 1
    hooks = set()
    for base in a.sources:
        for dirpath, _, names in os.walk(base):
            for name in names:
                if name.endswith(('.cpp', '.h', '.inc', '.c')):
                    text = open(os.path.join(dirpath, name), encoding='utf-8', errors='replace').read()
                    hooks.update(m.group(1).upper() for m in HOOK_MACRO.finditer(text))
    real = [t for t in hooked_targets if t in hooks]
    total_calls = sum(calls.values())
    print('%s: %d files, %d functions' % (a.gen, len(files), fn['functions']))
    print('calls: %d sites: %d direct, %d through the hookable name, %d indirect (%.0f %%), %d kernel imports' %
          (total_calls, calls['direct'], calls['hookable'], calls['indirect'],
           100.0 * calls['indirect'] / max(1, total_calls), calls['imports']))
    print('  functions called through the hookable name: %d, of which %d have a hook macro in the sources (%d call '
          'sites) and %d are only mentioned (%d call sites)' %
          (len(hooked_targets), len(real), sum(hooked_targets[t] for t in real), len(hooked_targets) - len(real),
           calls['hookable'] - sum(hooked_targets[t] for t in real)))
    print('leaves: %d functions without any call (%.1f %%); uses of r3-r10 / f1-f13 / v0-v13 in ctx: %d, of which %d '
          '(%.0f %%) in the %d functions with loop barriers and %d in leaves (%d leaves have barriers)' %
          (fn['leaves'], 100.0 * fn['leaves'] / max(1, fn['functions']), fn['ctx uses'],
           fn['ctx uses in functions with barriers'],
           100.0 * fn['ctx uses in functions with barriers'] / max(1, fn['ctx uses']), fn['functions with barriers'],
           fn['ctx uses in leaves'], fn['leaves with barriers']))
    print('barriers: ' + ', '.join('%d %s' % (n, k) for k, n in loops.items()))
    print('idioms: ' + ', '.join('%d %s' % (n, k) for k, n in idioms.items()))
    print('vector intrinsics: ' + ', '.join('%s %d' % (k.replace('simde_mm_', ''), n)
                                            for k, n in intrinsics.most_common(40)))


if __name__ == '__main__':
    main()
