#!/usr/bin/env python3
"""Build the README images from the raw framebuffer captures.

Raw captures (320x170, one per screen) live in store/screenshots/ and are also
the App Store assets. This script upscales them 2x with nearest-neighbour (the
UI is pixel-exact, so no smoothing), adds a bezel, and composes the pairs used
by README.md into docs/images/.

    python3 tools/make_readme_images.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "store" / "screenshots"
OUT = ROOT / "docs" / "images"
SCALE = 2
BEZEL = 10
RADIUS = 10
GAP = 20
BEZEL_FILL = (11, 14, 17, 255)
BEZEL_LINE = (42, 49, 56, 255)


def framed(name):
    """One screen: 2x nearest upscale inside a rounded dark bezel."""
    raw = Image.open(SRC / name).convert("RGB")
    screen = raw.resize((raw.width * SCALE, raw.height * SCALE), Image.NEAREST)
    w, h = screen.size
    outer = Image.new("RGBA", (w + 2 * BEZEL, h + 2 * BEZEL), (0, 0, 0, 0))
    mask = Image.new("L", outer.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, outer.width - 1, outer.height - 1], RADIUS, fill=255)
    body = Image.new("RGBA", outer.size, BEZEL_FILL)
    body.putalpha(mask)
    outer = Image.alpha_composite(outer, body)
    screen_mask = Image.new("L", screen.size, 0)
    ImageDraw.Draw(screen_mask).rounded_rectangle([0, 0, w - 1, h - 1], 4, fill=255)
    outer.paste(screen, (BEZEL, BEZEL), screen_mask)
    ImageDraw.Draw(outer).rounded_rectangle(
        [0, 0, outer.width - 1, outer.height - 1], RADIUS, outline=BEZEL_LINE, width=1
    )
    return outer


def row(names, out):
    imgs = [framed(n) for n in names]
    width = sum(i.width for i in imgs) + GAP * (len(imgs) - 1)
    height = max(i.height for i in imgs)
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    x = 0
    for i in imgs:
        canvas.alpha_composite(i, (x, 0))
        x += i.width + GAP
    canvas.save(OUT / out, optimize=True)
    print(f"{out}: {canvas.width}x{canvas.height}")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    row(["baby.png", "parent.png"], "monitoring.png")
    row(["pair.png", "pair-enter.png"], "pairing.png")
    row(["home.png", "link-lost.png"], "home-linklost.png")


if __name__ == "__main__":
    main()
