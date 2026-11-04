"""Snapshot tests for axe-tsx: tests/tsx/cases/<name>.tsx compiles to <name>.ts.out, and what it
reports on stderr is <name>.err (absent when nothing). Reviewed as diffs.

    python3 -I tools/axe-tsx/snapshots.py --tsx build/debug/tools/axe-tsx [--update]
"""

import argparse
import os
import subprocess
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tsx", required=True, help="the axe-tsx binary")
    ap.add_argument("--update", action="store_true", help="rewrite the expected files")
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    cases = os.path.join(root, "tests", "tsx", "cases")
    failed = 0
    names = sorted(f[:-4] for f in os.listdir(cases) if f.endswith(".tsx"))
    for name in names:
        rel = os.path.join("tests", "tsx", "cases", name + ".tsx")
        r = subprocess.run([args.tsx, rel], cwd=root, capture_output=True, text=True, timeout=60)
        outputs = {".ts.out": r.stdout, ".err": r.stderr}
        for ext, got in outputs.items():
            path = os.path.join(cases, name + ext)
            if args.update:
                if got:
                    with open(path, "w", encoding="utf-8") as f:
                        f.write(got)
                elif os.path.exists(path):
                    os.remove(path)
                continue
            want = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
            if got != want:
                failed += 1
                print(f"FAIL {name}{ext}: differs from the snapshot (--update to accept)")
                for line in difflines(want, got):
                    print("   ", line)
        if not args.update:
            print(f"checked {name}")
    print(f"{len(names)} cases, {failed} differences")
    return 1 if failed else 0


def difflines(a: str, b: str) -> list[str]:
    import difflib
    return list(difflib.unified_diff(a.splitlines(), b.splitlines(), "snapshot", "now", lineterm="", n=1))[:40]


if __name__ == "__main__":
    sys.exit(main())
