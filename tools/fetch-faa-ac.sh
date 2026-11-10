#!/usr/bin/env bash
# Downloads the FAA's airport lighting standard, AC 150/5340-30J (Design and
# Installation Details for Airport Visual Aids, 2018-02-12), into
# docs/reference/ (git-ignored), checked against a pinned SHA-256 hash.
# The runway light layouts in apps/fovea-compiler/lights.cpp are written
# from it, and cite its paragraphs and figures (D-033).
#   tools/fetch-faa-ac.sh
#
# faa.gov refuses requests without a browser-like User-Agent.
set -euo pipefail

dir="$(cd "$(dirname "$0")/.." && pwd)/docs/reference"
base="https://www.faa.gov/documentLibrary/media/Advisory_Circular"
files=(
    "150-5340-30J.pdf 65d10b7ca60caf4515574d995ca5af3aca34de2d59fa07fc6e46c002e9ae54c5"
)

mkdir -p "$dir"
for entry in "${files[@]}"; do
    read -r name sha <<<"$entry"
    if [ ! -f "$dir/$name" ]; then
        curl -fsSL --max-time 300 -A "Mozilla/5.0" -o "$dir/$name.part" "$base/$name"
        mv "$dir/$name.part" "$dir/$name"
    fi
    echo "$sha  $dir/$name" | sha256sum --check --quiet || {
        echo "fetch-faa-ac: $name doesn't match its pinned hash" >&2
        exit 1
    }
    echo "$dir/$name"
done
