#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Part of maxivmac (GPL v2, see LICENSE).
"""Decode an Apple NDIF disk image (what a ".img" from a classic Mac
download usually is) into a raw HFS image.

  python3 -I ndif.py INPUT.img OUTPUT.hfs

NDIF keeps its block map in a 'bcem' resource in the resource fork, and the
data in the data fork.  On macOS the fork is read from INPUT/..namedfork/rsrc
(`unar` writes it there when it unpacks a StuffIt archive); an
INPUT.rsrc file next to the image works too.  Supported chunk types: zero
(0x00), raw (0x02) and ADC-compressed (0x83), which is all the classic
Dayna/Apple installer images use.
"""
import struct
import sys


def read_resource(rsrc, rtype, rid=None):
    """Return the data of the first resource of `rtype` (and `rid`) in a resource fork."""
    data_off, map_off, _, _ = struct.unpack(">4I", rsrc[:16])
    tl_off = struct.unpack(">H", rsrc[map_off + 24:map_off + 26])[0]
    tl = map_off + tl_off
    ntypes = struct.unpack(">H", rsrc[tl:tl + 2])[0] + 1
    for i in range(ntypes):
        t, cnt, ref_off = struct.unpack(">4sHH", rsrc[tl + 2 + 8 * i:tl + 10 + 8 * i])
        if t != rtype:
            continue
        for j in range(cnt + 1):
            r = tl + ref_off + 12 * j
            this_id = struct.unpack(">h", rsrc[r:r + 2])[0]
            if rid is not None and this_id != rid:
                continue
            off = struct.unpack(">I", b"\0" + rsrc[r + 5:r + 8])[0]
            n = struct.unpack(">I", rsrc[data_off + off:data_off + off + 4])[0]
            return rsrc[data_off + off + 4:data_off + off + 4 + n]
    raise ValueError("no %r resource" % rtype)


def adc(src, expect):
    """Apple Data Compression."""
    out = bytearray()
    i = 0
    while i < len(src) and len(out) < expect:
        c = src[i]
        if c & 0x80:  # literal run
            n = (c & 0x7F) + 1
            out += src[i + 1:i + 1 + n]
            i += 1 + n
        else:  # copy from the output so far
            if c & 0x40:
                n = (c & 0x3F) + 4
                off = ((src[i + 1] << 8) | src[i + 2]) + 1
                i += 3
            else:
                n = ((c >> 2) & 0xF) + 3
                off = (((c & 3) << 8) | src[i + 1]) + 1
                i += 2
            for _ in range(n):
                out.append(out[-off])
    return bytes(out)


def decode(data, bcem):
    nchunks = struct.unpack(">I", bcem[0x7C:0x80])[0]
    chunks = [struct.unpack(">III", bcem[0x80 + 12 * i:0x8C + 12 * i]) for i in range(nchunks)]
    out = bytearray()
    for k in range(nchunks - 1):  # the last entry is a terminator
        sec_type, off, length = chunks[k]
        kind, sector = sec_type & 0xFF, sec_type >> 8
        if len(out) != sector * 512:
            raise ValueError("chunk %d starts at sector %d, output has %d bytes" % (k, sector, len(out)))
        size = ((chunks[k + 1][0] >> 8) - sector) * 512
        if kind == 0x00:
            out += bytes(size)
        elif kind == 0x02:
            out += data[off:off + length]
        elif kind == 0x83:
            piece = adc(data[off:off + length], size)
            if len(piece) != size:
                raise ValueError("chunk %d: ADC gave %d bytes, wanted %d" % (k, len(piece), size))
            out += piece
        else:
            raise ValueError("chunk %d: unsupported NDIF chunk type 0x%02x" % (k, kind))
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1:]
    data = open(src, "rb").read()
    try:
        rsrc = open(src + "/..namedfork/rsrc", "rb").read()
    except OSError:
        rsrc = open(src + ".rsrc", "rb").read()
    open(dst, "wb").write(decode(data, read_resource(rsrc, b"bcem")))
    print("%s: %d bytes" % (dst, len(open(dst, "rb").read())))


if __name__ == "__main__":
    main()
