#!/usr/bin/env python3
"""Checks the committed airport files against the FAA's NASR airport data
(milestone 6 step 8, D-033): every runway's width, length and edge lights,
and every end's position, elevation and lights, field by field.

    python3 tools/check-airports.py data/airports/khsv.toml data/airports/khua.toml
    python3 tools/check-airports.py --nasr 29_Oct_2026_APT_CSV.zip data/airports/*.toml

Each file names the NASR cycle it was taken from (nasr_cycle); its
airport CSVs are downloaded from the FAA into data/cache/nasr/ unless
--nasr gives them. Give a newer cycle's zip to see what has changed since.
Exit status 1 if anything differs, including a light NASR lists that the
file leaves out.

Standard library only.
"""

import argparse
import csv
import io
import sys
import tomllib
import urllib.request
import zipfile
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FEET = 0.3048

# The NASR fields an end's lights come from, and the airport file's keys.
# Codes are kept as NASR writes them.
END_CODES = {
    "approach_lights": "APCH_LGT_SYSTEM_CODE",
    "vgsi": "VGSI_CODE",
}
END_FLAGS = {
    "centreline_lights": "CNTRLN_LGTS_AVBL_FLAG",
    "touchdown_zone_lights": "TDZ_LGT_AVBL_FLAG",
    "reil": "RWY_END_LGTS_FLAG",
}
END_NUMBERS = {  # key: (NASR field, tolerance)
    "elevation_ft": ("RWY_END_ELEV", 0.05),
    "glide_path_deg": ("VISUAL_GLIDE_PATH_ANGLE", 0.005),
    "threshold_crossing_height_ft": ("THR_CROSSING_HGT", 0.5),
    "displaced_threshold_ft": ("DISPLACED_THR_LEN", 0.5),
}
# Every key a file may use; any other (a misspelling) is reported, since a
# missing light key reads as "none" and would pass wherever NASR has none.
AIRPORT_KEYS = {"name", "nasr_cycle", "field_elevation_ft", "runway"}
RUNWAY_KEYS = {"name", "width_ft", "length_ft", "edge_lights", "end"}
END_KEYS = {"id", "lat", "lon"} | set(END_CODES) | set(END_FLAGS) | set(END_NUMBERS)


def nasr_url(cycle):
    d = date.fromisoformat(cycle)
    return (
        "https://nfdc.faa.gov/webContent/28DaySub/extra/"
        f"{d.day:02d}_{d.strftime('%b')}_{d.year}_APT_CSV.zip"
    )


def load_nasr(path_or_cycle):
    """The runway and runway-end rows, keyed by airport and runway (end)."""
    path = Path(path_or_cycle)
    if not path.exists():
        url = nasr_url(path_or_cycle)
        path = ROOT / "data" / "cache" / "nasr" / url.rsplit("/", 1)[1]
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            print(f"downloading {url}")
            req = urllib.request.Request(url, headers={"User-Agent": "fovea check-airports"})
            with urllib.request.urlopen(req, timeout=120) as r:
                path.write_bytes(r.read())
    with zipfile.ZipFile(path) as z:

        def rows(name):
            with z.open(name) as f:
                return list(csv.DictReader(io.TextIOWrapper(f, encoding="latin-1")))

        runways = {(r["ARPT_ID"], r["RWY_ID"]): r for r in rows("APT_RWY.csv")}
        ends = {(r["ARPT_ID"], r["RWY_END_ID"]): r for r in rows("APT_RWY_END.csv")}
    return path, runways, ends


def degrees(text):
    """'34-39.167130N' (degrees-minutes and a hemisphere) in degrees."""
    hemisphere = text[-1]
    d, m = text[:-1].split("-")
    v = int(d) + float(m) / 60.0
    return -v if hemisphere in "SW" else v


def check_airport(path, nasr_runways, nasr_ends, problems):
    airport = tomllib.loads(path.read_text())
    ident = airport["name"].removeprefix("K")  # NASR's ids drop the K
    checked = 0

    def differ(where, what, ours, theirs):
        problems.append(f"{path.name}: {where}: {what} is {ours!r}, NASR has {theirs!r}")

    def unknown(where, table, known):
        for key in sorted(set(table) - known):
            problems.append(f"{path.name}: {where}: unknown key {key!r}")

    # Every runway and end NASR lists for the airport must be in the file.
    for kind, rows, ours in (
        ("runway", nasr_runways, {rw["name"] for rw in airport["runway"]}),
        ("end", nasr_ends, {e["id"] for rw in airport["runway"] for e in rw["end"]}),
    ):
        for apt, id in sorted(rows):
            if apt == ident and id not in ours:
                problems.append(f"{path.name}: NASR has {kind} {id}, the file doesn't")

    unknown("airport", airport, AIRPORT_KEYS)
    for rw in airport["runway"]:
        name = rw["name"]
        unknown(name, rw, RUNWAY_KEYS)
        n = nasr_runways.get((ident, name))
        if n is None:
            problems.append(f"{path.name}: runway {name}: not in NASR")
            continue
        for key, field in (("width_ft", "RWY_WIDTH"), ("length_ft", "RWY_LEN")):
            if key in rw and abs(rw[key] - float(n[field])) > 0.5:
                differ(name, key, rw[key], n[field])
        if rw.get("edge_lights", "") != n["RWY_LGT_CODE"]:
            differ(name, "edge_lights", rw.get("edge_lights"), n["RWY_LGT_CODE"])
        for end in rw["end"]:
            where = f"{name} end {end['id']}"
            unknown(where, end, END_KEYS)
            e = nasr_ends.get((ident, end["id"]))
            if e is None:
                problems.append(f"{path.name}: {where}: not in NASR")
                continue
            checked += 1
            # The pavement's end: within 1e-7 degrees (about a centimetre).
            for key, field in (("lat", "LAT_DECIMAL"), ("lon", "LONG_DECIMAL")):
                if abs(degrees(end[key]) - float(e[field])) > 1e-7:
                    differ(where, key, end[key], e[field])
            for key, field in END_CODES.items():
                if end.get(key, "") != e[field].strip():
                    differ(where, key, end.get(key), e[field])
            for key, field in END_FLAGS.items():
                theirs = e[field].strip() == "Y"
                if end.get(key, False) != theirs:
                    differ(where, key, end.get(key, False), e[field])
            for key, (field, tolerance) in END_NUMBERS.items():
                text = e[field].strip()
                theirs = float(text) if text else None
                ours = end.get(key)
                if (ours is None) != (theirs is None) or (
                    ours is not None and abs(ours - theirs) > tolerance
                ):
                    differ(where, key, ours, text)
    return airport.get("nasr_cycle"), checked


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("airports", nargs="+", type=Path)
    ap.add_argument("--nasr", help="a NASR APT_CSV zip (default: each file's nasr_cycle, downloaded)")
    args = ap.parse_args()
    problems = []
    loaded = {}
    for path in args.airports:
        cycle = tomllib.loads(path.read_text()).get("nasr_cycle")
        source = args.nasr or cycle
        if source is None:
            problems.append(f"{path.name}: no nasr_cycle, and no --nasr")
            continue
        if source not in loaded:
            loaded[source] = load_nasr(source)
        zip_path, runways, ends = loaded[source]
        _, checked = check_airport(path, runways, ends, problems)
        print(f"{path.name}: {checked} runway ends checked against {zip_path.name}")
    for p in problems:
        print(f"  DIFFERS {p}")
    print("FAIL" if problems else "PASS")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
