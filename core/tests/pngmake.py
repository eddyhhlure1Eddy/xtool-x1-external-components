"""Minimal PNG writer for tests: any colour type / bit depth / Adam7 / per-row filters / PLTE / tRNS.
pixels: list of rows, each row a list of tuples (one int per channel, raw sample values)."""
import random
import struct
import zlib

CH = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)


def pack_row(samples, bd):
    if bd == 8:
        return bytes(samples)
    if bd == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    out, acc, nbits = bytearray(), 0, 0
    for s in samples:
        acc = (acc << bd) | s
        nbits += bd
        if nbits == 8:
            out.append(acc)
            acc, nbits = 0, 0
    if nbits:
        out.append((acc << (8 - nbits)) & 0xFF)
    return bytes(out)


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_row(ftype, row, prev, bpp):
    out = bytearray([ftype])
    for i, x in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i] if prev is not None else 0
        c = prev[i - bpp] if (prev is not None and i >= bpp) else 0
        if ftype == 0:
            v = x
        elif ftype == 1:
            v = x - a
        elif ftype == 2:
            v = x - b
        elif ftype == 3:
            v = x - ((a + b) >> 1)
        else:
            v = x - _paeth(a, b, c)
        out.append(v & 0xFF)
    return bytes(out)


def make_png(w, h, ct, bd, pixels, interlace=0, plte=None, trns=None, filters="random", seed=1,
             extra_pre=(), extra_post=(), idat_split=0, ihdr_interlace_byte=None, level=6):
    rnd = random.Random(seed)
    ch = CH[ct]
    bpp = max(1, ch * bd // 8)
    raw = bytearray()

    def emit(rows):
        prev = None
        for row in rows:
            flat = [c for px in row for c in px]
            packed = pack_row(flat, bd)
            if filters == "random":
                ft = rnd.randrange(5)
            elif filters == "none":
                ft = 0
            else:
                ft = int(filters)
            raw.extend(filter_row(ft, packed, prev, bpp))
            prev = packed

    if interlace:
        for (x0, y0, dx, dy) in ADAM7:
            sub = [[pixels[y][x] for x in range(x0, w, dx)] for y in range(y0, h, dy)]
            if sub and sub[0]:
                emit(sub)
    else:
        emit(pixels)
    ib = interlace if ihdr_interlace_byte is None else ihdr_interlace_byte
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bd, ct, 0, 0, ib))
    for c in extra_pre:
        out += c
    if plte is not None:
        out += chunk(b"PLTE", bytes(v for e in plte for v in e))
    if trns is not None:
        out += chunk(b"tRNS", trns)
    z = zlib.compress(bytes(raw), level)
    if idat_split:
        for i in range(0, len(z), idat_split):
            out += chunk(b"IDAT", z[i:i + idat_split])
    else:
        out += chunk(b"IDAT", z)
    for c in extra_post:
        out += c
    out += chunk(b"IEND", b"")
    return out
