#!/usr/bin/env python3
"""The older-reader check (docs/command-buffer.md, "Evolving the format").

Builds the wire reader of a base commit (its generated codec and its tests/wire/wire_dump.c)
and runs the command fixtures added or changed since that commit through it. Each must be
read as the evolution rules promise: the framing is accepted, every op the older reader
knows decodes as this version's dump (tests/wire/fixtures/<name>.bin.txt) says, with only the
data that version doesn't know left out, and ops it doesn't know are skipped as unknown.

    python3 -I tools/wire/old_reader.py --base REF [--cc CC] [FIXTURE.bin ...]

Without fixtures, it checks those `git diff` finds added or changed since REF. Stdlib only.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURES = "tests/wire/fixtures"

# What a newer dump says that the older reader can't: these lines describe data it skips.
NEW_ONLY = ("    tokens=", "    token ")

MAIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include "wire_dump.h"
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) return 2;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *data = malloc((size_t)n + 1);
        if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) return 2;
        fclose(f);
        wire_text t = {0};
        wire_dump_commands(&t, data, (size_t)n);
        printf("=== %s\n%s", argv[i], t.data);
        wire_text_free(&t);
        free(data);
    }
    return 0;
}
"""


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout


def changed_fixtures(base: str) -> list[str]:
    out = git("diff", "--name-only", "--diff-filter=AM", base, "HEAD", "--", FIXTURES)
    names = [line for line in out.splitlines() if line.endswith(".bin") and "/cmd-" in line]
    # Uncommitted ones too, when run on a working tree.
    out = git("status", "--porcelain", "--", FIXTURES)
    for line in out.splitlines():
        path = line[3:]
        if path.endswith(".bin") and "/cmd-" in path and path not in names:
            names.append(path)
    return sorted(names)


def build_old_reader(base: str, cc: str, tmp: str) -> str:
    """The base's reader and dumper, compiled from `git archive`."""
    archive = subprocess.run(["git", "archive", base, "src", "include", "tests/wire/wire_dump.c",
                              "tests/wire/wire_dump.h"], cwd=ROOT, check=True, capture_output=True).stdout
    subprocess.run(["tar", "-x", "-C", tmp], input=archive, check=True)
    main = os.path.join(tmp, "old_reader_main.c")
    with open(main, "w", encoding="utf-8") as f:
        f.write(MAIN)
    exe = os.path.join(tmp, "old_reader")
    sources = [main, "src/wire/wire.c", "src/wire/gen/wire_gen.c", "src/unicode/utf8.c", "tests/wire/wire_dump.c"]
    cmd = [cc, "-std=c2x", "-O1", "-w", "-D_DEFAULT_SOURCE", "-Isrc", "-Iinclude", "-Itests/wire", "-o", exe]
    cmd += [s if os.path.isabs(s) else os.path.join(tmp, s) for s in sources] + ["-lm"]
    subprocess.run(cmd, cwd=tmp, check=True)
    return exe


def groups(dump: str) -> tuple[str, list[list[str]]]:
    """The dump's first line (framing), and each op's lines (its header, then indented ones)."""
    lines = dump.rstrip("\n").split("\n")
    ops: list[list[str]] = []
    for line in lines[1:]:
        if line.startswith("op "):
            ops.append([line])
        elif ops:
            ops[-1].append(line)
    return lines[0], ops


def compare(name: str, old: str, new: str) -> list[str]:
    errors = []
    old_head, old_ops = groups(old)
    new_head, new_ops = groups(new)
    if not new_head.endswith("valid") or new_head.endswith("malformed"):
        return [f"{name}: this version rejects it; fixtures for the older reader must be valid"]
    if old_head != new_head:
        return [f"{name}: the older reader says `{old_head}`, this version `{new_head}`"]
    if len(old_ops) != len(new_ops):
        return [f"{name}: the older reader read {len(old_ops)} ops, this version {len(new_ops)}"]
    for i, (o, n) in enumerate(zip(old_ops, new_ops)):
        prefix = n[0].split(":")[0]
        if o[0].endswith(": unknown"):
            if o[0] != prefix + ": unknown" or len(o) != 1:
                errors.append(f"{name}, op {i}: `{o[0]}` for `{n[0]}`")
            continue
        expected = [line for line in n if not line.startswith(NEW_ONLY)]
        if o != expected:
            errors.append(f"{name}, op {i}: the older reader reads\n    " + "\n    ".join(o) +
                          "\n  where this version, without what it adds, reads\n    " + "\n    ".join(expected))
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", required=True, help="the commit whose reader reads the fixtures")
    ap.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or "cc")
    ap.add_argument("fixtures", nargs="*")
    args = ap.parse_args()
    fixtures = args.fixtures or changed_fixtures(args.base)
    if not fixtures:
        print(f"no command fixtures added or changed since {args.base}")
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_old_reader(args.base, args.cc, tmp)
        paths = [f if os.path.isabs(f) else os.path.join(ROOT, f) for f in fixtures]
        out = subprocess.run([exe, *paths], check=True, capture_output=True, text=True).stdout
    dumps = {}
    for chunk in out.split("=== ")[1:]:
        path, _, dump = chunk.partition("\n")
        dumps[path] = dump
    errors = []
    for path in paths:
        with open(path + ".txt", encoding="utf-8") as f:
            errors += compare(os.path.relpath(path, ROOT), dumps[path], f.read())
    for e in errors:
        print(e, file=sys.stderr)
    print(f"{len(paths)} fixtures through {args.base}'s reader: {len(errors)} problems")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
