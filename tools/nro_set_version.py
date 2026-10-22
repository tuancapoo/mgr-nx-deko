#!/usr/bin/env python3
"""Sets the version the homebrew menu shows for an NRO, in place of a rebuild: the display version of the NACP in the
NRO's asset section. Nothing else in the file changes, so the program is byte for byte the build that was tested.

  python tools/nro_set_version.py <in.nro> <out.nro> <version> [--expect <current version>]

The NRO ends with an asset section ("ASET": icon, NACP, RomFS, each an offset and a size from the section's start);
the NACP's display version is 16 bytes at 0x3060. The input is never written; the output must not exist.
"""
import os
import struct
import sys

DISPLAY_VERSION = 0x3060
FIELD = 16


def main():
    args = sys.argv[1:]
    expect = None
    if '--expect' in args:
        i = args.index('--expect')
        expect = args[i + 1]
        del args[i:i + 2]
    if len(args) != 3:
        sys.exit(__doc__)
    src, dst, version = args
    if os.path.exists(dst):
        sys.exit('%s exists already' % dst)
    encoded = version.encode('ascii')
    if not 0 < len(encoded) < FIELD:
        sys.exit('the version must be 1 to %d ASCII characters' % (FIELD - 1))
    data = bytearray(open(src, 'rb').read())
    if data[0x10:0x14] != b'NRO0':
        sys.exit('%s is not an NRO' % src)
    aset = struct.unpack_from('<I', data, 0x18)[0]          # the program's size: the asset section follows it
    if data[aset:aset + 4] != b'ASET':
        sys.exit('no asset section after the program (at 0x%X)' % aset)
    nacp_offset, nacp_size = struct.unpack_from('<QQ', data, aset + 0x18)
    if nacp_size < DISPLAY_VERSION + FIELD:
        sys.exit('the NACP is too small (0x%X bytes)' % nacp_size)
    at = aset + nacp_offset + DISPLAY_VERSION
    current = bytes(data[at:at + FIELD]).split(b'\0')[0].decode('ascii', 'replace')
    if expect is not None and current != expect:
        sys.exit('the NRO says version %r, not %r' % (current, expect))
    data[at:at + FIELD] = encoded + b'\0' * (FIELD - len(encoded))
    open(dst, 'wb').write(data)
    original = open(src, 'rb').read()
    changed = [i for i in range(len(original)) if original[i] != data[i]]
    print('%s: version %r -> %r at 0x%X; %d byte(s) differ from the input (0x%X..0x%X)' % (
        dst, current, version, at, len(changed), changed[0] if changed else 0, changed[-1] if changed else 0))


if __name__ == '__main__':
    main()
