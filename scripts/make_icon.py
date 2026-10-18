"""Draws the application icon (assets/app.ico): a bone chain on a blue-violet rounded square.

Usage: python scripts/make_icon.py [out.ico]
"""
import math
import sys

from PIL import Image, ImageDraw

SIZE = 512


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def draw(size=SIZE):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    # Diagonal gradient background inside a rounded square.
    bg = Image.new("RGBA", (size, size))
    px = bg.load()
    top, bottom = (24, 92, 160, 255), (78, 44, 130, 255)
    for y in range(size):
        for x in range(size):
            px[x, y] = lerp(top, bottom, (x + y) / (2 * size))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=size * 0.22, fill=255)
    img.paste(bg, (0, 0), mask)

    d = ImageDraw.Draw(img)
    # Three bones bending like a raised arm, from the bottom left upwards.
    joints = [(0.24, 0.80)]
    angle, length = -62.0, 0.30
    for bend in (24.0, 28.0, 0.0):
        r = math.radians(angle)
        x, y = joints[-1]
        joints.append((x + math.cos(r) * length, y + math.sin(r) * length))
        angle += bend
        length *= 0.78
    joints = joints[:-1] if len(joints) > 4 else joints
    cols = [(64, 214, 255, 255), (120, 190, 255, 255), (220, 150, 240, 255)]
    for i in range(len(joints) - 1):
        (ax, ay), (bx, by) = joints[i], joints[i + 1]
        ax, ay, bx, by = ax * size, ay * size, bx * size, by * size
        dx, dy = bx - ax, by - ay
        ln = math.hypot(dx, dy)
        nx, ny = -dy / ln, dx / ln
        w = ln * 0.2
        mx, my = ax + dx * 0.2, ay + dy * 0.2
        kite = [(ax, ay), (mx + nx * w, my + ny * w), (bx, by), (mx - nx * w, my - ny * w)]
        d.polygon(kite, fill=cols[i % len(cols)], outline=(12, 16, 32, 255), width=max(1, size // 96))
    for i, (x, y) in enumerate(joints):
        r = size * (0.055 if i == 0 else 0.042)
        d.ellipse((x * size - r, y * size - r, x * size + r, y * size + r), fill=(255, 255, 255, 255),
                  outline=(12, 16, 32, 255), width=max(1, size // 128))
    return img


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/app.ico"
    icon = draw()
    icon.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    icon.resize((256, 256), Image.LANCZOS).save(out.rsplit(".", 1)[0] + ".png")
    print("wrote", out)
