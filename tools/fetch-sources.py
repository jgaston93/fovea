#!/usr/bin/env python3
"""Fetch a terrain database's source files (milestone 3 step 1).

    python3 tools/fetch-sources.py data/sources/khsv.toml [--update] [--verify]

Reads the area and layers from the sources file, downloads every file into
data/cache/<area>/<layer>/, and keeps <sources>.lock.json beside it: each
file's URL, size and SHA-256. With a lock file the file list comes from it
(the same inputs every time); --update asks the services again and rewrites
it. Files already present at the locked size are not downloaded again
(--verify also re-hashes them). A download whose hash differs from the lock
is an error: the source changed upstream.

Standard library only. The services: the USGS TNM Access API (3DEP), a STAC
API (the Microsoft Planetary Computer, for NAIP), and plain URLs.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import pathlib
import re
import sys
import time
import tomllib
import urllib.error
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
TNM = "https://tnmaccess.nationalmap.gov/api/v1/products"
USER_AGENT = "fovea-fetch-sources/1"


def get_json(url: str, body: dict | None = None, tries: int = 4) -> dict:
    """A JSON request, retried with backoff: these public services return
    the occasional 500 or time out."""
    data = json.dumps(body).encode() if body is not None else None
    for attempt in range(tries):
        req = urllib.request.Request(url, data=data, headers={"User-Agent": USER_AGENT})
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return json.load(r)
        except (urllib.error.URLError, TimeoutError) as err:
            if attempt == tries - 1 or (isinstance(err, urllib.error.HTTPError) and err.code < 500):
                raise
            wait = 2 ** attempt * 5
            print(f"  ({url.split('?')[0]}: {err}; again in {wait} s)")
            time.sleep(wait)
    raise AssertionError("unreachable")


def bbox(area: dict) -> list[float]:
    return [area["west"], area["south"], area["east"], area["north"]]


def list_tnm(layer: dict, area: dict) -> list[dict]:
    """3DEP products in the area: every project's tiles, or the newest
    edition of each tile."""
    items, offset = [], 0
    while True:
        q = urllib.parse.urlencode({
            "datasets": layer["dataset"],
            "bbox": ",".join(str(v) for v in bbox(area)),
            "max": 100, "offset": offset, "outputFormat": "JSON"})
        page = get_json(f"{TNM}?{q}")
        items += page.get("items", [])
        offset += 100
        if offset >= page.get("total", 0):
            break
    files = [{"name": i["downloadURL"].rsplit("/", 1)[-1], "url": i["downloadURL"],
              "published": i.get("publicationDate", ""), "title": i.get("title", "")}
             for i in items if i.get("downloadURL", "").lower().endswith(".tif")]
    if layer.get("pick") == "newest":
        # Editions of one tile share its code (n35w087); their titles don't
        # follow one pattern ("... n35w087 1 x 1 degree", "... n35w087 20261020").
        newest: dict[str, dict] = {}
        for f in files:
            code = re.search(r"[ns]\d{2}[ew]\d{3}", f["name"] + " " + f["title"])
            key = code.group(0) if code else f["name"]
            if key not in newest or f["published"] > newest[key]["published"]:
                newest[key] = f
        files = list(newest.values())
    return sorted(files, key=lambda f: f["name"])


def list_stac(layer: dict, area: dict) -> list[dict]:
    body = {"collections": [layer["collection"]], "bbox": bbox(area), "limit": 500,
            "query": {"naip:year": {"eq": str(layer["year"])}}}
    found = get_json(f"{layer['endpoint']}/search", body)["features"]
    files = []
    for feat in found:
        href = feat["assets"][layer["asset"]]["href"]
        files.append({"name": href.rsplit("/", 1)[-1], "url": href,
                      "published": feat["properties"].get("datetime", ""),
                      "title": feat["id"]})
    return sorted(files, key=lambda f: f["name"])


def resolve(sources: dict) -> list[dict]:
    """Every file the sources name, asking the services."""
    out = []
    for layer in sources["layer"]:
        # A layer can ask for a larger extent than the area (e.g. all the
        # database's tiles, for a layer without a backdrop).
        area = layer.get("extent", sources["area"])
        kind = layer["kind"]
        if kind == "tnm":
            files = list_tnm(layer, area)
        elif kind == "stac":
            files = list_stac(layer, area)
        elif kind == "url":
            files = [{"name": layer["url"].rsplit("/", 1)[-1], "url": layer["url"],
                      "published": "", "title": ""}]
        else:
            raise SystemExit(f"unknown layer kind '{kind}' in layer {layer['name']}")
        if not files:
            raise SystemExit(f"layer {layer['name']}: the service listed no files for the area")
        for f in files:
            f["layer"] = layer["name"]
        out += files
        print(f"  {layer['name']}: {len(files)} files")
    return out


def sign_tokens(sources: dict) -> dict[str, str]:
    """Short-lived read tokens for layers whose links must be signed."""
    tokens = {}
    for layer in sources["layer"]:
        if "sign" in layer:
            tokens[layer["name"]] = get_json(layer["sign"])["token"]
    return tokens


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(entry: dict, dest: pathlib.Path, token: str | None, verify: bool) -> tuple[str, int]:
    """Downloads one file unless it's already there. Returns what happened."""
    if dest.exists() and "size" in entry and dest.stat().st_size == entry["size"]:
        if not verify or sha256(dest) == entry["sha256"]:
            return "kept", 0
    url = entry["url"] + (("&" if "?" in entry["url"] else "?") + token if token else "")
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_suffix(dest.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=300) as r, part.open("wb") as out:
        while block := r.read(1 << 20):
            out.write(block)
    digest, size = sha256(part), part.stat().st_size
    if "sha256" in entry and digest != entry["sha256"]:
        part.unlink()
        raise RuntimeError(f"{entry['name']}: SHA-256 {digest} is not the locked "
                           f"{entry['sha256']}: the source changed upstream (--update to accept)")
    part.replace(dest)
    entry["size"], entry["sha256"] = size, digest
    return "downloaded", size


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("sources", type=pathlib.Path)
    ap.add_argument("--update", action="store_true", help="ask the services again; rewrite the lock")
    ap.add_argument("--verify", action="store_true", help="re-hash files already downloaded")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    sources_path = args.sources.resolve()
    sources = tomllib.loads(sources_path.read_text())
    lock_path = sources_path.with_suffix(".lock.json")
    if lock_path.exists() and not args.update:
        entries = json.loads(lock_path.read_text())["files"]
        print(f"{len(entries)} files from {lock_path.relative_to(ROOT)}")
    else:
        print("asking the services:")
        old = {}
        if lock_path.exists():
            old = {e["url"]: e for e in json.loads(lock_path.read_text())["files"]}
        entries = resolve(sources)
        for e in entries:  # keep known hashes for unchanged URLs
            if e["url"] in old:
                e.update({k: old[e["url"]][k] for k in ("size", "sha256") if k in old[e["url"]]})

    cache = ROOT / "data" / "cache" / sources["area"]["name"]
    tokens = sign_tokens(sources)
    counts = {"kept": 0, "downloaded": 0}
    total = 0
    failures = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        jobs = {pool.submit(fetch, e, cache / e["layer"] / e["name"], tokens.get(e["layer"]),
                            args.verify): e for e in entries}
        for done in concurrent.futures.as_completed(jobs):
            e = jobs[done]
            try:
                what, size = done.result()
            except Exception as err:  # report all failures, not just the first
                failures.append(f"{e['name']}: {err}")
                continue
            counts[what] += 1
            total += size
            if what == "downloaded":
                print(f"  {e['layer']}/{e['name']}: {size / 1e6:.0f} MB")

    lock = {"area": sources["area"], "files": sorted(entries, key=lambda e: (e["layer"], e["name"]))}
    lock_path.write_text(json.dumps(lock, indent=1) + "\n")
    print(f"{counts['downloaded']} downloaded ({total / 1e9:.2f} GB), {counts['kept']} already here; "
          f"lock: {lock_path.relative_to(ROOT)}")
    for f in failures:
        print(f"failed: {f}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
