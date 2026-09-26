#!/usr/bin/env python3
"""Console photos for VitaOS's Play tab, from Wikimedia Commons.

    python3 arcadehub/consoles_public/make.py

The photos are Evan-Amos's, on white. 15 are public domain; the Atari 7800
and Vectrex are CC BY-SA 3.0 (credit in CREDITS.md, and these cut-outs are
shared under the same licence). sources.json lists each file, URL and licence.
The white background is flood-filled from the edges (so white parts of a
console, like a Game Boy's screen frame, stay), feathered, trimmed to the
visible pixels, and fitted in 440x300.
"""
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
BOX = (440, 300)


def key_out(im):
    im = im.convert("RGB")
    w, h = im.size
    px = im.load()
    mask = Image.new("L", (w, h), 0)                 # 255 = could be background
    mp = mask.load()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            lo, hi = min(r, g, b), max(r, g, b)
            if lo > 228 and hi - lo < 20:            # near white (and the palest shadow)
                mp[x, y] = 255
    for x in range(0, w, 4):                         # only what touches the edge is background
        for y in (0, h - 1):
            if mp[x, y] == 255:
                ImageDraw.floodfill(mask, (x, y), 128)
    for y in range(0, h, 4):
        for x in (0, w - 1):
            if mp[x, y] == 255:
                ImageDraw.floodfill(mask, (x, y), 128)
    # White pockets that cables or controllers wall off from the edge (between
    # an Atari and its joystick): key any big near-white region out too; small
    # ones are the console's own white details and stay.
    thresh = w * h * 0.0015
    for y in range(0, h, 6):
        for x in range(0, w, 6):
            if mp[x, y] != 255:
                continue
            ImageDraw.floodfill(mask, (x, y), 100)
            n = mask.histogram()[100]
            ImageDraw.floodfill(mask, (x, y), 128 if n > thresh else 50)
    alpha = mask.point(lambda v: 0 if v == 128 else 255)
    alpha = alpha.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(1.2))   # a soft edge
    out = im.convert("RGBA")
    out.putalpha(alpha)
    return out


def fit(im):
    bbox = im.getchannel("A").point(lambda v: 255 if v > 24 else 0).getbbox()
    if bbox:
        im = im.crop(bbox)
    im.thumbnail(BOX, Image.LANCZOS)
    return im


def main():
    src = HERE / "src"
    (HERE / "out").mkdir(exist_ok=True)
    for f in sorted(src.glob("*.jpg")):
        im = fit(key_out(Image.open(f)))
        im.save(HERE / "out" / (f.stem + ".png"))
        print(f.stem, im.size)


if __name__ == "__main__":
    main()
