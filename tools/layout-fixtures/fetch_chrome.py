#!/usr/bin/env python3
"""Downloads the pinned headless Chrome that records the layout fixtures.

The build is Chrome for Testing's chrome-headless-shell, pinned by version and sha256, and
unpacked into the gitignored third_party/chrome-headless-shell/<version>/. Only
record.py runs it; CI never does (it compares against the committed expected results).

    python3 -I tools/layout-fixtures/fetch_chrome.py [--from path/to/archive.zip]
"""
import argparse
import hashlib
import os
import pathlib
import shutil
import sys
import urllib.request
import zipfile

VERSION = "156.0.8078.80"
URL = ("https://storage.googleapis.com/chrome-for-testing-public/"
       f"{VERSION}/linux64/chrome-headless-shell-linux64.zip")
SHA256 = "547dcd374e02961bf998f6046a267644f8502225c36bb78ca307dab32a0b3eb4"

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEST = ROOT / "third_party" / "chrome-headless-shell" / VERSION


def binary() -> pathlib.Path:
    return DEST / "chrome-headless-shell-linux64" / "chrome-headless-shell"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--from", dest="archive", type=pathlib.Path,
                        help="use an already downloaded archive instead of fetching it")
    args = parser.parse_args()
    if binary().exists():
        print(binary())
        return 0
    DEST.mkdir(parents=True, exist_ok=True)
    archive = args.archive or DEST.with_suffix(".zip")
    if not args.archive:
        print(f"downloading {URL}", file=sys.stderr)
        with urllib.request.urlopen(URL) as response, open(archive, "wb") as out:
            shutil.copyfileobj(response, out)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SHA256:
        print(f"sha256 mismatch for {archive}: {digest}, expected {SHA256}", file=sys.stderr)
        return 1
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            path = DEST / info.filename
            if not path.resolve().is_relative_to(DEST.resolve()):
                print(f"refusing to extract {info.filename}", file=sys.stderr)
                return 1
            z.extract(info, DEST)
            mode = info.external_attr >> 16  # zipfile drops Unix permissions
            if mode:
                os.chmod(path, mode & 0o777)
    if not args.archive:
        archive.unlink()
    print(binary())
    return 0


if __name__ == "__main__":
    sys.exit(main())
