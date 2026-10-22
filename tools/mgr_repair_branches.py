"""Repair the verified MGR dispatcher in existing generated code, without raw codegen.

The SDK fix prevents recurrence on fresh generation. This migration accepts only
targets present in the same emitted function and verifies the original PPC image.
It never suppresses an unknown branch: remaining unresolved branches fail closed.
"""
from pathlib import Path
import argparse
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
HEAD = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{', re.M)
JUMP = re.compile(r'REX_FATAL\("Jump target 0x([0-9A-F]{8}) unresolved at bctr 0x([0-9A-F]{8})"\);')
COND = re.compile(r'REX_FATAL\("Unresolved branch from 0x([0-9A-F]{8}) to 0x([0-9A-F]{8})"\);')

def word(image, addr):
    return struct.unpack_from('>I', image, addr - 0x82000000)[0]

def repair(text, image):
    changes = 0
    def function(match):
        nonlocal changes
        body = match[0]
        labels = {int(x, 16) for x in re.findall(r'^loc_([0-9A-F]{8}):$', body, re.M)}
        def jump(m):
            nonlocal changes
            target, origin = int(m[1],16), int(m[2],16)
            if origin != 0x82EEF490 or 'DEFINE_REX_FUNC(sub_82EEEEF0)' not in body:
                raise ValueError(f'Unverified dispatcher {origin:08X}')
            table = [word(image, 0x82EEF494 + 4*i) for i in range(15)]
            if table != list(range(0x82EEF4D0, 0x82EEF50C, 4)) or word(image, origin) != 0x4E800420:
                raise ValueError('MGR jump-table image does not match')
            if target not in table or target not in labels:
                raise ValueError('Jump target is not a verified local label')
            changes += 1
            return f'goto loc_{target:08X};'
        def conditional(m):
            nonlocal changes
            origin, target = int(m[1],16), int(m[2],16)
            if origin not in (0x82EEF464, 0x82EEF468, 0x82EEF540) or target not in labels:
                raise ValueError(f'Unverified conditional {origin:08X}')
            insn = word(image, origin)
            disp = insn & 0xFFFC
            if disp & 0x8000: disp -= 0x10000
            actual = (disp if insn & 2 else origin + disp) & 0xFFFFFFFF
            if insn >> 26 != 16 or insn & 1 or actual != target:
                raise ValueError('Conditional does not match PPC image')
            changes += 1
            return f'goto loc_{target:08X};'
        body = JUMP.sub(jump, body)
        body = COND.sub(conditional, body)
        return body
    # Function boundaries are emitted at column zero; instruction braces are indented.
    text = re.sub(r'^DEFINE_REX_FUNC\(\w+\) \{.*?^\}', function, text, flags=re.M|re.S)
    if re.search(r'REX_FATAL\("(?:Jump target .* unresolved|Unresolved (?:branch|call))', text):
        raise ValueError('Unresolved control flow remains; refusing a deliverable build')
    return text, changes

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--gen', type=Path, default=ROOT/'app/generated/default')
    ap.add_argument('--image', type=Path, default=ROOT/'out/default_image.bin')
    args=ap.parse_args()
    image=args.image.read_bytes()
    count=0
    pending=[]
    for path in sorted(args.gen.glob('*_recomp.*.cpp')):
        old=path.read_text(encoding='utf-8')
        new,n=repair(old,image)
        if n:pending.append((path,new)); count+=n
    for path,new in pending:
        path.write_text(new,encoding='utf-8',newline='\n')
    print(f'mgr_repair_branches: {count} image-verified local branches repaired; unresolved checks passed')

if __name__=='__main__':main()
