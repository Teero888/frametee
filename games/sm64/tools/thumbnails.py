#!/usr/bin/env python3
"""The game picker's thumbnails: data/games/sm64/thumbnail.png, the card of
Super Mario 64 as a whole, and thumbnail_<version>.png, each version's in the
start screen's version switcher.

  games/sm64/tools/thumbnails.py

A version's is its box art, from the libretro-thumbnails project (named
after the ROM sets' names), whole in front of a blurred copy of itself, with
the version's name beside it. The game's is the logo (tools/logo.png) over
all four. Needs Pillow and network access.
"""
import io
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[3]
FONT = str(ROOT / "data/fonts/Roboto-SemiBold.ttf")
OUT = ROOT / "data/games/sm64"
BOXARTS = "https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Nintendo_64/master/Named_Boxarts/"
W, H = 460, 215
# version, box art, name, what sets it apart
VERSIONS = [
    ("us", "Super Mario 64 (USA)", "USA", "North America · 1996"),
    ("jp", "Super Mario 64 (Japan)", "Japan", "Original · 1996"),
    ("eu", "Super Mario 64 (Europe) (En,Fr,De)", "Europe", "PAL · En, Fr, De · 1997"),
    ("sh", "Super Mario 64 (Japan) (Rev 3) (Shindou Edition)", "Shindou", "Japan · Rumble Pak · 1997"),
]


def boxart(name):
    with urllib.request.urlopen(BOXARTS + urllib.parse.quote(name) + ".png") as response:
        return Image.open(io.BytesIO(response.read())).convert("RGB")


def fit_text(draw, text, size, max_w):
    while size > 10:
        f = ImageFont.truetype(FONT, size)
        if draw.textlength(text, font=f) <= max_w:
            return f
        size -= 2
    return ImageFont.truetype(FONT, size)


def family(boxes):
    # the four boxes side by side, covering the card, blurred and darkened
    strip = [b.resize((round(b.width * H / b.height), H)) for b in boxes]
    width = sum(s.width for s in strip)
    bg = Image.new("RGB", (width, H))
    x = 0
    for s in strip:
        bg.paste(s, (x, 0))
        x += s.width
    bg = bg.crop(((width - W) // 2, 0, (width - W) // 2 + W, H))
    card = ImageEnhance.Brightness(bg.filter(ImageFilter.GaussianBlur(12))).enhance(0.40).convert("RGBA")
    # the logo, with a soft shadow, and the versions under it
    logo = Image.open(Path(__file__).parent / "logo.png").convert("RGBA")
    lh = 132
    lw = round(logo.width * lh / logo.height)
    logo = logo.resize((lw, lh), Image.LANCZOS)
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 170), ((W - lw) // 2 + 3, 25), logo)
    card = Image.alpha_composite(card, shadow.filter(ImageFilter.GaussianBlur(5)))
    card.alpha_composite(logo, ((W - lw) // 2, 20))
    d = ImageDraw.Draw(card)
    font = ImageFont.truetype(FONT, 17)
    text = "  ·  ".join(title for _, _, title, _ in VERSIONS)
    d.text(((W - d.textlength(text, font=font)) / 2, 20 + lh + 16), text, font=font, fill=(215, 222, 235, 255))
    card.convert("RGB").save(OUT / "thumbnail.png", optimize=True)


def main():
    boxes = []
    for gid, name, title, detail in VERSIONS:
        box = boxart(name)
        boxes.append(box)
        # background: the box art covering the card, blurred and darkened
        s = max(W / box.width, H / box.height)
        bg = box.resize((round(box.width * s), round(box.height * s)), Image.LANCZOS)
        bg = bg.crop(((bg.width - W) // 2, (bg.height - H) // 2, (bg.width - W) // 2 + W, (bg.height - H) // 2 + H))
        bg = bg.filter(ImageFilter.GaussianBlur(14))
        bg = ImageEnhance.Brightness(bg).enhance(0.42)
        card = bg.convert("RGBA")
        # a darker band behind the text
        shade = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(shade).rectangle((0, 0, W, H), fill=(8, 10, 16, 70))
        card = Image.alpha_composite(card, shade)
        # the box, whole, fitted to the height with a margin and a soft shadow
        m = 14
        bh = H - 2 * m
        bw = round(box.width * bh / box.height)
        if bw > 228:
            bw = 228
        bh = round(box.height * bw / box.width)
        art = box.resize((bw, bh), Image.LANCZOS)
        x, y = m, (H - bh) // 2
        sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(sh).rounded_rectangle((x + 3, y + 5, x + bw + 3, y + bh + 5), 6, fill=(0, 0, 0, 150))
        card = Image.alpha_composite(card, sh.filter(ImageFilter.GaussianBlur(6)))
        card.paste(art, (x, y))
        # the version
        d = ImageDraw.Draw(card)
        tx = x + bw + 22
        tw = W - tx - 24
        f_small = ImageFont.truetype(FONT, 17)
        f_title = fit_text(d, title, 48, tw)
        f_detail = ImageFont.truetype(FONT, 16)
        th = f_title.getbbox(title)[3]
        block = 22 + 8 + th + 10 + 22
        ty = (H - block) // 2
        d.text((tx, ty), "SUPER MARIO 64", font=f_small, fill=(170, 190, 225, 255))
        ty += 22 + 8
        d.text((tx + 1, ty + 2), title, font=f_title, fill=(0, 0, 0, 160))
        d.text((tx, ty), title, font=f_title, fill=(255, 255, 255, 255))
        ty += th + 10
        d.text((tx, ty), detail, font=f_detail, fill=(200, 208, 222, 255))
        card.convert("RGB").save(OUT / f"thumbnail_{gid}.png", optimize=True)

    family(boxes)


if __name__ == "__main__":
    main()
