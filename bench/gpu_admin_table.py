#!/usr/bin/env python3
"""The table of bench_gpu_admin's runs, and the pass criterion: over the baseline, frame time p99
rises by at most 1 ms, with 0 added missed refreshes. Frame time compares the median across runs,
and missed refreshes all the runs of a mode pooled, since one run's are noisy. The first table
prints each run's, because the verdict can't tell a real rise from that noise."""
import json
import sys
from collections import defaultdict
from statistics import median

runs = defaultdict(list)
for line in open(sys.argv[1]):
    run = json.loads(line)
    runs[(run["label"], run["mode"] + ("-same" if run["same"] else ""))].append(run)


def med(group, stat, field="p99"):
    return median(r[stat][field] for r in group if r[stat]["n"])


def missed_pct(run):
    total = run["frames"] + run["missed"]
    return 100.0 * run["missed"] / total if total else 0.0


print("| Config | Mode | Runs | Hz | Frames | Missed % (each run) | Frame p50 | Frame p99 | Max | "
      "Acquire p99 | Submit p99 | Swap p99 | Interval p99 |")
print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
for (label, mode), group in sorted(runs.items()):
    each = ", ".join(f"{missed_pct(r):.2f}" for r in group)
    print(f"| {label} | {mode} | {len(group)} | {median(r['hz'] for r in group):.1f} "
          f"| {sum(r['frames'] for r in group)} | {each} | {med(group, 'frame', 'p50'):.2f} "
          f"| {med(group, 'frame'):.2f} | {max(r['frame']['max'] for r in group):.2f} "
          f"| {med(group, 'acquire'):.3f} | {med(group, 'submit'):.3f} | {med(group, 'swap'):.3f} "
          f"| {med(group, 'interval'):.2f} |")

print()
print("| Config | Mode | Frame p99 rise | Missed, all runs | Baseline missed, all runs | Criterion |")
print("| --- | --- | --- | --- | --- | --- |")
for (label, mode), group in sorted(runs.items()):
    base = runs.get((label.replace("-stress", "").replace("-nice", ""), "baseline"))
    if mode == "baseline" or not base:
        continue
    rise = med(group, "frame") - med(base, "frame")
    pooled = 100.0 * sum(r["missed"] for r in group) / sum(r["missed"] + r["frames"] for r in group)
    base_pooled = 100.0 * sum(r["missed"] for r in base) / sum(r["missed"] + r["frames"] for r in base)
    verdict = "pass" if rise <= 1.0 and pooled <= base_pooled else "FAIL"
    print(f"| {label} | {mode} | {rise:+.2f} ms | {pooled:.2f}% | {base_pooled:.2f}% | {verdict} |")

print()
compiles = [(key, name) for key, group in sorted(runs.items())
            for name in group[0] if name.split("_")[0] in ("small", "large")]
print("| Config | Mode | Object | Count | p50 ms | p99 ms | Max ms |")
print("| --- | --- | --- | --- | --- | --- | --- |")
for key, name in compiles:
    group = runs[key]
    print(f"| {key[0]} | {key[1]} | {name.replace('_', ' ')} | {sum(r[name]['n'] for r in group)} "
          f"| {med(group, name, 'p50'):.2f} | {med(group, name):.2f} "
          f"| {max(r[name]['max'] for r in group):.2f} |")
