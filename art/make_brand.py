#!/usr/bin/env python3
"""VitaOS's bubble icon, LiveArea background and startup gate, drawn here
(no outside art): the navy gradient and soft glows of VitaOS's own ambient
background, and an Inter Bold wordmark. LiveArea wants 8-bit palette PNGs."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
FONT = HERE.parent / "assets" / "Inter-Bold.ttf"
REG = HERE.parent / "assets" / "Inter-Regular.ttf"
OUT = HERE.parent / "sce_sys"


def base(w, h):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            t = y / h
            px[x, y] = (int(18 + 10 * t), int(22 + 16 * t), int(38 + 40 * t))
    glow = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(glow)
    for cx, cy, r, c in ((0.25, 0.25, 0.55, (70, 120, 255)), (0.85, 0.85, 0.5, (150, 80, 255)), (0.6, 0.35, 0.3, (60, 170, 255))):
        d.ellipse([(cx - r) * w, (cy - r) * h, (cx + r) * w, (cy + r) * h], fill=c)
    glow = glow.filter(ImageFilter.GaussianBlur(min(w, h) * 0.22))
    return Image.blend(im, Image.eval(glow, lambda v: v).convert("RGB"), 0.35)


def ribbon(im, y0, amp, alpha):
    w, h = im.size
    over = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    import math
    pts = [(x, y0 + amp * math.sin(x / w * 5.2) + amp * 0.4 * math.sin(x / w * 12)) for x in range(0, w + 8, 8)]
    for k in range(18):
        d.line([(x, y + k) for x, y in pts], fill=(120, 170, 255, int(alpha * (1 - k / 18))), width=2)
    return Image.alpha_composite(im.convert("RGBA"), over.filter(ImageFilter.GaussianBlur(2))).convert("RGB")


def wordmark(im, size, cy, sub=None):
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(str(FONT), size)
    a, b = "Vita", "OS"
    wa, wb = d.textlength(a, font=f), d.textlength(b, font=f)
    x = (im.width - wa - wb) / 2
    d.text((x, cy), a, font=f, fill=(240, 244, 250), anchor="lm")
    d.text((x + wa, cy), b, font=f, fill=(110, 170, 255), anchor="lm")
    if sub:
        fs = ImageFont.truetype(str(REG), max(10, size // 4))
        d.text((im.width / 2, cy + size * 0.75), sub, font=fs, fill=(170, 185, 210), anchor="mm")


def icon():
    im = ribbon(base(128, 128), 92, 6, 150)
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(str(FONT), 64)
    d.text((64, 56), "V", font=f, fill=(245, 248, 255), anchor="mm")
    fo = ImageFont.truetype(str(FONT), 22)
    d.text((64, 96), "OS", font=fo, fill=(120, 175, 255), anchor="mm")
    return im


def save_p(im, path):
    im.convert("P", palette=Image.ADAPTIVE, colors=256).save(path)


if __name__ == "__main__":
    save_p(icon(), OUT / "icon0.png")
    bg = ribbon(ribbon(base(840, 500), 360, 26, 110), 400, 18, 70)
    wordmark(bg, 92, 200, "a modern home for your PS Vita")
    save_p(bg, OUT / "livearea/contents/bg.png")
    st = ribbon(base(280, 158), 120, 8, 120)
    wordmark(st, 44, 70)
    save_p(st, OUT / "livearea/contents/startup.png")
    print("wrote icon0, bg, startup")
