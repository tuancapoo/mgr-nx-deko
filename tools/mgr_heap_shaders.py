import struct, mmap, os, hashlib
data = open('heap_40000000.bin', 'rb').read()
regions = []; p = 0
while p + 8 <= len(data):
    a, n = struct.unpack('<II', data[p:p+8]); regions.append((a, p + 8, n)); p += 8 + n
pf = open('phys_00000000.bin', 'rb'); phys = mmap.mmap(pf.fileno(), 0, access=mmap.ACCESS_READ)
out_dir = '../heap_shaders'; os.makedirs(out_dir, exist_ok=True)
for f in os.listdir(out_dir): os.remove(os.path.join(out_dir, f))
seen = set(); count = {'v': 0, 'p': 0}; objects = {'v': 0, 'p': 0}
for a, off, n in regions:
    b = data[off:off+n]
    for i in range(0, n - 1000, 4):
        t = b[i+3]
        if t not in (6, 7): continue
        vertex = t == 6
        h = i + (872 if vertex else 40)
        if h + 12 > n: continue
        magic, hs, us = struct.unpack('>III', b[h:h+12])
        if (magic & 0xFFFFFF00) != 0x102A1100 or (magic & 1) != (1 if vertex else 0): continue
        if hs < 12 or hs > 0x10000 or us == 0 or us > 0x100000 or h + hs > n: continue
        ptr = struct.unpack('>I', b[i + (32 if vertex else 24): i + (36 if vertex else 28)])[0]
        if (ptr & 0xE0000000) != 0xE0000000: continue
        ph = ptr & 0x1FFFFFFF
        if ph + us > len(phys): continue
        objects['v' if vertex else 'p'] += 1
        container = b[h:h+hs] + phys[ph:ph+us]
        key = hashlib.sha1(container).hexdigest()[:16]
        if key in seen: continue
        seen.add(key); count['v' if vertex else 'p'] += 1
        open(os.path.join(out_dir, f"{'v' if vertex else 'p'}_{key}.bin"), 'wb').write(container)
print('objects found', objects, 'unique containers written', count)
