#!/usr/bin/env python3
"""The imagery's colour over each placed building's roof (milestone 4 step
5): the mean, in linear light, of the NAIP pixels inside its footprint (its
model's bounds, turned to its heading, less a 15% margin each side so the
ground and shadows round it stay out). The model scripts take these as their
roof colours, so generic buildings still sit in the photograph round them.

    python3 tools/measure-roof-colours.py data/compile/khsv.toml

Prints each placement's model and colour (linear, and sRGB 0-255). Needs
python3-gdal and python3-numpy.
"""

import argparse
import fnmatch
import json
import math
import struct
import sys
import tomllib
from pathlib import Path

import numpy as np
from osgeo import gdal, osr

gdal.UseExceptions()


def model_bounds(path):
    """(x0, x1, z0, z1) of a .glb's positions, from its accessors' min and max."""
    data = Path(path).read_bytes()
    length, = struct.unpack_from("<I", data, 12)
    gltf = json.loads(data[20:20 + length])
    lo, hi = [math.inf] * 3, [-math.inf] * 3
    for mesh in gltf["meshes"]:
        for prim in mesh["primitives"]:
            a = gltf["accessors"][prim["attributes"]["POSITION"]]
            lo = [min(p, q) for p, q in zip(lo, a["min"])]
            hi = [max(p, q) for p, q in zip(hi, a["max"])]
    return lo[0], hi[0], lo[2], hi[2]


def to_linear(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    c = min(max(c, 0.0), 1.0)
    return round(255 * (c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("settings")
    args = ap.parse_args()
    base = Path(args.settings).parent
    with open(args.settings, "rb") as f:
        s = tomllib.load(f)
    cache = base / s["imagery"]["cache"]
    files = []  # each once, as the compiler takes them
    for pattern in s["imagery"]["sources"]:
        p = Path(pattern)
        files += [f for f in sorted((cache / p.parent).iterdir())
                  if fnmatch.fnmatchcase(f.name, p.name) and f not in files]
    vrt = gdal.BuildVRT("/vsimem/roofs.vrt", [str(f) for f in files])
    srs = osr.SpatialReference(vrt.GetProjection())
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    geo = osr.SpatialReference()
    geo.ImportFromEPSG(4269)
    geo.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    to_src = osr.CoordinateTransformation(geo, srs)
    gt = vrt.GetGeoTransform()
    files = s["placements"]["file"]
    placements = []
    for name in [files] if isinstance(files, str) else files:
        with open(base / name, "rb") as f:
            placements += tomllib.load(f)["placement"]
    models = base / s["placements"]["models"]
    for p in placements:
        x0, x1, z0, z1 = model_bounds(models / (p["model"] + ".glb"))
        h = math.radians(p["heading"])
        # Metres north and east per degree here, near enough over a building.
        m_lat = 111132.954 - 559.822 * math.cos(2 * math.radians(p["lat"]))
        m_lon = 111412.84 * math.cos(math.radians(p["lat"]))
        samples = []
        for fx in np.linspace(0.15, 0.85, 24):
            for fz in np.linspace(0.15, 0.85, 24):
                x = x0 + (x1 - x0) * fx
                z = z0 + (z1 - z0) * fz
                north = z * math.cos(h) + x * math.sin(h)  # +X is the model's left
                east = z * math.sin(h) - x * math.cos(h)
                lat = p["lat"] + north / m_lat
                lon = p["lon"] + east / m_lon
                ex, ny, _ = to_src.TransformPoint(lon, lat)
                col = int((ex - gt[0]) / gt[1])
                row = int((ny - gt[3]) / gt[5])
                samples.append(vrt.ReadAsArray(col, row, 1, 1)[:3, 0, 0])
        linear = to_linear(np.array(samples, dtype=np.float64)).mean(axis=0)
        print(f'{p["model"]:22s} linear ({linear[0]:.3f}, {linear[1]:.3f}, {linear[2]:.3f})  '
              f"sRGB {tuple(to_srgb(c) for c in linear)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
