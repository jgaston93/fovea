#!/usr/bin/env python3
"""Checks a compiled terrain database's imagery against its sources
(milestone 3 step 4), independently of the compiler: the sources are read
through GDAL's Python bindings and interpolated here, tiles are read by this
script's own reader, and BC7 and BC1 are decoded by Pillow's decoder, not
the encoder library's.

    python3 tools/check-imagery.py data/compile/khsv.toml

The reference for a finest-level tile is what the compiler is meant to do:
at each texel centre, bilinear between pixel centres in the first source
group covering it, from the file the point is deepest inside (furthest
from its nearest edge, then from its nearest on the other axis), rounded to 8
bits; each coarser level and mip the average of four texels in linear
light, rounded again. Checks, each against a stated tolerance (exit status
1 if any fails):
  - PSNR of each level's decoded tiles (mip 0) against the reference: random
    finest tiles, and every tile under one root at the coarser levels;
  - PSNR of every mip of the sampled finest tiles;
  - seams: the outermost texels of each tile are no worse than the three
    rings inside them (texels like them, but no tile's edge), and the
    texture isn't shifted (the reference moved by half a texel either way
    matches worse).

Needs python3-gdal, python3-numpy and python3-pil (Ubuntu packages).
"""

import argparse
import fnmatch
import io
import math
import random
import struct
import sys
import tomllib
from pathlib import Path

import numpy as np
from osgeo import gdal, osr
from PIL import Image

gdal.UseExceptions()

# Least PSNR (dB, over R, G and B) of decoded tiles against the reference,
# by format. RGBA8 is the reference itself, but for rounding.
TOL_PSNR = {"bc7": 38.0, "bc1": 30.0, "rgba8": 60.0}
TOL_EDGE_DB = 1.0     # the outermost texels may be this much worse than the next rings in
TOL_SHIFT_DB = 1.0    # the reference half a texel off must match at least this much worse

FORMATS = {1: "bc7", 2: "bc1", 3: "rgba8"}


def srgb_to_linear(v):
    c = v / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    s = np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)
    return np.floor(s * 255.0 + 0.5).astype(np.uint8)


def half(img):
    """Each texel the average of four in linear light (rows: south first)."""
    if img.shape[0] == 1:
        return img
    lin = srgb_to_linear(img.astype(np.float64))
    lin = (lin[0::2, 0::2] + lin[0::2, 1::2] + lin[1::2, 0::2] + lin[1::2, 1::2]) / 4
    return linear_to_srgb(lin)


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return math.inf if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)


def rings(n):
    """Masks of the outermost texels and of the three rings inside them."""
    outer = np.ones((n, n), bool)
    outer[1:-1, 1:-1] = False
    inner = np.zeros((n, n), bool)
    inner[1:-1, 1:-1] = True
    inner[4:-4, 4:-4] = False
    return outer, inner


def dds(data, size, fmt):
    """A DDS file Pillow decodes: BC7 (sRGB) through a DX10 header, BC1 as DXT1."""
    four = b"DX10" if fmt == "bc7" else b"DXT1"
    pf = struct.pack("<II4s5I", 32, 0x4, four, 0, 0, 0, 0, 0)
    head = (struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, size, size, len(data),
                        0, 0) + b"\0" * 44 + pf + struct.pack("<5I", 0x1000, 0, 0, 0, 0))
    dx10 = struct.pack("<5I", 99, 3, 0, 1, 0) if fmt == "bc7" else b""
    return b"DDS " + head + dx10 + data


class Tiles:
    """Reads a database's imagery layers (D-018, D-017; little-endian)."""

    def __init__(self, root):
        self.root = Path(root)
        with open(self.root / "manifest.toml", "rb") as f:
            self.manifest = tomllib.load(f)
        self.levels = {lv["level"]: lv for lv in self.manifest["level"]}
        self.finest = max(self.levels)

    @staticmethod
    def bounds(level, x, y):
        s = 180.0 / 2**level
        return -90.0 + y * s, -180.0 + x * s, s  # south, west, size

    def keys(self, level):
        lv = self.levels[level]
        return [(level, x, y) for x in range(lv["x"][0], lv["x"][1] + 1)
                for y in range(lv["y"][0], lv["y"][1] + 1)]

    def mips(self, key):
        """The decoded mip chain, each (n, n, 3) uint8, row 0 south."""
        level, x, y = key
        data = (self.root / "tiles" / str(level) / str(x) / f"{y}.fvt").read_bytes()
        if data[:4] != b"FVTL":
            raise ValueError(f"tile {key}: no FVTL magic")
        header, = struct.unpack_from("<H", data, 6)
        if struct.unpack_from("<BxxxIII", data, 8)[:3] != key:
            raise ValueError(f"tile {key}: names another tile")
        count, = struct.unpack_from("<I", data, 20)
        for i in range(count):
            e = header + i * 48
            if data[e:e + 16].rstrip(b"\0") != b"imagery":
                continue
            fmt, _crc, offset, _size = struct.unpack_from("<IIQQ", data, e + 16)
            fmt = FORMATS[fmt]
            size, n_mips = struct.unpack_from("<II", data, offset)
            at = offset + 8
            out = []
            for m in range(n_mips):
                s = max(size >> m, 1)
                blocks = (s + 3) // 4
                if fmt == "rgba8":
                    n = s * s * 4
                    img = np.frombuffer(data, np.uint8, n, at).reshape(s, s, 4)
                else:
                    n = blocks * blocks * (16 if fmt == "bc7" else 8)
                    im = Image.open(io.BytesIO(dds(data[at:at + n], blocks * 4, fmt)))
                    img = np.asarray(im.convert("RGBA"))[:s, :s]
                out.append(img[:, :, :3].copy())
                at += n
            return fmt, out
        raise ValueError(f"tile {key}: no imagery layer")


class Source:
    def __init__(self, path, group, geographic):
        self.path, self.group = Path(path), group
        self.ds = gdal.Open(str(path))
        self.gt = self.ds.GetGeoTransform()
        srs = self.ds.GetSpatialRef()
        srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        self.to_source = osr.CoordinateTransformation(geographic, srs)
        back = osr.CoordinateTransformation(srs, geographic)
        w, h = self.ds.RasterXSize, self.ds.RasterYSize
        x0, y0 = self.gt[0], self.gt[3]
        x1, y1 = x0 + self.gt[1] * w, y0 + self.gt[5] * h
        self.bounds = back.TransformBounds(min(x0, x1), min(y0, y1), max(x0, x1),
                                           max(y0, y1), 21)  # west, south, east, north
        self.w, self.h = w, h


class Reference:
    def __init__(self, settings_path):
        settings_path = Path(settings_path)
        with open(settings_path, "rb") as f:
            s = tomllib.load(f)
        base = settings_path.parent
        im = s["imagery"]
        cache = base / im["cache"]
        self.texels = im["texels"]
        geographic = osr.SpatialReference()
        geographic.ImportFromEPSG(4269)
        geographic.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        self.sources, taken = [], set()
        for g, pattern in enumerate(im["sources"]):
            d = (cache / pattern).parent
            for f in sorted(d.iterdir()):
                if fnmatch.fnmatchcase(f.name, Path(pattern).name) and f.resolve() not in taken:
                    taken.add(f.resolve())
                    self.sources.append(Source(f, g, geographic))
        self.output = base / s["output"]
        self.cache = {}

    def finest(self, key, shift=(0, 0)):
        """The reference image of a finest tile; `shift` moves it by texels
        (columns east, rows north)."""
        ck = (key, shift)
        if ck in self.cache:
            return self.cache[ck]
        level, x, y = key
        south, west, size = Tiles.bounds(level, x, y)
        n = self.texels
        step = size / n
        cols = west + (np.arange(n) + 0.5 + shift[0]) * step
        rows = south + (np.arange(n) + 0.5 + shift[1]) * step
        lon, lat = np.meshgrid(cols, rows)
        pts = np.column_stack([lon.ravel(), lat.ravel()])
        best_depth = np.full(len(pts), -1.0)
        best_other = np.full(len(pts), -1.0)
        best_src = np.full(len(pts), -1)
        best_group = np.full(len(pts), 1 << 30)
        px = np.zeros(len(pts))
        py = np.zeros(len(pts))
        for i, src in enumerate(self.sources):
            w_, s_, e_, n_ = src.bounds
            if w_ >= west + size or e_ <= west or s_ >= south + size or n_ <= south:
                continue
            xy = np.array(src.to_source.TransformPoints(pts))[:, :2]
            sx = (xy[:, 0] - src.gt[0]) / src.gt[1] - 0.5
            sy = (xy[:, 1] - src.gt[3]) / src.gt[5] - 0.5
            dx = np.minimum(sx, src.w - 1 - sx)
            dy = np.minimum(sy, src.h - 1 - sy)
            depth, other = np.minimum(dx, dy), np.maximum(dx, dy)
            deeper = (depth > best_depth) | ((depth == best_depth) & (other > best_other))
            take = (depth >= 0) & ((src.group < best_group) | ((src.group == best_group) & deeper))
            best_depth[take], best_other[take] = depth[take], other[take]
            best_src[take], best_group[take] = i, src.group
            px[take], py[take] = sx[take], sy[take]
        if (best_src < 0).any():
            raise ValueError(f"tile {key}: {int((best_src < 0).sum())} texels have no source")
        out = np.zeros((len(pts), 3))
        for i in np.unique(best_src):
            src = self.sources[i]
            sel = best_src == i
            c = np.minimum(np.floor(px[sel]).astype(int), src.w - 2)
            r = np.minimum(np.floor(py[sel]).astype(int), src.h - 2)
            c0, r0 = c.min(), r.min()
            win = src.ds.ReadAsArray(int(c0), int(r0), int(c.max() - c0 + 2),
                                     int(r.max() - r0 + 2), band_list=[1, 2, 3]).astype(np.float64)
            tx, ty = px[sel] - c, py[sel] - r
            cc, rr = c - c0, r - r0
            top = win[:, rr, cc] * (1 - tx) + win[:, rr, cc + 1] * tx
            bottom = win[:, rr + 1, cc] * (1 - tx) + win[:, rr + 1, cc + 1] * tx
            out[sel] = (top * (1 - ty) + bottom * ty).T
        img = np.floor(out + 0.5).astype(np.uint8).reshape(n, n, 3)
        if len(self.cache) > 600:
            self.cache.clear()
        self.cache[ck] = img
        return img

    def tile(self, key, finest_level):
        """The reference mip 0 of any tile: its children's halves."""
        level, x, y = key
        if level == finest_level:
            return self.finest(key)
        n = self.texels
        out = np.zeros((n, n, 3), np.uint8)
        h = n // 2
        for dy in (0, 1):
            for dx in (0, 1):
                child = self.tile((level + 1, 2 * x + dx, 2 * y + dy), finest_level)
                out[dy * h:(dy + 1) * h, dx * h:(dx + 1) * h] = half(child)
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("settings", type=Path)
    ap.add_argument("--tiles", type=int, default=150, help="random finest tiles to check")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    ref = Reference(args.settings)
    db = Tiles(ref.output)
    rng = random.Random(args.seed)
    failures = []

    def check(name, ok, detail):
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: {detail}")
        if not ok:
            failures.append(name)

    finest = db.finest
    fmt = db.manifest["layers"]["imagery"]["format"]
    tol = TOL_PSNR[fmt]
    print(f"{ref.output}: {fmt}, {ref.texels} texels a side, levels "
          f"{min(db.levels)} to {finest}")

    # Finest tiles at random, every mip; edges; alignment.
    keys = rng.sample(db.keys(finest), min(args.tiles, len(db.keys(finest))))
    err = {}
    outer, inner = rings(ref.texels)
    edge_a, edge_b, in_a, in_b = [], [], [], []
    for key in keys:
        got_fmt, mips = db.mips(key)
        if got_fmt != fmt:
            failures.append(f"tile {key} is {got_fmt}, the manifest says {fmt}")
        want = ref.finest(key)
        for m, got in enumerate(mips):
            err.setdefault(m, []).append((got.astype(np.float64) - want) ** 2)
            want = half(want)
        got = mips[0].astype(np.float64)
        want = ref.finest(key).astype(np.float64)
        edge_a.append(got[outer]), edge_b.append(want[outer])
        in_a.append(got[inner]), in_b.append(want[inner])
    print(f"finest level ({finest}), {len(keys)} tiles:")
    for m in sorted(err):
        mse = np.mean(np.concatenate([e.ravel() for e in err[m]]))
        p = math.inf if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)
        check(f"mip {m} PSNR", p >= tol, f"{p:.1f} dB (at least {tol})")
    p_in = psnr(np.concatenate([a.ravel() for a in in_a]), np.concatenate([b.ravel() for b in in_b]))
    p_edge = psnr(np.concatenate([a.ravel() for a in edge_a]),
                  np.concatenate([b.ravel() for b in edge_b]))
    check("outermost texels", p_edge >= p_in - TOL_EDGE_DB,
          f"{p_edge:.1f} dB, the three rings inside {p_in:.1f} dB")
    for key in keys[:10]:
        got = db.mips(key)[1][0]
        here = psnr(got, ref.finest(key))
        off = max(psnr(got, ref.finest(key, s))
                  for s in ((0.5, 0), (-0.5, 0), (0, 0.5), (0, -0.5)))
        check(f"alignment {key}", here >= off + TOL_SHIFT_DB,
              f"{here:.1f} dB in place, {off:.1f} dB half a texel off")

    # Every tile under one root, at each coarser level: the root holding the
    # middle of the area.
    a = db.manifest["area"]
    lat, lon = (a["south"] + a["north"]) / 2, (a["west"] + a["east"]) / 2
    root = min(db.levels)
    s = 180.0 / 2**root
    rkey = (root, int((lon + 180) // s), int((lat + 90) // s))
    for level in range(finest - 1, root - 1, -1):
        sub = [k for k in db.keys(level) if (k[1] >> (level - root), k[2] >> (level - root)) ==
               rkey[1:]]
        got = np.stack([db.mips(k)[1][0] for k in sub])
        want = np.stack([ref.tile(k, finest) for k in sub])
        p = psnr(got, want)
        check(f"level {level} ({len(sub)} tiles under {rkey})", p >= tol, f"{p:.1f} dB")
        pe = psnr(got[:, outer], want[:, outer])
        pi = psnr(got[:, inner], want[:, inner])
        check(f"level {level} outermost texels", pe >= pi - TOL_EDGE_DB,
              f"{pe:.1f} dB, the three rings inside {pi:.1f} dB")

    print("all checks pass" if not failures else f"{len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
