#!/usr/bin/env python3
"""Generates the wire codec, the TypeScript wire package and the documentation tables from
idl/wire.idl (grammar and outputs: docs/idl.md).

    python3 -I tools/idl/axe_idl.py                     write every output
    python3 -I tools/idl/axe_idl.py --check             fail if an output is out of date
    python3 -I tools/idl/axe_idl.py --compat-base REF   also check the evolution rules against
                                                        the IDL at git REF
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import idl_c  # noqa: E402
import idl_docs  # noqa: E402
import idl_model  # noqa: E402
import idl_ts  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
IDL = "idl/wire.idl"


def outputs(schema: idl_model.Schema, root: str) -> dict[str, str]:
    """Every generated file, by path relative to the repository root."""
    out = {
        "include/ui/wire_ids.h": idl_c.wire_ids_h(schema),
        "src/wire/gen/wire_gen.h": idl_c.gen_h(schema),
        "src/wire/gen/wire_gen.c": idl_c.gen_c(schema),
    }
    out.update(idl_ts.files(schema))
    for path, render in idl_docs.documents(schema).items():
        with open(os.path.join(root, path), encoding="utf-8") as f:
            out[path] = render(f.read())
    return out


def load(path: str, text: str | None = None) -> idl_model.Schema:
    if text is None:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    return idl_model.parse(text, os.path.relpath(path, ROOT) if os.path.isabs(path) else path)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="compare instead of writing")
    ap.add_argument("--compat-base", metavar="REF", help="git ref whose IDL this one must stay compatible with")
    ap.add_argument("--root", default=ROOT, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    root = args.root
    try:
        schema = load(os.path.join(root, IDL))
        if args.compat_base:
            try:
                old_text = subprocess.run(["git", "-C", root, "show", f"{args.compat_base}:{IDL}"],
                                          check=True, capture_output=True, text=True).stdout
            except subprocess.CalledProcessError:
                print(f"{IDL} doesn't exist at {args.compat_base}: no compatibility to check")
            else:
                old = idl_model.parse(old_text, f"{args.compat_base}:{IDL}")
                errors = idl_model.compat(old, schema)
                if errors:
                    for e in errors:
                        print(f"{IDL}: breaks the evolution rules against {args.compat_base}: {e}",
                              file=sys.stderr)
                    return 1
        files = outputs(schema, root)
    except idl_model.IdlError as e:
        print(e, file=sys.stderr)
        return 1
    stale = []
    for path, text in files.items():
        full = os.path.join(root, path)
        try:
            with open(full, encoding="utf-8") as f:
                current = f.read()
        except FileNotFoundError:
            current = None
        if current == text:
            continue
        stale.append(path)
        if not args.check:
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8") as f:
                f.write(text)
    if args.check:
        if stale:
            print("out of date (run python3 -I tools/idl/axe_idl.py):\n  " + "\n  ".join(stale), file=sys.stderr)
            return 1
        print(f"{len(files)} generated files are up to date")
    else:
        print(f"wrote {len(stale)} of {len(files)} files" + (": " + ", ".join(stale) if stale else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
