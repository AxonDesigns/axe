#!/usr/bin/env python3
"""Counts the instructions of every CPU benchmark case with Callgrind and compares them with
bench/baselines.json (CONTRIBUTING.md, "Benchmarks").

Instruction counts don't move between runs on the same machine, unlike times, so CI can
catch a regression of a few percent. Each case is a noinline `bench_case_*` function in
bench/bench_layout.c or bench/bench_text.c; with `--callgrind` each benchmark runs its CPU
cases once, and Callgrind collects inside those functions only (--toggle-collect). A case's
count is its inclusive count.

    python3 -I tools/bench/callgrind.py --build build/release [--out counts.json]

lists every case with its baseline, its count and the change, and fails (exit 1) if a case
counts more than the threshold (2%) above its baseline or has no count. A case more than
the threshold below its baseline is reported, not failed, so the gain can be locked in.
--out writes the counts in bench/baselines.json's format, stamped with the CI run that
measured them (CI uploads it as the `bench-callgrind` artifact).

    python3 -I tools/bench/callgrind.py --update callgrind-counts.json --reason "why"

makes such a file bench/baselines.json, with the reason; commit that alone. Counts measured
outside CI are refused: compilers and libraries differ, so local counts can be a few
percent off CI's and are for investigation only.

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
    """Prints every case; returns False if one regressed past the threshold or is missing."""
    threshold = float(baselines.get("threshold_percent", 2))
    base = baselines.get("counts", {})
    rows, regressed, improved, missing, new = [], [], [], [], []
    for name in sorted(set(counts) | set(base)):
        got, want = counts.get(name), base.get(name)
        if got is None:
            rows.append((name, f"{want:,}", "", "", "MISSING: case gone or renamed"))
            missing.append(name)
            continue
        if want is None:
            rows.append((name, "", f"{got:,}", "", "new: no baseline yet"))
            new.append(name)
            continue
        change = 100.0 * (got - want) / want
        note = ""
        if change > threshold:
            note = f"REGRESSION: more than {threshold:g}% above the baseline"
            regressed.append(f"{name} {change:+.2f}%")
        elif change < -threshold:
            note = f"improvement: more than {threshold:g}% below; update the baseline"
            improved.append(f"{name} {change:+.2f}%")
        rows.append((name, f"{want:,}", f"{got:,}", f"{change:+.2f}%", note))
    header = ("case", "baseline", "measured", "change", "")
    widths = [max(len(r[i]) for r in rows + [header]) for i in range(5)]
    lines = ["  ".join(c.ljust(w) for c, w in zip(r, widths)).rstrip() for r in [header] + rows]
    print("\n".join(lines))
    print()
    verdict = []
    if regressed:
        verdict.append(f"FAILED: {len(regressed)} case(s) more than {threshold:g}% above their baselines: "
                       + ", ".join(regressed) + ". Find the cause, or, if the cost is intended, update the "
                       "baselines from this run's bench-callgrind artifact in a commit of their own "
                       "(CONTRIBUTING.md, \"Benchmarks\").")
    if missing:
        verdict.append(f"FAILED: no count for {', '.join(missing)}: a case was removed or renamed; update the "
                       "baselines from this run's artifact.")
    if improved:
        verdict.append(f"Improved by more than {threshold:g}%: {', '.join(improved)}. Passing; update the "
                       "baselines from this run's bench-callgrind artifact to lock the gain in.")
    if new:
        verdict.append(f"New, without a baseline: {', '.join(new)}. Passing; add them from this run's artifact.")
    if not regressed and not missing:
        verdict.insert(0, f"Passed: no case more than {threshold:g}% above its baseline.")
    print("\n".join(verdict))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("### Callgrind instruction counts\n\n| Case | Baseline | Measured | Change | |\n"
                    "| --- | ---: | ---: | ---: | --- |\n")
            f.writelines(f"| {' | '.join(r)} |\n" for r in rows)
            f.write("\n" + "\n\n".join(verdict) + "\n")
    return not regressed and not missing


def ci_source() -> dict:
    """Where counts measured now come from: the CI run, or nothing outside CI."""
    if os.environ.get("GITHUB_ACTIONS") != "true":
        return {}
    env = os.environ.get
    return {"run_id": int(env("GITHUB_RUN_ID", "0")), "run_attempt": int(env("GITHUB_RUN_ATTEMPT", "1")),
            "workflow": env("GITHUB_WORKFLOW", ""), "event": env("GITHUB_EVENT_NAME", ""),
            "ref": env("GITHUB_REF_NAME", ""), "commit": env("GITHUB_SHA", ""),
            "url": f"{env('GITHUB_SERVER_URL', '')}/{env('GITHUB_REPOSITORY', '')}/actions/runs/{env('GITHUB_RUN_ID', '')}"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=pathlib.Path, help="build directory with bench/ (a release build)")
    ap.add_argument("--out", type=pathlib.Path, help="write the counts here, in bench/baselines.json's format")
    ap.add_argument("--update", type=pathlib.Path, metavar="COUNTS",
                    help="make these counts (CI's bench-callgrind artifact) bench/baselines.json")
    ap.add_argument("--reason", help="why the baselines change (required with --update)")
    ap.add_argument("--baselines", type=pathlib.Path, default=BASELINES, help="default: bench/baselines.json")
    args = ap.parse_args()
    baselines = json.loads(args.baselines.read_text()) if args.baselines.exists() else {}
    threshold = baselines.get("threshold_percent", 2)
    if args.update:
        if not args.reason:
            ap.error("--update needs --reason")
        counts = json.loads(args.update.read_text())
        source = counts.get("source") or {}
        if not source.get("run_id"):
            raise SystemExit(f"{args.update}: not measured in CI (no run in its source); baselines come from "
                             "CI's bench-callgrind artifact only")
        baselines = {"threshold_percent": threshold, "reason": args.reason, "source": source,
                     "counts": dict(sorted(counts["counts"].items()))}
        args.baselines.write_text(json.dumps(baselines, indent=2) + "\n")
        print(f"wrote {len(counts['counts'])} baselines from CI run {source['run_id']} to {args.baselines}")
        return 0
    if not args.build:
        ap.error("--build or --update is required")
    counts = measure(args.build)
    if args.out:
        out = {"threshold_percent": threshold, "reason": "", "source": ci_source(),
               "counts": dict(sorted(counts.items()))}
        args.out.write_text(json.dumps(out, indent=2) + "\n")
    return 0 if compare(counts, baselines) else 1


if __name__ == "__main__":
    sys.exit(main())
