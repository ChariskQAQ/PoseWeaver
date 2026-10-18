"""Writes samples/character.psd: a tiny layered RGB document for trying "Import PSD".

Layers (bottom to top): body, a "head" group with face + hair (RLE compressed), and a hidden
"hat" layer. Usage: python scripts/make_sample_psd.py [output.psd]
"""
import math
import struct
import sys

W, H = 200, 260


def be16(v):
    return struct.pack(">H", v & 0xFFFF)


def be32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def packbits(row):
    out = bytearray()
    i = 0
    while i < len(row):
        run = 1
        while i + run < len(row) and run < 128 and row[i + run] == row[i]:
            run += 1
        if run >= 2:
            out += bytes([257 - run, row[i]])
            i += run
        else:
            j = i
            while j < len(row) and j - i < 128 and (j + 1 >= len(row) or row[j + 1] != row[j]):
                j += 1
            j = max(j, i + 1)
            out += bytes([j - i - 1]) + bytes(row[i:j])
            i = j
    return bytes(out)


def ellipse_layer(name, cx, cy, rx, ry, rgb, group=None, hidden=False, rle=False):
    left, top = int(cx - rx), int(cy - ry)
    w, h = int(2 * rx), int(2 * ry)
    planes = [bytearray(w * h) for _ in range(4)]
    for y in range(h):
        for x in range(w):
            dx, dy = (x + 0.5 - rx) / rx, (y + 0.5 - ry) / ry
            d = math.hypot(dx, dy)
            if d <= 1:
                shade = 0.75 + 0.25 * (1 - d)
                for c in range(3):
                    planes[c][y * w + x] = int(rgb[c] * shade)
                planes[3][y * w + x] = 255 if d < 0.97 else int(255 * (1 - d) / 0.03)
    return dict(name=name, left=left, top=top, w=w, h=h, planes=planes, hidden=hidden, rle=rle, section=0)


def marker(name, section):
    return dict(name=name, left=0, top=0, w=0, h=0, planes=[bytearray()] * 4, hidden=False, rle=False, section=section)


def layer_record(l, channel_lengths):
    rec = be32(l["top"]) + be32(l["left"]) + be32(l["top"] + l["h"]) + be32(l["left"] + l["w"])
    rec += be16(4)
    for cid, ln in zip((0, 1, 2, 0xFFFF), channel_lengths):
        rec += be16(cid) + be32(ln)
    rec += b"8BIMnorm" + bytes([255, 0, 0x02 if l["hidden"] else 0x00, 0])
    name = l["name"].encode("utf-8")[:255]
    extra = be32(0) + be32(0) + bytes([len(name)]) + name
    while (len(extra) - 8) % 4:
        extra += b"\0"
    units = l["name"].encode("utf-16-be")
    luni = be32(len(units) // 2) + units
    if len(luni) % 2:
        luni += b"\0"
    extra += b"8BIMluni" + be32(len(luni)) + luni
    if l["section"]:
        extra += b"8BIMlsct" + be32(4) + be32(l["section"])
    return rec + be32(len(extra)) + extra


def channel_data(l):
    out = []
    for plane in l["planes"]:
        if l["w"] == 0 or l["h"] == 0:
            out.append(be16(0))
            continue
        if not l["rle"]:
            out.append(be16(0) + bytes(plane))
            continue
        rows = [packbits(plane[y * l["w"]:(y + 1) * l["w"]]) for y in range(l["h"])]
        out.append(be16(1) + b"".join(be16(len(r)) for r in rows) + b"".join(rows))
    return out


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "samples/character.psd"
    # File order is bottom to top; a group is [end marker, children..., folder record].
    layers = [
        ellipse_layer("body", 100, 175, 60, 80, (90, 140, 220)),
        marker("</Layer group>", 3),
        ellipse_layer("face", 100, 75, 45, 45, (240, 200, 160), rle=True),
        ellipse_layer("hair", 100, 45, 50, 22, (90, 60, 40), rle=True),
        marker("head", 1),
        ellipse_layer("hat", 100, 25, 40, 18, (200, 60, 60), hidden=True),
    ]
    data = [channel_data(l) for l in layers]
    info = be16(len(layers))
    for l, chans in zip(layers, data):
        info += layer_record(l, [len(c) for c in chans])
    for chans in data:
        info += b"".join(chans)
    if len(info) % 2:
        info += b"\0"
    layer_and_mask = be32(len(info)) + info
    psd = b"8BPS" + be16(1) + b"\0" * 6 + be16(3) + be32(H) + be32(W) + be16(8) + be16(3)
    psd += be32(0) + be32(0) + be32(len(layer_and_mask)) + layer_and_mask
    # Merged image: raw white (required by the format, unused by the importer).
    psd += be16(0) + b"\xff" * (W * H * 3)
    open(out, "wb").write(psd)
    print("wrote", out)


main()
