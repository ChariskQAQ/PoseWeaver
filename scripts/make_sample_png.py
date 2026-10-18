"""Generates samples/arm.png (a rounded, shaded limb used by the --demo scene) or, with
--variant, samples/arm_alt.png (a flat green version used as a second skin)."""
import math, struct, sys, zlib

VARIANT = "--variant" in sys.argv
args = [a for a in sys.argv[1:] if a != "--variant"]

W, H = 64, 260

def pixel(x, y):
    # capsule: centre line x = W/2, from y = 30 to H - 30, radius 26
    cx, r = W / 2, 26
    cy = min(max(y, 30), H - 30)
    d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
    if d > r:
        return (0, 0, 0, 0)
    a = int(255 * min(1.0, (r - d) * 1.5))
    shade = 0.65 + 0.35 * (1 - abs(x + 0.5 - cx) / r)
    band = 1.0 if (y // 26) % 2 == 0 else 0.85   # stripes make deformation visible
    if VARIANT:  # flat green variant used as a second skin
        band = 1.0
        rgb = (110, 200, 120)
    else:
        t = y / H
        rgb = (230 * (1 - t) + 90 * t, 140 * (1 - t) + 170 * t, 80 * (1 - t) + 240 * t)
    return tuple(int(c * shade * band) for c in rgb) + (a,)

raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(W)) for y in range(H))
def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)) + \
      chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
out = args[0] if args else "samples/arm.png"
open(out, "wb").write(png)
print("wrote", out)
