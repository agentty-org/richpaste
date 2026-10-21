#!/usr/bin/env python3
# Checks our BMP->PNG path against an independent decoder (zlib + PNG unfilter in Python).
import os, random, struct, subprocess, sys, zlib

tool = sys.argv[1]
tmp = sys.argv[2]
random.seed(7)


def bmp(w, h, bpp, top_down=False, masks=None):
    rows = []
    pixels = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b = (x * 37 + y * 11) % 256, (x * 5 + y * 91) % 256, random.randrange(256)
            a = 255 if bpp != 32 else (x + y * 3) % 256
            row.append((r, g, b, a))
        pixels.append(row)
    stride = ((w * bpp + 31) // 32) * 4
    data = b""
    order = range(h) if top_down else range(h - 1, -1, -1)
    for y in order:
        line = b""
        for (r, g, b, a) in pixels[y]:
            if bpp == 24:
                line += bytes([b, g, r])
            elif bpp == 32:
                line += bytes([b, g, r, a])
        data += line + b"\0" * (stride - len(line))
    comp = 3 if masks else 0
    hdr = struct.pack("<IiiHHIIiiII", 40, w, -h if top_down else h, 1, bpp, comp, len(data), 2835, 2835, 0, 0)
    extra = struct.pack("<III", *masks) if masks else b""
    body = hdr + extra + data
    f = b"BM" + struct.pack("<IHHI", 14 + len(body), 0, 0, 14 + len(hdr) + len(extra)) + body
    return f, pixels


def decode_png(blob):
    assert blob[:8] == b"\x89PNG\r\n\x1a\n", "bad signature"
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(blob):
        n, = struct.unpack(">I", blob[pos:pos + 4])
        typ = blob[pos + 4:pos + 8]
        body = blob[pos + 8:pos + 8 + n]
        crc, = struct.unpack(">I", blob[pos + 8 + n:pos + 12 + n])
        assert zlib.crc32(typ + body) & 0xffffffff == crc, "bad crc on " + typ.decode()
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype == 6
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)  # also checks adler32
    stride = w * 4
    out, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - 4] if i >= 4 else 0
            if f == 1: line[i] = (line[i] + left) & 255
            elif f == 2: line[i] = (line[i] + prev[i]) & 255
            elif f != 0: raise AssertionError("unexpected filter %d" % f)
        out.append([tuple(line[x * 4:x * 4 + 4]) for x in range(w)])
        prev = line
    return w, h, out


cases = [(1, 1, 24, False, None), (7, 5, 24, False, None), (64, 33, 32, False, None),
         (13, 9, 32, True, None), (300, 200, 24, False, None),
         (10, 10, 32, False, (0x00FF0000, 0x0000FF00, 0x000000FF))]
fails = 0
for (w, h, bpp, td, masks) in cases:
    f, want = bmp(w, h, bpp, td, masks)
    inp, outp = os.path.join(tmp, "in.bmp"), os.path.join(tmp, "out.png")
    open(inp, "wb").write(f)
    subprocess.run([tool, inp, outp], check=True)
    W, H, got = decode_png(open(outp, "rb").read())
    # 32-bit alpha passes through unless it's all zero (then it means opaque).
    ok = (W, H) == (w, h) and got == want
    print(("ok  " if ok else "FAIL"), f"{w}x{h} {bpp}bpp{' top-down' if td else ''}{' bitfields' if masks else ''}")
    fails += not ok
sys.exit(1 if fails else 0)
