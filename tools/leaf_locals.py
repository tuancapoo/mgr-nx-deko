#!/usr/bin/env python3
"""Guest registers as C++ locals in leaf functions with loops (post-codegen step of the "optimised" codegen flavour;
docs/enhancements.md, "The second CPU round of 2026-10-12", candidate 3). Works on any ReXGlue port.

The registers the generator's options do not turn into locals (the argument registers r3-r10, f1-f13, v0-v13, and
whatever else a function mentions as ctx.<register>) live in the PPCContext. In straight-line code that costs little:
the compiler keeps them in host registers between calls. A loop barrier (tools/loop_barriers.py) clobbers all memory,
though, so inside a loop that has one every such register is stored to ctx and loaded back on every iteration.

A leaf function (no call of any kind, no trap) can keep them all in locals for its whole body: nothing else can look
at ctx while it runs. They are loaded at the top and written back at every return:
    REX_LEAF_LOCAL(PPCRegister, l_r4, r4);         PPCRegister l_r4 = ctx.r4;
    ... l_r4 instead of ctx.r4 ...
    REX_LEAF_STORE(l_r4, r4);                      ctx.r4 = l_r4;      before each return and at the end
This is what tools/localize_native.py does for the hand-picked hot functions, without a hook in front. Only leaves
are taken: a local that lives across a call needs a callee-saved host register, which costs more than the reloads it
saves (measured: the stack pointer as such a local made three files 1.3-1.6 % larger). Only leaves that still have a
loop barrier are taken: elsewhere the compiler does the same by itself.

A register is written back only if the function can have changed it: general and float registers whose every mention
is a plain read of a field are loaded but not stored; vector registers (written through pointers) always are.
REX_LEAF_LOCALS_OFF (compile time) makes each local a reference to its ctx register: the old code.

Left alone: functions with setjmp / longjmp or with a register mentioned in a form this script does not know.

  python tools/leaf_locals.py --gen app/generated/default [--check] [--all-leaves]
Idempotent (a rewritten function no longer mentions ctx registers). Run it last among the steps that read ctx.<reg>.
"""
import argparse
import os
import re
import sys

FUNC = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
REG = re.compile(r'\bctx\.(r\d+|f\d+|v\d+|cr\d|ctr|xer|reserved)\b')
CALL = re.compile(r'\(ctx, base[,)]|REX_CALL_INDIRECT_|ppc_trap\(|\bctx\s*[,)]')
RETURN = re.compile(r'\breturn;')
MARKER = '// rexglue ports: registers as locals in leaf functions with loops (tools/leaf_locals.py)'
MACROS = MARKER + '''
#ifndef REX_LEAF_LOCAL
#if defined(REX_LEAF_LOCALS_OFF)
#define REX_LEAF_LOCAL(type, name, reg) type& name = ctx.reg
#define REX_LEAF_STORE(name, reg) ((void)0)
#else
#define REX_LEAF_LOCAL(type, name, reg) type name = ctx.reg
#define REX_LEAF_STORE(name, reg) ctx.reg = name
#endif
#endif
'''


def type_of(reg):
    if reg.startswith('cr'):
        return 'PPCCRRegister'
    if reg == 'xer':
        return 'PPCXERRegister'
    if reg.startswith('v'):
        return 'PPCVRegister'
    return 'PPCRegister'


def order(reg):
    return (re.sub(r'\d', '', reg), int(re.sub(r'\D', '', reg) or 0))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--all-leaves', action='store_true', help='also leaves without a loop barrier')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): leaf_locals: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    done = already = skipped = registers = read_only = changed = 0
    for f in files:
        path = os.path.join(a.gen, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        out = []
        i = 0
        touched = False
        while i < len(lines):
            line = lines[i]
            out.append(line)
            if not FUNC.match(line):
                i += 1
                continue
            j = i + 1
            while j < len(lines) and lines[j] != '}':
                j += 1
            body = lines[i + 1:j]
            code = [l for l in body if not l.lstrip().startswith('//')]
            text = '\n'.join(code)
            i = j
            if 'REX_LEAF_LOCAL(' in text:
                already += 1
                out.extend(body)
                continue
            regs = sorted({m.group(1) for m in REG.finditer(text)}, key=order)
            if not regs or CALL.search(text) or (not a.all_leaves and 'REX_LOOP_BARRIER()' not in text):
                out.extend(body)
                continue
            if 'setjmp' in text or 'longjmp' in text or re.search(r'\bl_(?:r|f|v|cr)\d', text) or \
                    not any(l.strip() == 'REX_FUNC_PROLOGUE();' for l in code):
                skipped += 1
                out.extend(body)
                continue
            # Which registers can the function have changed? r and f: only through an assignment to a field.
            stored = []
            for r in regs:
                if r[0] in 'rf' and r[1:].isdigit():
                    mentions = re.findall(r'\bctx\.' + r + r'\b(\.\w+)?(\s*(?:[-+*/|&^%]|<<|>>)?=(?!=))?', text)
                    if all(field and not assign for field, assign in mentions) and \
                            not re.search(r'&\s*ctx\.' + r + r'\b', text):
                        read_only += 1
                        continue
                stored.append(r)
            sync = ''.join(' REX_LEAF_STORE(l_%s, %s);' % (r, r) for r in stored).strip()
            new = []
            for l in body:
                if l.lstrip().startswith('//'):
                    new.append(l)
                    continue
                l = REG.sub(lambda m: 'l_' + m.group(1), l)
                if sync:
                    # Braces: the generated code also writes a return as the body of a one-line `if`.
                    l = RETURN.sub('{ ' + sync + ' return; }', l)
                new.append(l)
                if l.strip() == 'REX_FUNC_PROLOGUE();':
                    for r in regs:
                        new.append('\tREX_LEAF_LOCAL(%s, l_%s, %s);' % (type_of(r), r, r))
            if sync:
                new.append('\t' + sync)  # falling off the end is a return too
            out.extend(new)
            done += 1
            registers += len(regs)
            touched = True
        if touched:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if MARKER not in text:
        anchor = '#define REX_LOAD_U8(x)'
        if anchor not in text:
            sys.exit('ERROR (post-codegen patch): leaf_locals: no REX_LOAD_U8 in %s: the generated pch changed' % pch)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + anchor, 1))
    print('leaf_locals%s: %d leaf functions%s with %d registers as locals (%d of them only read: not written back), '
          '%d left alone, %d already done; %d files changed' %
          (' (check only)' if a.check else '', done, '' if a.all_leaves else ' with loops', registers, read_only,
           skipped, already, changed))


if __name__ == '__main__':
    main()
