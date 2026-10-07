"""Writes src/night_data.c and src/night_data.h for Island at Night: each course's stars and moon,
the three small pictures they are drawn with, and the numbers the mod's code needs about each sky.

Each of these courses has its sky as a six-sided box around the camera with a 64x32 picture (the
Valley has two) on its sides. This script reads a course's sky list from the player's own ROM and
follows a sight line to the box: which picture it lands on, and where. That answers two things. A
star is kept only where the picture shows open sky, so none shines through a cloud; and each row of
a picture is given its height above the horizon, so the mod's code can repaint the picture as night
sky, lighter toward the horizon. Nothing of the ROM's pictures is written out.

The stars follow the game's own, the ones the cartridge shows above Rainbow Cloud: soft blue-gray
dots sprinkled evenly, and a few bright white sparkles with long rays. Here each is a sprite, most
drawn with a soft dot and the bright ones with a four-pointed glint. Each belongs to one of a few
groups whose brightness the mod's code changes from frame to frame: the twinkle.

    python gen_night.py <pokemonsnap.z64> [--hist]
"""
import math
import os
import random
import struct
import sys

COMMON = {
    'magikarp_textures': (0x800F5D90, 0x800FFF90, 0x000731B0),
    'pikachu1': (0x800FFF90, 0x8011B050, 0x0007D3B0),
    'bulbasaur1': (0x8011CB50, 0x8012A0C0, 0x00099F70),
}
# r_lo, r_hi: the picture's red (5 bits) at open sky and at full cloud.
# moon: (azimuth from, to, elevation from, to) to look for the clearest place, or None.
COURSES = {
    'beach': dict(setup=0x8011B914, segs=[(0x8011B050, 0x801B0310, 0x000A74E0)], r_lo=14, r_hi=23,
                  moon=(150, 260, 8.5, 12.5), stars=(700, 260, 9), seed=1999),
    'river': dict(setup=0x8012AC90, segs=[(0x8012A0C0, 0x8019AEE0, 0x0029A190)], r_lo=23, r_hi=27,
                  moon=(0, 359, 9.0, 16.0), stars=(520, 190, 7), seed=2001),
    'volcano': dict(setup=0x800FFFB8, segs=[(0x800FFF90, 0x801A9900, 0x00326C10)], r_lo=24, r_hi=31,
                    moon=None, stars=(170, 60, 3), seed=2003),
    'valley': dict(setup=0x80100720, segs=[(0x800FFF90, 0x80186B10, 0x003F63D0)], r_lo=14, r_hi=26,
                   moon=(0, 359, 10.0, 18.0), stars=(760, 280, 10), seed=2005),
}
DIST = 1400.0           # how far the sprites stand from the camera, inside every box
FAINT_GROUPS = 6        # twinkle groups: the faint stars, all but steady;
CALM_GROUPS = 16        # the middle stars in three tempers: calm,
BREATHING_GROUPS = 10   # breathing,
SHIMMER_GROUPS = 6      # and a few that shimmer quickly but slightly;
BRIGHT_MAX = 16         # each bright star's rays twinkle alone
MID_GROUPS = CALM_GROUPS + BREATHING_GROUPS + SHIMMER_GROUPS
MOON_HALF = 46.0        # half the moon's width at DIST: about 1.9 degrees


class Rom:
    def __init__(self, path, segs):
        self.rom = open(path, 'rb').read()
        self.segs = segs

    def off(self, va):
        for lo, hi, rom in self.segs:
            if lo <= va < hi:
                return rom + va - lo
        raise KeyError(hex(va))

    def u32(self, va):
        return struct.unpack('>I', self.rom[self.off(va):self.off(va) + 4])[0]

    def data(self, va, n):
        return self.rom[self.off(va):self.off(va) + n]


def read_sky(rom, setup):
    """The sky list's triangles: three corners each (position, picture coordinate) and the picture."""
    sky = rom.u32(rom.u32(setup) + 8)
    va = rom.u32(sky)
    start = va
    buf = [None] * 64
    pic = None
    pics = []
    tris = []
    arrays = []
    spare = None
    last_sync = None
    while True:
        w0, w1 = struct.unpack('>II', rom.data(va, 8))
        op = w0 >> 24
        if op == 0x01:
            n = (w0 >> 12) & 0xFF
            v0 = ((w0 >> 1) & 0x7F) - n
            arrays.append((w1, n))
            for i in range(n):
                x, y, z, _, s, t, r, g, b, a = struct.unpack('>hhhHhhBBBB', rom.data(w1 + 16 * i, 16))
                buf[v0 + i] = ((x, y, z), (s, t))
        elif op == 0xFD:
            pic = w1
            if pic not in pics:
                pics.append(pic)
        elif op in (0x05, 0x06, 0x07):
            idx = [((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2]
            tris.append((buf[idx[0]], buf[idx[1]], buf[idx[2]], pic))
            if op != 0x05:
                idx = [((w1 >> 16) & 0xFF) // 2, ((w1 >> 8) & 0xFF) // 2, (w1 & 0xFF) // 2]
                tris.append((buf[idx[0]], buf[idx[1]], buf[idx[2]], pic))
        elif op == 0xE7:
            if last_sync == va - 8:
                spare = va
            last_sync = va
        elif op == 0xDF:
            break
        va += 8
    return dict(list=start, tris=tris, pics=pics, arrays=arrays[1:], spare=spare)


def hit(tris, d):
    """Where the sight line d from the camera meets the sky: (picture, column, row) in texels."""
    best = None
    for a, b, c, pic in tris:
        p0, p1, p2 = a[0], b[0], c[0]
        e1 = [p1[i] - p0[i] for i in range(3)]
        e2 = [p2[i] - p0[i] for i in range(3)]
        h = (d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0])
        det = sum(e1[i] * h[i] for i in range(3))
        if abs(det) < 1e-9:
            continue
        s = [-p0[i] for i in range(3)]
        u = sum(s[i] * h[i] for i in range(3)) / det
        if u < -1e-6 or u > 1 + 1e-6:
            continue
        q = (s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0])
        v = sum(d[i] * q[i] for i in range(3)) / det
        if v < -1e-6 or u + v > 1 + 1e-6:
            continue
        t = sum(e2[i] * q[i] for i in range(3)) / det
        if t <= 0:
            continue
        if best is None or t < best[0]:
            st = [a[1][i] + u * (b[1][i] - a[1][i]) + v * (c[1][i] - a[1][i]) for i in range(2)]
            best = (t, pic, st[0] / 32.0 - 0.5, st[1] / 32.0 - 0.5)
    return best and best[1:]


def direction(az_deg, el_deg):
    az, el = math.radians(az_deg), math.radians(el_deg)
    return (math.cos(el) * math.cos(az), math.sin(el), math.cos(el) * math.sin(az))


def basis(d):
    up = (0.0, 1.0, 0.0) if abs(d[1]) < 0.95 else (1.0, 0.0, 0.0)
    right = (d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0])
    m = math.sqrt(sum(c * c for c in right))
    right = tuple(c / m for c in right)
    up = (right[1] * d[2] - right[2] * d[1], right[2] * d[0] - right[0] * d[2], right[0] * d[1] - right[1] * d[0])
    return right, up


def angle_between(a, b):
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(a[i] * b[i] for i in range(3))))))


def make_pictures():
    """The mod's own three pictures: a soft dot, a four-pointed glint, the moon."""
    dot = []
    for y in range(16):
        for x in range(16):
            r = math.hypot(x - 7.5, y - 7.5)
            v = math.exp(-(r / 2.1) ** 2) + 0.30 * math.exp(-(r / 4.6) ** 2)
            v *= max(0.0, min(1.0, (7.6 - r) / 2.0))
            dot.append(max(0, min(255, int(round(255 * min(1.0, v))))))
    glint = []
    for y in range(32):
        for x in range(32):
            dx, dy = x - 15.5, y - 15.5
            r = math.hypot(dx, dy)
            core = math.exp(-(r / 2.2) ** 2) + 0.30 * math.exp(-(r / 5.0) ** 2)
            # A compact sparkle, after the crisp points of the game's own night sky
            # (the Rainbow Cloud's): short rays, a bright heart.
            ray = max(math.exp(-(dy / 0.8) ** 2) * math.exp(-abs(dx) / 3.4),
                      math.exp(-(dx / 0.8) ** 2) * math.exp(-abs(dy) / 3.4)) * 0.9
            d1, d2 = (dx + dy) / math.sqrt(2), (dx - dy) / math.sqrt(2)
            diag = max(math.exp(-(d1 / 0.7) ** 2) * math.exp(-abs(d2) / 1.6),
                       math.exp(-(d2 / 0.7) ** 2) * math.exp(-abs(d1) / 1.6)) * 0.45
            v = min(1.0, core + ray + diag) * max(0.0, min(1.0, (15.6 - max(abs(dx), abs(dy))) / 2.5))
            glint.append(max(0, min(255, int(round(255 * v)))))
    rng = random.Random(7)
    seas = [(-0.38, -0.30, 0.30, 0.26, 0.17), (-0.04, -0.42, 0.26, 0.18, 0.15), (0.22, -0.20, 0.22, 0.22, 0.14),
            (-0.56, 0.06, 0.18, 0.32, 0.15), (0.56, -0.32, 0.10, 0.12, 0.12), (0.10, 0.30, 0.18, 0.13, 0.09)]
    craters = [(0.30, 0.52, 0.07), (-0.20, 0.62, 0.05), (0.62, 0.18, 0.05), (-0.52, 0.46, 0.04)]
    moon = []
    for y in range(32):
        for x in range(32):
            u, v = (x - 15.5) / 14.5, (y - 15.5) / 14.5      # the picture's top is the moon's north
            r = math.hypot(u, v)
            alpha = max(0.0, min(1.0, (1.0 - r) * 14.5 / 1.3 + 0.5))
            light = 0.95 - 0.16 * max(0.0, r) ** 2.4
            for cx, cy, rx, ry, depth in seas:
                light -= depth * math.exp(-(((u - cx) / rx) ** 2 + ((v - cy) / ry) ** 2) * 1.6)
            for cx, cy, cr in craters:
                light += 0.07 * math.exp(-(((u - cx) / cr) ** 2 + ((v - cy) / cr) ** 2))
            light += rng.uniform(-0.012, 0.012)
            i8 = max(0, min(255, int(round(255 * max(0.0, min(1.0, light))))))
            a8 = max(0, min(255, int(round(255 * alpha))))
            moon.append((i8 << 8) | a8)
    sine = [int(round(127 * math.sin(2 * math.pi * i / 256.0))) for i in range(256)]
    return dot, glint, moon, sine


def make_twinkles():
    """How each twinkle group behaves. A group's brightness is its base plus two slow waves of
    different lengths (so it never quite repeats) and, for some, a brief swell now and then.
    Most of the sky is calm; a part breathes; a few shimmer quickly but slightly; the bright stars'
    rays swell and fade slowly. Lengths are in seconds; the code counts sixtieths, the pace of the world's clock."""
    rng = random.Random(42)
    out = []

    def groups(count, base, amp, seconds, second_wave, flare=0, flare_every=(0, 0)):
        # The wave lengths of one kind are spread evenly over their range (then dealt out in a
        # shuffled order), so that no two groups keep step with each other.
        lo, hi = seconds
        lengths = [lo + (hi - lo) * (k + 0.5 + rng.uniform(-0.3, 0.3)) / count for k in range(count)]
        rng.shuffle(lengths)
        for length in lengths:
            p1 = length * 60.0
            p2 = p1 * second_wave * rng.uniform(0.92, 1.08)
            p3 = rng.uniform(*flare_every) * 60.0 if flare else 0.0
            out.append((base, amp, flare, int(65536 / p1), int(65536 / p2), int(65536 / p3) if p3 else 0,
                        rng.randrange(65536), rng.randrange(65536), rng.randrange(65536)))

    groups(FAINT_GROUPS, 188, 12, (4.0, 9.0), 2.37)
    groups(CALM_GROUPS, 205, 24, (3.0, 6.5), 0.41)
    groups(BREATHING_GROUPS, 188, 52, (1.6, 3.2), 1.73)
    groups(SHIMMER_GROUPS, 196, 26, (0.45, 0.9), 2.9, flare=46, flare_every=(9.0, 20.0))
    groups(BRIGHT_MAX, 165, 40, (4.0, 9.0), 0.37, flare=58, flare_every=(11.0, 24.0))
    return out


RAIN_LAYERS = 10        # sheets of drops, each falling at its own pace
RAIN_DROPS = 120        # drops in a sheet
RAIN_HEIGHT = 1400      # a sheet's height: it is drawn twice, one above the other, and wraps unseen
RAIN_NEAR, RAIN_FAR = 220.0, 1000.0     # how near and how far from the camera the drops stand
BOLTS = 8               # lightning shapes to choose from
BOLT_DIST = 1400.0      # how far off a bolt stands (it is drawn with the sky, behind the land)
RAIN_SLANT = (0.07, 0.03)               # the wind: how far a drop drifts for each unit it falls


def make_rain(w, h):
    """The rain: sheets of short soft streaks around the camera, as plain geometry. Each drop is two
    crossed upright quads (so it shows from any side) drawn with the soft dot, leaning back along
    its fall. The mod's code moves each sheet down by a matrix; nothing here changes."""
    rng = random.Random(77)
    speeds = []
    for layer in range(RAIN_LAYERS):
        speed = 18 + (layer * 3) % 10              # units a tick (a sixtieth of a second): 18..27, no two sheets alike
        speeds.append(speed)
        verts = []
        order = list(range(RAIN_DROPS))
        rng.shuffle(order)
        for i in range(RAIN_DROPS):
            a = rng.uniform(0, 2 * math.pi)
            r = math.sqrt(rng.uniform(RAIN_NEAR ** 2, RAIN_FAR ** 2))
            x, z = r * math.cos(a), r * math.sin(a)
            y = (order[i] + rng.uniform(0.1, 0.9)) * RAIN_HEIGHT / RAIN_DROPS
            length = rng.uniform(40.0, 66.0)
            half = rng.uniform(1.2, 1.7)
            tone = rng.uniform(0.86, 1.0)
            col = (int(170 * tone), int(192 * tone), int(226 * tone))
            top = (x - RAIN_SLANT[0] * length, y + length, z - RAIN_SLANT[1] * length)
            for ax, az in ((1.0, 0.0), (0.0, 1.0)):
                for (px, py, pz), sgn, (s, t) in (((x, y, z), -1, (0, 512)), ((x, y, z), 1, (512, 512)),
                                                   (top, 1, (512, 0)), (top, -1, (0, 0))):
                    verts.append('    V(%d, %d, %d, %d, %d, %d, %d, %d),' % (
                        int(round(px + sgn * half * ax)), int(round(py)), int(round(pz + sgn * half * az)), s, t, col[0], col[1], col[2]))
        w('static Vtx night_rain_vtx%d[] = {' % layer)
        for v in verts:
            w(v)
        w('};')
        w('static Gfx night_rain_sheet%d[] = {' % layer)
        for first in range(0, len(verts), 32):
            n = min(32, len(verts) - first)
            w('    gsSPVertex(&night_rain_vtx%d[%d], %d, 0),' % (layer, first, n))
            for o in range(0, n, 4):
                w('    gsSP2Triangles(%d, %d, %d, 0, %d, %d, %d, 0),' % (o, o + 1, o + 2, o, o + 2, o + 3))
        w('    gsSPEndDisplayList(),')
        w('};')
        w('')
    w('Gfx* const night_rain_sheets[] = { %s };' % ', '.join('night_rain_sheet%d' % i for i in range(RAIN_LAYERS)))
    w('const u8 night_rain_speed[] = { %s };' % ', '.join(str(s) for s in speeds))
    w('')
    w('// The rain is drawn after the solid world, among the see-through things: tested against the')
    w('// depth of what is already there, writing none of its own. The states the game expects at this')
    w('// point (lighting and back-face culling on) are put back at the end.')
    w('Gfx night_rain_begin[] = {')
    w('    gsDPPipeSync(),')
    w('    gsDPSetCycleType(G_CYC_1CYCLE),')
    w('    gsSPGeometryMode(G_LIGHTING | G_CULL_BACK | G_FOG | G_TEXTURE_GEN, G_ZBUFFER | G_SHADE),')
    w('    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON),')
    w('    gsDPSetTextureLUT(G_TT_NONE),')
    w('    gsSPDisplayList(night_use_dot),')
    w('    gsDPSetCombineLERP(0, 0, 0, SHADE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, SHADE, TEXEL0, 0, PRIMITIVE, 0),')
    w('    gsDPSetRenderMode(G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2),')
    w('    gsDPSetPrimColor(0, 0, 255, 255, 255, 132),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('Gfx night_rain_end[] = {')
    w('    gsDPPipeSync(),')
    w('    gsSPGeometryMode(0, G_LIGHTING | G_CULL_BACK),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('')
    h.append('')
    h.append('// rain')
    h.append('#define NIGHT_RAIN_SHEETS %d' % RAIN_LAYERS)
    h.append('#define NIGHT_RAIN_HEIGHT %d' % RAIN_HEIGHT)
    h.append('#define NIGHT_RAIN_SLANT_X %.3ff' % RAIN_SLANT[0])
    h.append('#define NIGHT_RAIN_SLANT_Z %.3ff' % RAIN_SLANT[1])
    h.append('extern Gfx* const night_rain_sheets[];')
    h.append('extern const u8 night_rain_speed[];')
    h.append('extern Gfx night_rain_begin[];')
    h.append('extern Gfx night_rain_end[];')
    make_storm(w, h)
    make_snow(w, h)


SNOW_LAYERS = 6         # sheets of flakes
SNOW_FLAKES = 130       # flakes in a sheet
SNOW_HEIGHT = 1000      # a sheet's height


FIREFLY_GROUPS = 4
FIREFLY_DOTS = 16


def make_glow(w, h, built):
    """The additive glow state (the rain's state with an adding blend, which the moon's glint on the
    water draws with too), the River's fireflies and the shooting star's streak."""
    w('// An adding blend: what is drawn brightens what is behind it, by its own alpha, and hides nothing;')
    w("// the depth test keeps it behind the land. (The gbi's G_RM_ADD blends by the fog's alpha, not the")
    w('// pixel\'s, and added nothing here.)')
    w('#define RM_GLOW(clk) (Z_CMP | IM_RD | CVG_DST_FULL | ZMODE_XLU | FORCE_BL | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1))')
    w('Gfx night_glow_begin[] = {')
    w('    gsSPDisplayList(night_rain_begin),')
    w('    gsDPSetRenderMode(RM_GLOW(1), RM_GLOW(2)),')
    w('    gsDPSetPrimColor(0, 0, 255, 255, 255, 255),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('')
    # Fireflies: small dots around the camera, in a ring from 120 to 700 units, from a little below the
    # eye to well above it; each group is drawn with its own prim color, which the mod pulses.
    rng = random.Random(4242)
    for g in range(FIREFLY_GROUPS):
        verts = []
        for i in range(FIREFLY_DOTS):
            a = rng.uniform(0, 2 * math.pi)
            r = math.sqrt(rng.uniform(120.0 ** 2, 700.0 ** 2))
            x, z = r * math.cos(a), r * math.sin(a)
            y = rng.uniform(-60.0, 140.0)
            half = rng.uniform(3.2, 5.0)
            col = (210, 255, 130)
            for ax, az in ((1.0, 0.0), (0.0, 1.0)):
                for sgn, dy, (s_, t) in ((-1, -1, (0, 512)), (1, -1, (512, 512)), (1, 1, (512, 0)), (-1, 1, (0, 0))):
                    verts.append('    V(%d, %d, %d, %d, %d, %d, %d, %d),' % (
                        int(round(x + sgn * half * ax)), int(round(y + dy * half)), int(round(z + sgn * half * az)), s_, t, col[0], col[1], col[2]))
        w('static Vtx night_firefly_vtx%d[] = {' % g)
        for v in verts:
            w(v)
        w('};')
        w('static Gfx night_firefly_group%d[] = {' % g)
        for first in range(0, len(verts), 32):
            cnt = min(32, len(verts) - first)
            w('    gsSPVertex(&night_firefly_vtx%d[%d], %d, 0),' % (g, first, cnt))
            for o in range(0, cnt, 4):
                w('    gsSP2Triangles(%d, %d, %d, 0, %d, %d, %d, 0),' % (o, o + 1, o + 2, o, o + 2, o + 3))
        w('    gsSPEndDisplayList(),')
        w('};')
        w('')
    w('Gfx* const night_firefly_groups[] = { %s };' % ', '.join('night_firefly_group%d' % g for g in range(FIREFLY_GROUPS)))
    w('')
    h.append('')
    h.append('// the moon path and the fireflies')
    h.append('#define NIGHT_FIREFLY_GROUPS %d' % FIREFLY_GROUPS)
    h.append('extern Gfx night_glow_begin[];')
    h.append('extern Gfx* const night_firefly_groups[];')
    # A shooting star: a streak along +x, its head (+x) white and its tail dark, the soft dot
    # stretched along it. About six degrees of sky at DIST; the mod turns it onto the sky with a
    # matrix (place_frame), moves it along a great circle and fades it by the primitive alpha.
    streak_len, streak_half = 150.0, 6.0
    w('static Vtx night_streak_vtx[] = {')
    for (x, y), (s_, t), col in (((-streak_len / 2, -streak_half), (0, 512), (50, 60, 100)),
                                  ((streak_len / 2, -streak_half), (512, 512), (255, 255, 255)),
                                  ((streak_len / 2, streak_half), (512, 0), (255, 255, 255)),
                                  ((-streak_len / 2, streak_half), (0, 0), (50, 60, 100))):
        w('    V(%d, %d, 0, %d, %d, %d, %d, %d),' % (int(round(x)), int(round(y)), s_, t, col[0], col[1], col[2]))
    w('};')
    w('Gfx night_streak[] = {')
    w('    gsSPDisplayList(night_sprites_begin),')
    w('    gsSPDisplayList(night_use_dot),')
    w('    gsSPVertex(night_streak_vtx, 4, 0),')
    w('    gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),')
    w('    gsSPDisplayList(night_sprites_end),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('')
    h.append('extern Gfx night_streak[];')


def make_snow(w, h):
    """Snow: sheets of soft flakes, made and moved as the rain is, but slow, and each sheet swaying
    from side to side as it falls. A flake is two crossed upright quads of the soft dot."""
    rng = random.Random(1225)
    speeds = []
    for layer in range(SNOW_LAYERS):
        speeds.append(15 + (layer * 5) % 12)        # eighths of a unit a tick: about 1.1 to 2 m a second
        verts = []
        order = list(range(SNOW_FLAKES))
        rng.shuffle(order)
        for i in range(SNOW_FLAKES):
            a = rng.uniform(0, 2 * math.pi)
            r = math.sqrt(rng.uniform(160.0 ** 2, 900.0 ** 2))
            x, z = r * math.cos(a), r * math.sin(a)
            y = (order[i] + rng.uniform(0.1, 0.9)) * SNOW_HEIGHT / SNOW_FLAKES
            half = rng.uniform(3.0, 5.6)
            tone = rng.uniform(0.88, 1.0)
            col = (int(236 * tone), int(242 * tone), int(255 * tone))
            for ax, az in ((1.0, 0.0), (0.0, 1.0)):
                for sgn, dy, (s, t) in ((-1, -1, (0, 512)), (1, -1, (512, 512)), (1, 1, (512, 0)), (-1, 1, (0, 0))):
                    verts.append('    V(%d, %d, %d, %d, %d, %d, %d, %d),' % (
                        int(round(x + sgn * half * ax)), int(round(y + dy * half)), int(round(z + sgn * half * az)), s, t, col[0], col[1], col[2]))
        w('static Vtx night_snow_vtx%d[] = {' % layer)
        for v in verts:
            w(v)
        w('};')
        w('static Gfx night_snow_sheet%d[] = {' % layer)
        for first in range(0, len(verts), 32):
            n = min(32, len(verts) - first)
            w('    gsSPVertex(&night_snow_vtx%d[%d], %d, 0),' % (layer, first, n))
            for o in range(0, n, 4):
                w('    gsSP2Triangles(%d, %d, %d, 0, %d, %d, %d, 0),' % (o, o + 1, o + 2, o, o + 2, o + 3))
        w('    gsSPEndDisplayList(),')
        w('};')
        w('')
    w('Gfx* const night_snow_sheets[] = { %s };' % ', '.join('night_snow_sheet%d' % i for i in range(SNOW_LAYERS)))
    w('const u8 night_snow_speed[] = { %s };' % ', '.join(str(s) for s in speeds))
    w('// The flakes are brighter and more solid than the rain; the other states are those of the rain.')
    w('Gfx night_snow_alpha[] = {')
    w('    gsDPSetPrimColor(0, 0, 255, 255, 255, 215),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('')
    h.append('')
    h.append('// snow')
    h.append('#define NIGHT_SNOW_SHEETS %d' % SNOW_LAYERS)
    h.append('#define NIGHT_SNOW_HEIGHT %d' % SNOW_HEIGHT)
    h.append('extern Gfx* const night_snow_sheets[];')
    h.append('extern const u8 night_snow_speed[];')
    h.append('extern Gfx night_snow_alpha[];')


def make_storm(w, h):
    """Lightning: a box of light around the camera for the flash, and a few jagged bolts. A bolt is a
    chain of upright quads standing off to one side (the code turns it to where it strikes), drawn
    with the soft dot's middle row so that it is bright along its spine and fades to both sides;
    each has a wide faint copy for its glow."""
    rng = random.Random(1752)
    r = 1200
    w('static Vtx night_flash_vtx[] = {')
    for (a, b, c, d) in (((-r, -r, r), (r, -r, r), (r, r, r), (-r, r, r)), ((r, -r, -r), (-r, -r, -r), (-r, r, -r), (r, r, -r)),
                         ((r, -r, r), (r, -r, -r), (r, r, -r), (r, r, r)), ((-r, -r, -r), (-r, -r, r), (-r, r, r), (-r, r, -r)),
                         ((-r, r, r), (r, r, r), (r, r, -r), (-r, r, -r))):
        for p in (a, b, c, d):
            w('    V(%d, %d, %d, 0, 0, 255, 255, 255),' % p)
    w('};')
    w('// The flash: plain light over the whole sky, as strong as the primitive alpha the code sets.')
    w('Gfx night_flash_dome[] = {')
    w('    gsDPPipeSync(),')
    w('    gsSPGeometryMode(G_LIGHTING | G_FOG | G_CULL_BACK, 0),')
    w('    gsSPTexture(0, 0, 0, G_TX_RENDERTILE, G_OFF),')
    w('    gsDPSetCombineMode(G_CC_PRIMITIVE, G_CC_PRIMITIVE),')
    w('    gsDPSetRenderMode(G_RM_XLU_SURF, G_RM_XLU_SURF2),')
    w('    gsSPVertex(night_flash_vtx, 20, 0),')
    for o in range(0, 20, 4):
        w('    gsSP2Triangles(%d, %d, %d, 0, %d, %d, %d, 0),' % (o, o + 1, o + 2, o, o + 2, o + 3))
    w('    gsSPEndDisplayList(),')
    w('};')
    w('// The states for a bolt: the soft dot as its picture, its strength times the primitive alpha.')
    w('Gfx night_bolt_begin[] = {')
    w('    gsDPPipeSync(),')
    w('    gsSPGeometryMode(G_LIGHTING | G_FOG | G_CULL_BACK, 0),')
    w('    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON),')
    w('    gsSPDisplayList(night_use_dot),')
    w('    gsDPSetCombineLERP(0, 0, 0, SHADE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, SHADE, TEXEL0, 0, PRIMITIVE, 0),')
    w('    gsDPSetRenderMode(G_RM_XLU_SURF, G_RM_XLU_SURF2),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('// After a flash or a bolt: the states the drawing of the sky left (lighting, fog and culling on,')
    w('// its opaque render mode).')
    w('Gfx night_storm_end[] = {')
    w('    gsDPPipeSync(),')
    w('    gsDPSetRenderMode(G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2),')
    w('    gsSPGeometryMode(0, G_LIGHTING | G_FOG | G_CULL_BACK),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('')

    def path(start, end, steps, sway):
        pts = [start]
        for i in range(1, steps):
            f = i / float(steps)
            pts.append((start[0] + (end[0] - start[0]) * f + rng.uniform(-sway, sway),
                        start[1] + (end[1] - start[1]) * f + rng.uniform(-10, 10)))
        pts.append(end)
        return pts

    for b in range(BOLTS):
        top = (rng.uniform(-60, 60), rng.uniform(560, 760))
        bottom = (top[0] + rng.uniform(-220, 220), rng.uniform(30, 150))
        trunk = path(top, bottom, rng.randrange(10, 15), 46.0)
        paths = [(trunk, 1.0)]
        for _ in range(rng.randrange(1, 3)):
            at = trunk[rng.randrange(2, len(trunk) - 3)]
            tip = (at[0] + rng.choice((-1, 1)) * rng.uniform(90, 220), at[1] - rng.uniform(110, 260))
            paths.append((path(at, tip, rng.randrange(4, 7), 30.0), 0.55))
        for kind, width, tone in (('glow', 44.0, (150, 170, 255)), ('core', 9.0, (236, 242, 255))):
            verts = []
            for pts, strength in paths:
                wd = width * (0.6 + 0.4 * strength)
                col = tuple(int(c * (0.7 + 0.3 * strength)) for c in tone)
                for (z0, y0), (z1, y1) in zip(pts, pts[1:]):
                    # a quad along the segment, as wide as wd across it; the picture's middle row runs its length
                    dz, dy = z1 - z0, y1 - y0
                    n = math.hypot(dz, dy) or 1.0
                    oz, oy = -dy / n * wd, dz / n * wd
                    for (pz, py), (s, t) in (((z0 - oz, y0 - oy), (0, 256)), ((z0 + oz, y0 + oy), (512, 256)),
                                             ((z1 + oz, y1 + oy), (512, 256)), ((z1 - oz, y1 - oy), (0, 256))):
                        verts.append('    V(%d, %d, %d, %d, %d, %d, %d, %d),' % (int(BOLT_DIST), int(round(py)), int(round(pz)), s, t, col[0], col[1], col[2]))
            w('static Vtx night_bolt%d_%s_vtx[] = {' % (b, kind))
            for v in verts:
                w(v)
            w('};')
            w('static Gfx night_bolt%d_%s[] = {' % (b, kind))
            for first in range(0, len(verts), 32):
                n = min(32, len(verts) - first)
                w('    gsSPVertex(&night_bolt%d_%s_vtx[%d], %d, 0),' % (b, kind, first, n))
                for o in range(0, n, 4):
                    w('    gsSP2Triangles(%d, %d, %d, 0, %d, %d, %d, 0),' % (o, o + 1, o + 2, o, o + 2, o + 3))
            w('    gsSPEndDisplayList(),')
            w('};')
    w('Gfx* const night_bolt_glows[] = { %s };' % ', '.join('night_bolt%d_glow' % b for b in range(BOLTS)))
    w('Gfx* const night_bolt_cores[] = { %s };' % ', '.join('night_bolt%d_core' % b for b in range(BOLTS)))
    w('')
    h.append('')
    h.append('// storm')
    h.append('#define NIGHT_BOLTS %d' % BOLTS)
    h.append('extern Gfx night_flash_dome[];')
    h.append('extern Gfx night_bolt_begin[];')
    h.append('extern Gfx night_storm_end[];')
    h.append('extern Gfx* const night_bolt_glows[];')
    h.append('extern Gfx* const night_bolt_cores[];')


def build_course(name, cfg, rom_path, show_hist):
    rom = Rom(rom_path, cfg['segs'] + list(COMMON.values()))
    sky = read_sky(rom, cfg['setup'])
    clouds = {}
    for pic in sky['pics']:
        raw = rom.data(pic, 64 * 32 * 2)
        reds = [(struct.unpack('>H', raw[2 * i:2 * i + 2])[0] >> 11) & 31 for i in range(64 * 32)]
        clouds[pic] = [max(0.0, min(1.0, (r - cfg['r_lo']) / float(cfg['r_hi'] - cfg['r_lo']))) for r in reds]
        if show_hist:
            hist = {}
            for r in reds:
                hist[r] = hist.get(r, 0) + 1
            print('   %s picture %08X red histogram: %s' % (name, pic, sorted(hist.items())))
            print('      open sky (weight < 0.12): %d%% of texels' % (100 * sum(1 for c in clouds[pic] if c < 0.12) // len(reds)))

    def cloud_at(d, reach):
        h = hit(sky['tris'], d)
        if h is None:
            return 1.0
        pic, cx, cy = h
        worst = 0.0
        for y in range(int(math.floor(cy - reach)), int(math.ceil(cy + reach)) + 1):
            for x in range(int(math.floor(cx - reach)), int(math.ceil(cx + reach)) + 1):
                worst = max(worst, clouds[pic][min(31, max(0, y)) * 64 + min(63, max(0, x))])
        return worst

    # Each picture row's height above the horizon, 0 (horizon) to 255 (straight up).
    rows = {pic: [[] for _ in range(32)] for pic in sky['pics']}
    for a, b, c, pic in sky['tris']:
        for i in range(21):
            for j in range(21 - i):
                u, v = i / 20.0, j / 20.0
                p = [a[0][k] + u * (b[0][k] - a[0][k]) + v * (c[0][k] - a[0][k]) for k in range(3)]
                t = (a[1][1] + u * (b[1][1] - a[1][1]) + v * (c[1][1] - a[1][1])) / 32.0 - 0.5
                el = math.degrees(math.atan2(p[1], math.hypot(p[0], p[2])))
                rows[pic][min(31, max(0, int(round(t))))].append(el)
    row_table = {}
    for pic in sky['pics']:
        vals = [(sum(r) / len(r)) if r else None for r in rows[pic]]
        for i in range(32):
            if vals[i] is None:
                near = [vals[j] for j in sorted(range(32), key=lambda j: abs(j - i)) if vals[j] is not None]
                vals[i] = near[0] if near else 0.0
        row_table[pic] = [max(0, min(255, int(round(255 * max(0.0, v) / 60.0)))) for v in vals]

    rng = random.Random(cfg['seed'])
    moon_dir = None
    if cfg['moon']:
        az0, az1, el0, el1 = cfg['moon']
        best = None
        az = float(az0)
        while az <= az1:
            el = el0
            while el <= el1 + 1e-6:
                worst = max(cloud_at(direction(az + da, el + de), 1.0) for da in (-4, 0, 4) for de in (-3.5, 0, 3.5))
                worst += 0.004 * (el - el0)     # the lower of two equally clear places
                if best is None or worst < best[0] - 1e-6:
                    best = (worst, az, el)
                el += 0.5
            az += 1.0
        moon_dir = direction(best[1], best[2])
        print('   %s: moon at azimuth %.0f, elevation %.1f (cloud near it %.2f)' % (name, best[1], best[2], best[0]))

    def try_star(reach, others, apart):
        """A place in open sky, clear of the moon and no nearer than `apart` degrees to its like."""
        limit = math.cos(math.radians(apart))
        for _ in range(40):
            y = rng.uniform(math.sin(math.radians(2.5)), 1.0)
            az = rng.uniform(0.0, 2 * math.pi)
            c = math.sqrt(1 - y * y)
            d = (c * math.cos(az), y, c * math.sin(az))
            if moon_dir and angle_between(d, moon_dir) < 7.5:
                continue
            if any(d[0] * o[0][0] + d[1] * o[0][1] + d[2] * o[0][2] > limit for o in others):
                continue
            if cloud_at(d, reach) > 0.12:
                continue
            return d
        return None

    n_dust, n_mid, n_bright = cfg['stars']
    # The faint stars are a soft blue-gray, a few leaning warm. The game's own
    # night sky (the Rainbow Cloud's) tints its stars in pastels -- pink,
    # orange, violet, cyan, light blue -- so a third of the middle stars and
    # every bright one take a pastel of that set, lightened.
    tones = [(150, 170, 205)] * 6 + [(168, 184, 214), (196, 196, 186)]
    pastels = [(255, 214, 206), (255, 230, 200), (216, 206, 255), (204, 242, 255), (210, 226, 255), (255, 255, 255)]
    dust, mid, bright = [], [], []
    for i in range(n_dust):
        d = try_star(0.6, dust, 1.5)
        if d:
            k = rng.uniform(0.62, 0.95)
            dust.append((d, rng.uniform(7.5, 10.5), tuple(int(c * k) for c in tones[rng.randrange(len(tones))])))
    for i in range(n_mid):
        d = try_star(0.9, mid, 3.0)
        if d:
            k = rng.uniform(0.95, 1.22)
            tone = pastels[rng.randrange(len(pastels))] if rng.random() < 0.35 else tones[rng.randrange(len(tones))]
            mid.append((d, rng.uniform(12.5, 18.0), tuple(min(255, int(c * k)) for c in tone)))
    tries = 0
    while len(bright) < n_bright and tries < 400:
        tries += 1
        d = try_star(1.8, bright, 24.0)
        if d and d[1] > math.sin(math.radians(8.0)):
            bright.append((d, rng.uniform(26.0, 36.0), pastels[rng.randrange(len(pastels))]))
    print('   %s: %d faint, %d middle, %d bright stars; sky list %08X, spare %08X, pictures %s' % (
        name, len(dust), len(mid), len(bright), sky['list'], sky['spare'] or 0, ['%08X' % p for p in sky['pics']]))
    return dict(sky=sky, rows=row_table, dust=dust, mid=mid, bright=bright, moon=moon_dir, rng=rng)


def main():
    rom_path = sys.argv[1]
    show_hist = '--hist' in sys.argv
    dot, glint, moon_pic, sine = make_pictures()
    built = {}
    for name, cfg in COURSES.items():
        built[name] = build_course(name, cfg, rom_path, show_hist)
    if show_hist:
        return

    c = []
    w = c.append
    w('// Written by tools/gen_night.py; do not edit by hand. Island at Night: each course\'s stars and')
    w('// moon, the mod\'s three small pictures, and what the code needs to know of each sky.')
    w('')
    w('#include "common.h"')
    w('#include "night_data.h"')
    w('')
    w('#define V(x, y, z, s, t, r, g, b) { { { (x), (y), (z) }, 0, { (s), (t) }, { (r), (g), (b), 255 } } }')
    w('')

    def bytes_array(name, kind, values, per_line, fmt):
        w('%s %s[] __attribute__((aligned(8))) = {' % (kind, name))
        for i in range(0, len(values), per_line):
            w('    ' + ' '.join(fmt % v + ',' for v in values[i:i + per_line]))
        w('};')
        w('')

    bytes_array('night_dot_pic', 'static u8', dot, 16, '%3d')
    bytes_array('night_glint_pic', 'static u8', glint, 32, '%3d')
    bytes_array('night_moon_pic', 'static u16', moon_pic, 16, '0x%04X')
    bytes_array('night_sine', 'const s8', sine, 16, '%4d')

    twinkles = make_twinkles()
    w('// Each twinkle group: base, swing, swell; the pace of its two waves and of its swell (parts of')
    w('// 65536 a tick); where each starts.')
    w('const NightTwinkle night_twinkles[] = {')
    for t in twinkles:
        w('    { %d, %d, %d, 0, %d, %d, %d, %d, %d, %d },' % t)
    w('};')
    w('')
    w('// Set-up shared by every sky\'s list: the sprite states and the three pictures.')
    w('static Gfx night_sprites_begin[] = {')
    w('    gsDPPipeSync(),')
    w('    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON),')
    w('    gsDPSetCombineLERP(0, 0, 0, SHADE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, SHADE, TEXEL0, 0, PRIMITIVE, 0),')
    w('    gsDPSetRenderMode(G_RM_XLU_SURF, G_RM_XLU_SURF2),')
    w('    gsSPClearGeometryMode(G_CULL_BACK),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('static Gfx night_use_dot[] = {')
    w('    gsDPLoadTextureBlock(night_dot_pic, G_IM_FMT_I, G_IM_SIZ_8b, 16, 16, 0, G_TX_CLAMP, G_TX_CLAMP, 4, 4, G_TX_NOLOD, G_TX_NOLOD),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('static Gfx night_use_glint[] = {')
    w('    gsDPLoadTextureBlock(night_glint_pic, G_IM_FMT_I, G_IM_SIZ_8b, 32, 32, 0, G_TX_CLAMP, G_TX_CLAMP, 5, 5, G_TX_NOLOD, G_TX_NOLOD),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('static Gfx night_use_moon[] = {')
    w('    gsDPLoadTextureBlock(night_moon_pic, G_IM_FMT_IA, G_IM_SIZ_16b, 32, 32, 0, G_TX_CLAMP, G_TX_CLAMP, 5, 5, G_TX_NOLOD, G_TX_NOLOD),')
    w('    gsDPSetCombineLERP(TEXEL0, 0, SHADE, 0, TEXEL0, 0, PRIMITIVE, 0, TEXEL0, 0, SHADE, 0, TEXEL0, 0, PRIMITIVE, 0),')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('// The sky\'s own states, put back at the end: its combiner (picture times shade), its texture')
    w('// switch and its render mode.')
    w('static Gfx night_sprites_end[] = {')
    w('    gsDPPipeSync(),')
    w('    gsDPSetRenderMode(G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2),')
    w('    { .words = { 0xFC121824, 0xFF33FFFF } },')
    w('    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON),')
    w('    gsSPSetGeometryMode(G_CULL_BACK),    // back on, as the lists of the game leave it')
    w('    gsSPEndDisplayList(),')
    w('};')
    w('')

    h = []
    h.append('// Written by tools/gen_night.py; do not edit by hand.')
    h.append('#ifndef NIGHT_DATA_H')
    h.append('#define NIGHT_DATA_H')
    h.append('')
    h.append('#include "common.h"')
    h.append('')
    h.append('#define NIGHT_GROUPS_MAX %d' % (FAINT_GROUPS + MID_GROUPS + BRIGHT_MAX))
    h.append('typedef struct NightTwinkle {')
    h.append('    u8 base, swing, swell, pad;')
    h.append('    u16 rate1, rate2, rate3;')
    h.append('    u16 phase1, phase2, phase3;')
    h.append('} NightTwinkle;')
    h.append('extern const s8 night_sine[];')
    h.append('extern const NightTwinkle night_twinkles[];')
    make_rain(w, h)
    make_glow(w, h, built)

    def quad(d, half, size_px, col, spin=0.0):
        right, up = basis(d)
        cx = [DIST * d[i] for i in range(3)]
        out = []
        st = size_px * 32
        for (dx, dy), (s, t) in zip(((-1, 1), (1, 1), (1, -1), (-1, -1)), ((0, 0), (st, 0), (st, st), (0, st))):
            rx = dx * math.cos(spin) - dy * math.sin(spin)
            ry = dx * math.sin(spin) + dy * math.cos(spin)
            p = tuple(int(round(cx[i] + half * (rx * right[i] + ry * up[i]))) for i in range(3))
            out.append('    V(%d, %d, %d, %d, %d, %d, %d, %d),' % (p + (s, t) + tuple(col)))
        return out

    for name, b in built.items():
        cfg = COURSES[name]
        rng = b['rng']
        verts = []
        groups = []         # (first quad, quad count, kind)
        first = 0
        buckets = [[] for _ in range(FAINT_GROUPS + MID_GROUPS)]
        for s in b['dust']:
            buckets[rng.randrange(FAINT_GROUPS)].append(s)
        for s in b['mid']:
            # Low stars are seen through more air: more of them breathe or shimmer. Overhead, calm.
            el = math.degrees(math.asin(max(-1.0, min(1.0, s[0][1]))))
            shimmer, breathing = (0.20, 0.42) if el < 15 else ((0.11, 0.30) if el < 35 else (0.05, 0.20))
            roll = rng.random()
            if roll < shimmer:
                g = CALM_GROUPS + BREATHING_GROUPS + rng.randrange(SHIMMER_GROUPS)
            elif roll < shimmer + breathing:
                g = CALM_GROUPS + rng.randrange(BREATHING_GROUPS)
            else:
                g = rng.randrange(CALM_GROUPS)
            buckets[FAINT_GROUPS + g].append(s)
        for bucket in buckets:
            for d, half, col in bucket:
                verts += quad(d, half, 16, col)
            groups.append((first, len(bucket), 'star'))
            first += len(bucket)
        for d, half, col in b['bright']:        # each bright star's steady heart
            verts += quad(d, half * 0.5, 16, col)
        groups.append((first, len(b['bright']), 'heart'))
        first += len(b['bright'])
        for d, half, col in b['bright']:        # and its rays
            verts += quad(d, half, 32, col, rng.uniform(-0.25, 0.25))
            groups.append((first, 1, 'rays'))
            first += 1
        moon_first = first
        if b['moon']:
            verts += quad(b['moon'], MOON_HALF * 7.0, 16, (150, 172, 235))      # the glow
            verts += quad(b['moon'], MOON_HALF * 1.1, 32, (255, 252, 238))      # the moon itself
        # The sun, for the sunset: low on the moon's side of the sky (where the course's open view
        # is), a wide warm haze and a disc.
        sun_first = moon_first + (2 if b['moon'] else 0)
        sun_az = math.degrees(math.atan2(b['moon'][2], b['moon'][0])) if b['moon'] else 200.0
        sun_dir = direction(sun_az, 6.0)
        b['sun'] = sun_dir
        verts += quad(sun_dir, MOON_HALF * 9.0, 16, (255, 150, 70))      # the haze around it
        verts += quad(sun_dir, MOON_HALF * 2.2, 16, (255, 236, 200))     # the disc
        # The dawn's sun, across the sky from the sunset's: a cooler haze and a paler disc.
        dawn_first = sun_first + 2
        dawn_dir = direction(sun_az + 180.0, 6.0)
        b['dawn'] = dawn_dir
        verts += quad(dawn_dir, MOON_HALF * 9.0, 16, (255, 190, 150))
        verts += quad(dawn_dir, MOON_HALF * 2.2, 16, (255, 244, 230))
        w('static Vtx night_%s_vtx[] = {' % name)
        c.extend(verts)
        w('};')
        w('')
        n_tw = FAINT_GROUPS + MID_GROUPS + len(b['bright'])
        w('// One command each, rewritten every frame: the brightness of a twinkle group.')
        w('Gfx night_%s_twinkle[][2] = {' % name)
        for i in range(n_tw):
            w('    { gsDPSetPrimColor(0, 0, 255, 255, 255, 200), gsSPEndDisplayList() },')
        w('};')
        w('')

        def draw(first_quad, count):
            q = first_quad
            end = first_quad + count
            while q < end:
                n = min(8, end - q)
                w('    gsSPVertex(&night_%s_vtx[%d], %d, 0),' % (name, 4 * q, 4 * n))
                for i in range(n):
                    o = 4 * i
                    w('    gsSP2Triangles(%d, %d, %d, 0, %d, %d, %d, 0),' % (o, o + 1, o + 2, o, o + 2, o + 3))
                q += n

        w('Gfx night_%s_sky[] = {' % name)
        w('    gsSPDisplayList(night_sprites_begin),')
        w('    gsSPDisplayList(night_use_dot),')
        tw = 0
        glint_on = False
        for first_quad, count, kind in groups:
            if kind == 'heart':
                w('    gsDPSetPrimColor(0, 0, 255, 255, 255, 235),')
                draw(first_quad, count)
                continue
            if kind == 'rays' and not glint_on:
                w('    gsSPDisplayList(night_use_glint),')
                glint_on = True
            w('    gsSPDisplayList(night_%s_twinkle[%d]),' % (name, tw))
            tw += 1
            draw(first_quad, count)
        if b['moon']:
            w('    gsSPDisplayList(night_use_dot),')
            w('    gsDPSetPrimColor(0, 0, 255, 255, 255, 112),')
            draw(moon_first, 1)
            w('    gsSPDisplayList(night_use_moon),')
            w('    gsDPSetPrimColor(0, 0, 255, 255, 255, 255),')
            draw(moon_first + 1, 1)
        w('    gsSPDisplayList(night_sprites_end),')
        w('    gsSPEndDisplayList(),')
        w('};')
        w('')
        w('// The sun, attached to the sky at sunset in place of the stars.')
        w('Gfx night_%s_sun[] = {' % name)
        w('    gsSPDisplayList(night_sprites_begin),')
        w('    gsSPDisplayList(night_use_dot),')
        w('    gsDPSetPrimColor(0, 0, 255, 255, 255, 112),')
        draw(sun_first, 1)
        w('    gsDPSetPrimColor(0, 0, 255, 255, 255, 255),')
        draw(sun_first + 1, 1)
        w('    gsSPDisplayList(night_sprites_end),')
        w('    gsSPEndDisplayList(),')
        w('};')
        w('')
        w('// The dawn\'s sun, attached to the sky at dawn.')
        w('Gfx night_%s_dawn[] = {' % name)
        w('    gsSPDisplayList(night_sprites_begin),')
        w('    gsSPDisplayList(night_use_dot),')
        w('    gsDPSetPrimColor(0, 0, 255, 255, 255, 100),')
        draw(dawn_first, 1)
        w('    gsDPSetPrimColor(0, 0, 255, 255, 255, 255),')
        draw(dawn_first + 1, 1)
        w('    gsSPDisplayList(night_sprites_end),')
        w('    gsSPEndDisplayList(),')
        w('};')
        w('')
        pics = b['sky']['pics']
        for i, pic in enumerate(pics):
            w('const u8 night_%s_rows%d[32] = { %s };' % (name, i, ', '.join(str(v) for v in b['rows'][pic])))
        w('')
        up = name.upper()
        h.append('')
        h.append('// %s' % name)
        h.append('#define NIGHT_%s_SETUP 0x%08Xu' % (up, cfg['setup']))
        h.append('#define NIGHT_%s_SPARE 0x%08Xu' % (up, b['sky']['spare']))
        # The key light's direction: toward the moon, or straight up where there is none.
        key = b['moon'] if b['moon'] else (0.0, 1.0, 0.0)
        h.append('#define NIGHT_%s_KEY { %d, %d, %d }' % (up, int(round(key[0] * 127)), int(round(key[1] * 127)), int(round(key[2] * 127))))
        sun = b['sun']
        h.append('#define NIGHT_%s_SUN { %d, %d, %d }' % (up, int(round(sun[0] * 127)), int(round(sun[1] * 127)), int(round(sun[2] * 127))))
        h.append('extern Gfx night_%s_sun[];' % name)
        dawn = b['dawn']
        h.append('#define NIGHT_%s_DAWN { %d, %d, %d }' % (up, int(round(dawn[0] * 127)), int(round(dawn[1] * 127)), int(round(dawn[2] * 127))))
        h.append('extern Gfx night_%s_dawn[];' % name)
        h.append('#define NIGHT_%s_PICS { %s }' % (up, ', '.join('0x%08Xu' % p for p in (pics + [0, 0])[:2])))
        h.append('#define NIGHT_%s_ROWS { %s }' % (up, ', '.join(['night_%s_rows%d' % (name, i) for i in range(len(pics))] + ['0'] * (2 - len(pics)))))
        arr = b['sky']['arrays']
        h.append('#define NIGHT_%s_SKY_VTX { %s }' % (up, ', '.join('0x%08Xu' % a for a, n in (arr + [(0, 0)] * 2)[:2])))
        h.append('#define NIGHT_%s_SKY_VTX_N { %s }' % (up, ', '.join(str(n) for a, n in (arr + [(0, 0)] * 2)[:2])))
        h.append('#define NIGHT_%s_R_LO %d' % (up, cfg['r_lo']))
        h.append('#define NIGHT_%s_R_SCALE %d' % (up, int(round(256.0 / (cfg['r_hi'] - cfg['r_lo'])))))
        h.append('#define NIGHT_%s_TWINKLES %d' % (up, n_tw))
        h.append('extern Gfx night_%s_sky[];' % name)
        h.append('extern Gfx night_%s_twinkle[][2];' % name)
        for i in range(len(pics)):
            h.append('extern const u8 night_%s_rows%d[32];' % (name, i))
    h.append('')
    h.append('#endif')
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
    os.makedirs(src, exist_ok=True)
    open(os.path.join(src, 'night_data.c'), 'w', encoding='utf-8', newline='\n').write('\n'.join(c) + '\n')
    open(os.path.join(src, 'night_data.h'), 'w', encoding='utf-8', newline='\n').write('\n'.join(h) + '\n')
    print('wrote night_data.c (%d lines) and night_data.h' % len(c))


if __name__ == '__main__':
    main()
