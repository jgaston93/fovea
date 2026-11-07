#!/usr/bin/env python3
"""Checks a compiled terrain database against its elevation sources
(milestone 3 step 3), independently of the compiler: the sources are read
through GDAL's Python bindings and interpolated here, and tiles are read by
this script's own decoder.

    python3 tools/check-elevation.py data/compile/khsv.toml

Checks, each against a stated tolerance (exit status 1 if any fails):
  - finest level vs the sources: bilinear in the highest-priority source
    covering the point, plus the geoid (h = H + N), at random points and at
    points on tile edges;
  - shared edges: neighbouring tiles' edge samples are the same, at every
    level;
  - each coarser tile stays within its recorded error of the finest level;
  - KHSV: the FAA's surveyed runway-end and field elevations (NAVD88),
    against the database's heights less the geoid.

Needs python3-gdal and python3-numpy (Ubuntu packages).
"""

import argparse
import fnmatch
import math
import random
import struct
import sys
import tomllib
from pathlib import Path

import numpy as np
from osgeo import gdal, osr

gdal.UseExceptions()

FT = 0.3048

# Huntsville International (KHSV), from the FAA's airport data as AirNav
# publishes it (checked 2026-11-07): runway ends (lat, lon in degrees and
# minutes) and their surveyed elevations in feet, NAVD88; the field
# elevation is the highest point of the runways.
KHSV_RUNWAY_ENDS = {
    "18R": ((34, 39.167130), (86, 46.926028), 628.4),
    "36L": ((34, 37.090260), (86, 46.967450), 611.2),
    "18L": ((34, 39.185908), (86, 45.927885), 609.3),
    "36R": ((34, 37.537492), (86, 45.961032), 588.6),
}
KHSV_RUNWAYS = [("18R", "36L"), ("18L", "36R")]
KHSV_FIELD_FT = 628.9

# Tolerances (metres).
TOL_SOURCE_P99 = 0.05   # finest level vs bilinear in the source: 99th percentile
TOL_SOURCE_MAX = 0.50   # and the largest difference, steep ground included
TOL_EDGE = 0.001        # neighbouring tiles' shared samples
TOL_ERROR = 0.01        # coarse tiles beyond their recorded error (float32 rounding)
TOL_SURVEY = 0.30       # FAA surveyed elevations vs the database


def dms(d_m, west=False):
    v = d_m[0] + d_m[1] / 60.0
    return -v if west else v


class Tiles:
    """Reads a database's tiles (D-018 layout, little-endian)."""

    def __init__(self, root):
        self.root = Path(root)
        with open(self.root / "manifest.toml", "rb") as f:
            self.manifest = tomllib.load(f)
        self.levels = {lv["level"]: lv for lv in self.manifest["level"]}
        self.finest = max(self.levels)
        self.cache = {}

    @staticmethod
    def size(level):
        return 180.0 / 2**level

    def bounds(self, level, x, y):
        s = self.size(level)
        return -90.0 + y * s, -180.0 + x * s, s  # south, west, size

    def has(self, key):
        lv = self.levels.get(key[0])
        return (lv is not None and lv["x"][0] <= key[1] <= lv["x"][1] and
                lv["y"][0] <= key[2] <= lv["y"][1])

    def key_at(self, level, lat, lon):
        s = self.size(level)
        return level, int((lon + 180.0) // s), int((lat + 90.0) // s)

    def grid(self, level, x, y):
        key = (level, x, y)
        if key in self.cache:
            return self.cache[key]
        data = (self.root / "tiles" / str(level) / str(x) / f"{y}.fvt").read_bytes()
        if data[:4] != b"FVTL":
            raise ValueError(f"tile {key}: no FVTL magic")
        header, = struct.unpack_from("<H", data, 6)
        lv, = struct.unpack_from("<B", data, 8)
        tx, ty, count = struct.unpack_from("<III", data, 12)
        if (lv, tx, ty) != key:
            raise ValueError(f"tile {key}: names itself {(lv, tx, ty)}")
        for i in range(count):
            e = header + i * 48
            name = data[e:e + 16].rstrip(b"\0").decode()
            fmt, _crc, offset, size = struct.unpack_from("<IIQQ", data, e + 16)
            if name == "elevation" and fmt == 1:
                n, lo, hi, err = struct.unpack_from("<Ifff", data, offset)
                h = np.frombuffer(data, "<f4", n * n, offset + 16).reshape(n, n)
                self.cache[key] = (h.astype(np.float64), float(err))
                if len(self.cache) > 4096:
                    self.cache.pop(next(iter(self.cache)))
                return self.cache[key]
        raise ValueError(f"tile {key}: no elevation layer")

    def height(self, level, lat, lon, key=None):
        """Bilinear in one tile (row 0 south, column 0 west, edges shared)."""
        level, x, y = key or self.key_at(level, lat, lon)
        h, _ = self.grid(level, x, y)
        south, west, s = self.bounds(level, x, y)
        n = h.shape[0]
        fx = (lon - west) / s * (n - 1)
        fy = (lat - south) / s * (n - 1)
        c = min(max(int(math.floor(fx)), 0), n - 2)
        r = min(max(int(math.floor(fy)), 0), n - 2)
        tx, ty = fx - c, fy - r
        return ((h[r, c] * (1 - tx) + h[r, c + 1] * tx) * (1 - ty) +
                (h[r + 1, c] * (1 - tx) + h[r + 1, c + 1] * tx) * ty)


class Raster:
    """A source raster, sampled bilinearly between pixel centres."""

    def __init__(self, path):
        self.path = Path(path)
        self.ds = gdal.Open(str(path))
        self.band = self.ds.GetRasterBand(1)
        self.nodata = self.band.GetNoDataValue()
        self.gt = self.ds.GetGeoTransform()
        srs = self.ds.GetSpatialRef()
        srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        nad83 = osr.SpatialReference()
        nad83.ImportFromEPSG(4269)
        nad83.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
        self.geographic = srs.IsGeographic()
        self.to_src = osr.CoordinateTransformation(nad83, srs)

    def sample(self, lat, lon):
        """The value, or None if any of the four pixels around it is empty."""
        if self.geographic:
            x, y = lon, lat
            if x < self.gt[0]:
                x += 360.0  # a grid in 0-360 longitudes (GEOID18)
        else:
            x, y, _ = self.to_src.TransformPoint(lon, lat)
        fx = (x - self.gt[0]) / self.gt[1] - 0.5
        fy = (y - self.gt[3]) / self.gt[5] - 0.5
        c, r = int(math.floor(fx)), int(math.floor(fy))
        if c < 0 or r < 0 or c + 1 >= self.ds.RasterXSize or r + 1 >= self.ds.RasterYSize:
            return None
        v = self.band.ReadAsArray(c, r, 2, 2).astype(np.float64)
        if not np.all(np.isfinite(v)) or (self.nodata is not None and
                                          np.any(v == np.float32(self.nodata))):
            return None
        tx, ty = fx - c, fy - r
        return ((v[0, 0] * (1 - tx) + v[0, 1] * tx) * (1 - ty) +
                (v[1, 0] * (1 - tx) + v[1, 1] * tx) * ty)


class Sources:
    def __init__(self, settings_file):
        base = Path(settings_file).parent
        with open(settings_file, "rb") as f:
            s = tomllib.load(f)
        self.output = base / s["output"]
        cache = base / s["elevation"]["cache"]
        self.rasters, seen = [], set()
        for pattern in s["elevation"]["sources"]:
            p = Path(pattern)
            for f in sorted((cache / p.parent).iterdir()):
                if fnmatch.fnmatchcase(f.name, p.name) and f not in seen:
                    seen.add(f)
                    self.rasters.append(Raster(f))
        self.geoid = Raster(cache / s["elevation"]["geoid"])
        area_file = base / s["area"]
        with open(area_file, "rb") as f:
            self.area = tomllib.load(f)["area"]

    def orthometric(self, lat, lon):
        """(H, source) from the highest-priority source with all four
        pixels around the point (the compiler's rule), or None."""
        for r in self.rasters:
            v = r.sample(lat, lon)
            if v is not None:
                return v, r
        return None


def report(name, diffs, p99_tol, max_tol):
    d = np.abs(np.asarray(diffs))
    p99, worst = np.percentile(d, 99), d.max()
    ok = p99 <= p99_tol and worst <= max_tol
    print(f"  {name}: {len(d)} points, mean {d.mean():.4f} m, RMS {math.sqrt((d**2).mean()):.4f} m, "
          f"p99 {p99:.4f} m (<= {p99_tol}), max {worst:.4f} m (<= {max_tol}): "
          f"{'ok' if ok else 'FAIL'}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("settings")
    ap.add_argument("--points", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    src = Sources(args.settings)
    db = Tiles(src.output)
    rng = random.Random(args.seed)
    # Points over the whole database (the root tiles), beyond the area too,
    # where the backdrop fills in.
    root = db.levels[min(db.levels)]
    rs = Tiles.size(root["level"])
    a = {"south": -90.0 + root["y"][0] * rs, "north": -90.0 + (root["y"][1] + 1) * rs,
         "west": -180.0 + root["x"][0] * rs, "east": -180.0 + (root["x"][1] + 1) * rs}
    ok = True
    print(f"{src.output}: levels {min(db.levels)} to {db.finest}")

    # 1. The finest level against the sources, at random points and on
    # tile edges (both tiles' values, through their own grids).
    print(f"finest level ({db.finest}) vs the sources, bilinear, plus GEOID18:")
    diffs, edge_diffs, by_source, skipped = [], [], {}, 0
    size = Tiles.size(db.finest)
    for i in range(args.points):
        lat = rng.uniform(a["south"], a["north"] - 1e-9)
        lon = rng.uniform(a["west"], a["east"] - 1e-9)
        on_edge = i % 3 == 0
        if on_edge:  # snap to the nearest tile edge, north-south or east-west
            if i % 2 == 0:
                lat = -90.0 + round((lat + 90.0) / size) * size
            else:
                lon = -180.0 + round((lon + 180.0) / size) * size
        h = src.orthometric(lat, lon)
        if h is None:
            skipped += 1
            continue
        expected = h[0] + src.geoid.sample(lat, lon)
        lv, x, y = db.key_at(db.finest, lat, lon)
        keys = [(lv, x, y)]
        if on_edge:  # the neighbour on the other side of the edge
            keys.append((lv, x, y - 1) if i % 2 == 0 else (lv, x - 1, y))
        for k in keys:
            if not db.has(k):  # the database's own outer edge
                continue
            d = db.height(db.finest, lat, lon, k) - expected
            (edge_diffs if on_edge else diffs).append(d)
            by_source.setdefault(h[1].path.parent.name, []).append(d)
    if skipped:
        ok = False
        print(f"  {skipped} points have no source: FAIL")
    ok &= report("inside tiles", diffs, TOL_SOURCE_P99, TOL_SOURCE_MAX)
    ok &= report("on tile edges, from both tiles", edge_diffs, TOL_SOURCE_P99, TOL_SOURCE_MAX)
    for name, d in sorted(by_source.items()):
        report(f"  from {name}", d, TOL_SOURCE_P99, TOL_SOURCE_MAX)

    # 2. Shared edges: every east-west and north-south neighbour pair,
    # except at the finest level (2000 tiles at random).
    print("shared tile edges:")
    for level in sorted(db.levels):
        lv = db.levels[level]
        (x0, x1), (y0, y1) = lv["x"], lv["y"]
        tiles = [(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]
        if level == db.finest and len(tiles) > 2000:
            tiles = rng.sample(tiles, 2000)
        worst, pairs = 0.0, 0
        for x, y in tiles:
            h, _ = db.grid(level, x, y)
            if x < x1:
                worst = max(worst, np.abs(h[:, -1] - db.grid(level, x + 1, y)[0][:, 0]).max())
                pairs += 1
            if y < y1:
                worst = max(worst, np.abs(h[-1, :] - db.grid(level, x, y + 1)[0][0, :]).max())
                pairs += 1
        good = worst <= TOL_EDGE
        ok &= good
        print(f"  level {level}: {pairs} edges, largest difference {worst:.6f} m: "
              f"{'ok' if good else 'FAIL'}")

    # 3. Coarser tiles within their recorded error of the finest level.
    print("coarse levels within their recorded error of the finest:")
    for level in sorted(db.levels)[:-1]:
        excess, worst_err, n = 0.0, 0.0, 400
        for _ in range(n):
            lat = rng.uniform(a["south"], a["north"] - 1e-9)
            lon = rng.uniform(a["west"], a["east"] - 1e-9)
            k = db.key_at(level, lat, lon)
            err = db.grid(*k)[1]
            d = abs(db.height(level, lat, lon) - db.height(db.finest, lat, lon))
            excess = max(excess, d - err)
            worst_err = max(worst_err, err)
        good = excess <= TOL_ERROR
        ok &= good
        print(f"  level {level}: {n} points, recorded errors up to {worst_err:.2f} m, "
              f"largest excess {max(excess, 0.0):.4f} m: {'ok' if good else 'FAIL'}")

    # 4. KHSV's surveyed elevations (the database less the geoid: NAVD88).
    def navd88(lat, lon):
        return db.height(db.finest, lat, lon) - src.geoid.sample(lat, lon)

    print("KHSV, FAA surveyed elevations (NAVD88):")
    ends = {}
    for name, (lat_dm, lon_dm, ft) in KHSV_RUNWAY_ENDS.items():
        lat, lon = dms(lat_dm), dms(lon_dm, west=True)
        ends[name] = (lat, lon)
        got = navd88(lat, lon)
        good = abs(got - ft * FT) <= TOL_SURVEY
        ok &= good
        print(f"  runway {name} end: published {ft * FT:.2f} m ({ft} ft), database {got:.2f} m, "
              f"difference {got - ft * FT:+.2f} m: {'ok' if good else 'FAIL'}")
    highest = max(navd88(ends[a_][0] + t * (ends[b][0] - ends[a_][0]),
                         ends[a_][1] + t * (ends[b][1] - ends[a_][1]))
                  for a_, b in KHSV_RUNWAYS for t in np.linspace(0.0, 1.0, 2001))
    good = abs(highest - KHSV_FIELD_FT * FT) <= TOL_SURVEY
    ok &= good
    print(f"  field elevation (highest point on the runway centrelines): published "
          f"{KHSV_FIELD_FT * FT:.2f} m ({KHSV_FIELD_FT} ft), database {highest:.2f} m, "
          f"difference {highest - KHSV_FIELD_FT * FT:+.2f} m: {'ok' if good else 'FAIL'}")

    print("all checks pass" if ok else "SOME CHECKS FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
