"""Draws the Unbound Fix app icon into Resources/AppIcon.iconset.

A flat orange tile with a white bandage: simple, and readable down to 16 px.

    pip install pillow
    python3 Tools/make_icon.py
"""
import os

from PIL import Image, ImageDraw, ImageFilter

S = 4096  # drawn at 4x of 1024, then scaled down for smooth edges
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Resources", "AppIcon.iconset")

TILE = (242, 92, 38, 255)
BANDAGE = (255, 255, 255, 255)
PAD = (250, 214, 190, 255)
HOLE = (236, 178, 145, 255)


def draw_icon():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    box = (int(S * 0.098), int(S * 0.098), int(S * 0.902), int(S * 0.902))
    radius = int((box[2] - box[0]) * 0.225)

    # Soft shadow under the tile, as on other macOS app icons
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (box[0], box[1] + int(S * 0.01), box[2], box[3] + int(S * 0.01)), radius=radius, fill=(0, 0, 0, 90))
    img = Image.alpha_composite(img, shadow.filter(ImageFilter.GaussianBlur(S * 0.01)))
    ImageDraw.Draw(img).rounded_rectangle(box, radius=radius, fill=TILE)

    # Bandage, drawn level and then turned 45°
    band = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(band)
    c = S / 2
    w, h = S * 0.66, S * 0.24
    d.rounded_rectangle((c - w / 2, c - h / 2, c + w / 2, c + h / 2), radius=h / 2, fill=BANDAGE)
    pw, ph = S * 0.21, S * 0.165
    d.rounded_rectangle((c - pw / 2, c - ph / 2, c + pw / 2, c + ph / 2), radius=S * 0.03, fill=PAD)
    r = S * 0.014
    for dx in (-0.05, 0, 0.05):
        for dy in (-0.035, 0.035):
            x, y = c + S * dx, c + S * dy
            d.ellipse((x - r, y - r, x + r, y + r), fill=HOLE)
    band = band.rotate(45, resample=Image.BICUBIC, center=(c, c))
    return Image.alpha_composite(img, band)


def main():
    os.makedirs(OUT, exist_ok=True)
    icon = draw_icon()
    for size in (16, 32, 128, 256, 512):
        icon.resize((size, size), Image.LANCZOS).save(os.path.join(OUT, f"icon_{size}x{size}.png"))
        icon.resize((size * 2, size * 2), Image.LANCZOS).save(os.path.join(OUT, f"icon_{size}x{size}@2x.png"))


if __name__ == "__main__":
    main()
