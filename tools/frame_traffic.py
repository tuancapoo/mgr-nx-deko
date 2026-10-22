#!/usr/bin/env python3
"""Stack-frame traffic of the recompiled functions that nothing in the function itself reads (post-codegen step of
the "optimised" codegen flavour; docs/enhancements.md, "The second CPU round of 2026-10-12"). Works on any ReXGlue
port's generated code that uses elide_lr_stores and non_volatile_as_local.

EVERY ELISION HERE IS OPT-IN AND OFF BY DEFAULT. The step only marks the statements; without a switch they compile
as before. Reason (SA2, 2026-10-12, PC lockstep capture): with the non-volatile saves removed the two-player demo's
characters came out tinted blue. Those saves write zeros (the callee's fresh locals) into the frame, and the game
reads a stack word it never initialised: zero is harmless there, the stale data left without the save is not. The
link-register half passed the same capture, but it changes stale frame contents in the same way in 9,880 functions,
so it is opt-in too. A port may turn one on after its own frame-exact test of every scene it can reach.

Every non-leaf PowerPC function starts with `mflr r12; stw r12,-8(r1); std r31,-16(r1) ...; stwu r1,-N(r1)` and ends
with the mirror image. In the recompiled code two of those three things move data nobody can use:

  - The link register. With elide_lr_stores no call sets ctx.lr any more (a C++ call returns by itself), so `mflr`
    reads a stale word, the prologue saves it and the epilogue loads it back and writes it to ctx.lr again: a load of
    ctx, a store and a load of the guest stack and a store to ctx on every call, for a value that is garbage.
    Here: REX_LR_READ(reg) sets the register to 0, the save of it becomes REX_DEAD_SAVE (nothing) and `mtlr` becomes
    REX_LR_WRITE (nothing); the epilogue's load then feeds nothing and the compiler drops it. Only in functions where
    the link register is never data: no `bl` to a local label (position-independent code reads the LR it sets; the
    generator keeps `ctx.lr = 0x...` there), no `blrl` (a call through the LR), no other mention of ctx.lr.
    A `mflr` is replaced only in the compiler's own shape, with the save of that register at -8(r1) right after it.
  - The non-volatile registers. With non_volatile_as_local r14-r31 and f14-f31 are C++ locals of each function: a
    callee cannot change its caller's. The prologue still stored the callee's (fresh, zero) locals to the stack and
    the epilogue loaded them back into locals that die at the return. The stores `std rN,-K(r1)` / `stfd fN,-K(r1)`
    of a LOCAL register before the frame is allocated become REX_DEAD_SAVE; the loads die by themselves. (The
    __savegprlr_N / __restgprlr_N helpers the larger functions call were dropped by the generator already.)
  - The back chain (`stwu r1,-N(r1)` also stores the old stack pointer at the new one). Only functions that pop
    their frame through it (`lwz r1,0(r1)`), or copy r1, read it, but a callee could read its caller's in principle,
    so this one is OPT-IN (REX_BACK_CHAIN_ELIDE) and off by default: REX_CHAIN_STORE(statement).

What an elision changes for the guest: words of the stack frame that only the matching epilogue reads keep their
old content instead of receiving a zero or a stale LR. That is visible to guest code that reads its frame before
writing it (see above).

Switches (compile time, in the generated pch), each of them an opt-in: REX_LR_MOVES_ELIDE, REX_NV_SAVES_ELIDE,
REX_BACK_CHAIN_ELIDE. With the Switch compiler the first two together made three recompiled files 1.3 to 1.5 %
smaller (2.5 % fewer stores), the back chain another 0.9 %.

  python tools/frame_traffic.py --gen app/generated/default [--check]
Idempotent (the rewritten statements no longer match). Run after the other steps that read the generated statements.
"""
import argparse
import os
import re
import sys

FUNC = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
COMMENT = re.compile(r'^\t// (\S+)(?: (.*))?$')
MFLR = re.compile(r'^\t((?:ctx\.)?r\d+)\.u64 = ctx\.lr;$')
MTLR = re.compile(r'^\tctx\.lr = ((?:ctx\.)?r\d+)\.u64;$')
LR_SLOT = re.compile(r'^\tREX_STORE_D_U32\(ctx\.r1\.u32, -8, ((?:ctx\.)?r\d+)\.u32\);$')
NV_SAVE = re.compile(r'^\tREX_STORE_D_U64\(ctx\.r1\.u32, -\d+, ([rf])(\d+)\.u64\);$')
CHAIN = re.compile(r'^\tREX_STORE_U32\(ea, ctx\.r1\.u32\);$')
LR_DATA = re.compile(r'ctx\.lr = 0x|old_lr|REX_CALL_INDIRECT_FUNC\(uint32_t\(')
LRL = re.compile(r'^b\w*lrl$')
MARKER = '// rexglue ports: dead stack-frame traffic (tools/frame_traffic.py)'
MACROS = MARKER + '''
#ifndef REX_LR_READ
#if defined(REX_LR_MOVES_ELIDE)
#define REX_LR_READ(reg) (reg).u64 = 0
#define REX_LR_WRITE(reg) ((void)0)
#define REX_DEAD_LR_SAVE(statement) ((void)0)
#else
#define REX_LR_READ(reg) (reg).u64 = ctx.lr
#define REX_LR_WRITE(reg) ctx.lr = (reg).u64
#define REX_DEAD_LR_SAVE(statement) statement
#endif
#if defined(REX_NV_SAVES_ELIDE)
#define REX_DEAD_SAVE(statement) ((void)0)
#else
#define REX_DEAD_SAVE(statement) statement
#endif
#if defined(REX_BACK_CHAIN_ELIDE)
#define REX_CHAIN_STORE(statement) ((void)0)
#else
#define REX_CHAIN_STORE(statement) statement
#endif
#endif
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): frame_traffic: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    stats = dict(functions=0, lr_functions=0, mflr=0, mtlr=0, lr_saves=0, nv_saves=0, chains=0, lr_data=0, changed=0,
                 already=0)
    for f in files:
        path = os.path.join(a.gen, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        new = list(lines)
        i = 0
        while i < len(lines):
            if not FUNC.match(lines[i]):
                i += 1
                continue
            j = i + 1
            while j < len(lines) and lines[j] != '}':
                j += 1
            body = range(i + 1, j)
            stats['functions'] += 1
            text = '\n'.join(lines[k] for k in body)
            stats['already'] += text.count('REX_LR_READ(') + text.count('REX_DEAD_SAVE(')
            # The link register is data here: leave every mention of it alone.
            lr_data = bool(LR_DATA.search(text))
            chain_read = False
            for k in body:
                m = COMMENT.match(lines[k])
                if not m:
                    if not lr_data and 'ctx.lr' in lines[k] and not MFLR.match(lines[k]) and not MTLR.match(lines[k]):
                        lr_data = True
                    continue
                name, operands = m.group(1), m.group(2) or ''
                if LRL.match(name):
                    lr_data = True
                # Anything that could read a back chain: a load at 0 from r1, a copy of r1, an indexed update of r1.
                if (re.search(r',0\(r1\)$', operands) and name[0] == 'l') or name in ('stwux', 'stdux') or \
                        re.match(r'^(mr|or) r\d+,r1(,r1)?$', name + ' ' + operands) or \
                        re.match(r'^addi r\d+,r1,0$', name + ' ' + operands):
                    chain_read = True
            if lr_data:
                stats['lr_data'] += 1
            else:
                stats['lr_functions'] += 1
            frame_seen = False  # the first stwu r1: the saves the compiler makes come before it
            straight = True     # still in the entry's straight line (no label, no branch)
            k = i + 1
            while k < j:
                line = lines[k]
                s = line.strip()
                m = COMMENT.match(line)
                if m:
                    name, operands = m.group(1), m.group(2) or ''
                    if name in ('stwu', 'stdu') and operands.startswith('r1,'):
                        if not chain_read and k + 2 < j and CHAIN.match(lines[k + 2]):
                            new[k + 2] = '\tREX_CHAIN_STORE(' + lines[k + 2].strip().rstrip(';') + ');'
                            stats['chains'] += 1
                        frame_seen = True
                    k += 1
                    continue
                if s.startswith('loc_') or s.startswith('goto ') or s.startswith('if (') or s.startswith('return'):
                    straight = False
                if not lr_data:
                    m = MFLR.match(line)
                    if m:
                        # The compiler's shape only: `mflr rX` and, as the next instruction, `stw rX,-8(r1)`.
                        nxt = k + 1
                        while nxt < j and COMMENT.match(lines[nxt]):
                            nxt += 1
                        s2 = LR_SLOT.match(lines[nxt]) if nxt < j else None
                        if s2 and s2.group(1) == m.group(1):
                            new[k] = '\tREX_LR_READ(%s);' % m.group(1)
                            new[nxt] = '\tREX_DEAD_LR_SAVE(' + lines[nxt].strip().rstrip(';') + ');'
                            stats['mflr'] += 1
                            stats['lr_saves'] += 1
                        k += 1
                        continue
                    m = MTLR.match(line)
                    if m:
                        new[k] = '\tREX_LR_WRITE(%s);' % m.group(1)
                        stats['mtlr'] += 1
                        k += 1
                        continue
                m = NV_SAVE.match(line)
                if m and not frame_seen and straight and 14 <= int(m.group(2)) <= 31:
                    new[k] = '\tREX_DEAD_SAVE(' + s.rstrip(';') + ');'
                    stats['nv_saves'] += 1
                k += 1
            i = j + 1
        if new != lines:
            stats['changed'] += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(new))
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if MARKER not in text:
        anchor = '#define REX_LOAD_U8(x)'
        if anchor not in text:
            sys.exit('ERROR (post-codegen patch): frame_traffic: no REX_LOAD_U8 in %s: the generated pch changed' % pch)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + anchor, 1))
    print('frame_traffic%s: %d functions (%d where the link register is data, left alone); link register: %d mflr '
          'with their %d saves and %d mtlr; %d saves of non-volatile locals; %d back-chain stores marked (kept unless '
          'REX_BACK_CHAIN_ELIDE); %d files changed, %d statements already in the new form' %
          (' (check only)' if a.check else '', stats['functions'], stats['lr_data'], stats['mflr'], stats['lr_saves'],
           stats['mtlr'], stats['nv_saves'], stats['chains'], stats['changed'], stats['already']))


if __name__ == '__main__':
    main()
