"""Renders the placeholder app icon into App/Assets.xcassets/AppIcon.appiconset.

A graphite tile with two stacked cards; the front card carries a rising green
line over a dotted baseline, like a Stocks sparkline.

    python3 Tools/render_icon.py
"""
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "App" / "Assets.xcassets" / "AppIcon.appiconset"
S = 4  # supersampling
SIZE = 1024 * S


def superellipse(box, n=5.0, steps=720):
    x0, y0, x1, y1 = box
    cx, cy, a, b = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        pts.append((cx + a * math.copysign(abs(c) ** (2 / n), c), cy + b * math.copysign(abs(s) ** (2 / n), s)))
    return pts


def vertical_gradient(size, top, bottom):
    w, h = size
    grad = Image.new("RGBA", (1, h))
    for y in range(h):
        t = y / (h - 1)
        grad.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return grad.resize((w, h))


def rounded_card(w, h, radius, fill):
    card = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(card).rounded_rectangle((0, 0, w - 1, h - 1), radius=radius, fill=fill)
    return card


def paste_rotated(base, layer, center, angle, shadow=True):
    rotated = layer.rotate(angle, resample=Image.BICUBIC, expand=True)
    x, y = int(center[0] - rotated.width / 2), int(center[1] - rotated.height / 2)
    if shadow:
        alpha = rotated.split()[3]
        sh = Image.new("RGBA", rotated.size, (0, 0, 0, 0))
        sh.putalpha(alpha.point(lambda a: a * 0.45))
        sh = sh.filter(ImageFilter.GaussianBlur(14 * S))
        base.alpha_composite(sh, (x, y + 10 * S))
    base.alpha_composite(rotated, (x, y))


def render():
    icon = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))

    # Tile, with the standard macOS margin and a soft drop shadow.
    margin = 100 * S
    box = (margin, margin, SIZE - margin, SIZE - margin)
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).polygon(superellipse(box), fill=255)
    shadow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    shadow.putalpha(mask.point(lambda a: a * 0.35))
    icon.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(10 * S)), (0, 8 * S))
    tile = vertical_gradient((SIZE, SIZE), (58, 60, 66), (28, 29, 33))
    icon.paste(tile, (0, 0), mask)

    # Cards: 63 x 88 proportions.
    cw, ch = 400 * S, int(400 * S * 88 / 63)
    radius = int(cw * 0.06)
    center = (SIZE / 2, SIZE / 2 + 6 * S)

    back = rounded_card(cw, ch, radius, (122, 126, 136, 255))
    paste_rotated(icon, back, (center[0] - 46 * S, center[1] - 4 * S), 9)

    front = rounded_card(cw, ch, radius, (250, 250, 252, 255))
    d = ImageDraw.Draw(front)
    # Dotted baseline.
    base_y = int(ch * 0.62)
    x = int(cw * 0.12)
    while x < cw * 0.88:
        d.ellipse((x, base_y - 5 * S, x + 10 * S, base_y + 5 * S), fill=(190, 192, 198, 255))
        x += 26 * S
    # Rising line.
    pts = [(0.12, 0.70), (0.26, 0.64), (0.38, 0.68), (0.50, 0.55), (0.62, 0.58), (0.74, 0.42), (0.88, 0.30)]
    line = [(int(px * cw), int(py * ch)) for px, py in pts]
    green = (52, 199, 89, 255)
    d.line(line, fill=green, width=30 * S, joint="curve")
    for px, py in (line[0], line[-1]):
        r = 15 * S
        d.ellipse((px - r, py - r, px + r, py + r), fill=green)
    # Price bar placeholder at the top, like a title line.
    d.rounded_rectangle((int(cw * 0.12), int(ch * 0.12), int(cw * 0.58), int(ch * 0.12) + 34 * S), radius=17 * S,
                        fill=(28, 29, 33, 255))
    d.rounded_rectangle((int(cw * 0.12), int(ch * 0.12) + 60 * S, int(cw * 0.40), int(ch * 0.12) + 84 * S),
                        radius=12 * S, fill=(196, 198, 204, 255))
    paste_rotated(icon, front, (center[0] + 30 * S, center[1]), -4)

    return icon.resize((1024, 1024), Image.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    master = render()
    images = []
    for points in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = points * scale
            name = f"icon_{points}x{points}{'@2x' if scale == 2 else ''}.png"
            master.resize((px, px), Image.LANCZOS).save(OUT / name)
            images.append({"filename": name, "idiom": "mac", "scale": f"{scale}x", "size": f"{points}x{points}"})
    (OUT / "Contents.json").write_text(json.dumps({"images": images, "info": {"author": "xcode", "version": 1}}, indent=2) + "\n")
    master.save(ROOT / "docs" / "icon.png")


if __name__ == "__main__":
    main()
