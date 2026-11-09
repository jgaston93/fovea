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
  - runways (with [runways] in the settings, D-022): the database within
    10 cm of this script's own smooth fit to the sources on each runway, as
    smooth along it, and blending into the ground with no step (the
    runways and their blend are left out of the comparison with the sources);
  - the FAA's surveyed runway-end and field elevations (NAVD88) from the
    airport file, against the database's heights less the geoid.

The database is WGS 84 (G2139) at the settings' [datum] epoch, and the
sources (and the FAA's survey) are NAD83(2011) (D-037): each database point
is taken to NAD83(2011) by PROJ here, point by point, independently of the
compiler's interpolated grid, and its height moved to WGS-84's ellipsoid.

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

from fovea_datum import Datum, check_frame

gdal.UseExceptions()
osr.UseExceptions()

FT = 0.3048


# Tolerances (metres).
TOL_SOURCE_P99 = 0.05   # finest level vs bilinear in the source: 99th percentile
TOL_SOURCE_MAX = 0.50   # and the largest difference, steep ground included
TOL_EDGE = 0.001        # neighbouring tiles' shared samples
TOL_ERROR = 0.01        # coarse tiles beyond their recorded error (float32 rounding)
TOL_SURVEY = 0.30       # FAA surveyed elevations vs the database
TOL_RUNWAY_P99 = 0.05   # the database vs this script's own fit, on the runways
TOL_RUNWAY_MAX = 0.10
TOL_SLOPE_CHANGE = 0.003  # along a runway: between 5 m chords (0.3%), a wheel's bumps
                          # (the sources: up to 1%; KHSV 18R's own end crest: 0.24%)
TOL_BLEND_JUMP = 0.02   # the blend's change in (database - sources) per 0.5 m


def file_list(value, what):
    """A settings file name, or a list of them, as the compiler takes it."""
    files = [value] if isinstance(value, str) else value
    if not isinstance(files, list) or not files or not all(isinstance(f, str) and f for f in files):
        sys.exit(f"{what}: needs a file name or a list of them")
    return files


def degrees_minutes(text):
    """ "34-39.167130N", "086-46.926028W": as the FAA publishes them."""
    d, m = text[:-1].split("-")
    v = int(d) + float(m) / 60.0
    return -v if text[-1] in "SW" else v


# WGS-84, and positions on the ellipsoid in ECEF (this script's own, not
# the compiler's).
WGS84_A = 6378137.0
WGS84_E2 = (1.0 / 298.257223563) * (2.0 - 1.0 / 298.257223563)


def ecef(lat, lon):
    la, lo = np.radians(lat), np.radians(lon)
    n = WGS84_A / np.sqrt(1.0 - WGS84_E2 * np.sin(la) ** 2)
    return np.stack([n * np.cos(la) * np.cos(lo), n * np.cos(la) * np.sin(lo),
                     n * (1.0 - WGS84_E2) * np.sin(la)], axis=-1)


def geodetic(p):
    lon = np.degrees(np.arctan2(p[..., 1], p[..., 0]))
    r = np.hypot(p[..., 0], p[..., 1])
    lat = np.arctan2(p[..., 2], r * (1.0 - WGS84_E2))
    for _ in range(6):
        n = WGS84_A / np.sqrt(1.0 - WGS84_E2 * np.sin(lat) ** 2)
        lat = np.arctan2(p[..., 2] + WGS84_E2 * n * np.sin(lat), r)
    return np.degrees(lat), lon


class Runway:
    """A runway's rectangle: metres along it from its first end, and across
    (positive to the right, looking along it), in the tangent plane at its
    middle."""

    def __init__(self, t, datum, airport=""):
        self.name = f"{airport} {t['name']}" if airport else t["name"]  # as the compiler names it
        self.width = t["width_ft"] * FT
        # The FAA's ends, NAD83(2011), in the database's WGS-84.
        self.ends = [(e["id"], *datum.to_wgs84(degrees_minutes(e["lat"]), degrees_minutes(e["lon"])),
                      e["elevation_ft"]) for e in t["end"]]
        (_, la0, lo0, _), (_, la1, lo1, _) = self.ends
        la, lo = np.radians((la0 + la1) / 2), np.radians((lo0 + lo1) / 2)
        self.east = np.array([-np.sin(lo), np.cos(lo), 0.0])
        self.north = np.array([-np.sin(la) * np.cos(lo), -np.sin(la) * np.sin(lo), np.cos(la)])
        self.origin = ecef(la0, lo0)
        d = ecef(la1, lo1) - self.origin
        e, n = d @ self.east, d @ self.north
        self.length = math.hypot(e, n)
        self.along = (e * self.east + n * self.north) / self.length
        self.right = (n * self.east - e * self.north) / self.length

    def uv(self, lat, lon):
        d = ecef(np.asarray(lat), np.asarray(lon)) - self.origin
        return d @ self.along, d @ self.right

    def latlon(self, u, v):
        u, v = np.asarray(u, float), np.asarray(v, float)
        return geodetic(self.origin + u[..., None] * self.along + v[..., None] * self.right)

    def outside(self, lat, lon):
        """Metres from the rectangle (0 on it)."""
        u, v = self.uv(lat, lon)
        return math.hypot(max(0.0, -u, u - self.length), max(0.0, abs(v) - self.width / 2))


class RunwayFit:
    """This script's own smooth surface for a runway, independent of the
    compiler's: a station every 5 m, each a least-squares fit over the 25 m
    either side (equal weights) of a + b du + c du^2 + crown(v) (v, |v|,
    v^2), refitted three times without the samples beyond 4 robust
    standard deviations of the whole runway's residuals."""

    def __init__(self, runway, heights):
        self.r = runway
        u = np.linspace(0.0, runway.length, int(math.ceil(runway.length / 2.0)) + 1)
        v = np.linspace(-runway.width / 2, runway.width / 2, int(math.ceil(runway.width)) + 1)
        uu, vv = np.meshgrid(u, v, indexing="ij")
        lat, lon = runway.latlon(uu, vv)
        h = [heights(a, b) for a, b in zip(lat.ravel(), lon.ravel())]
        self.missing = sum(x is None for x in h)
        h = np.array([np.nan if x is None else x for x in h]).reshape(uu.shape)
        if self.missing:
            return
        y = vv / (runway.width / 2)
        self.stations = np.linspace(0.0, runway.length, int(math.ceil(runway.length / 5.0)) + 1)
        keep = np.ones(uu.shape, bool)
        for _ in range(4):
            coef = []
            for us in self.stations:
                w = (np.abs(uu - us) <= 25.0) & keep
                x = (uu[w] - us) / 25.0
                a = np.stack([np.ones_like(x), x, x * x, y[w], np.abs(y[w]), y[w] ** 2], axis=1)
                c, *_ = np.linalg.lstsq(a, h[w], rcond=None)
                coef.append(c[[0, 3, 4, 5]])
            self.coef = np.array(coef)
            res = h - self.surface(uu, vv)
            mad = np.median(np.abs(res))
            keep = np.abs(res) <= max(4 * 1.4826 * mad, 0.02)
        self.residual = res
        self.raw = h
        self.uu, self.vv = uu, vv

    def surface(self, u, v):
        u = np.clip(u, 0.0, self.r.length)
        y = np.clip(v, -self.r.width / 2, self.r.width / 2) / (self.r.width / 2)
        c = np.stack([np.interp(u, self.stations, self.coef[:, i]) for i in range(4)])
        return c[0] + c[1] * y + c[2] * np.abs(y) + c[3] * y * y


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
        self.datum = Datum(s["datum"]["epoch"])
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
        # Each airport: its runways, and its field elevation if published.
        self.runways, self.fields, self.blend = [], [], 0.0
        if "runways" in s:
            for name in file_list(s["runways"]["airport"], "[runways] airport"):
                with open(base / name, "rb") as f:
                    airport = tomllib.load(f)
                runways = [Runway(t, self.datum, airport.get("name", "")) for t in airport["runway"]]
                self.runways += runways
                if "field_elevation_ft" in airport:
                    self.fields.append((airport.get("name", name), airport["field_elevation_ft"],
                                        runways))
            self.blend = s["runways"].get("blend_m", 20.0)

    def near_runway(self, lat, lon, margin):
        """Whether a point is on a runway or its blend, or within `margin`
        metres of them (where bilinear sampling reads a sample in them)."""
        return any(r.outside(lat, lon) <= self.blend + margin for r in self.runways)

    def ellipsoidal(self, lat, lon):
        """The height above WGS-84's ellipsoid at a WGS-84 point, and its
        source: the sources' H and the geoid's N at its NAD83(2011) point,
        moved to WGS-84's ellipsoid; or None."""
        lat_n, lon_n, shift = self.datum.to_nad83(lat, lon)
        h = self.orthometric(lat_n, lon_n)
        return None if h is None else (h[0] + self.geoid.sample(lat_n, lon_n) + shift, h[1])

    def height(self, lat, lon):
        """The ellipsoidal height alone, or None."""
        h = self.ellipsoidal(lat, lon)
        return None if h is None else h[0]

    def orthometric(self, lat, lon):
        """(H, source) at a NAD83(2011) point from the highest-priority
        source with all four pixels around it (the compiler's rule), or
        None."""
        for r in self.rasters:
            v = r.sample(lat, lon)
            if v is not None:
                return v, r
        return None

    def navd88(self, height, lat, lon):
        """A database height at a WGS-84 point as NAVD88: less the WGS-84
        shift and the geoid at its NAD83(2011) point."""
        lat_n, lon_n, shift = self.datum.to_nad83(lat, lon)
        return height - shift - self.geoid.sample(lat_n, lon_n)


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
    wrong = check_frame(db.manifest, src.datum, src.output)
    if wrong:
        print(wrong)
        return 1
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
    print(f"finest level ({db.finest}) vs the sources, bilinear, plus GEOID18, in WGS-84 at "
          f"{src.datum.epoch}:")
    diffs, edge_diffs, by_source, skipped, on_runways = [], [], {}, 0, 0
    size = Tiles.size(db.finest)
    samples = db.manifest["layers"]["elevation"]["samples"]
    margin = math.sqrt(2) * size / (samples - 1) * 111_000  # a sample's diagonal, at most
    for i in range(args.points):
        lat = rng.uniform(a["south"], a["north"] - 1e-9)
        lon = rng.uniform(a["west"], a["east"] - 1e-9)
        on_edge = i % 3 == 0
        if on_edge:  # snap to the nearest tile edge, north-south or east-west
            if i % 2 == 0:
                lat = -90.0 + round((lat + 90.0) / size) * size
            else:
                lon = -180.0 + round((lon + 180.0) / size) * size
        if src.near_runway(lat, lon, margin):
            on_runways += 1  # smoothed: checked below
            continue
        h = src.ellipsoidal(lat, lon)
        if h is None:
            skipped += 1
            continue
        expected = h[0]
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
    if on_runways:
        print(f"  {on_runways} points on the runways or their blend left out")
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

    # 4. Runways: against this script's own fit to the sources, along
    # them, and across their blend.
    for r in src.runways:
        print(f"runway {r.name} ({r.length:.0f} m x {r.width:.1f} m), smoothed:")
        fit = RunwayFit(r, src.height)
        if fit.missing:
            ok = False
            print(f"  {fit.missing} points on it have no source: FAIL")
            continue
        print(f"  the sources on it: {np.sqrt((fit.residual ** 2).mean()):.4f} m RMS from this "
              f"script's fit, at most {np.abs(fit.residual).max():.3f} m")
        d = []
        for _ in range(args.points):
            u = rng.uniform(0.5, r.length - 0.5)
            v = rng.uniform(-r.width / 2 + 0.5, r.width / 2 - 0.5)
            lat, lon = r.latlon(u, v)
            d.append(db.height(db.finest, float(lat), float(lon)) - float(fit.surface(u, v)))
        ok &= report("the database vs the fit", d, TOL_RUNWAY_P99, TOL_RUNWAY_MAX)
        # Along the centreline and both edges, a sample every 5 m: the
        # change of slope from one 5 m chord to the next (bumps), in the
        # database and in the sources; and the largest change of grade
        # between 30 m chords, the runway's own profile, kept.
        u = np.arange(0.0, r.length + 1e-9, 5.0)
        for v, where in ((0.0, "centreline"), (-r.width / 2 + 0.5, "left edge"),
                         (r.width / 2 - 0.5, "right edge")):
            lat, lon = r.latlon(u, np.full_like(u, v))
            got = np.array([db.height(db.finest, a, b) for a, b in zip(lat, lon)])
            raw = np.array([src.height(a, b) for a, b in zip(lat, lon)])
            dg, dr = np.abs(np.diff(np.diff(got) / 5.0)), np.abs(np.diff(np.diff(raw) / 5.0))
            g30 = np.abs(np.diff(np.diff(got[::6]) / 30.0)).max()
            good = dg.max() <= TOL_SLOPE_CHANGE
            ok &= good
            print(f"  {where}: slope changes up to {dg.max() * 100:.3f}% between 5 m chords "
                  f"(the sources: {dr.max() * 100:.3f}%) (<= {TOL_SLOPE_CHANGE * 100:.1f}%): "
                  f"{'ok' if good else 'FAIL'}; grade changes up to {g30 * 100:.2f}% "
                  f"between 30 m chords")
        # Out of the rectangle, across its sides and off its ends: the
        # database less the sources changes smoothly, and is nothing past
        # the blend.
        jump, beyond, largest = 0.0, [], 0.0
        lines = [(uu, side * r.width / 2, 0.0, side) for uu in np.arange(25.0, r.length, 100.0)
                 for side in (-1.0, 1.0)]
        lines += [(end, vv, -1.0 if end == 0.0 else 1.0, 0.0) for end in (0.0, r.length)
                  for vv in np.arange(-r.width / 2 + 2.0, r.width / 2, 8.0)]
        steps = np.arange(0.0, src.blend + 5.0 + 1e-9, 0.5)
        for u0, v0, du, dv in lines:
            lat, lon = r.latlon(u0 + du * steps, v0 + dv * steps)
            diff = np.array([db.height(db.finest, a, b) - src.height(a, b)
                             for a, b in zip(lat, lon)])
            jump = max(jump, np.abs(np.diff(diff)).max())
            largest = max(largest, np.abs(diff).max())
            beyond += list(diff[steps >= src.blend + margin])
        good = jump <= TOL_BLEND_JUMP
        ok &= good
        print(f"  the blend ({src.blend:.0f} m), {len(lines)} lines out of it: the database less "
              f"the sources at most {largest:.3f} m, changing at most {jump:.4f} m per 0.5 m "
              f"(<= {TOL_BLEND_JUMP}): {'ok' if good else 'FAIL'}")
        ok &= report("past the blend, the database vs the sources", beyond, TOL_SOURCE_P99,
                     TOL_SOURCE_MAX)

    # 5. The surveyed elevations (the database less the geoid: NAVD88).
    def navd88(lat, lon):
        return src.navd88(db.height(db.finest, lat, lon), lat, lon)

    if src.runways:
        print("FAA surveyed elevations (NAVD88):")
    for r in src.runways:
        for name, lat, lon, ft in r.ends:
            got = navd88(lat, lon)
            good = abs(got - ft * FT) <= TOL_SURVEY
            ok &= good
            print(f"  runway {name} end: published {ft * FT:.2f} m ({ft} ft), database "
                  f"{got:.2f} m, difference {got - ft * FT:+.2f} m: {'ok' if good else 'FAIL'}")
    for name, field_ft, runways in src.fields:
        highest = max(navd88(float(a), float(b)) for r in runways
                      for a, b in zip(*r.latlon(np.linspace(0.0, r.length, 2001), np.zeros(2001))))
        good = abs(highest - field_ft * FT) <= TOL_SURVEY
        ok &= good
        print(f"  {name} field elevation (highest point on the runway centrelines): published "
              f"{field_ft * FT:.2f} m ({field_ft} ft), database {highest:.2f} m, "
              f"difference {highest - field_ft * FT:+.2f} m: {'ok' if good else 'FAIL'}")

    print("all checks pass" if ok else "SOME CHECKS FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
