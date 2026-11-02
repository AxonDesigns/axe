#!/bin/sh
# Downloads the pinned Unicode Character Database files into build/ucd/<version>/ and checks
# them against tools/unicode/SHA256SUMS. Then copies the conformance test files into
# tests/unicode/, where they are committed so tests run offline.
#
# Bumping the version: change UCD_VERSION and run with
# UCD_UPDATE_SUMS=1, review the diff of the generated tables, and re-run the conformance
# tests. The version must match the UCD that the pinned HarfBuzz ships
# (third_party/harfbuzz/src/hb-ucd-table.hh) so shaping and segmentation agree.
set -eu
UCD_VERSION=18.0.0
root=$(cd "$(dirname "$0")/../.." && pwd)
dest="$root/build/ucd/$UCD_VERSION"
sums="$root/tools/unicode/SHA256SUMS"
mkdir -p "$dest"
cd "$dest"
for f in $(awk '{print $2}' "$sums"); do
    if [ ! -f "$f" ]; then
        mkdir -p "$(dirname "$f")"
        curl -sfL -o "$f" "https://www.unicode.org/Public/$UCD_VERSION/ucd/$f"
    fi
done
if [ "${UCD_UPDATE_SUMS:-0}" = 1 ]; then
    sha256sum $(awk '{print $2}' "$sums") > "$sums"
fi
sha256sum --quiet -c "$sums"
for f in BidiCharacterTest.txt auxiliary/LineBreakTest.txt auxiliary/GraphemeBreakTest.txt; do
    cp "$f" "$root/tests/unicode/"
done
echo "UCD $UCD_VERSION in $dest"
