"""The mods' 256x256 icons (mods/<name>/thunderstore/icon.png), one family:
the port's blue-violet tile and its film canister, both taken from the
port's own logo with the port's own tools/icon_gen.py, and a gold badge at
the lower right with the mod's sign drawn on it. Island Skies has its own
four skies behind the canister instead of the plain tile, inside the same
rim: the Beach's sunset, night, dawn and day as the mod draws them
(mods/island_skies/tools/sky, square cuts of captured frames, no HUD). The
same picture is each mod's thumb.png, the one its details page shows.

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


def sky_quads(folder, big):
    """Four square skies as the tile's face: sunset and night above, dawn
    and day below, the night at the upper right where the canister and
    the badge leave the most of it showing."""
    quads = Image.new("RGBA", (big, big), (0, 0, 0, 255))
    q = big // 2
    for i, name in enumerate(("sunset", "night", "dawn", "day")):
        sky = Image.open(os.path.join(folder, name + ".png")).convert("RGBA").resize((q, q), Image.LANCZOS)
        quads.paste(sky, ((i % 2) * q, (i // 2) * q))
    return quads


def base_with_canister(skies_folder=None):
    logo = Image.open(ig.LOGO).convert("RGBA")
    can = ig.cut_canister(logo)
    can = can.crop(ig.alpha_bbox(can))
    tile, face = ig.tile(S)                  # drawn at ig.SS times the size
    big = tile.size[0]
    if skies_folder is not None:
        tile.paste(sky_quads(skies_folder, big), (0, 0), face)   # inside the rim
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


def skies(img):
    """A crescent moon and a small sun: every hour's sign."""
    d, cx, cy, r = badge(img)
    mr = r * 0.46
    mx, my = cx - r * 0.22, cy + r * 0.12
    d.ellipse((mx - mr, my - mr, mx + mr, my + mr), fill=(*INK, 255))
    bx, by, br = mx + mr * 0.42, my - mr * 0.28, mr * 0.86
    d.ellipse((bx - br, by - br, bx + br, by + br), fill=(*GOLD, 255))
    # The sun, upper right: a disc with eight short rays.
    sx, sy, sr = cx + r * 0.42, cy - r * 0.42, r * 0.17
    d.ellipse((sx - sr, sy - sr, sx + sr, sy + sr), fill=(*INK, 255))
    for k in range(8):
        a = k * math.pi / 4
        x0, y0 = sx + math.cos(a) * sr * 1.35, sy + math.sin(a) * sr * 1.35
        x1, y1 = sx + math.cos(a) * sr * 1.95, sy + math.sin(a) * sr * 1.95
        d.line((x0, y0, x1, y1), fill=(*INK, 255), width=max(2, int(r * 0.05)))


def write(name, sign, skies_folder=None):
    img = base_with_canister(skies_folder)
    sign(img)
    small = img.resize((S, S), Image.LANCZOS).convert("RGB")
    for out in (os.path.join(ROOT, "mods", name, "thunderstore", "icon.png"),
                os.path.join(ROOT, "mods", name, "thumb.png")):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        small.save(out)
        print("wrote", os.path.relpath(out, ROOT))


write("unlimited_film", infinity)
write("unlock_everything", open_padlock)
write("island_skies", skies, os.path.join(ROOT, "mods", "island_skies", "tools", "sky"))
