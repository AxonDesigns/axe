#!/usr/bin/env python3
"""Counts the instructions of every CPU benchmark case with Callgrind and compares them with
bench/baselines.json (CONTRIBUTING.md, "Benchmarks").

Instruction counts barely move between runs or machines, unlike times, so CI can catch a
regression of a few percent. Each case is a noinline `bench_case_*` function in
bench/bench_layout.c or bench/bench_text.c; with `--callgrind` each benchmark runs its CPU
cases once, and Callgrind collects inside those functions only (--toggle-collect). A case's
count is its inclusive count.

    python3 -I tools/bench/callgrind.py --build build/release [--out counts.json]

fails (exit 1) if a case counts more than the baselines' threshold above its baseline, and
says which cases count well below theirs. Baselines marked provisional are only reported.

    python3 -I tools/bench/callgrind.py --update counts.json --reason "why"

writes counts (normally CI's `bench-callgrind` artifact) into bench/baselines.json, with the
reason; commit that alone.

Standard library, valgrind and callgrind_annotate only.
"""
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINES = ROOT / "bench" / "baselines.json"
BENCHES = ["bench_layout", "bench_text"]
CASE = re.compile(r"^\s*([\d,]+)\s.*?[:\s](bench_case_\w+?)(?:\.\w+)?(?:\s|$)")


def count(binary: pathlib.Path) -> dict:
    """Runs one benchmark under Callgrind; returns {case: instructions}."""
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "callgrind.out"
        subprocess.run(["valgrind", "--tool=callgrind", f"--callgrind-out-file={out}",
                        "--toggle-collect=bench_case_*", str(binary), "--callgrind"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        text = subprocess.run(["callgrind_annotate", "--inclusive=yes", "--threshold=100", str(out)],
                              check=True, capture_output=True, text=True).stdout
    cases = {}
    for line in text.splitlines():
        m = CASE.match(line)
        if m:
            name = m.group(2)[len("bench_case_"):]
            cases[name] = max(cases.get(name, 0), int(m.group(1).replace(",", "")))
    if not cases:
        raise SystemExit(f"{binary}: no bench_case_* functions in the profile")
    return cases


def measure(build: pathlib.Path) -> dict:
    counts = {}
    for bench in BENCHES:
        for case, n in count(build / "bench" / bench).items():
            counts[f"{bench}/{case}"] = n
    return counts


def compare(counts: dict, baselines: dict) -> bool:
    """Prints a table; returns False if a case regressed past the threshold."""
    threshold = float(baselines.get("threshold_percent", 2))
    base = baselines.get("counts", {})
    provisional = baselines.get("provisional", False)
    ok, rows = True, []
    for name in sorted(set(counts) | set(base)):
        got, want = counts.get(name), base.get(name)
        if got is None:
            rows.append((name, "", f"{want:,}", "", "missing: case gone or renamed"))
            ok = False
            continue
        if want is None:
            rows.append((name, f"{got:,}", "", "", "new: no baseline yet"))
            continue
        change = 100.0 * (got - want) / want
        note = ""
        if change > threshold:
            note = f"REGRESSION (over {threshold:g}%)"
            ok = False
        elif change < -threshold:
            note = "faster: consider updating the baseline"
        rows.append((name, f"{got:,}", f"{want:,}", f"{change:+.2f}%", note))
    header = ("case", "instructions", "baseline", "change", "")
    widths = [max(len(r[i]) for r in rows + [header]) for i in range(5)]
    lines = ["  ".join(c.ljust(w) for c, w in zip(r, widths)).rstrip() for r in [header] + rows]
    print("\n".join(lines))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("### Callgrind instruction counts\n\n| Case | Instructions | Baseline | Change | |\n"
                    "| --- | ---: | ---: | ---: | --- |\n")
            f.writelines(f"| {' | '.join(r)} |\n" for r in rows)
            if provisional:
                f.write("\nBaselines are provisional: reported, not enforced.\n")
    if provisional and not ok:
        print("baselines are provisional: not failing (replace them with this run's counts)")
        return True
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=pathlib.Path, help="build directory with bench/ (a release build)")
    ap.add_argument("--out", type=pathlib.Path, help="write the counts here as JSON")
    ap.add_argument("--update", type=pathlib.Path, metavar="COUNTS",
                    help="write these counts (from --out) into bench/baselines.json")
    ap.add_argument("--reason", help="why the baselines change (required with --update)")
    ap.add_argument("--source", default="", help="where the counts come from (with --update)")
    ap.add_argument("--baselines", type=pathlib.Path, default=BASELINES, help="default: bench/baselines.json")
    ap.add_argument("--provisional", action="store_true",
                    help="with --update: report against these baselines without failing")
    args = ap.parse_args()
    baselines = json.loads(args.baselines.read_text()) if args.baselines.exists() else {}
    if args.update:
        if not args.reason:
            ap.error("--update needs --reason")
        counts = json.loads(args.update.read_text())["counts"]
        baselines = {"threshold_percent": baselines.get("threshold_percent", 2), "provisional": args.provisional,
                     "reason": args.reason, "source": args.source, "counts": dict(sorted(counts.items()))}
        args.baselines.write_text(json.dumps(baselines, indent=2) + "\n")
        print(f"wrote {len(counts)} baselines to {args.baselines}")
        return 0
    if not args.build:
        ap.error("--build or --update is required")
    counts = measure(args.build)
    if args.out:
        args.out.write_text(json.dumps({"counts": dict(sorted(counts.items()))}, indent=2) + "\n")
    return 0 if compare(counts, baselines) else 1


if __name__ == "__main__":
    sys.exit(main())
