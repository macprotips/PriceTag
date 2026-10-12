import sys, os
# Draws Resources/AppIcon.iconset:  python3 Tools/make_icon.py Resources/AppIcon.iconset  (needs Pillow)
from PIL import Image, ImageDraw, ImageFilter
S = 4096  # draw big, scale down for antialiasing
out = sys.argv[1]
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

# macOS icon grid: 824/1024 body, centered
inset = int(S * 100 / 1024)
radius = int(S * 185 / 1024)
box = [inset, inset, S - inset, S - inset]

# diagonal gradient deep red -> navy
grad = Image.new("RGBA", (S, S))
top, bot = (214, 32, 48), (22, 34, 92)
px = grad.load()
for y in range(0, S, 4):
    for x in range(0, S, 4):
        t = min(1, max(0, (x * 0.35 + y * 0.65) / S))
        c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,)
        for dy in range(4):
            for dx in range(4):
                px[x + dx, y + dy] = c
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle(box, radius, fill=255)

# soft shadow under the body
shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(shadow).rounded_rectangle([box[0], box[1] + S // 80, box[2], box[3] + S // 80], radius, fill=(0, 0, 0, 90))
shadow = shadow.filter(ImageFilter.GaussianBlur(S // 60))
img.alpha_composite(shadow)
body = Image.new("RGBA", (S, S), (0, 0, 0, 0))
body.paste(grad, (0, 0), mask)
img.alpha_composite(body)

# subtle web-like rings from the top left corner
web = Image.new("RGBA", (S, S), (0, 0, 0, 0))
wd = ImageDraw.Draw(web)
cx, cy = box[0], box[1]
for r in range(1, 9):
    rr = int(S * 0.11 * r)
    wd.arc([cx - rr, cy - rr, cx + rr, cy + rr], 0, 90, fill=(255, 255, 255, 15), width=S // 220)
import math
for a in range(0, 91, 15):
    ex = cx + int(math.cos(math.radians(a)) * S)
    ey = cy + int(math.sin(math.radians(a)) * S)
    wd.line([cx, cy, ex, ey], fill=(255, 255, 255, 15), width=S // 220)
webm = Image.new("RGBA", (S, S), (0, 0, 0, 0))
webm.paste(web, (0, 0), mask)
img.alpha_composite(webm)

def seg(draw, a, b, color, w):
    draw.line([*a, *b], fill=color, width=w)
    for p in (a, b):
        draw.ellipse([p[0] - w // 2, p[1] - w // 2, p[0] + w // 2, p[1] + w // 2], fill=color)

def figure(draw, cx, top, scale, arms, color, w):
    head_r = int(110 * scale)
    hy = top + head_r
    draw.ellipse([cx - head_r, hy - head_r, cx + head_r, hy + head_r], fill=color)
    neck = hy + head_r + int(40 * scale)
    hip = neck + int(560 * scale)
    seg(draw, (cx, neck), (cx, hip), color, w)
    sh = neck + int(60 * scale)
    if arms == "T":
        seg(draw, (cx - int(560 * scale), sh), (cx + int(560 * scale), sh), color, w)
    else:
        for s in (-1, 1):
            elbow = (cx + s * int(150 * scale), sh + int(260 * scale))
            hand = (cx + s * int(200 * scale), sh + int(520 * scale))
            seg(draw, (cx, sh), elbow, color, w)
            seg(draw, elbow, hand, color, w)
            draw.ellipse([elbow[0] - w // 2, elbow[1] - w // 2, elbow[0] + w // 2, elbow[1] + w // 2], fill=color)
    for s in (-1, 1):
        knee = (cx + s * int(130 * scale), hip + int(330 * scale))
        foot = (cx + s * int(170 * scale), hip + int(660 * scale))
        seg(draw, (cx, hip), knee, color, w)
        seg(draw, knee, foot, color, w)
        draw.ellipse([knee[0] - w // 2, knee[1] - w // 2, knee[0] + w // 2, knee[1] + w // 2], fill=color)
    for p in [(cx, neck), (cx, hip), (cx, sh)]:
        draw.ellipse([p[0] - w // 2, p[1] - w // 2, p[0] + w // 2, p[1] + w // 2], fill=color)

scale = S / 4096 * 1.45
w = int(115 * scale)
ghost = Image.new("RGBA", (S, S), (0, 0, 0, 0))
figure(ImageDraw.Draw(ghost), S // 2, int(S * 0.20), scale, "T", (255, 255, 255, 60), w)
img.alpha_composite(ghost)
fig = Image.new("RGBA", (S, S), (0, 0, 0, 0))
figure(ImageDraw.Draw(fig), S // 2, int(S * 0.20), scale, "down", (255, 255, 255, 255), w)
fs = fig.copy().filter(ImageFilter.GaussianBlur(S // 120))
dark = Image.new("RGBA", (S, S), (0, 0, 0, 0))
dark.paste((0, 0, 0, 80), (0, 0), fs.split()[3])
img.alpha_composite(dark, (0, S // 200))
img.alpha_composite(fig)

# check badge
br = int(S * 0.13)
bx, by = int(S * 0.73), int(S * 0.73)
badge = Image.new("RGBA", (S, S), (0, 0, 0, 0))
bd = ImageDraw.Draw(badge)
bd.ellipse([bx - br - S // 90, by - br - S // 90, bx + br + S // 90, by + br + S // 90], fill=(255, 255, 255, 255))
bd.ellipse([bx - br, by - br, bx + br, by + br], fill=(52, 199, 89, 255))
cw = int(br * 0.26)
p1 = (bx - int(br * 0.48), by + int(br * 0.02))
p2 = (bx - int(br * 0.12), by + int(br * 0.38))
p3 = (bx + int(br * 0.52), by - int(br * 0.36))
bd.line([p1, p2, p3], fill=(255, 255, 255, 255), width=cw, joint="curve")
for p in (p1, p3):
    bd.ellipse([p[0] - cw // 2, p[1] - cw // 2, p[0] + cw // 2, p[1] + cw // 2], fill=(255, 255, 255, 255))
img.alpha_composite(badge)

os.makedirs(out, exist_ok=True)
for size in (16, 32, 128, 256, 512):
    for scale2 in (1, 2):
        px_ = size * scale2
        name = f"icon_{size}x{size}{'@2x' if scale2 == 2 else ''}.png"
        img.resize((px_, px_), Image.LANCZOS).save(os.path.join(out, name))
img.resize((1024, 1024), Image.LANCZOS).save(os.path.join(os.path.dirname(out.rstrip('/')), "icon-preview.png"))
