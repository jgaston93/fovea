#!/usr/bin/env bash
# Downloads the CIGI 3.3 Interface Control Document and its errata (Appendix C)
# into docs/reference/ (git-ignored), checked against pinned SHA-256 hashes.
# The CIGI codec in libs/net is written from these, and cites their pages.
#   tools/fetch-cigi-icd.sh
#
# The cigi project's SourceForge download page sits behind a browser check,
# so this goes to a SourceForge mirror directly.
set -euo pipefail

dir="$(cd "$(dirname "$0")/.." && pwd)/docs/reference"
base="https://netix.dl.sourceforge.net/project/cigi/CIGI%20ICD/Version%203.3"
files=(
    "CIGI_ICD_3_3.pdf 9c3c79169e1bfe598a5b390f9ac3f77fbc5f871fdde7f847900e3aa3fae600e8"
    "CIGI_3_3_Appendix_C_5-4-09.pdf a42b38f5b3d62de27e7da4d4b0711959b595e2f0dabaecd1a9a5e27d2c5fed93"
)

mkdir -p "$dir"
for entry in "${files[@]}"; do
    read -r name sha <<<"$entry"
    if [ ! -f "$dir/$name" ]; then
        curl -fsSL --max-time 300 -o "$dir/$name.part" "$base/$name"
        mv "$dir/$name.part" "$dir/$name"
    fi
    echo "$sha  $dir/$name" | sha256sum --check --quiet || {
        echo "fetch-cigi-icd: $name doesn't match its pinned hash" >&2
        exit 1
    }
    echo "$dir/$name"
done
