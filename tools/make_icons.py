"""The mods' 256x256 icons (mods/<name>/thunderstore/icon.png), one family:
the port's blue-violet tile and its film canister, both taken from the
port's own logo with the port's own tools/icon_gen.py, and a gold badge at
the lower right with the mod's sign drawn on it. Nothing of the game's.
The same picture is each mod's thumb.png, the one its details page shows.

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


def stamp(d, pts, width, color=INK):
    """A stroke as discs stamped along its points: no joins, so no pinholes
    (a joined line left gold specks all along the infinity loop)."""
    h = width / 2.0
    for x, y in pts:
        d.ellipse((x - h, y - h, x + h, y + h), fill=(*color, 255))


def seg(d, p0, p1, width, color=INK):
    """A straight stroke with round ends."""
    n = max(2, int(math.hypot(p1[0] - p0[0], p1[1] - p0[1]) / (width * 0.25)) + 1)
    stamp(d, [(p0[0] + (p1[0] - p0[0]) * i / (n - 1), p0[1] + (p1[1] - p0[1]) * i / (n - 1)) for i in range(n)], width, color)


def infinity(img):
    d, cx, cy, r = badge(img)
    a = r * 0.62
    pts = []
    for i in range(1440):
        t = 2 * math.pi * i / 1440
        den = 1 + math.sin(t) ** 2
        pts.append((cx + a * math.cos(t) / den, cy + a * math.sin(t) * math.cos(t) / den * 1.25))
    stamp(d, pts, r * 0.20)


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


def crescent(img):
    """A waxing moon with one small star: the night's sign."""
    d, cx, cy, r = badge(img)
    mr = r * 0.52
    mx, my = cx - r * 0.10, cy + r * 0.04
    d.ellipse((mx - mr, my - mr, mx + mr, my + mr), fill=(*INK, 255))
    # The dark of the moon, cut back out in the badge's gold, offset to the upper right.
    bx, by, br = mx + mr * 0.42, my - mr * 0.28, mr * 0.86
    d.ellipse((bx - br, by - br, bx + br, by + br), fill=(*GOLD, 255))
    # The star, four points, to the upper right of the horns.
    sx, sy, sl, ss = cx + r * 0.46, cy - r * 0.46, r * 0.22, r * 0.07
    d.polygon([(sx, sy - sl), (sx + ss, sy - ss), (sx + sl, sy), (sx + ss, sy + ss),
               (sx, sy + sl), (sx - ss, sy + ss), (sx - sl, sy), (sx - ss, sy - ss)], fill=(*INK, 255))


def disc(d, x, y, rad, color=INK):
    d.ellipse((x - rad, y - rad, x + rad, y + rad), fill=(*color, 255))


def skies(img):
    """Every hour and every weather: the sun and the moon above, a storm
    cloud with rain and a bolt and a snowflake below, four marks of one
    weight set at the badge's four corners."""
    d, cx, cy, r = badge(img)
    w = r * 0.055                            # the one stroke width

    # The sun, upper left: a disc with eight rays.
    sx, sy = cx - r * 0.44, cy - r * 0.42
    disc(d, sx, sy, r * 0.13)
    for k in range(8):
        a = k * math.pi / 4
        seg(d, (sx + math.cos(a) * r * 0.20, sy + math.sin(a) * r * 0.20),
            (sx + math.cos(a) * r * 0.30, sy + math.sin(a) * r * 0.30), w)

    # The moon, upper right: a disc with its dark cut back out in gold.
    mx, my, mr = cx + r * 0.44, cy - r * 0.42, r * 0.24
    disc(d, mx, my, mr)
    disc(d, mx + mr * 0.42, my - mr * 0.28, mr * 0.86, GOLD)

    # The storm cloud, lower left: three lobes on a flat base, and under
    # it a bolt between two drops of rain.
    ex, ey = cx - r * 0.42, cy + r * 0.26
    disc(d, ex - r * 0.14, ey, r * 0.13)
    disc(d, ex + r * 0.01, ey - r * 0.08, r * 0.16)
    disc(d, ex + r * 0.16, ey + r * 0.01, r * 0.12)
    d.rectangle((ex - r * 0.14, ey, ex + r * 0.16, ey + r * 0.12), fill=(*INK, 255))
    bolt = [(0.05, 0.00), (-0.06, 0.17), (0.01, 0.17), (-0.05, 0.32), (0.08, 0.13), (0.01, 0.13)]
    d.polygon([(ex + r * bx, ey + r * (0.14 + by)) for bx, by in bolt], fill=(*INK, 255))
    for dx in (-0.17, 0.17):
        seg(d, (ex + r * dx, ey + r * 0.18), (ex + r * (dx - 0.04), ey + r * 0.30), w)

    # The snowflake, lower right: six arms, each with a pair of branches.
    fx, fy = cx + r * 0.44, cy + r * 0.40
    for k in range(6):
        a = k * math.pi / 3
        ux, uy = math.cos(a), math.sin(a)
        seg(d, (fx, fy), (fx + ux * r * 0.22, fy + uy * r * 0.22), w)
        bx, by = fx + ux * r * 0.13, fy + uy * r * 0.13
        for side in (-1, 1):
            b = a + side * math.pi / 3
            seg(d, (bx, by), (bx + math.cos(b) * r * 0.08, by + math.sin(b) * r * 0.08), w)


def write(name, sign):
    img = base_with_canister()
    sign(img)
    small = img.resize((S, S), Image.LANCZOS).convert("RGB")
    for out in (os.path.join(ROOT, "mods", name, "thunderstore", "icon.png"),
                os.path.join(ROOT, "mods", name, "thumb.png")):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        small.save(out)
        print("wrote", os.path.relpath(out, ROOT))


write("unlimited_film", infinity)
write("unlock_everything", open_padlock)
write("island_skies", skies)
