#!/usr/bin/env python3
"""Generates src/unicode/ucd_tables.{h,c} from the pinned UCD files.

Usage: tools/unicode/fetch.sh && python3 -I tools/unicode/gen_tables.py

Every code point maps to one small record of the properties the text stack needs (line
break class, bidi class, grapheme break, general category, script, flags, script
extensions). Distinct records are deduplicated and found through a two-stage table:
stage1[cp >> SHIFT] names a block of stage2 entries, and stage2 holds record indices.
The output is deterministic, so regenerating unchanged data gives an identical diff.
"""
import os
import re
import sys

UCD_VERSION = "18.0.0"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UCD = os.path.join(ROOT, "build", "ucd", UCD_VERSION)
OUT = os.path.join(ROOT, "src", "unicode")
MAX_CP = 0x10FFFF

# Enum orders are fixed here, not taken from file order, so values stay stable.
LB = ["XX", "AI", "AK", "AL", "AP", "AS", "B2", "BA", "BB", "BK", "CB", "CJ", "CL", "CM",
      "CP", "CR", "EB", "EM", "EX", "GL", "H2", "H3", "HH", "HL", "HY", "ID", "IN", "IS",
      "JL", "JT", "JV", "LF", "NL", "NS", "NU", "OP", "PO", "PR", "QU", "RI", "SA", "SG",
      "SP", "SY", "VF", "VI", "WJ", "ZW", "ZWJ"]
BC = ["L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S", "WS", "ON",
      "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"]
GCB = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator", "Prepend",
       "SpacingMark", "L", "V", "T", "LV", "LVT"]
GC = ["Cn", "Cc", "Cf", "Co", "Cs", "Ll", "Lm", "Lo", "Lt", "Lu", "Mc", "Me", "Mn", "Nd",
      "Nl", "No", "Pc", "Pd", "Pe", "Pf", "Pi", "Po", "Ps", "Sc", "Sk", "Sm", "So", "Zl",
      "Zp", "Zs"]
INCB = ["None", "Linker", "Consonant", "Extend"]

FLAG_EXTPICT = 1
FLAG_EAST_ASIAN = 2       # East_Asian_Width F, W or H ($EastAsian in UAX #14)
FLAG_DEFAULT_IGNORABLE = 16
INCB_SHIFT = 2            # bits 2-3


def path(name):
    return os.path.join(UCD, name)


def parse(name, fields=2):
    """Yields (first, last, values...) for data lines, @missing lines first in file order."""
    missing, data = [], []
    with open(path(name), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"#\s*@missing:\s*(.*)", line)
            target = missing
            if m:
                line = m.group(1)
            else:
                target = data
                line = line.split("#", 1)[0]
            if not line.strip():
                continue
            parts = [p.strip() for p in line.split(";")]
            rng = parts[0]
            if ".." in rng:
                a, b = rng.split("..")
                first, last = int(a, 16), int(b, 16)
            else:
                first = last = int(rng, 16)
            target.append((first, last, *parts[1:fields]))
    return missing, data


def fill(table, name, convert, fields=2, select=None):
    missing, data = parse(name, fields)
    for rows in (missing, data):
        for first, last, *vals in rows:
            if select and not select(vals):
                continue
            v = convert(vals)
            if v is None:
                continue
            for cp in range(first, last + 1):
                table[cp] = v


def aliases():
    """Long and short property value names -> short name, per property."""
    out = {}
    with open(path("PropertyValueAliases.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            prop, short = parts[0], parts[1]
            m = out.setdefault(prop, {})
            for name in parts[1:]:
                m[name] = short
    return out


def main():
    if not os.path.isdir(UCD):
        sys.exit(f"missing {UCD}: run tools/unicode/fetch.sh first")
    al = aliases()
    n = MAX_CP + 1

    lb = [LB.index("XX")] * n
    fill(lb, "LineBreak.txt", lambda v: LB.index(v[0]))

    bc = [BC.index("L")] * n
    fill(bc, "extracted/DerivedBidiClass.txt", lambda v: BC.index(al["bc"][v[0]]))

    gcb = [0] * n
    fill(gcb, "auxiliary/GraphemeBreakProperty.txt", lambda v: GCB.index(v[0]))

    gc = [0] * n
    fill(gc, "extracted/DerivedGeneralCategory.txt", lambda v: GC.index(v[0]))

    flags = [0] * n
    ea = [False] * n
    fill(ea, "EastAsianWidth.txt", lambda v: v[0] in ("F", "W", "H"))
    extpict = [False] * n
    fill(extpict, "emoji/emoji-data.txt", lambda v: True,
         select=lambda v: v[0] == "Extended_Pictographic")
    di = [False] * n
    fill(di, "DerivedCoreProperties.txt", lambda v: True,
         select=lambda v: v[0] == "Default_Ignorable_Code_Point")
    incb = [0] * n
    fill(incb, "DerivedCoreProperties.txt", lambda v: INCB.index(v[1]), fields=3,
         select=lambda v: v[0] == "InCB")
    for cp in range(n):
        flags[cp] = ((FLAG_EXTPICT if extpict[cp] else 0) |
                     (FLAG_EAST_ASIAN if ea[cp] else 0) |
                     (FLAG_DEFAULT_IGNORABLE if di[cp] else 0) |
                     (incb[cp] << INCB_SHIFT))

    # Scripts: Common, Inherited, Unknown first, then the rest by short code.
    sc_short = sorted(set(al["sc"].values()) - {"Zyyy", "Zinh", "Zzzz"})
    scripts = ["Zyyy", "Zinh", "Zzzz"] + sc_short
    assert len(scripts) <= 255
    sc_index = {s: i for i, s in enumerate(scripts)}
    sc = [sc_index["Zzzz"]] * n
    fill(sc, "Scripts.txt", lambda v: sc_index[al["sc"][v[0]]])

    scx_sets = [()]  # index 0: none (use the Script value)
    scx_ids = {(): 0}
    scx = [0] * n

    def scx_convert(v):
        if v[0].startswith("<"):
            return None
        s = tuple(sorted(sc_index[al["sc"][x]] for x in v[0].split()))
        if s not in scx_ids:
            scx_ids[s] = len(scx_sets)
            scx_sets.append(s)
        return scx_ids[s]

    fill(scx, "ScriptExtensions.txt", scx_convert)
    assert len(scx_sets) < 65536

    brackets = []
    _, rows = parse("BidiBrackets.txt", 3)
    for first, _, pair, kind in rows:
        brackets.append((first, int(pair, 16), kind))
    brackets.sort()

    # Records and the two-stage table.
    rec_ids, records = {}, []
    stage = [0] * n
    for cp in range(n):
        r = (lb[cp], bc[cp], gcb[cp], gc[cp], sc[cp], flags[cp], scx[cp])
        i = rec_ids.get(r)
        if i is None:
            i = rec_ids[r] = len(records)
            records.append(r)
        stage[cp] = i
    assert len(records) < 65536

    best = None
    for shift in range(5, 10):
        size = 1 << shift
        blocks, ids, stage1 = [], {}, []
        for start in range(0, n, size):
            b = tuple(stage[start:start + size])
            if b not in ids:
                ids[b] = len(blocks)
                blocks.append(b)
            stage1.append(ids[b])
        total = len(stage1) * 2 + len(blocks) * size * 2
        if best is None or total < best[0]:
            best = (total, shift, stage1, blocks)
    total, shift, stage1, blocks = best

    write_header(scripts, shift, len(records), len(blocks))
    write_source(scripts, shift, stage1, blocks, records, scx_sets, brackets)
    print(f"UCD {UCD_VERSION}: {len(records)} records, {len(blocks)} blocks of {1 << shift}, "
          f"{len(scx_sets)} script-extension sets, {total + len(records) * 8} bytes")


HEADER = """// Generated by tools/unicode/gen_tables.py from UCD {version}. Do not edit.
"""


def enum(name, prefix, values, out):
    out.append(f"typedef enum {name} {{")
    for i, v in enumerate(values):
        out.append(f"    {prefix}{v.upper()} = {i},")
    out.append(f"    {prefix}COUNT_ = {len(values)},")
    out.append(f"}} {name};")
    out.append("")


def write_header(scripts, shift, nrec, nblocks):
    o = [HEADER.format(version=UCD_VERSION) + "#pragma once", "", "#include <stdint.h>", "",
         f'#define UI_UCD_VERSION "{UCD_VERSION}"', f"#define UI_UCD_SHIFT {shift}u", ""]
    enum("ui_lb_class", "UI_LB_", LB, o)
    enum("ui_bidi_class", "UI_BC_", BC, o)
    enum("ui_gcb", "UI_GCB_", GCB, o)
    enum("ui_gc", "UI_GC_", GC, o)
    enum("ui_incb", "UI_INCB_", INCB, o)
    enum("ui_script", "UI_SCRIPT_", scripts, o)
    o += [
        "enum {",
        f"    UI_UCD_EXTPICT = {FLAG_EXTPICT},",
        f"    UI_UCD_EAST_ASIAN = {FLAG_EAST_ASIAN},          // East_Asian_Width F, W or H",
        f"    UI_UCD_DEFAULT_IGNORABLE = {FLAG_DEFAULT_IGNORABLE},",
        f"    UI_UCD_INCB_SHIFT = {INCB_SHIFT},",
        "};",
        "",
        "typedef struct ui_ucd_record {",
        "    uint8_t lb, bidi, gcb, gc, script, flags;",
        "    uint16_t scx; // index into ui_ucd_scx_sets; 0: Script_Extensions = {script}",
        "} ui_ucd_record;",
        "",
        "typedef struct ui_ucd_scx_set {",
        "    uint16_t offset; // into ui_ucd_scx_scripts",
        "    uint16_t count;",
        "} ui_ucd_scx_set;",
        "",
        "typedef struct ui_ucd_bracket {",
        "    uint32_t cp, pair;",
        "    uint8_t open; // 1: Bidi_Paired_Bracket_Type=Open, 0: Close",
        "} ui_ucd_bracket;",
        "",
        "extern const uint16_t ui_ucd_stage1[];",
        "extern const uint16_t ui_ucd_stage2[];",
        "extern const ui_ucd_record ui_ucd_records[];",
        "extern const ui_ucd_scx_set ui_ucd_scx_sets[];",
        "extern const uint8_t ui_ucd_scx_scripts[];",
        "extern const ui_ucd_bracket ui_ucd_brackets[];",
        "extern const uint32_t ui_ucd_bracket_count;",
        "// ISO 15924 tag per ui_script, big-endian ('L' << 24 | 'a' << 16 | 't' << 8 | 'n').",
        "extern const uint32_t ui_ucd_script_tags[];",
        "",
    ]
    with open(os.path.join(OUT, "ucd_tables.h"), "w") as f:
        f.write("\n".join(o))


def array(ctype, name, values, per_line, out, fmt=str):
    out.append(f"const {ctype} {name}[] = {{")
    for i in range(0, len(values), per_line):
        out.append("    " + ", ".join(fmt(v) for v in values[i:i + per_line]) + ",")
    out.append("};")
    out.append("")


def write_source(scripts, shift, stage1, blocks, records, scx_sets, brackets):
    o = [HEADER.format(version=UCD_VERSION) + '#include "unicode/ucd_tables.h"', ""]
    array("uint16_t", "ui_ucd_stage1", stage1, 16, o)
    stage2 = [v for b in blocks for v in b]
    array("uint16_t", "ui_ucd_stage2", stage2, 16, o)
    o.append("const ui_ucd_record ui_ucd_records[] = {")
    for r in records:
        o.append("    {%d, %d, %d, %d, %d, %d, %d}," % r)
    o += ["};", ""]
    flat, sets = [], []
    for s in scx_sets:
        sets.append((len(flat), len(s)))
        flat.extend(s)
    o.append("const ui_ucd_scx_set ui_ucd_scx_sets[] = {")
    for off, cnt in sets:
        o.append(f"    {{{off}, {cnt}}},")
    o += ["};", ""]
    array("uint8_t", "ui_ucd_scx_scripts", flat or [0], 24, o)
    o.append("const ui_ucd_bracket ui_ucd_brackets[] = {")
    for cp, pair, kind in brackets:
        o.append(f"    {{0x{cp:04X}, 0x{pair:04X}, {1 if kind == 'o' else 0}}},")
    o += ["};", f"const uint32_t ui_ucd_bracket_count = {len(brackets)};", ""]

    def tag(s):
        return "0x%08Xu /* %s */" % (int.from_bytes(s.encode(), "big"), s)

    array("uint32_t", "ui_ucd_script_tags", scripts, 4, o, tag)
    with open(os.path.join(OUT, "ucd_tables.c"), "w") as f:
        f.write("\n".join(o))


if __name__ == "__main__":
    main()
