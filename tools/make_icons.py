"""The mods' 256x256 icons (mods/<name>/thunderstore/icon.png), one family:
the port's blue-violet tile and its film canister, both taken from the
port's own logo with the port's own tools/icon_gen.py, and a gold badge at
the lower right with the mod's sign drawn on it. Nothing of the game's.

    python tools/make_icons.py <Snap64Recomp checkout>

Needs Pillow and NumPy, as icon_gen.py does.
"""
import math
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(sys.argv[1], "tools"))
import icon_gen as ig  # noqa: E402

S = 256
SS = 4                      # drawn at four times the size, then reduced
GOLD = (236, 186, 52)
GOLD_EDGE = (150, 104, 18)
INK = (46, 34, 78)          # the tile's darkest violet, for the signs


def base_with_canister():
    logo = Image.open(ig.LOGO).convert("RGBA")
    can = ig.cut_canister(logo)
    can = can.crop(ig.alpha_bbox(can))
    tile, _ = ig.tile(S)                     # drawn at ig.SS times the size
    big = tile.size[0]
    h = round(big * 0.74)
    w = round(can.width * h / can.height)
    can = can.resize((w, h), Image.LANCZOS)
    tile.alpha_composite(can, (round(big * 0.30) - w // 2, (big - h) // 2 - round(big * 0.02)))
    return tile.resize((S * SS, S * SS), Image.LANCZOS)


def badge(img):
    d = ImageDraw.Draw(img)
    cx, cy, r = S * SS * 0.69, S * SS * 0.68, S * SS * 0.25
    d.ellipse((cx - r - 3 * SS, cy - r - 3 * SS, cx + r + 3 * SS, cy + r + 3 * SS), fill=(*GOLD_EDGE, 255))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(*GOLD, 255))
    return d, cx, cy, r


def infinity(img):
    d, cx, cy, r = badge(img)
    a = r * 0.62
    pts = []
    for i in range(721):
        t = 2 * math.pi * i / 720
        den = 1 + math.sin(t) ** 2
        pts.append((cx + a * math.cos(t) / den, cy + a * math.sin(t) * math.cos(t) / den * 1.25))
    d.line(pts, fill=(*INK, 255), width=round(r * 0.20), joint="curve")


def open_padlock(img):
    d, cx, cy, r = badge(img)
    bw, bh = r * 0.98, r * 0.74
    top = cy - bh * 0.18
    d.rounded_rectangle((cx - bw / 2, top, cx + bw / 2, top + bh), radius=r * 0.12, fill=(*INK, 255))
    # The shackle, lifted out of its left socket: an arch over the right half.
    sw = round(r * 0.16)
    sr = bw * 0.30
    sx = cx + bw * 0.08
    d.arc((sx - sr, top - sr * 1.95, sx + sr, top - sr * 0.05), 180, 360, fill=(*INK, 255), width=sw)
    d.line((sx + sr - sw / 2, top - sr, sx + sr - sw / 2, top + 2), fill=(*INK, 255), width=sw)
    d.line((sx - sr + sw / 2, top - sr, sx - sr + sw / 2, top - sr * 0.45), fill=(*INK, 255), width=sw)
    # The keyhole.
    kr = r * 0.10
    ky = top + bh * 0.42
    d.ellipse((cx - kr, ky - kr, cx + kr, ky + kr), fill=(*GOLD, 255))
    d.polygon([(cx - kr * 0.55, ky), (cx + kr * 0.55, ky), (cx + kr * 0.3, ky + bh * 0.30), (cx - kr * 0.3, ky + bh * 0.30)],
              fill=(*GOLD, 255))


def write(name, sign):
    img = base_with_canister()
    sign(img)
    out = os.path.join(ROOT, "mods", name, "thunderstore", "icon.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.resize((S, S), Image.LANCZOS).convert("RGB").save(out)
    print("wrote", os.path.relpath(out, ROOT))


write("unlimited_film", infinity)
write("unlock_everything", open_padlock)
