# Generic models of the KHSV landmarks (milestone 4 steps 4 and 5, D-025):
# the right footprint, height and colour, and generic detail (window bands,
# doors, corrugated and ribbed metal, roof membranes, rooftop plant, arched
# hangar roofs, a faceted tower cab), not any building's own. Footprints
# were read off the NAIP 2023 imagery (to a few metres); roof colours are
# the imagery's over each footprint (tools/measure-roof-colours.py); heights
# are estimates, as the imagery doesn't give them. Fovea's own work, under
# the repository's licence. Builds each model and exports it as a .glb, run
# by Blender (5.2):
#
#   blender --background --factory-startup --python data/models/khsv_landmarks.py -- <out dir>
#
# Each model's ground line is at height 0 (glTF's y = 0), its front (glTF's
# +Z, Blender's -Y) along its depth; width is across it. Every wall reaches
# PLINTH metres below the ground line, so a model placed at the lowest
# ground under its footprint (the compiler's rule, D-026) is never left
# floating. Textures are made here (numpy), sized in metres on the model
# (texture coordinates from positions), so windows are the same size on
# every building.
#
# Each material names its Fovea material ID (a custom property, exported in
# the glTF's extras): 1 walls and roofs, 2 metal, 3 glass.

import math
import os
import random
import sys
import zlib

import bmesh
import bpy
import numpy as np

PLINTH = 3.0
PX = 32  # texels a metre


# --- Textures (sRGB, row 0 at the bottom, as Blender keeps images) --------

def srgb(c):
    return np.array(c[:3], dtype=np.float32) / 255.0


def blank(w_m, h_m, colour, px=PX):
    img = np.ones((int(h_m * px), int(w_m * px), 4), dtype=np.float32)
    img[:, :, :3] = srgb(colour)
    return img


def shade(c, f):
    return tuple(min(255.0, max(0.0, v * f)) for v in c)


def office_facade(wall, glass_from=1.0, glass_to=2.6, bay=1.5, seed=0):
    """4 m floors, 24 m before it repeats: a glass band each floor, its
    panes `bay` metres wide behind thin frames that barely show, each pane
    lighter up its height (the sky in it) and a little lighter or darker
    than its neighbours (blinds drawn or not), so no grid shows."""
    rng = np.random.default_rng(seed)
    img = blank(24.0, 4.0, wall)
    a, b = int(glass_from * PX), int(glass_to * PX)
    glass = srgb((112, 130, 146))
    sky = np.linspace(0.86, 1.02, b - a)[:, None, None]
    panes = int(round(24.0 / bay))
    edges = np.linspace(0, img.shape[1], panes + 1).astype(int)
    for k in range(panes):
        x0, x1 = edges[k], edges[k + 1]
        tint = rng.choice([0.97, 0.99, 1.0, 1.0, 1.0, 1.01, 1.05])  # now and then blinds down
        img[a:b, x0:x1, :3] = np.minimum(glass * sky * tint, 1.0)
        img[a:b, x0:x0 + 1, :3] *= 0.86  # the frame, faint
    img[a:a + 1, :, :3] *= 0.86
    img[b - 1:b, :, :3] *= 0.86
    img[0:2, :, :3] = srgb(shade(wall, 0.9))  # the floor slab's line, faint
    return img


def corrugated(wall, w_m=4.0, h_m=4.0, depth=0.14):
    """Vertical ribs every 0.25 m, `depth` their shading."""
    img = blank(w_m, h_m, wall)
    x = np.arange(img.shape[1])
    img[:, :, :3] *= (1.0 - depth + depth * (0.5 + 0.5 * np.cos(2 * np.pi * x / (0.25 * PX))))[
        None, :, None]
    return img


def metal_wall(wall, eave, height, bay, feature):
    """A metal-clad wall, one bay wide and `height` tall, the wall's full
    height, gables included (texture coordinates run from the ground up, so
    it doesn't repeat upwards): ribs, a darker band along the base, a trim
    under the eaves, and per bay either a small window ("window") or a
    loading-dock door ("dock")."""
    img = corrugated(wall, bay, height, depth=0.2)
    img[0:int(1.2 * PX), :, :3] *= 0.72  # base band
    t0 = int((eave - 0.5) * PX)
    img[t0:int(eave * PX), :, :3] = srgb(shade(wall, 0.6))  # eave trim
    mid = img.shape[1] // 2
    if feature == "window":
        y0, y1, half = int(1.8 * PX), int(2.8 * PX), int(0.7 * PX)
        img[y0:y1, mid - half:mid + half, :3] = srgb((96, 112, 124))
    elif feature == "dock":
        y0, y1, half = int(1.2 * PX), int(4.2 * PX), int(1.5 * PX)
        img[y0:y1, mid - half:mid + half, :3] = srgb(shade(wall, 0.62))
        for y in range(y0, y1, int(0.5 * PX)):
            img[y:y + 1, mid - half:mid + half, :3] *= 0.85
    return img


def hangar_door(wall, height, seed=0):
    """Hangar doors, 16 m of them before they repeat and the door's full
    height (so nothing repeats upwards): leaves 4 m wide of fine vertical
    ribbing, thin seams between them, each leaf a shade apart (doors are
    repainted and weather leaf by leaf), a dark track along the bottom, a
    header along the top, and a personnel door in one leaf."""
    rng = np.random.default_rng(seed)
    img = corrugated(shade(wall, 0.95), 16.0, height, depth=0.1)
    leaf = 4 * PX
    for x0 in range(0, img.shape[1], leaf):
        img[:, x0:x0 + leaf, :3] *= rng.uniform(0.95, 1.04)
        img[:, x0:x0 + 2, :3] *= 0.75  # the seam
    img[0:int(0.25 * PX), :, :3] *= 0.55  # the track
    top = int((height - 0.8) * PX)
    img[top:top + int(0.5 * PX), :, :3] = srgb(shade(wall, 0.75))  # the header
    x0 = int(9.5 * PX)  # a personnel door
    img[0:int(2.1 * PX), x0:x0 + int(0.9 * PX), :3] = srgb(shade(wall, 0.7))
    return img


def membrane(roof, seed):
    """Roof membrane in 2 m strips, a little uneven (16 texels a metre: a
    roof is seen from tens of metres at least)."""
    rng = np.random.default_rng(seed)
    img = blank(8.0, 8.0, roof, px=16)
    # Unevenness in steps of 1%: smooth enough, and small as a PNG.
    img[:, :, :3] *= (0.96 + 0.01 * rng.integers(0, 5, img.shape[:2]))[:, :, None]
    for x in range(0, img.shape[1], 2 * 16):
        img[:, x:x + 1, :3] *= 0.85
    return img


def standing_seam(roof):
    """Metal roofing, seams every 0.6 m."""
    img = blank(4.8, 4.8, roof)
    for x in range(0, img.shape[1], int(0.6 * PX)):
        img[:, x:x + 2, :3] *= 0.8
        img[:, x + 2:x + 3, :3] = np.minimum(img[:, x + 2:x + 3, :3] * 1.1, 1.0)
    return img


def cab_glass():
    """A tower cab facet: tinted glass, a frame round it."""
    img = blank(2.0, 2.0, (70, 90, 104))
    img[:, :4, :3] = srgb((180, 182, 184))
    img[:4, :, :3] = srgb((180, 182, 184))
    return img


def ac_top(casing):
    """An air-conditioning unit's top, seen once edge to edge: two fans in
    dark grilles, a rim of casing round them."""
    img = blank(3.0, 2.2, casing)
    h, w = img.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    r = 0.42 * h
    for cxp in (w * 0.27, w * 0.73):
        d = np.hypot(xx - cxp, yy - h / 2)
        fan = d < r
        img[fan, :3] = srgb((58, 60, 62))
        ring = fan & (np.abs(d - r * 0.6) < 1.2)
        img[ring, :3] = srgb((96, 98, 100))
        img[d < r * 0.18, :3] = srgb((84, 86, 88))  # the hub
    return img


def louvres(casing):
    """An air-conditioning unit's sides: louvres every 0.1 m."""
    img = blank(2.0, 1.4, casing)
    for y in range(0, img.shape[0], int(0.1 * PX)):
        img[y:y + 1, :, :3] *= 0.72
    return img


def plain(colour):
    return blank(1.0, 1.0, colour)


# --- Materials -------------------------------------------------------------

def material(name, texels, fovea, size_m):
    """A material showing `texels` (size_m[0] x size_m[1] metres)."""
    m = bpy.data.materials.new(name)
    try:
        m.use_nodes = True  # always so from Blender 5.0
    except AttributeError:
        pass
    img = bpy.data.images.new(name, texels.shape[1], texels.shape[0], alpha=False)
    img.pixels.foreach_set(np.clip(texels, 0.0, 1.0).ravel())
    img.pack()
    nodes = m.node_tree.nodes
    bsdf = nodes["Principled BSDF"]
    tex = nodes.new("ShaderNodeTexImage")
    tex.image = img
    m.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.85
    m["fovea_material"] = fovea
    m["size_m"] = size_m
    return m


# --- Geometry --------------------------------------------------------------

class Model:
    """Faces by material; texture coordinates from positions, each face
    projected on its own plane (walls: along and up; roofs: from above)."""

    def __init__(self, name):
        self.name = name
        self.faces = {}  # material: [(points, projection, texture coordinates)]

    def face(self, mat, points, projection=None, uvs=None):
        """A face; its texture coordinates from its positions, unless given."""
        self.faces.setdefault(mat, []).append((points, projection, uvs))

    def quad_strip(self, mat, ring_lo, ring_hi, closed=True):
        """Walls between two rings of points (anticlockwise from above)."""
        n = len(ring_lo)
        for i in range(n if closed else n - 1):
            j = (i + 1) % n
            self.face(mat, [ring_lo[i], ring_lo[j], ring_hi[j], ring_hi[i]])

    def box(self, wall, roof, cx, cy, w, d, z0, z1, front=None):
        """A box from z0 to z1 (z0 <= 0 reaches the plinth); `front`, if
        given, is the material of its front face (-Y)."""
        z0 = -PLINTH if z0 <= 0.0 else z0
        x0, x1, y0, y1 = cx - w / 2, cx + w / 2, cy - d / 2, cy + d / 2
        lo = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)]
        hi = [(x, y, z1) for x, y, _ in lo]
        for i in range(4):
            j = (i + 1) % 4
            self.face(front if (front and i == 0) else wall, [lo[i], lo[j], hi[j], hi[i]])
        self.face(roof, hi, "top")
        self.face(roof, list(reversed(lo)), "top")

    def prism(self, wall, roof, cx, cy, r0, r1, z0, z1, sides, top=True, bottom=False,
              one_image_a_facet=False):
        """An upright prism (cylinder or frustum): radius r0 at z0, r1 at z1;
        with `one_image_a_facet`, each side shows its texture once, edge to
        edge (a tower cab's framed panes)."""
        z0 = -PLINTH if z0 <= 0.0 else z0
        ring = lambda r, z: [(cx + r * math.cos(2 * math.pi * k / sides + math.pi / sides),
                              cy + r * math.sin(2 * math.pi * k / sides + math.pi / sides), z)
                             for k in range(sides)]
        lo, hi = ring(r0, z0), ring(r1, z1)
        if one_image_a_facet:
            for i in range(sides):
                j = (i + 1) % sides
                self.face(wall, [lo[i], lo[j], hi[j], hi[i]], uvs=[(0, 0), (1, 0), (1, 1), (0, 1)])
        else:
            self.quad_strip(wall, lo, hi)
        if top:
            self.face(roof, hi, "top")
        if bottom:
            self.face(roof, list(reversed(lo)), "top")

    def parapet(self, wall, cx, cy, w, d, z, height=0.8, thick=0.4):
        """A low wall round a flat roof's edge (its outside flush with the
        walls), in a plain material of its own: a facade's would show its
        windows on it."""
        for (px, py, pw, pd) in [(cx, cy - d / 2 + thick / 2, w, thick),
                                 (cx, cy + d / 2 - thick / 2, w, thick),
                                 (cx - w / 2 + thick / 2, cy, thick, d - 2 * thick),
                                 (cx + w / 2 - thick / 2, cy, thick, d - 2 * thick)]:
            self.box(wall, wall, px, py, pw, pd, z, z + height)

    def rooftop_plant(self, side, top, cx, cy, w, d, z, seed):
        """Air-conditioning units as roofs have them: one size, in tidy rows
        of a few, a cluster or two per roof (one more per 8,000 m2, up to
        four), square to the building, clear of the edges. Each unit's top
        (`top`) shows its fans once, edge to edge; its sides (`side`)
        louvres."""
        rng = random.Random(seed)
        uw, ud, uh, gap = 3.0, 2.2, 1.4, 1.0
        clusters = min(4, 1 + int(w * d / 8000))
        rows, cols = (2, 3) if w >= 30 and d >= 30 else (1, 2)
        span_x = cols * uw + (cols - 1) * gap
        span_y = rows * ud + (rows - 1) * gap
        if span_x > w - 8 or span_y > d - 8:
            return
        placed = []
        for _ in range(clusters * 20):  # tries, for clusters clear of each other
            if len(placed) == clusters:
                break
            ox = cx + rng.uniform(-(w - span_x) / 2 + 4, (w - span_x) / 2 - 4)
            oy = cy + rng.uniform(-(d - span_y) / 2 + 4, (d - span_y) / 2 - 4)
            if any(abs(ox - px) < span_x + 6 and abs(oy - py) < span_y + 6 for px, py in placed):
                continue
            placed.append((ox, oy))
            for i in range(cols):
                for j in range(rows):
                    ux = ox - span_x / 2 + uw / 2 + i * (uw + gap)
                    uy = oy - span_y / 2 + ud / 2 + j * (ud + gap)
                    x0, x1, y0, y1 = ux - uw / 2, ux + uw / 2, uy - ud / 2, uy + ud / 2
                    lo = [(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)]
                    hi = [(x, y, z + uh) for x, y, _ in lo]
                    self.quad_strip(side, lo, hi)
                    self.face(top, hi, uvs=[(0, 0), (1, 0), (1, 1), (0, 1)])

    def arched_hangar(self, wall, door, roof, w, d, eave, rise, segments=12):
        """Walls to `eave`, an arched roof rising `rise` more across the
        width, the door on the front (-Y) gable."""
        y0, y1 = -d / 2, d / 2
        xs = [-w / 2 + w * k / segments for k in range(segments + 1)]
        arch = [(x, eave + rise * (1 - (2 * x / w) ** 2)) for x in xs]
        # Sides.
        for x, y_a, y_b in [(-w / 2, y1, y0), (w / 2, y0, y1)]:
            self.face(wall, [(x, y_a, -PLINTH), (x, y_b, -PLINTH), (x, y_b, eave), (x, y_a, eave)])
        # Gables: front (the door) and back.
        for y, mat, order in [(y0, door, 1), (y1, wall, -1)]:
            pts = [(-w / 2, y, -PLINTH), (w / 2, y, -PLINTH)] + [(x, y, z) for x, z in reversed(arch)]
            self.face(mat, pts if order > 0 else list(reversed(pts)))
        # The roof, strip by strip.
        for (xa, za), (xb, zb) in zip(arch, arch[1:]):
            self.face(roof, [(xa, y0, za), (xb, y0, zb), (xb, y1, zb), (xa, y1, za)], "top")

    def build(self):
        for mat, faces in self.faces.items():
            su, sv = mat["size_m"]
            bm = bmesh.new()
            uv = bm.loops.layers.uv.new("UVMap")
            for points, projection, given in faces:
                verts = [bm.verts.new(p) for p in points]
                f = bm.faces.new(verts)
                f.normal_update()
                if given:
                    for loop, coords in zip(f.loops, given):
                        loop[uv].uv = coords
                    continue
                n = f.normal
                top = projection == "top" or abs(n.z) > 0.7
                if not top:
                    t = (-n.y, n.x)  # along the wall, horizontal
                    tl = math.hypot(*t) or 1.0
                    t = (t[0] / tl, t[1] / tl)
                for loop in f.loops:
                    p = loop.vert.co
                    # Below the ground line (the plinth) a wall shows its
                    # texture's bottom row, not the top wrapped round.
                    loop[uv].uv = ((p.x / su, p.y / sv) if top else
                                   ((p.x * t[0] + p.y * t[1]) / su, max(p.z, 0.0) / sv))
            mesh = bpy.data.meshes.new(f"{self.name}_{mat.name}")
            bm.to_mesh(mesh)
            bm.free()
            mesh.materials.append(mat)
            obj = bpy.data.objects.new(mesh.name, mesh)
            bpy.context.scene.collection.objects.link(obj)


# --- The landmarks -----------------------------------------------------------

WALL = (196, 192, 184)    # light concrete and render
GLASS_WALL = (150, 160, 168)


def roof_colour(linear):
    """The imagery's colour (linear), as sRGB 0-255."""
    return tuple(255 * (12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055) for c in linear)


# tools/measure-roof-colours.py data/compile/khsv.toml, 2026-11-07.
ROOF = {
    "khsv_terminal": (0.623, 0.661, 0.675),
    "khsv_terminal_hall": (0.292, 0.404, 0.474),
    "khsv_concourse": (0.259, 0.471, 0.619),
    "khsv_tower_base": (0.795, 0.782, 0.755),
    "khsv_hangar_27x105": (0.679, 0.725, 0.739),
    "khsv_hangar_24x147": (0.839, 0.842, 0.826),
    "khsv_hangar_27x144": (0.831, 0.849, 0.845),
    "khsv_hangar_28x166": (0.637, 0.674, 0.675),
    "khsv_hangar_35x108": (0.790, 0.809, 0.806),
    "khsv_hangar_73x38": (0.287, 0.519, 0.637),
    "khsv_warehouse": (0.873, 0.876, 0.866),
    "khsv_cargo_42x194": (0.819, 0.792, 0.748),
    "khsv_cargo_53x115": (0.692, 0.712, 0.706),
    "khsv_cargo_70x261": (0.823, 0.812, 0.785),
    "khsv_hangar_72x100": (0.663, 0.654, 0.628),
    "khsv_hangar_60x108": (0.858, 0.872, 0.874),
}


def flat_roofed(name, w, d, h, facade="office", plant=True):
    """An office, terminal or cargo building: a facade, a membrane roof
    behind a parapet, plant on the roof."""
    roof = roof_colour(ROOF[name])
    if facade == "glass":
        walls = material("glass_wall",
                         office_facade(GLASS_WALL, 0.8, 3.2, 2.0, zlib.crc32(name.encode())), 1,
                         (24.0, 4.0))
    elif facade == "office":
        walls = material("office_wall", office_facade(WALL, seed=zlib.crc32(name.encode())), 1,
                         (24.0, 4.0))
    else:
        walls = material("cargo_wall", metal_wall(shade(roof, 0.9), h, h + 1.0, 4.5, "dock"), 2,
                         (4.5, h + 1.0))
    top = material("roof", membrane(roof, zlib.crc32(name.encode())), 1, (8.0, 8.0))
    casing = shade(roof, 0.9)
    unit_side = material("ac_side", louvres(casing), 2, (2.0, 1.4))
    unit_top = material("ac_top", ac_top(casing), 2, (1.0, 1.0))
    coping = material("coping", plain(shade(WALL if facade != "cargo" else roof, 0.85)), 1,
                      (1.0, 1.0))
    m = Model(name)
    m.box(walls, top, 0, 0, w, d, 0, h)
    m.parapet(coping, 0, 0, w, d, h)
    if plant:
        m.rooftop_plant(unit_side, unit_top, 0, 0, w, d, h, seed=name)
    return m


def hangar(name, w, d, eave):
    roof = roof_colour(ROOF[name])
    rise = min(w * 0.12, 6.0)
    height = eave + rise + 1.0  # to the gables' tops
    walls = material("hangar_wall", metal_wall(shade(roof, 0.92), eave, height, 6.0, "window"),
                     2, (6.0, height))
    door = material("hangar_door",
                    hangar_door(shade(roof, 0.92), height, zlib.crc32(name.encode())), 2,
                    (16.0, height))
    top = material("hangar_roof", standing_seam(roof), 2, (4.8, 4.8))
    m = Model(name)
    m.arched_hangar(walls, door, top, w, d, eave, rise)
    return m


def tower(name):
    concrete = material("concrete", plain((176, 172, 164)), 1, (1.0, 1.0))
    glass = material("cab_glass", cab_glass(), 3, (1.0, 1.0))  # one image a facet
    dark = material("roof_dark", plain((70, 72, 74)), 2, (1.0, 1.0))
    m = Model(name)
    m.prism(concrete, concrete, 0, 0, 3.5, 3.5, 0, 38.0, 16)        # the shaft
    m.prism(concrete, concrete, 0, 0, 6.5, 6.5, 38.0, 40.0, 8, bottom=True)  # the deck
    m.prism(glass, dark, 0, 0, 6.0, 7.0, 40.0, 45.0, 8, top=False,
            one_image_a_facet=True)  # the cab, leaning out
    m.prism(dark, dark, 0, 0, 7.6, 7.6, 45.0, 46.0, 8, bottom=True)  # its roof
    m.box(dark, dark, 0, 0, 0.3, 0.3, 46.0, 52.0)                   # an antenna
    return m


MODELS = {
    "khsv_terminal": lambda n: flat_roofed(n, 197.0, 128.0, 15.0),
    "khsv_terminal_hall": lambda n: flat_roofed(n, 180.0, 83.0, 12.0, "glass"),
    "khsv_concourse": lambda n: flat_roofed(n, 37.0, 210.0, 10.0, "glass", plant=False),
    "khsv_tower": tower,
    "khsv_tower_base": lambda n: flat_roofed(n, 40.0, 26.0, 8.0),
    "khsv_hangar_27x105": lambda n: hangar(n, 27.0, 105.0, 10.0),
    "khsv_hangar_24x147": lambda n: hangar(n, 24.0, 147.0, 10.0),
    "khsv_hangar_27x144": lambda n: hangar(n, 27.0, 144.0, 10.0),
    "khsv_hangar_28x166": lambda n: hangar(n, 28.0, 166.0, 10.0),
    "khsv_hangar_35x108": lambda n: hangar(n, 35.0, 108.0, 11.0),
    "khsv_hangar_73x38": lambda n: hangar(n, 73.0, 38.0, 10.0),
    "khsv_hangar_72x100": lambda n: hangar(n, 72.0, 100.0, 12.0),
    "khsv_hangar_60x108": lambda n: hangar(n, 60.0, 108.0, 12.0),
    "khsv_cargo_42x194": lambda n: flat_roofed(n, 42.0, 194.0, 12.0, "cargo"),
    "khsv_cargo_53x115": lambda n: flat_roofed(n, 53.0, 115.0, 10.0, "cargo"),
    "khsv_cargo_70x261": lambda n: flat_roofed(n, 70.0, 261.0, 13.0, "cargo"),
    "khsv_warehouse": lambda n: flat_roofed(n, 215.0, 261.0, 13.0, "cargo"),
}


def build(name, make, out_dir):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    make(name).build()
    bpy.ops.export_scene.gltf(
        filepath=os.path.join(out_dir, name + ".glb"),
        export_format="GLB",
        export_yup=True,
        export_extras=True,
        export_normals=True,
        export_texcoords=True,
        export_materials="EXPORT",
        export_image_format="AUTO",
        export_apply=True,
    )


def main():
    out_dir = sys.argv[sys.argv.index("--") + 1]
    os.makedirs(out_dir, exist_ok=True)
    for name, make in MODELS.items():
        build(name, make, out_dir)
    print(f"wrote {len(MODELS)} models to {out_dir}")


main()
