#!/usr/bin/env python3
"""One signature table of the C runtime (and other library) functions for every ReXGlue port
(docs/enhancements.md, "The second CPU round of 2026-10-12", candidate 13).

Every port finds the game's memcpy, memset, memmove, sin ... by hand before it can replace them with host functions,
although games built with the same Xbox 360 SDK carry the same library code. This tool keeps what one port found as
position-independent signatures and looks for them in any other game's image:

  python tools/crt_signatures.py make --image out/default_image.bin --gen app/generated/default
                                      --list tools/crt_known_sa2.txt --db tools/crt_signatures.json --source sa2-nx
      adds the functions of the list (`<address> <name> [what it is]` lines) to the database. A function runs from
      its address to the next function of the generated tree (trailing padding dropped).
  python tools/crt_signatures.py scan --image <dump> --db tools/crt_signatures.json [--min-words 12]
      prints every place in the image's code that matches a signature: address, name, source, and whether the match
      is the only one. A unique match of a long signature is the same function; check short ones by hand.

The image is the generator's dump (REXGLUE_DUMP_IMAGE, tools/codegen.ps1 writes it for the game's own module).

THE DATABASE HOLDS NO GAME CODE, so it can be committed and shared: per function its length, which bits of which
words depend on the link address (the mask), a CRC-32 of one run of four words (to find candidates fast) and the
SHA-256 of all its words under the mask. A scan hashes the image the same way.

The mask takes out what depends on where the game was linked: the target of `b` / `bl`, the immediate of `lis`, and
the displacement of an `addi` / `ori` / load / store whose base register a `lis` of the same function set (an address
being built or used). Conditional branches stay (they are relative and inside the function). A match therefore means
the same code up to addresses.

A match only says where a function is. The replacement itself must still be guarded the way each port guards its
natives (run the original next to it and compare), because the same name can hide different contracts.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import zlib

FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$', re.M)
ANCHOR = 4  # words of the run the scan looks for first


class Image:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        if d[:2] != b'MZ' or d[pe:pe + 4] != b'PE\0\0':
            sys.exit('crt_signatures: %s is not a PE image dump' % path)
        sections, opt = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
        self.base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
        self.code = []  # (rva, size) of executable sections
        table = pe + 24 + opt
        for i in range(sections):
            name, vsize, rva, _, _, _, _, _, _, flags = struct.unpack_from('<8sIIIIIIHHI', d, table + 40 * i)
            if flags & 0x20000000:
                self.code.append((rva, min(vsize, len(d) - rva)))

    def words(self, address, count):
        off = address - self.base
        return list(struct.unpack_from('>%dI' % count, self.data, off))


def mask_of(words):
    """The mask of each word: 0 bits are the parts that depend on the link address."""
    masks = []
    address_regs = set()
    for w in words:
        op = w >> 26
        rd = (w >> 21) & 31
        ra = (w >> 16) & 31
        m = 0xFFFFFFFF
        if op == 18:  # b / bl
            m = 0xFC000003
            address_regs.clear()  # a call: the volatile registers are gone
        elif op == 15 and ra == 0:  # lis
            m = 0xFFFF0000
            address_regs.add(rd)
        elif (op == 14 and ra in address_regs) or (op == 24 and rd in address_regs):
            m = 0xFFFF0000  # addi rD, rA(lis), lo / ori rA, rS(lis), lo
            address_regs.add(rd if op == 14 else ra)
        elif 32 <= op <= 55 and ra in address_regs:  # loads and stores off an address register
            m = 0xFFFF0000
            if op in (32, 33, 34, 35, 40, 41, 42, 43):  # an integer load overwrites rD
                address_regs.discard(rd)
        elif op in (7, 8, 12, 13, 14, 15, 20, 21, 23, 31) and rd in address_regs:
            # Anything else that writes rD ends its life as an address register (rough; it only ever masks more).
            address_regs.discard(rd)
        masks.append(m)
    return masks


def digest(words, masks):
    return hashlib.sha256(struct.pack('>%dI' % len(words), *[w & m for w, m in zip(words, masks)])).hexdigest()


def anchor_of(masks):
    """The first run of ANCHOR words without a masked bit: (offset in words) or None."""
    run = 0
    for i, m in enumerate(masks):
        run = run + 1 if m == 0xFFFFFFFF else 0
        if run == ANCHOR:
            return i - ANCHOR + 1
    return None


def make(a):
    image = Image(a.image)
    starts = set()
    for f in os.listdir(a.gen):
        if re.match(r'\w+_recomp\.\d+\.cpp$', f):
            starts.update(int(x, 16) for x in FUNC.findall(open(os.path.join(a.gen, f), encoding='utf-8').read()))
    starts = sorted(starts)
    db = json.load(open(a.db, encoding='utf-8')) if os.path.exists(a.db) else {'signatures': []}
    known = {(s['name'], s['sha256']) for s in db['signatures']}
    added = 0
    for line in open(a.list, encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        address, name, *what = line.split(None, 2)
        address = int(address, 16)
        if address not in starts:
            sys.exit('crt_signatures: %08X (%s) is not a function of %s' % (address, name, a.gen))
        nxt = next((s for s in starts if s > address), address + 4)
        words = image.words(address, (nxt - address) // 4)
        while words and words[-1] == 0:
            words.pop()
        masks = mask_of(words)
        at = anchor_of(masks)
        if at is None:
            print('%08X %-12s skipped: no run of %d words without an address in it' % (address, name, ANCHOR))
            continue
        entry = {'name': name, 'what': what[0] if what else '', 'source': a.source, 'address': '%08X' % address,
                 'words': len(words), 'mask': ''.join('%08X' % m for m in masks), 'anchor': at,
                 'anchor_crc32': '%08X' % zlib.crc32(struct.pack('>%dI' % ANCHOR, *words[at:at + ANCHOR])),
                 'sha256': digest(words, masks)}
        if (entry['name'], entry['sha256']) in known:
            continue
        db['signatures'].append(entry)
        added += 1
        print('%08X %-12s %4d words, %d with a masked part' % (address, name, len(words),
                                                                sum(1 for m in masks if m != 0xFFFFFFFF)))
    db['signatures'].sort(key=lambda s: (s['name'], s['source'], s['address']))
    json.dump(db, open(a.db, 'w', encoding='utf-8', newline='\n'), indent=1)
    print('crt_signatures: %d added, %d in %s' % (added, len(db['signatures']), a.db))


def scan(a):
    image = Image(a.image)
    db = json.load(open(a.db, encoding='utf-8'))
    wanted = {}
    for s in db['signatures']:
        if s['words'] >= a.min_words:
            wanted.setdefault(int(s['anchor_crc32'], 16), []).append(s)
    hits = {id(s): [] for group in wanted.values() for s in group}
    for rva, size in image.code:
        blob = image.data[rva:rva + (size & ~3)]
        view = memoryview(blob)
        for at in range(0, len(blob) - 4 * ANCHOR + 1, 4):
            group = wanted.get(zlib.crc32(view[at:at + 4 * ANCHOR]))
            if not group:
                continue
            for s in group:
                start = at - 4 * s['anchor']
                if start < 0 or start + 4 * s['words'] > len(blob):
                    continue
                masks = [int(s['mask'][i:i + 8], 16) for i in range(0, len(s['mask']), 8)]
                words = struct.unpack_from('>%dI' % s['words'], blob, start)
                if digest(words, masks) == s['sha256']:
                    hits[id(s)].append(image.base + rva + start)
    found = 0
    for group in wanted.values():
        for s in group:
            for h in hits[id(s)]:
                found += 1
                print('%08X %-12s %4d words  from %s %s  %s' %
                      (h, s['name'], s['words'], s['source'], s['address'],
                       'unique' if len(hits[id(s)]) == 1 else '%d matches' % len(hits[id(s)])))
    print('crt_signatures: %d matches of %d signatures in %s' % (found, len(db['signatures']), a.image))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    m = sub.add_parser('make')
    m.add_argument('--image', required=True)
    m.add_argument('--gen', required=True)
    m.add_argument('--list', required=True)
    m.add_argument('--db', required=True)
    m.add_argument('--source', required=True, help='the port the list comes from, e.g. sa2-nx')
    s = sub.add_parser('scan')
    s.add_argument('--image', required=True)
    s.add_argument('--db', required=True)
    s.add_argument('--min-words', type=int, default=12)
    a = ap.parse_args()
    make(a) if a.command == 'make' else scan(a)


if __name__ == '__main__':
    main()
