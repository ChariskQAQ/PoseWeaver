"""Generate original geometric robot sprites for the starter project (stdlib only)."""
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "samples"
def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

def draw(part, x, y, w, h, base):
    ink, light, dark, gold = (28, 39, 55), (225, 244, 246), tuple(c * 3 // 5 for c in base), (255, 198, 81)
    def rgba(c): return tuple(c) + (255,)
    if part == "head":
        if not (3 <= x < w-3 and 4 <= y < h-4): return (0, 0, 0, 0)
        if x < 6 or x >= w-6 or y < 7 or y >= h-7: return rgba(ink)
        if 10 <= x < w-10 and 15 <= y < 36:
            return rgba(light if ((17 <= x < 24) or (40 <= x < 47)) and 21 <= y < 29 else ink)
        if 25 <= x < 39 and 42 <= y < 45: return rgba(dark)
        return rgba(base)
    if part == "body":
        if not (5 <= x < w-5 and 3 <= y < h-3): return (0, 0, 0, 0)
        if x < 8 or x >= w-8 or y < 6 or y >= h-6: return rgba(ink)
        if 13 <= x < w-13 and 16 <= y < 50:
            if 25 <= x < 39 and 24 <= y < 38: return rgba(gold)
            return rgba(dark)
        if y >= h-17: return rgba(ink if y % 5 < 2 else dark)
        if x in (12, w-13) and y in (10, h-24): return rgba(light)
        return rgba(base)
    if part == "limb":
        if not (2 <= x < w-2 and 2 <= y < h-2): return (0, 0, 0, 0)
        if x < 5 or x >= w-5 or y < 4 or y >= h-4: return rgba(ink)
        if 6 <= x < 13 or w-13 <= x < w-6: return rgba(gold)
        return rgba(light if y < 8 else base if y < h-7 else dark)
    if not (2 <= x < w-2 and 3 <= y < h-2): return (0, 0, 0, 0)
    return rgba(ink if y >= h-6 or x < 5 else base)

for skin, base in [("ocean", (62, 170, 190)), ("ember", (222, 112, 65))]:
    for part, w, h in [("body", 64, 80), ("head", 64, 56), ("limb", 52, 22), ("foot", 36, 18)]:
        raw = b"".join(b"\0" + b"".join(bytes(draw(part, x, y, w, h, base)) for x in range(w)) for y in range(h))
        data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
        (ROOT / f"robot_{part}_{skin}.png").write_bytes(data)
print("Generated 8 robot sprites")
