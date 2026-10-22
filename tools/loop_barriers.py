#!/usr/bin/env python3
"""Compiler barriers at the loop headers of the generated code (post-codegen step of the "optimised" codegen flavour,
REQUIRED together with tools/pch_no_volatile.py; from futari-nx tools/perf_codegen_post.py pass 3).

With guest loads that are not volatile, GCC -O3 (the Switch build) hoists the load of a guest polling loop
(`while (device[x] != 0) {}`: a real wait on the Xbox 360, another thread or the GPU changes the word) out of the loop
and turns it into a branch to itself: the thread spins forever. clang on the PC did not, so PC runs looked fine. Found
by futari-nx on its console; tools/find_self_loops.py counts these in an ELF (SA2 optimised build without barriers:
40 in recompiled functions, e.g. sub_8255A760 in Direct3D).

A loop header is a label `loc_X:` that a later `goto loc_X;` of the same function jumps back to. The line after it gets
REX_LOOP_BARRIER(), a compiler memory barrier (defined in the pch): no guest load is reused across iterations, also
inside inlined callees; straight-line code keeps the plain loads.

Since 2026-10-12 (candidate 2 of the second CPU round) a header gets NO barrier when the barrier has nothing to
protect. The barrier clobbers all memory, so every guest register still kept in ctx is stored and reloaded on every
iteration of a loop that has one; two kinds of loop can do without:
  - the loop's lines (from its header to its last backward goto) read no guest memory and make no call at all: there
    is no guest load to hoist, neither in the loop nor in anything it could inline;
  - every way back to the header passes an indirect call (REX_CALL_INDIRECT_*: a call through a pointer loaded from
    memory, which the compiler must assume changes memory, with or without LTO): no guest load can be carried
    around the loop. Calls of recompiled functions, hooks and kernel imports do NOT count: under LTO the compiler may
    see into them. The paths are followed over the function's labels and gotos; a function with a goto this script
    cannot read keeps a barrier at every header.
--all restores a barrier at every header. Idempotent: barriers no longer wanted are removed.

  python tools/loop_barriers.py app/generated/default [app/generated/data_dll ...] [--check] [--all]
"""
import os
import re
import sys

LABEL = re.compile(r'^(loc_[0-9A-F]{8}):\s*$')
GOTO = re.compile(r'goto (loc_[0-9A-F]{8});')
ONLY_GOTO = re.compile(r'^\tgoto (loc_[0-9A-F]{8});$')
IF_GOTO = re.compile(r'^\tif \(.*\) goto (loc_[0-9A-F]{8});$')
# Also the return of a leaf function after tools/leaf_locals.py: `{ REX_LEAF_STORE(l_r3, r3); return; }`.
RETURN = re.compile(r'^\t(?:\{ (?:REX_LEAF_STORE\(\w+, \w+\); )+)?return\b[^;]*;(?: \})?$')
BARRIER = '\tREX_LOOP_BARRIER();'
MARKER = '// sa2-nx optimised codegen: loop barrier (tools/loop_barriers.py)'
MACRO = (MARKER + '\n#ifndef REX_LOOP_BARRIER\n'
         '#define REX_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")\n#endif\n')
# A read of guest memory, or anything that could hide one: a load, a raw address, a call of any kind.
# REX_RO_: a load tools/ro_constants.py folded to a constant still counts, so that a second run of this script on
# processed text decides as the first one did.
GUEST = re.compile(r'REX_LOAD|REX_RO_|REX_RAW_ADDR|REX_ADDR|REX_GUEST_CAS|\bbase\b|REX_CALL_INDIRECT_|ppc_trap\(')
INDIRECT = re.compile(r'REX_CALL_INDIRECT_')
OTHER_CALL = re.compile(r'\b\w+\(ctx, base[,)]')


def needed_headers(body, headers):
    """Of the loop headers of one function (label -> line index in body), those that need a barrier."""
    label_at = {}
    for i, line in enumerate(body):
        m = LABEL.match(line)
        if m:
            label_at[m.group(1)] = i
    last_back = {}
    for i, line in enumerate(body):
        for target in GOTO.findall(line):
            if target in headers and label_at[target] <= i:
                last_back[target] = i
    # Successors of each line, for the paths back to a header. A line this cannot read makes every header needed.
    n = len(body)
    succ = [None] * n
    readable = True
    for i, line in enumerate(body):
        m = ONLY_GOTO.match(line)
        mi = IF_GOTO.match(line) if not m else None
        if 'goto' in line and not m and not mi and not line.lstrip().startswith('//'):
            readable = False
            break
        if m:
            succ[i] = [label_at[m.group(1)]] if m.group(1) in label_at else None
        elif mi:
            succ[i] = [i + 1, label_at[mi.group(1)]] if mi.group(1) in label_at else None
        elif RETURN.match(line):
            succ[i] = []
        else:
            succ[i] = [i + 1]
        if succ[i] is None:
            readable = False
            break
    need = set()
    for header, at in headers.items():
        end = last_back.get(header, at)
        lines = [l for l in body[at + 1:end + 1] if not l.lstrip().startswith('//') and l != BARRIER]
        if not any(GUEST.search(l) or OTHER_CALL.search(l) for l in lines):
            continue  # nothing to hoist
        if not readable:
            need.add(header)
            continue
        # Is there a way from the header back to it that passes no indirect call?
        seen = set()
        stack = [at]
        found = False
        while stack and not found:
            i = stack.pop()
            if i in seen or i >= n:
                continue
            seen.add(i)
            line = body[i]
            if INDIRECT.search(line) and not OTHER_CALL.search(line) and not line.lstrip().startswith('//'):
                continue  # this path is fenced by the call
            for j in succ[i]:
                if j == at:
                    found = True
                    break
                stack.append(j)
        if found:
            need.add(header)
    return need


def process(folder, check, everywhere):
    headers_total = barriers_total = files = 0
    for f in sorted(os.listdir(folder)):
        if not re.match(r'\w+_recomp\.\d+\.cpp$', f):
            continue
        path = os.path.join(folder, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        # Without the barriers of an earlier run: they are decided again.
        plain = [l for l in lines if l != BARRIER]
        out = []
        i = 0
        while i < len(plain):
            if not plain[i].startswith('DEFINE_REX_FUNC('):
                out.append(plain[i])
                i += 1
                continue
            j = i + 1
            while j < len(plain) and plain[j] != '}':
                j += 1
            body = plain[i + 1:j]
            headers, seen = {}, {}
            for k, line in enumerate(body):
                m = LABEL.match(line)
                if m:
                    seen[m.group(1)] = k
                    continue
                for target in GOTO.findall(line):
                    if target in seen:
                        headers[target] = seen[target]
            need = set(headers) if everywhere else needed_headers(body, headers)
            headers_total += len(headers)
            barriers_total += len(need)
            out.append(plain[i])
            for line in body:
                out.append(line)
                m = LABEL.match(line)
                if m and m.group(1) in need:
                    out.append(BARRIER)
            i = j
        if out != lines:
            files += 1
            if not check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    pch = next((os.path.join(folder, f) for f in os.listdir(folder) if f.endswith('_pch.h')), None)
    if not pch:
        sys.exit(f'loop barriers: no *_pch.h in {folder}')
    text = open(pch, encoding='utf-8').read()
    pch_changed = False
    if MARKER not in text:
        if '#define REX_LOAD_U8(x)' not in text:
            sys.exit(f'loop barriers: no REX_LOAD_U8 in {pch}: the generated pch changed')
        text = text.replace('#define REX_LOAD_U8(x)', MACRO + '#define REX_LOAD_U8(x)', 1)
        pch_changed = True
        if not check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'{folder}: {headers_total} loop headers, {barriers_total} with a barrier '
          f'({headers_total - barriers_total} have nothing to protect), {files} files '
          f'{"to change" if check else "changed"}{", pch macro added" if pch_changed else ""}')
    return files == 0 and not pch_changed


def main():
    check = '--check' in sys.argv
    everywhere = '--all' in sys.argv
    folders = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not folders:
        sys.exit(__doc__)
    ok = all([process(f, check, everywhere) for f in folders])
    sys.exit(0 if ok or not check else 1)


if __name__ == '__main__':
    main()
