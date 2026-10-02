#!/usr/bin/env python3
"""Approximates the app's renderer in Python, for README images and the app icon.

The real renderer is Sources/PriceTagKit/TagRenderer.swift; this follows the
same recipe (shadow, 3D depth, outline, gradient, shine) with raster masks.

    pip install pillow numpy scipy
    python3 Tools/render_preview.py
"""

import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy.ndimage import distance_transform_edt

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FONT = os.path.join(ROOT, "Font", "MintDisplay.otf")

PALETTES = {
    "green": (0xA6FF73, 0x2BD13B, 0x07280D, 0x0B4316),
    "red": (0xFF8A70, 0xE8261C, 0x3A0605, 0x6B0F0B),
    "gold": (0xFFEA94, 0xF5A714, 0x3B2400, 0x6E4300),
    "white": (0xFFFFFF, 0xD9E1EA, 0x0E1116, 0x2A3039),
}


def rgb(h):
    return np.array([(h >> 16) & 255, (h >> 8) & 255, h & 255], dtype=np.float32) / 255


def glyph_mask(text, cap, spacing, margin):
    font = ImageFont.truetype(FONT, round(cap * 1000 / 740))
    width = sum(font.getlength(c) for c in text) + spacing * cap * len(text)
    w, h = int(width + 2 * margin), int(cap * 1.4 + 2 * margin)
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    x = margin
    baseline = margin + cap * 1.2
    for c in text:
        d.text((x, baseline), c, font=font, fill=255, anchor="ls")
        x += font.getlength(c) + spacing * cap
    return np.asarray(img, dtype=np.float32) / 255


def dilate(mask, r):
    if r <= 0:
        return mask
    dist = distance_transform_edt(mask < 0.5)
    return np.clip(r + 0.5 - dist, 0, 1).astype(np.float32)


def shift(a, dy):
    out = np.zeros_like(a)
    dy = int(round(dy))
    if dy > 0:
        out[dy:] = a[:-dy]
    elif dy < 0:
        out[:dy] = a[-dy:]
    else:
        out[:] = a
    return out


def over(dst, color, alpha):
    a = alpha[..., None]
    dst[..., :3] = color * a + dst[..., :3] * (1 - a)
    dst[..., 3:] = a + dst[..., 3:] * (1 - a)


def render(text, color="green", cap=200, outline=0.55, depth=0.45, shadow=0.5,
           shine=True, spacing=0.02):
    top, bottom, line, deep = (rgb(h) for h in PALETTES[color])
    ow, dp = outline * 0.15 * cap, depth * 0.13 * cap
    blur, drop = shadow * 0.10 * cap, shadow * 0.05 * cap
    margin = int(ow + dp + blur + drop + 0.1 * cap)
    glyph = glyph_mask(text, cap, spacing, margin)
    sil = np.maximum(glyph, dilate(glyph, ow))

    layer = np.zeros(glyph.shape + (4,), np.float32)
    for i in range(int(np.ceil(dp)), 0, -1):
        over(layer, deep, shift(sil, i))
    over(layer, line, sil)
    ys, xs = np.nonzero(glyph > 0.5)
    t = np.clip((np.arange(glyph.shape[0]) - ys.min()) / max(1, ys.max() - ys.min()), 0, 1)
    grad = top[None, :] * (1 - t[:, None]) + bottom[None, :] * t[:, None]
    fill = np.zeros_like(layer)
    fill[..., :3] = grad[:, None, :]
    fill[..., 3] = glyph
    a = glyph[..., None]
    layer[..., :3] = fill[..., :3] * a + layer[..., :3] * (1 - a)
    layer[..., 3:] = a + layer[..., 3:] * (1 - a)
    if shine:
        lip = 0.04 * cap
        over(layer, np.ones(3, np.float32), glyph * (1 - shift(glyph, lip)) * 0.55)
        over(layer, np.zeros(3, np.float32), glyph * (1 - shift(glyph, -lip)) * 0.18)
        gloss = np.zeros_like(glyph)
        mid = (ys.min() + ys.max()) / 2 - 0.06 * cap
        gloss[: int(mid)] = 1
        over(layer, np.ones(3, np.float32), glyph * gloss * 0.14)

    out = np.zeros_like(layer)
    if shadow > 0:
        sh = Image.fromarray((shift(layer[..., 3], drop) * 255).astype(np.uint8))
        sh = np.asarray(sh.filter(ImageFilter.GaussianBlur(blur / 2)), np.float32) / 255
        over(out, np.zeros(3, np.float32), sh * (0.35 + 0.35 * shadow))
    a = layer[..., 3:]
    out[..., :3] = layer[..., :3] * a + out[..., :3] * (1 - a)
    out[..., 3:] = a + out[..., 3:] * (1 - a)
    img = Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8), "RGBA")
    return img.crop(img.getbbox())


def app_icon(size=1024):
    """Dark rounded square with a green tag-style dollar sign."""
    s = size
    bg = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    inset = int(s * 0.098)
    radius = int(s * 0.18)
    tile = Image.new("RGBA", (s - 2 * inset, s - 2 * inset))
    grad = np.linspace(0, 1, tile.height)[:, None]
    c0, c1 = rgb(0x1C2B3A), rgb(0x0A1018)
    arr = np.zeros((tile.height, tile.width, 4), np.float32)
    arr[..., :3] = (c0 * (1 - grad[..., None]) + c1 * grad[..., None])
    arr[..., 3] = 1
    tile = Image.fromarray((arr * 255).astype(np.uint8), "RGBA")
    m = Image.new("L", tile.size, 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, tile.width - 1, tile.height - 1), radius, fill=255)
    shadow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    sm = Image.new("L", (s, s), 0)
    ImageDraw.Draw(sm).rounded_rectangle((inset, inset + s * 0.012, s - inset, s - inset + s * 0.012),
                                         radius, fill=110)
    shadow.putalpha(sm.filter(ImageFilter.GaussianBlur(s * 0.015)))
    bg = Image.alpha_composite(bg, shadow)
    bg.paste(tile, (inset, inset), m)
    tag = render("$", "green", cap=int(s * 0.42), outline=0.7, depth=0.6, shadow=0.6)
    bg.alpha_composite(tag, ((s - tag.width) // 2, (s - tag.height) // 2 + int(s * 0.01)))
    return bg


if __name__ == "__main__":
    out = os.path.join(ROOT, "docs")
    os.makedirs(out, exist_ok=True)

    samples = [("$12.50", "green"), ("-$110", "red"), ("$0.71", "green"), ("$1,250", "gold")]
    tiles = [render(t, c, cap=150) for t, c in samples]
    pad = 40
    w = max(t.width for t in tiles) * 2 + pad * 3
    rows = (len(tiles) + 1) // 2
    h = max(t.height for t in tiles) * rows + pad * (rows + 1)
    sheet = Image.new("RGBA", (w, h), (24, 34, 48, 255))
    cell_w = (w - pad * 3) // 2
    cell_h = (h - pad * (rows + 1)) // rows
    for i, t in enumerate(tiles):
        cx = pad + (i % 2) * (cell_w + pad) + (cell_w - t.width) // 2
        cy = pad + (i // 2) * (cell_h + pad) + (cell_h - t.height) // 2
        sheet.alpha_composite(t, (cx, cy))
    sheet.save(os.path.join(out, "samples.png"))

    iconset = os.path.join(ROOT, "Resources", "AppIcon.iconset")
    os.makedirs(iconset, exist_ok=True)
    big = app_icon(1024)
    for pt in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = pt * scale
            name = f"icon_{pt}x{pt}{'@2x' if scale == 2 else ''}.png"
            big.resize((px, px), Image.LANCZOS).save(os.path.join(iconset, name))
    big.save(os.path.join(out, "icon.png"))
    print("wrote docs/samples.png, docs/icon.png and Resources/AppIcon.iconset")
