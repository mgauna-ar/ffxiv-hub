#!/usr/bin/env python3
"""Draws src/app/resources/app.ico: a faceted crystal on a dark rounded square.

Every size is rendered at 8x and box-filtered down. Tray sizes (<= 32px) use a
simplified pass: no drop shadow, no ridge highlight, thicker silhouette, since
those details turn to mush below ~40px.

Usage: python3 tools/gen_app_icon.py [--out src/app/resources/app.ico]
"""
import argparse
import os

from PIL import Image, ImageDraw, ImageFilter

SS = 8  # supersample factor
SIZES = [16, 20, 24, 32, 48, 64, 128, 256]

BG_TOP = (30, 37, 52)
BG_BOTTOM = (11, 14, 20)
RIM = (86, 106, 143)

# Crystal outline in unit canvas coordinates.
P_TOP = (0.500, 0.105)
P_UL = (0.245, 0.375)
P_LL = (0.300, 0.685)
P_BOT = (0.500, 0.905)
P_LR = (0.700, 0.685)
P_UR = (0.755, 0.375)
RIDGE = (0.432, 0.455)  # where the two facets meet mid-body

LEFT_TOP = (47, 106, 184)
LEFT_BOTTOM = (23, 52, 100)
RIGHT_TOP = (142, 219, 255)
RIGHT_BOTTOM = (46, 130, 212)
GLOW = (90, 190, 255)


def widen(pts, sx, sy):
    """Scale points about the canvas centre."""
    return [(0.5 + (x - 0.5) * sx, 0.5 + (y - 0.5) * sy) for x, y in pts]


def vgradient(size, top, bottom):
    """A width x height vertical gradient image."""
    w, h = size
    grad = Image.new("RGB", (1, h))
    px = grad.load()
    for y in range(h):
        t = y / max(1, h - 1)
        px[0, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
    return grad.resize((w, h), Image.NEAREST)


def fill_poly(target, pts, w, h, top, bottom):
    """Paint a vertical gradient through a polygon mask."""
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).polygon([(x * w, y * h) for x, y in pts], fill=255)
    target.paste(vgradient((w, h), top, bottom), (0, 0), mask)


def render(size, simple):
    w = h = size * SS
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))

    # Rounded-square plate with a vertical gradient and a hairline rim.
    plate = Image.new("L", (w, h), 0)
    radius = w * 0.235
    ImageDraw.Draw(plate).rounded_rectangle([0, 0, w - 1, h - 1], radius, fill=255)
    img.paste(vgradient((w, h), BG_TOP, BG_BOTTOM), (0, 0), plate)
    rim_w = max(1, round(w * (0.012 if simple else 0.008)))
    rim = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle(
        [rim_w / 2, rim_w / 2, w - 1 - rim_w / 2, h - 1 - rim_w / 2],
        radius, outline=RIM + (110,), width=rim_w)
    img.alpha_composite(rim)

    if not simple:
        # Cyan bloom behind the crystal, and a soft shadow under it.
        glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(glow).ellipse(
            [w * 0.20, h * 0.22, w * 0.80, h * 0.86], fill=GLOW + (60,))
        glow = glow.filter(ImageFilter.GaussianBlur(w * 0.09))
        img.alpha_composite(Image.composite(
            glow, Image.new("RGBA", (w, h), (0, 0, 0, 0)), plate))

        shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(shadow).ellipse(
            [w * 0.30, h * 0.84, w * 0.70, h * 0.96], fill=(0, 0, 0, 150))
        shadow = shadow.filter(ImageFilter.GaussianBlur(w * 0.03))
        img.alpha_composite(Image.composite(
            shadow, Image.new("RGBA", (w, h), (0, 0, 0, 0)), plate))

    # Tray sizes get a stubbier, wider crystal so the silhouette survives.
    sx, sy = (1.22, 0.94) if simple else (1.0, 1.0)
    top, ul, ll, bot, lr, ur, ridge = widen(
        [P_TOP, P_UL, P_LL, P_BOT, P_LR, P_UR, RIDGE], sx, sy)
    left = [top, ul, ll, bot, ridge]
    right = [top, ridge, bot, lr, ur]
    facets = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    fill_poly(facets, left, w, h, LEFT_TOP, LEFT_BOTTOM)
    fill_poly(facets, right, w, h, RIGHT_TOP, RIGHT_BOTTOM)

    if not simple:
        # Specular sliver riding the ridge between the two facets.
        sliver = [(0.500, 0.135), (0.530, 0.215), (0.487, 0.452),
                  (0.497, 0.735), (0.487, 0.820), (0.457, 0.452)]
        d = ImageDraw.Draw(facets)
        d.polygon([(x * w, y * h) for x, y in sliver], fill=(232, 247, 255, 195))
        d.line([(top[0] * w, top[1] * h), (ul[0] * w, ul[1] * h)],
               fill=(186, 228, 255, 130), width=max(1, round(w * 0.005)))

    img.alpha_composite(facets)
    return img.resize((size, size), Image.LANCZOS)


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(
        here, "src", "app", "resources", "app.ico"))
    ap.add_argument("--png-dir", help="also write one PNG per size here")
    args = ap.parse_args()

    frames = [render(s, simple=s <= 32) for s in SIZES]
    if args.png_dir:
        os.makedirs(args.png_dir, exist_ok=True)
        for size, frame in zip(SIZES, frames):
            frame.save(os.path.join(args.png_dir, "app_%d.png" % size))

    frames[-1].save(args.out, format="ICO",
                    sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
    print("wrote %s (%d sizes)" % (args.out, len(SIZES)))


if __name__ == "__main__":
    main()
