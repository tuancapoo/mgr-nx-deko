#!/usr/bin/env python3
"""Indirect calls: a census of where each one goes, and a direct call where it almost always goes to one place
(post-codegen step of the "optimised" codegen flavour; docs/enhancements.md, "The second CPU round of 2026-10-12",
candidate 6). Works on any ReXGlue port's generated code.

A `bctrl` (a virtual call, a call through a function pointer) is REX_CALL_INDIRECT_FUNC(ctr): a range check, a load
from the dispatch table, a null check and a call through a register, which the compiler can neither inline nor see
through. One in six call sites of SA2 is one (20,624).

1. Tagging. Every `bctrl` statement becomes
       REX_CALL_INDIRECT_FUNC_AT(0x<function>, <n>, <target expression>);
   with the function's address and the site's ordinal in it (stable across regenerations). On the Switch that is
   REX_CALL_INDIRECT_FUNC again. In the PC build it first reports the target to RexIndirectCensus when the census is
   on (g_rex_indirect_census, the app sets it from its diagnostic key), so a PC session yields, per site, how often
   it ran and where it went. (The name keeps "REX_CALL_INDIRECT_FUNC" in it: the other tools find it as before.)
2. Promotion. A list (app/indirect_promotions.txt, lines `<function> <n> <target>`, written by
   tools/indirect_promotions.py from a census file) turns a site into
       if (REX_PROMOTED_IS(ctr, 0x<target>u)) [[likely]] { REX_PROMOTED_CALL(0x<target>, sub_<target>);
           sub_<target>(ctx, base); } else { REX_CALL_INDIRECT_FUNC_AT(..); }
   The guard compares the very target, so the promoted call is the call the table would have made: the table holds
   sub_<target>, the hookable name (tools/direct_calls.py, which must run AFTER this step, makes the call
   __imp__sub_ where no hook exists, as for every other direct call). Any other target takes the old path.
   REX_PROMOTED_CALL is nothing on the Switch; in the PC build it counts itself (g_rex_promoted_calls) and calls
   RexPromotedMismatch when the table's entry for the target is not that function: the one assumption, tested on
   every promoted call a PC run makes. A target that is not a function of this tree, or a site the list names but
   the code no longer has, is an error: make the list again.
   A line of the list may name up to three targets (`<function> <n> <target> <target2> [<target3>]`, the most
   frequent first): the site then tests them in that order, each with its own guarded direct call, before the
   indirect one (third round of 2026-10-12: sites that go to two or three functions).

  python tools/indirect_calls.py --gen app/generated/default [--promote app/indirect_promotions.txt] [--check]
Idempotent; a changed list is applied by a new run (sites leave and enter the promoted form).
REX_INDIRECT_CENSUS_OFF (compile time) removes the census and the check from PC builds too; REX_PROMOTED_CALLS_OFF
makes every guard false, which leaves the indirect call (REX_PROMOTED_IS is the target comparison).
"""
import argparse
import os
import re
import sys

FUNC = re.compile(r'^DEFINE_REX_FUNC\((sub_([0-9A-F]{8}))\) \{$')
PLAIN = re.compile(r'^\tREX_CALL_INDIRECT_FUNC\(((?:ctx\.)?ctr\.u32)\);$')
SITE = re.compile(r'^\tREX_CALL_INDIRECT_FUNC_AT\(0x[0-9A-F]{8}, \d+, ((?:ctx\.)?ctr\.u32)\);$')
PROMOTED = re.compile(r'^\tif \(REX_PROMOTED_IS\(((?:ctx\.)?ctr\.u32), 0x[0-9A-F]{8}u\)\) \[\[likely\]\] \{ '
                      r'REX_PROMOTED_CALL\(.*\} else \{ REX_CALL_INDIRECT_FUNC_AT\(.*\); \}$')
MARKER = '// rexglue ports: indirect call sites (tools/indirect_calls.py)'
MACROS = MARKER + '''
#ifndef REX_PROMOTED_IS
#if defined(REX_PROMOTED_CALLS_OFF)
#define REX_PROMOTED_IS(x, target) false
#else
#define REX_PROMOTED_IS(x, target) ((uint32_t)(x) == (target))
#endif
#endif
#ifndef REX_CALL_INDIRECT_FUNC_AT
#if defined(__SWITCH__) || defined(REX_INDIRECT_CENSUS_OFF)
#define REX_CALL_INDIRECT_FUNC_AT(function, site, x) REX_CALL_INDIRECT_FUNC(x)
#define REX_PROMOTED_CALL(target, function) ((void)0)
#else
extern "C" unsigned char g_rex_indirect_census;
extern "C" volatile unsigned long long g_rex_promoted_calls;
extern "C" void RexIndirectCensus(unsigned function, unsigned site, unsigned target) noexcept;
extern "C" void RexPromotedMismatch(unsigned target) noexcept;
#define REX_CALL_INDIRECT_FUNC_AT(function, site, x)                                              \\
  do {                                                                                            \\
    if (g_rex_indirect_census) [[unlikely]] RexIndirectCensus((function), (site), (uint32_t)(x)); \\
    REX_CALL_INDIRECT_FUNC(x);                                                                    \\
  } while (0)
#define REX_PROMOTED_CALL(target, function)                                    \\
  do {                                                                         \\
    g_rex_promoted_calls = g_rex_promoted_calls + 1;                           \\
    if (REX_LOOKUP_FUNC(base, target) != &function) [[unlikely]] RexPromotedMismatch(target); \\
  } while (0)
#endif
#endif
'''
DECL_BEGIN = '// rexglue ports: targets of promoted indirect calls (tools/indirect_calls.py)'
DECL_END = '// end of promoted targets'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--promote', help='list of `<function> <site> <target>` lines (hex, decimal, hex)')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): indirect_calls: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    promote = {}
    if a.promote and os.path.exists(a.promote):
        for line in open(a.promote, encoding='utf-8'):
            line = line.split('#')[0].split()
            if len(line) >= 3:
                promote[(line[0].upper(), int(line[1]))] = [t.upper() for t in line[2:5]]
    texts = {}
    defined = set()
    for f in files:
        texts[f] = open(os.path.join(a.gen, f), encoding='utf-8').read().split('\n')
        for line in texts[f]:
            m = FUNC.match(line)
            if m:
                defined.add(m.group(2))
    tagged = promoted = chained = already = changed = 0
    used = set()
    for f in files:
        lines = texts[f]
        out = list(lines)
        function = None
        ordinal = 0
        targets = set()
        for i, line in enumerate(lines):
            m = FUNC.match(line)
            if m:
                function, ordinal = m.group(2), 0
                continue
            if function is None or not (line.startswith('\tREX_CALL_INDIRECT_') or line.startswith('\tif (REX_PROMOTED_IS(')):
                continue
            # Only the plain `bctrl` form: the generator's other indirect calls (the forms that save and restore
            # registers around the call, tail calls) are left as they are and not counted.
            m = PLAIN.match(line) or SITE.match(line) or PROMOTED.match(line)
            if not m or i == 0 or not lines[i - 1].startswith('\t// bctrl'):
                continue
            site = ordinal
            ordinal += 1
            expr = m.group(1)
            tag = 'REX_CALL_INDIRECT_FUNC_AT(0x%s, %d, %s);' % (function, site, expr)
            wanted = promote.get((function, site))
            if wanted:
                for target in wanted:
                    if target not in defined:
                        sys.exit('ERROR (post-codegen patch): indirect_calls: %s names target %s for %s site %d, '
                                 'which is not a function of %s' % (a.promote, target, function, site, a.gen))
                new = '\t'
                for n, target in enumerate(wanted):
                    new += ('if (REX_PROMOTED_IS(%s, 0x%su)) %s{ REX_PROMOTED_CALL(0x%s, sub_%s); sub_%s(ctx, base); } '
                            'else ' % (expr, target, '[[likely]] ' if n == 0 else '', target, target, target))
                new += '{ %s }' % tag
                targets.update(wanted)
                promoted += 1
                chained += len(wanted) > 1
                used.add((function, site))
                # An earlier run (and tools/direct_calls.py after it) may have written the same line with __imp__.
                if line.replace('__imp__sub_', 'sub_') == new:
                    already += 1
                    continue
            else:
                new = '\t' + tag
                if line == new:
                    already += 1
                    continue
            tagged += 1
            out[i] = new
        # The file's own header declares only what the generator saw it call.
        if DECL_BEGIN in out:
            b, e = out.index(DECL_BEGIN), out.index(DECL_END)
            del out[b:e + 1]
        if targets:
            first = next(k for k, l in enumerate(out) if l.startswith('#include'))
            out[first + 1:first + 1] = [DECL_BEGIN] + ['DECLARE_REX_FUNC(sub_%s);' % t for t in sorted(targets)] + \
                [DECL_END]
        if out != lines:
            changed += 1
            if not a.check:
                open(os.path.join(a.gen, f), 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    missing = [k for k in promote if k not in used]
    if missing:
        sys.exit('ERROR (post-codegen patch): indirect_calls: %d sites of %s are not in the generated code (first: %s '
                 'site %d): make the list again from a census of this code' %
                 (len(missing), a.promote, missing[0][0], missing[0][1]))
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if MARKER not in text:
        anchor = '//=============================================================================\n// Flush Mode'
        if anchor not in text:
            sys.exit('ERROR (post-codegen patch): indirect_calls: no "Flush Mode" section in %s: the generated pch '
                     'changed' % pch)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + '\n' + anchor, 1))
    print('indirect_calls%s: %d bctrl sites written (%d promoted to a guarded direct call, %d of them with two or '
          'three targets; %d in the list), %d already as wanted; %d files changed' %
          (' (check only)' if a.check else '', tagged, promoted, chained, len(promote), already, changed))


if __name__ == '__main__':
    main()
