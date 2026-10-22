#!/usr/bin/env python3
"""An estimate of how many guest functions never have their address taken, and how many of those are called from one
place only. That is the code GCC's link-time optimiser would treat best in an ordinary program (a function called
once is inlined and its body dropped; a function nobody can reach by address may change its calling convention) and
cannot in a recompiled one, because the function table takes the address of every function.

A function's address counts as taken when its start address is found
  - as a big-endian word in a data section of the image (.pdata left out: it lists every function), or
  - as a `lis` + `addi` / `ori` pair in the code (within a window of instructions).
This is an ESTIMATE for sizing the opportunity, not a proof: a pair further apart than the window is missed, and the
export table and the entry point (relative addresses) are not read. Do not prune the function table with it.

  python tools/address_taken_census.py [--gen app/generated/default] [--image out/default_image.bin] [--list FILE]

The image is the generator's dump (REXGLUE_DUMP_IMAGE; tools/codegen.ps1 writes out/default_image.bin). --list writes
the functions without an address, with their number of direct call statements.
"""
import argparse
import collections
import glob
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINDOW = 24   # instructions after a `lis` in which its `addi` / `ori` is looked for
DEFINITION = re.compile(r'^\w+\((?:__imp__)?sub_([0-9A-F]{8})\)\s*\{?\s*$', re.M)
CALL = re.compile(r'\b(?:__imp__)?sub_([0-9A-F]{8})\(ctx, base\)')


def sections(img):
    pe = struct.unpack_from('<I', img, 0x3C)[0]
    count = struct.unpack_from('<H', img, pe + 6)[0]
    at = pe + 24 + struct.unpack_from('<H', img, pe + 20)[0]
    out = []
    for _ in range(count):
        size, va = struct.unpack_from('<II', img, at + 8)
        out.append((img[at:at + 8].rstrip(b'\0').decode('latin1'), va, size))
        at += 40
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated', 'default'))
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'default_image.bin'))
    ap.add_argument('--list')
    args = ap.parse_args()

    pch = open(glob.glob(os.path.join(args.gen, '*_pch.h'))[0], encoding='utf-8').read()
    value = lambda name: int(re.search(r'#define %s (0x[0-9A-Fa-f]+)' % name, pch).group(1), 16)
    image_base, code_base, code_size = value('REX_IMAGE_BASE'), value('REX_CODE_BASE'), value('REX_CODE_SIZE')
    img = open(args.image, 'rb').read()

    starts = set()
    calls = collections.Counter()
    files = sorted(glob.glob(os.path.join(args.gen, '*_recomp.*.cpp')))
    for f in files:
        text = open(f, encoding='utf-8', errors='replace').read()
        starts.update(int(m.group(1), 16) for m in DEFINITION.finditer(text))
        calls.update(int(m.group(1), 16) for m in CALL.finditer(text))
    if not starts:
        raise SystemExit('no function definitions found in ' + args.gen)
    print('%d files, %d functions, %d direct call statements to %d of them' % (
        len(files), len(starts), sum(calls.values()), len(calls)))

    code_lo, code_hi = code_base - image_base, code_base - image_base + code_size
    in_data = set()
    for name, va, size in sections(img):
        lo, hi = va, min(va + size, len(img))
        if name == '.pdata' or (lo >= code_lo and hi <= code_hi) or hi <= lo:
            continue
        in_data.update(w for w in struct.unpack_from('>%dI' % ((hi - lo) // 4), img, lo) if w in starts)
    in_code = set()
    words = struct.unpack_from('>%dI' % (code_size // 4), img, code_lo)
    for i, w in enumerate(words):
        if w >> 26 != 15 or (w >> 16) & 31:          # lis rD, hi
            continue
        reg, hi16 = (w >> 21) & 31, w & 0xFFFF
        for v in words[i + 1:i + 1 + WINDOW]:
            op, lo16 = v >> 26, v & 0xFFFF
            if op == 14 and ((v >> 16) & 31) == reg:   # addi rX, rD, lo (sign-extended)
                a = ((hi16 << 16) + (lo16 - 0x10000 if lo16 & 0x8000 else lo16)) & 0xFFFFFFFF
            elif op == 24 and ((v >> 21) & 31) == reg:  # ori rX, rD, lo
                a = (hi16 << 16) | lo16
            else:
                continue
            if a in starts:
                in_code.add(a)
    taken = in_data | in_code
    free = starts - taken
    print('address found as a data word: %d, as a lis pair in the code: %d, either: %d' % (
        len(in_data), len(in_code), len(taken)))
    print('no address found: %d of %d functions (%.1f %%)' % (len(free), len(starts), 100.0 * len(free) / len(starts)))
    for label, group in (('functions without an address', free), ('functions with an address', taken)):
        by = collections.Counter(min(calls[a], 4) for a in group)
        print('  %s, by direct call statements: none %d, one %d, two %d, three %d, four or more %d' % (
            label, by[0], by[1], by[2], by[3], by[4]))
    if args.list:
        with open(args.list, 'w') as f:
            f.writelines('%08X %d\n' % (a, calls[a]) for a in sorted(free))
        print('written: ' + args.list)


if __name__ == '__main__':
    main()
