#!/usr/bin/env python3
"""Flush-mode checks whose outcome the control flow fixes (post-codegen step of the "optimised" codegen flavour;
docs/enhancements.md, "Build-time"). Works on any ReXGlue port's generated code.

The 360's VMX unit flushes denormals and its FPU does not, so the generator writes `ctx.fpscr.disableFlushMode();`
before scalar float code and `ctx.fpscr.enableFlushMode();` before vector code. Each is a load of the cached mode
word in ctx, a mask, a compare and a branch (the `msr fpcr` behind it only runs on a real change). The generator
already drops a check that follows another one in the same straight line; guest stores go through a pointer the
compiler must assume aliases ctx, so every check that is left is really compiled. This step proves more of them
redundant by following the function's control flow, and the calls it makes.

Within a function: a forward must-dataflow over its statements, on the cached mode word only (the test the check
performs). The mode is one of on / off / unknown:
  - unknown at the function's entry;
  - on after enableFlushMode[Unconditional], off after disableFlushMode[Unconditional];
  - after a direct call of a recompiled function (`__imp__sub_X(ctx, base);`, which tools/direct_calls.py only writes
    for functions no hook replaces): what the callee's summary says (below);
  - unknown after every other statement that passes ctx to other code (indirect, import and hooked calls, ppc_trap,
    the VMX save/restore helpers) and after every other mention of fpscr (mtfsf, mffs);
  - unknown after every structured statement (a line that opens or closes a block, switch/case/default/for/while/do/
    else, and the statement after a line that does not end one): what the generator writes besides labels and gotos
    is not followed, only fenced off;
  - at a label: the mode all its predecessors agree on (the statement before it and every goto to it), else unknown.
The any-mode helpers (rex::ppc::*AnyMode, diet_ops.h) save and restore the host mode and never touch the cached
word: they keep the mode. A function with a goto this script cannot read is left alone.

Across calls, a summary per recompiled function of what it does to the mode:
  - "keeps": the function has no flush-mode statement and no statement that could change the mode other than direct
    calls of functions that keep it (the largest such set: nothing reachable from it through direct calls touches the
    mode word). A call of it leaves the caller's mode as it was.
  - "on" / "off": every return of the function is reached in that mode (the same dataflow with the entry unknown;
    computed to a fixed point starting from "unknown" for every function, so recursion stays unknown).
  - "same": a function with flush-mode statements whose every return is reached in the mode it was entered with.
  - "unknown" otherwise.

Into a function (sa2-nx, 2026-10-12; with --image): the mode it is entered in, instead of "unknown". A function
that can only be entered by a direct call from recompiled code starts in the mode all those call sites are proven
to be in (computed from "unknown" for every function up to a fixed point, so each step only uses facts). "Only by a
direct call" means all of:
  - its address is nowhere in the image as a 32-bit word (any section but the unwind and relocation tables, at any
    byte offset: a virtual table, a function pointer, an export), and it is not the entry point;
  - no function builds its address in code: no `lis` constant of a function combined with an `addi` / `ori` immediate
    of the same function gives it, and no constant assignment is it;
  - no source file of the app or the SDK and no toml of the app mentions its address (a hook, a host call of
    __imp__sub_X, a function the generator was told about).
The localized copies (tools/localize_native.py) are made from these same statements and are entered through a hook
(their original's address is in the sources: unknown at entry), so their call sites are the ones analysed here.

A proven check becomes REX_FLUSH_KNOWN_ON() / REX_FLUSH_KNOWN_OFF(); the macros go into the generated pch:
  - on the Switch: nothing;
  - elsewhere (the PC build): the same test as before, and a call to RexFlushFlowMismatch(on) followed by the real
    mode change when the proof was wrong. Every PC run is therefore a check of the proof, at the old cost; the
    checks it executes are counted in g_rex_flush_flow_checked (a plain lossy counter: "at least this many");
  - with REX_FLUSH_FLOW_OFF defined: the original calls (the Switch build's off switch).

  python tools/flush_mode_flow.py --gen app/generated/default [--check] [--undo] [--redo] [--stats] [--no-calls]
                                  [--image out/default_image.bin] [--sources DIR ...] [--tomls DIR]
Idempotent. --check writes nothing; --undo restores the generator's calls; --no-calls uses no summaries (every call
makes the mode unknown).
--redo (sa2-nx; tools/post_codegen.py uses it): --undo and then the proof, in memory, writing only the files whose
final text differs. A proof depends on which callees are called directly, and tools/direct_calls.py --redo turns the
calls of a function hooked since the last run back into hooked calls: proofs from before that would be stale (the
native replacement need not leave the mode the original left). Run after direct_calls.py.
"""
import argparse
import array
import os
import re
import struct
import sys

ENABLE = 'ctx.fpscr.enableFlushMode();'
DISABLE = 'ctx.fpscr.disableFlushMode();'
KNOWN_ON = 'REX_FLUSH_KNOWN_ON();'
KNOWN_OFF = 'REX_FLUSH_KNOWN_OFF();'
SET_ON = ('ctx.fpscr.enableFlushModeUnconditional();', KNOWN_ON)
SET_OFF = ('ctx.fpscr.disableFlushModeUnconditional();', KNOWN_OFF)
FUNC = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
LABEL = re.compile(r'^(loc_[0-9A-Fa-f]+):$')
GOTO = re.compile(r'^goto (loc_[0-9A-Fa-f]+);$')
IFGOTO = re.compile(r'^if \(.*\) goto (loc_[0-9A-Fa-f]+);$')
# Also the return of a leaf function after tools/leaf_locals.py: `{ REX_LEAF_STORE(l_r3, r3); return; }`.
RETURN = re.compile(r'^(?:\{ (?:REX_LEAF_STORE\(\w+, \w+\); )+)?return\b[^;]*;(?: \})?$')
DIRECT_CALL = re.compile(r'^__imp__(sub_[0-9A-F]{8})\(ctx, base\);$')
TRAILING_COMMENT = re.compile(r'^(.*?[;{}:])\s*//.*$')
# Hands ctx to other code, or touches the mode word some other way.
RESET = re.compile(r'\bctx\s*[,)]|REX_CALL_INDIRECT_FUNC|fpscr|setjmp|longjmp|FlushMode|REX_FLUSH_')
MODE = re.compile(r'fpscr|FlushMode|REX_FLUSH_')
STRUCTURED = re.compile(r'^(?:switch|for|while|do|else|case|default|break|continue|try|catch)\b')
# Mode values. ENTRY: the mode the function was entered with (summaries only).
UNKNOWN, ON, OFF, TOP, ENTRY = 0, 1, 2, 3, 4
KEEP = 5  # a statement kind / callee summary: the mode stays what it was

MARKER = '// rexglue ports: flush-mode checks the control flow fixes (tools/flush_mode_flow.py)'
MACROS = MARKER + '''
#ifndef REX_FLUSH_KNOWN_ON
#if defined(REX_FLUSH_FLOW_OFF)
#define REX_FLUSH_KNOWN_ON() ctx.fpscr.enableFlushMode()
#define REX_FLUSH_KNOWN_OFF() ctx.fpscr.disableFlushMode()
#elif defined(__SWITCH__)
#define REX_FLUSH_KNOWN_ON() ((void)0)
#define REX_FLUSH_KNOWN_OFF() ((void)0)
#else
extern "C" void RexFlushFlowMismatch(int on) noexcept;
extern "C" volatile unsigned long long g_rex_flush_flow_checked;
#define REX_FLUSH_KNOWN_ON()                         \\
  do {                                               \\
    g_rex_flush_flow_checked = g_rex_flush_flow_checked + 1; \\
    if (!ctx.fpscr.flushModeIsOn()) [[unlikely]] {   \\
      RexFlushFlowMismatch(1);                       \\
      ctx.fpscr.enableFlushMode();                   \\
    }                                                \\
  } while (0)
#define REX_FLUSH_KNOWN_OFF()                        \\
  do {                                               \\
    g_rex_flush_flow_checked = g_rex_flush_flow_checked + 1; \\
    if (!ctx.fpscr.flushModeIsOff()) [[unlikely]] {  \\
      RexFlushFlowMismatch(0);                       \\
      ctx.fpscr.disableFlushMode();                  \\
    }                                                \\
  } while (0)
#endif
#endif
'''


def meet(a, b):
    if a == TOP:
        return b
    if b == TOP:
        return a
    return a if a == b else UNKNOWN


def statements(body):
    """The function's statements: (line indices, texts without a trailing comment)."""
    idx, code = [], []
    for i, line in enumerate(body):
        s = line.strip()
        if not s or s.startswith('//'):
            continue
        m = TRAILING_COMMENT.match(s)
        if m:
            s = m.group(1).rstrip()
        idx.append(i)
        code.append(s)
    return idx, code


class Flow:
    """One function's statements, their effect on the mode and their successors. None when it cannot be read."""

    def __init__(self, body):
        self.idx, self.code = statements(body)
        code = self.code
        n = len(code)
        self.ok = True
        labels = {}
        for k, s in enumerate(code):
            m = LABEL.match(s)
            if m:
                if m.group(1) in labels:
                    self.ok = False
                    return
                labels[m.group(1)] = k
        # kind: KEEP, ON, OFF, UNKNOWN, or ('call', callee). succ: explicit successors. exits: returns.
        self.kind = [KEEP] * n
        self.succ = [None] * n
        self.exits = []  # (statement, uses the state before it)
        forced = False  # the statement after a line that does not end one (a braceless for body, a wrapped expression)
        for k, s in enumerate(code):
            nxt = [k + 1] if k + 1 < n else []
            incomplete = s[-1] not in ';{}:'
            m = GOTO.match(s)
            mi = IFGOTO.match(s) if not m else None
            if 'goto' in s and not m and not mi:
                self.ok = False
                return
            if (m or mi) and (m or mi).group(1) not in labels:
                self.ok = False
                return
            if m:
                self.succ[k] = [labels[m.group(1)]]
            elif mi:
                self.succ[k] = nxt + [labels[mi.group(1)]]
            elif RETURN.match(s):
                self.succ[k] = []
                self.exits.append(k)
            else:
                self.succ[k] = nxt
                if 'return' in s:
                    self.exits.append(k)  # `if (...) return;`
                if not nxt:
                    self.exits.append(k)  # falls off the end
            call = DIRECT_CALL.match(s)
            if forced or incomplete:
                self.kind[k] = UNKNOWN
            elif s == ENABLE or s in SET_ON:
                self.kind[k] = ON
            elif s == DISABLE or s in SET_OFF:
                self.kind[k] = OFF
            elif call:
                self.kind[k] = ('call', call.group(1))
            elif RESET.search(s) or STRUCTURED.match(s) or s[0] == '}' or s[-1] == '{' or \
                    (s[-1] == ':' and not LABEL.match(s)):
                self.kind[k] = UNKNOWN
            forced = incomplete
        self.preds = [[] for _ in range(n)]
        for k in range(n):
            for j in self.succ[k]:
                self.preds[j].append(k)

    def run(self, entry, summaries):
        """The mode before each statement and the function's exit mode, given the entry mode."""
        n = len(self.code)
        kind = []
        for k in self.kind:
            if isinstance(k, tuple):
                k = summaries.get(k[1], UNKNOWN) if summaries is not None else UNKNOWN
            kind.append(k)
        out = [TOP] * n
        inn = [TOP] * n
        changed = True
        while changed:
            changed = False
            for k in range(n):
                st = entry if k == 0 else TOP
                for p in self.preds[k]:
                    st = meet(st, out[p])
                inn[k] = st
                kk = kind[k]
                o = st if kk == KEEP or kk == ENTRY else kk
                if o != out[k]:
                    out[k] = o
                    changed = True
        exit_state = TOP
        for k in self.exits:
            # A plain `return;` keeps the state before it; a line that also does something else is judged by its
            # effect (unknown when it hands ctx on).
            exit_state = meet(exit_state, out[k] if kind[k] != KEEP and kind[k] != ENTRY else inn[k])
        return inn, (UNKNOWN if exit_state == TOP else exit_state)

    def call_states(self, entry, summaries):
        """(callee, mode before the call) for every direct call of the function, given its entry mode."""
        inn, _ = self.run(entry, summaries)
        for k, kind in enumerate(self.kind):
            if isinstance(kind, tuple) and inn[k] != TOP:
                yield kind[1], inn[k]

    def proven(self, summaries, entry=UNKNOWN):
        inn, _ = self.run(entry, summaries)
        result = {}
        for k, s in enumerate(self.code):
            if self.kind[k] == UNKNOWN:
                continue
            if s == ENABLE and inn[k] == ON:
                result[self.idx[k]] = KNOWN_ON
            elif s == DISABLE and inn[k] == OFF:
                result[self.idx[k]] = KNOWN_OFF
        return result


CONST_ASSIGN = re.compile(r'\.s64 = (-?\d+);$')
ADD_IMM = re.compile(r'\.s64 = (?:ctx\.)?r\d+\.s64 \+ (-?\d+);$')
OR_IMM = re.compile(r'\.u64 = (?:ctx\.)?r\d+\.u64 \| (\d+);$')
ADDRESS = re.compile(r'(?<![0-9A-Fa-f])(8[2-9][0-9A-Fa-f]{6})(?![0-9A-Fa-f])')


def addresses_in_image(path, wanted):
    """The members of `wanted` (guest addresses) that are in the image as a big-endian 32-bit word, plus its entry
    point. Every section but the unwind table and the relocations, at every byte offset."""
    image = open(path, 'rb').read()
    pe = struct.unpack_from('<I', image, 0x3C)[0]
    if image[:2] != b'MZ' or image[pe:pe + 4] != b'PE\0\0':
        sys.exit('ERROR (post-codegen patch): flush_mode_flow: the image dump is not a PE image')
    sections, opt_size = struct.unpack_from('<H', image, pe + 6)[0], struct.unpack_from('<H', image, pe + 20)[0]
    base = struct.unpack_from('<I', image, pe + 24 + 28)[0]
    found = {(base + struct.unpack_from('<I', image, pe + 24 + 16)[0]) & 0xFFFFFFFF} & wanted
    table = pe + 24 + opt_size
    for i in range(sections):
        name, vsize, va = struct.unpack_from('<8sII', image, table + 40 * i)
        if name.rstrip(b'\0') in (b'.pdata', b'.reloc'):
            continue
        data = image[va:va + vsize]
        for shift in range(4):
            part = data[shift:]
            part = part[:len(part) & ~3]
            words = array.array('I', part)
            if sys.byteorder == 'little':
                words.byteswap()
            found |= wanted.intersection(words)
    return found


def addresses_in_text(folders, tomls):
    found = set()
    for folder in folders:
        for d, _, names in os.walk(folder):
            for name in names:
                if name.endswith(('.cpp', '.h', '.inc', '.hpp', '.c')):
                    text = open(os.path.join(d, name), encoding='utf-8', errors='ignore').read()
                    found.update(int(x, 16) for x in ADDRESS.findall(text))
    if tomls and os.path.isdir(tomls):
        for name in os.listdir(tomls):
            if name.endswith('.toml'):
                text = open(os.path.join(tomls, name), encoding='utf-8', errors='ignore').read()
                found.update(int(x, 16) for x in ADDRESS.findall(text))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument('--image', help="the code generator's image dump: enables the entry modes")
    ap.add_argument('--sources', nargs='*', default=[os.path.join(root, 'app', 'src'), os.path.join(root, 'sdk', 'src'),
                                                     os.path.join(root, 'sdk', 'include')])
    ap.add_argument('--tomls', default=os.path.join(root, 'app'))
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--undo', action='store_true')
    ap.add_argument('--redo', action='store_true', help='undo and prove again, writing only files that change')
    ap.add_argument('--stats', action='store_true', help='print the functions with the most proven checks')
    ap.add_argument('--no-calls', action='store_true', help='no callee summaries: every call makes the mode unknown')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): flush_mode_flow: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    texts = {}
    originals = {}
    if a.undo:
        changed_files = 0
        for f in files:
            path = os.path.join(a.gen, f)
            text = open(path, encoding='utf-8').read()
            new = text.replace(KNOWN_ON, ENABLE).replace(KNOWN_OFF, DISABLE)
            if new != text:
                changed_files += 1
                if not a.check:
                    open(path, 'w', encoding='utf-8', newline='\n').write(new)
        print('flush_mode_flow: undone in %d files' % changed_files)
        return

    # Pass 1: every function. Those with flush-mode statements keep their body for the dataflow; the others only
    # what a "keeps" summary needs: whether anything but direct calls could change the mode, and whom they call.
    with_mode = {}   # name -> (file, first body line, Flow)
    plain = {}       # name -> set of direct callees, for functions that may "keep"
    callers = {}     # name -> Flow, for functions without flush-mode statements that call directly (entry modes)
    all_callees = {} # name -> set of direct callees
    built = set()    # addresses a function builds in code: a `lis` constant with an immediate of the same function
    defined = set()
    already = 0
    for f in files:
        originals[f] = open(os.path.join(a.gen, f), encoding='utf-8').read()
        text = originals[f]
        if a.redo:
            text = text.replace(KNOWN_ON, ENABLE).replace(KNOWN_OFF, DISABLE)
        lines = text.split('\n')
        texts[f] = lines
        i = 0
        while i < len(lines):
            m = FUNC.match(lines[i])
            if not m:
                i += 1
                continue
            name = m.group(1)
            j = i + 1
            has_mode = False
            other_reset = False
            callees = set()
            highs, lows = set(), set()
            while j < len(lines) and lines[j] != '}':
                s = lines[j].strip()
                if s and not s.startswith('//'):
                    if 'REX_FLUSH_KNOWN_' in s:
                        already += 1
                    if a.image:
                        c = CONST_ASSIGN.search(s)
                        if c:
                            value = int(c.group(1)) & 0xFFFFFFFF
                            built.add(value)
                            if not value & 0xFFFF:
                                highs.add(value)
                        else:
                            c = ADD_IMM.search(s) or OR_IMM.search(s)
                            if c:
                                lows.add(int(c.group(1)))
                    c = DIRECT_CALL.match(s)
                    if c:
                        callees.add(c.group(1))
                    elif MODE.search(s):
                        has_mode = True
                    elif RESET.search(s):
                        other_reset = True
                j += 1
            defined.add(name)
            all_callees[name] = callees
            for h in highs:
                for low in lows:
                    built.add((h + low) & 0xFFFFFFFF)
                    built.add((h | (low & 0xFFFF)) & 0xFFFFFFFF)
            if has_mode:
                with_mode[name] = (f, i + 1, Flow(lines[i + 1:j]))
            else:
                if not other_reset:
                    plain[name] = callees
                if a.image and callees:
                    callers[name] = Flow(lines[i + 1:j])
            i = j + 1

    summaries = None
    kept_functions = by_exit = 0
    if not a.no_calls:
        # "keeps": the largest set of mode-free functions closed under direct calls.
        keeps = set(plain)
        changed = True
        while changed:
            changed = False
            for name in list(keeps):
                if not plain[name] <= keeps:
                    keeps.discard(name)
                    changed = True
        kept_functions = len(keeps)
        summaries = {name: KEEP for name in keeps}
        # Exit modes of the functions with flush-mode statements, from "unknown" up to a fixed point.
        for _ in range(12):
            changed = False
            for name, (_, _, flow) in with_mode.items():
                if not flow.ok:
                    continue
                _, exit_state = flow.run(ENTRY, summaries)
                if exit_state != summaries.get(name, UNKNOWN):
                    summaries[name] = exit_state
                    changed = True
            if not changed:
                break
        by_exit = sum(1 for name in with_mode if summaries.get(name, UNKNOWN) != UNKNOWN)

    # The mode each function is entered in (see the header). From "unknown" up: every round only uses proven facts.
    entry = {}
    entry_known = 0
    if a.image and summaries is not None:
        address_of = {int(name[4:], 16): name for name in defined if re.match(r'sub_[0-9A-F]{8}$', name)}
        wanted = set(address_of)
        taken = addresses_in_image(a.image, wanted) | (built & wanted) | \
            (addresses_in_text(a.sources, a.tomls) & wanted)
        eligible = {address_of[x] for x in wanted - taken}
        flows = {name: flow for name, (_, _, flow) in with_mode.items()}
        flows.update(callers)
        dirty = set(flows)
        sites_of = {}  # caller -> [(callee, mode)]
        for _ in range(40):
            for name in dirty:
                flow = flows[name]
                if flow.ok:
                    sites_of[name] = list(flow.call_states(entry.get(name, UNKNOWN), summaries))
                else:
                    sites_of[name] = [(c, UNKNOWN) for c in all_callees[name]]
            seen = {}
            for name, sites in sites_of.items():
                for callee, mode in sites:
                    if mode == ENTRY or mode == KEEP:
                        mode = UNKNOWN
                    seen[callee] = meet(seen.get(callee, TOP), mode)
            # A caller that is not in `flows` has no direct call; a function nobody calls directly stays unknown.
            new = {name: mode for name, mode in seen.items() if name in eligible and mode in (ON, OFF)}
            dirty = {name for name in flows if new.get(name, UNKNOWN) != entry.get(name, UNKNOWN)}
            entry = new
            if not dirty:
                break
        entry_known = len(entry)

    proven_total = kept = skipped = functions = 0
    top = []
    touched = set()
    for name, (f, first, flow) in with_mode.items():
        lines = texts[f]
        checks = sum(1 for s in flow.code if s in (ENABLE, DISABLE))
        if not checks:
            continue
        if not flow.ok:
            skipped += 1
            kept += checks
            continue
        functions += 1
        proven = flow.proven(summaries, entry.get(name, UNKNOWN))
        kept += checks - len(proven)
        proven_total += len(proven)
        if proven:
            top.append((len(proven), checks, name))
            touched.add(f)
        for k, macro in proven.items():
            line = lines[first + k]
            lines[first + k] = line[:len(line) - len(line.lstrip())] + macro
    written = 0
    for f in files:
        new = '\n'.join(texts[f])
        if new != originals[f]:
            written += 1
            if not a.check:
                open(os.path.join(a.gen, f), 'w', encoding='utf-8', newline='\n').write(new)
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if MARKER not in text:
        anchor = '#define REX_LOAD_U8(x)'
        if anchor not in text:
            sys.exit('ERROR (post-codegen patch): flush_mode_flow: no REX_LOAD_U8 in %s: the generated pch changed'
                     % pch)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + anchor, 1))
    print('flush_mode_flow%s: %d flush-mode checks proven by control flow (%d from earlier runs), %d kept, in %d '
          'functions (%d left alone), %d files with proofs, %d written; callee summaries: %s' %
          (' (check only)' if a.check else '', proven_total, already, kept, functions, skipped, len(touched), written,
           'none (--no-calls)' if a.no_calls else
           '%d functions keep the mode, %d with flush-mode statements return in a known mode; %s' %
           (kept_functions, by_exit,
            '%d functions are entered in a known mode' % entry_known if a.image else 'no entry modes (no --image)')))
    if a.stats:
        for p, c, name in sorted(top, reverse=True)[:25]:
            print('  %4d of %4d  %s' % (p, c, name))


if __name__ == '__main__':
    main()
