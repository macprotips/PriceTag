"""Draws the Unbound Fix app icon into Resources/AppIcon.iconset.

    pip install pillow
    python3 Tools/make_icon.py
"""
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 4096  # drawn at 4x of 1024, then scaled down for smooth edges
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Resources", "AppIcon.iconset")


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def vertical_gradient(size, top, bottom):
    w, h = size
    column = Image.new("RGBA", (1, h))
    for y in range(h):
        column.putpixel((0, y), lerp(top, bottom, y / (h - 1)))
    return column.resize((w, h))


def squircle_mask(size, box, radius_ratio=0.225):
    mask = Image.new("L", size, 0)
    x0, y0, x1, y1 = box
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=int((x1 - x0) * radius_ratio), fill=255)
    return mask


def shield_points(cx, top, w, h, steps=120):
    """A classic shield: rounded shoulders, straight upper sides, curving to a point."""
    left, right = cx - w / 2, cx + w / 2
    r = w * 0.12  # shoulder radius
    straight = 0.36  # fraction of the height where the sides start to curve in

    def side(t):  # distance in from the outer edge at height fraction t
        if t <= straight:
            return 0.0
        u = (t - straight) / (1 - straight)
        return (w / 2) * (1 - math.cos(u * math.pi / 2)) ** 0.85

    pts = []
    # top edge, dipping slightly in the middle, between the two shoulders
    for i in range(steps + 1):
        t = i / steps
        x = left + r + (w - 2 * r) * t
        pts.append((x, top + h * 0.035 * math.sin(math.pi * t)))
    # right shoulder
    for i in range(1, steps // 4):
        a = -math.pi / 2 + (math.pi / 2) * i / (steps // 4)
        pts.append((right - r + r * math.cos(a), top + r + r * math.sin(a)))
    # right side down to the point
    for i in range(steps + 1):
        t = (r / h) + (1 - r / h) * i / steps
        pts.append((right - side(t), top + h * t))
    # left side back up
    for i in range(steps, -1, -1):
        t = (r / h) + (1 - r / h) * i / steps
        pts.append((left + side(t), top + h * t))
    # left shoulder
    for i in range(1, steps // 4):
        a = math.pi + (math.pi / 2) * i / (steps // 4)
        pts.append((left + r + r * math.cos(a), top + r + r * math.sin(a)))
    return pts


def draw_icon():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    box = (int(S * 0.098), int(S * 0.098), int(S * 0.902), int(S * 0.902))

    # Soft shadow under the tile, like macOS app icons
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sbox = (box[0], box[1] + int(S * 0.012), box[2], box[3] + int(S * 0.012))
    shadow.putalpha(squircle_mask((S, S), sbox).point(lambda v: int(v * 0.45)))
    shadow = shadow.filter(ImageFilter.GaussianBlur(S * 0.012))
    img = Image.alpha_composite(img, shadow)

    # Tile: deep night blue with a warm neon glow at the bottom
    tile = vertical_gradient((S, S), (28, 33, 64, 255), (10, 11, 24, 255))
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse((S * 0.05, S * 0.62, S * 0.95, S * 1.15), fill=(255, 70, 140, 150))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.09))
    tile = Image.alpha_composite(tile, glow)
    glow2 = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow2).ellipse((S * 0.15, S * 0.0, S * 0.85, S * 0.45), fill=(60, 200, 255, 70))
    glow2 = glow2.filter(ImageFilter.GaussianBlur(S * 0.1))
    tile = Image.alpha_composite(tile, glow2)

    # Speed lines streaking behind the shield
    lines = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lines)
    for y, x0, x1, a in [(0.40, 0.16, 0.34, 120), (0.48, 0.13, 0.33, 160), (0.56, 0.18, 0.35, 110),
                         (0.44, 0.66, 0.84, 110), (0.52, 0.67, 0.87, 150), (0.60, 0.65, 0.82, 100)]:
        ld.rounded_rectangle((S * x0, S * y, S * x1, S * y + S * 0.016), radius=S * 0.008,
                             fill=(120, 225, 255, a))
    tile = Image.alpha_composite(tile, lines)

    # Shield
    cx = S / 2
    sw, sh, stop = S * 0.38, S * 0.47, S * 0.245
    shield_shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shield_shadow).polygon(shield_points(cx, stop + S * 0.02, sw, sh), fill=(0, 0, 0, 140))
    shield_shadow = shield_shadow.filter(ImageFilter.GaussianBlur(S * 0.02))
    tile = Image.alpha_composite(tile, shield_shadow)

    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).polygon(shield_points(cx, stop, sw, sh), fill=255)
    fill = vertical_gradient((S, S), (90, 225, 255, 255), (40, 110, 255, 255))
    fill.putalpha(mask)
    tile = Image.alpha_composite(tile, fill)

    # Inner bevel highlight
    inner = Image.new("L", (S, S), 0)
    ImageDraw.Draw(inner).polygon(shield_points(cx, stop + S * 0.03, sw * 0.84, sh * 0.86), fill=255)
    ring = ImageChops.subtract(mask, inner)
    highlight = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    highlight.putalpha(ring.point(lambda v: int(v * 0.22)))
    tile = Image.alpha_composite(tile, highlight)

    # Checkmark
    check = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    cd = ImageDraw.Draw(check)
    pts = [(cx - S * 0.095, stop + sh * 0.42), (cx - S * 0.025, stop + sh * 0.56), (cx + S * 0.105, stop + sh * 0.27)]
    width = int(S * 0.05)
    cd.line(pts, fill=(255, 255, 255, 255), width=width, joint="curve")
    for p in (pts[0], pts[2]):
        r = width / 2
        cd.ellipse((p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=(255, 255, 255, 255))
    check_shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    check_shadow.putalpha(check.getchannel("A").point(lambda v: int(v * 0.35)))
    check_shadow = ImageChops.offset(check_shadow, 0, int(S * 0.008)).filter(ImageFilter.GaussianBlur(S * 0.008))
    tile = Image.alpha_composite(tile, Image.composite(check_shadow, Image.new("RGBA", (S, S)), mask))
    tile = Image.alpha_composite(tile, check)

    # Subtle top sheen on the tile
    sheen = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sheen.paste(vertical_gradient((S, S // 2), (255, 255, 255, 28), (255, 255, 255, 0)), (0, 0))
    tile = Image.alpha_composite(tile, sheen)

    tile.putalpha(squircle_mask((S, S), box))
    img = Image.alpha_composite(img, tile)

    # Hairline edge
    edge = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle(box, radius=int((box[2] - box[0]) * 0.225),
                                           outline=(255, 255, 255, 40), width=int(S * 0.003))
    return Image.alpha_composite(img, edge)


def main():
    os.makedirs(OUT, exist_ok=True)
    icon = draw_icon()
    for size in (16, 32, 128, 256, 512):
        icon.resize((size, size), Image.LANCZOS).save(os.path.join(OUT, f"icon_{size}x{size}.png"))
        icon.resize((size * 2, size * 2), Image.LANCZOS).save(os.path.join(OUT, f"icon_{size}x{size}@2x.png"))


if __name__ == "__main__":
    main()
