"""Documentation output: the tables of docs/command-buffer.md, between
`<!-- idl:NAME -->` and `<!-- /idl:NAME -->` markers."""

from __future__ import annotations

import re

from idl_model import HANDLES, Field, IdlError, Record, Schema

TYPE_NAMES = {"u8": "u8", "u16": "u16", "u32": "u32", "u64": "u64", "f32": "f32"}


def bytes_column(r: Record) -> str:
    if not r.tail:
        return str(r.size)
    if len(r.tail) > 1:
        return f"{r.size} + variable"
    ti = r.tail[0]
    if ti.kind == "array" and not ti.record:
        return f"{r.size} + {4 if ti.elem in ('u32', 'f32') else 2 if ti.elem == 'u16' else 1}·{ti.count_field}"
    return f"{r.size} + {ti.name}"


def message_table(s: Schema, group: list[Record], noun: str) -> str:
    rows = [f"| Opcode | {noun} | Payload | Bytes |", "| --- | --- | --- | --- |"]
    for r in group:
        rows.append(f"| {r.opcode} | `{r.name}` | {r.doc} | {bytes_column(r)} |")
    return "\n".join(rows)


def type_name(f: Field) -> str:
    if f.record:
        return f.record.name
    if f.type.base in HANDLES:
        return "handle"
    return f.type.storage or TYPE_NAMES.get(f.type.base, f.type.base)


def walk(r: Record, prefix: str, base: int, skip: set) -> list[tuple[str, str]]:
    """(change, description) of the fields of r, and of records nested in it, that were
    appended or took reserved bytes; offsets from `base`."""
    out = []
    for f in r.fields:
        if f.since:
            took = ", formerly reserved" if f.offset < r.original else ""
            out.append((f.since, f"{prefix}`{f.name}` ({type_name(f)} at {base + f.offset}{took})"))
        if f.record and f.record.name not in skip:
            out += walk(f.record, prefix, base + f.offset, skip)
    return out


def appended(r: Record) -> list[tuple[str, str]]:
    """What was appended to an op: its own fields, then those of the records in its tail
    (reported per record, so a record also nested in the header is listed once)."""
    in_tail = [t.record for t in r.tail if t.kind == "array" and t.record and not t.record.size_field]
    nested = {f.record.name for rec in in_tail for f in rec.fields if f.record}
    out = walk(r, "", 0, nested)
    for rec in in_tail:
        out += walk(rec, rec.name.split("_")[-1] + " ", 0, set())
    return out


def history_table(s: Schema) -> str:
    rows = ["| Op | Original size | Appended |", "| --- | --- | --- |"]
    order = list(s.changes)
    for r in s.messages():
        items = appended(r)
        if not items:
            continue
        sizes = [f"header {r.original}" if r.tail else str(r.original)]
        for t in r.tail:
            if t.kind == "array" and t.record and not t.record.size_field and any(
                    f.since or (f.record and any(x.since for x in f.record.fields)) for f in t.record.fields):
                sizes.append(f"{t.record.name.split('_')[-1]} {t.record.original}")
        groups = []
        for change in order:
            names = [d for c, d in items if c == change]
            if names:
                joined = names[0] if len(names) == 1 else ", ".join(names[:-1]) + " and " + names[-1]
                groups.append(f'{joined}: {change}, "{s.changes[change]}"')
        rows.append(f"| `{r.name}` ({r.opcode}) | {', '.join(sizes)} | {'. '.join(groups)} |")
    return "\n".join(rows)


def property_table(s: Schema) -> str:
    e = s.enums["property"]
    rows = ["| Id | Property | Components |", "| --- | --- | --- |"]
    for v in e.values:
        extra = f": {v.doc}" if v.doc else ""
        rows.append(f"| {v.value} | {v.name} | {v.components}{extra} |")
    return "\n".join(rows)


def replace(text: str, name: str, body: str, path: str) -> str:
    pattern = re.compile(rf"(<!-- idl:{name} -->\n).*?(\n<!-- /idl:{name} -->)", re.S)
    if not pattern.search(text):
        raise IdlError(path, 1, f"missing the <!-- idl:{name} --> ... <!-- /idl:{name} --> markers")
    return pattern.sub(lambda m: m.group(1) + body + m.group(2), text, count=1)


def documents(s: Schema):
    path = "docs/command-buffer.md"

    def render(text: str) -> str:
        text = replace(text, "commands", message_table(s, s.commands, "Op"), path)
        text = replace(text, "spring-properties", property_table(s), path)
        text = replace(text, "events", message_table(s, s.events, "Event"), path)
        return replace(text, "history", history_table(s), path)

    return {path: render}
