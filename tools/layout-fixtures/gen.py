#!/usr/bin/env python3
"""Turns the layout fixtures and Chrome's recorded boxes into tests/layout/fixtures_generated.c.

Each fixture becomes a list of nodes in document order: its parent, its inline CSS mapped
to Axe's style declarations, its text (for a <p>), and the border box Chrome gave it
relative to its parent. test_layout_conformance builds the same tree through the wire and
compares. Fixtures listed in deviations.md ("- `name`: reason") are expected to fail; the
test fails if one of them passes, so the list can't go stale.

    python3 -I tools/layout-fixtures/gen.py
"""
import html.parser
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "tests" / "layout" / "fixtures_generated.c"

KEYWORDS = {
    "display": ("UI_STYLE_DISPLAY", {"flex": 0, "none": 1}),
    "position": ("UI_STYLE_POSITION", {"relative": 0, "absolute": 1}),
    "flex-direction": ("UI_STYLE_FLEX_DIRECTION", {"row": 0, "row-reverse": 1, "column": 2, "column-reverse": 3}),
    "flex-wrap": ("UI_STYLE_FLEX_WRAP", {"nowrap": 0, "wrap": 1, "wrap-reverse": 2}),
    "justify-content": ("UI_STYLE_JUSTIFY_CONTENT", {"normal": 0, "flex-start": 1, "start": 1, "flex-end": 2, "end": 2,
                        "center": 3, "space-between": 6, "space-around": 7, "space-evenly": 8}),
    "align-items": ("UI_STYLE_ALIGN_ITEMS", {"normal": 0, "flex-start": 1, "start": 1, "flex-end": 2, "end": 2,
                    "center": 3, "baseline": 4, "stretch": 5}),
    "align-self": ("UI_STYLE_ALIGN_SELF", {"auto": 0, "flex-start": 1, "start": 1, "flex-end": 2, "end": 2,
                   "center": 3, "baseline": 4, "stretch": 5}),
    "align-content": ("UI_STYLE_ALIGN_CONTENT", {"normal": 0, "flex-start": 1, "start": 1, "flex-end": 2, "end": 2,
                      "center": 3, "stretch": 5, "space-between": 6, "space-around": 7, "space-evenly": 8}),
    "text-align": ("UI_STYLE_TEXT_ALIGN", {"start": 0, "end": 1, "left": 2, "right": 3, "center": 4, "justify": 5}),
    "direction": ("UI_STYLE_DIRECTION", {"ltr": 0, "rtl": 1}),
}
LENGTHS = {
    "flex-basis": "UI_STYLE_FLEX_BASIS", "width": "UI_STYLE_WIDTH", "height": "UI_STYLE_HEIGHT",
    "min-width": "UI_STYLE_MIN_WIDTH", "min-height": "UI_STYLE_MIN_HEIGHT",
    "max-width": "UI_STYLE_MAX_WIDTH", "max-height": "UI_STYLE_MAX_HEIGHT",
    "row-gap": "UI_STYLE_ROW_GAP", "column-gap": "UI_STYLE_COLUMN_GAP",
    "top": "UI_STYLE_TOP", "right": "UI_STYLE_RIGHT", "bottom": "UI_STYLE_BOTTOM", "left": "UI_STYLE_LEFT",
    "border-width": "UI_STYLE_BORDER_WIDTH",
    "margin-inline-start": "UI_STYLE_MARGIN_START", "margin-inline-end": "UI_STYLE_MARGIN_END",
    "padding-inline-start": "UI_STYLE_PADDING_START", "padding-inline-end": "UI_STYLE_PADDING_END",
    "inset-inline-start": "UI_STYLE_INSET_START", "inset-inline-end": "UI_STYLE_INSET_END",
}
NUMBERS = {"flex-grow": "UI_STYLE_FLEX_GROW", "flex-shrink": "UI_STYLE_FLEX_SHRINK",
           "aspect-ratio": "UI_STYLE_ASPECT_RATIO"}
SIDES = ["TOP", "RIGHT", "BOTTOM", "LEFT"]


def length(v):
    v = v.strip()
    if v == "auto":
        return "{.kind = UI_VALUE_AUTO}"
    if v == "none":
        return "{.kind = UI_VALUE_NONE}"
    if v.endswith("%"):
        return f"{{.kind = UI_VALUE_PERCENT, .number = {float(v[:-1])!r}f}}"
    if v.endswith("px"):
        v = v[:-2]
    return f"{{.kind = UI_VALUE_PX, .number = {float(v)!r}f}}"


def number(v):
    return f"{{.kind = UI_VALUE_NUMBER, .number = {float(v)!r}f}}"


def boxes4(values):
    v = values.split()
    if len(v) == 1:
        v *= 4
    elif len(v) == 2:
        v = [v[0], v[1], v[0], v[1]]
    elif len(v) == 3:
        v = [v[0], v[1], v[2], v[1]]
    return v


def decls(css):
    """Inline CSS to C initializers of ui_style_decl, plus the font size (or None)."""
    out, font_size = [], None
    for part in filter(None, (s.strip() for s in css.split(";"))):
        name, value = (x.strip() for x in part.split(":", 1))
        if name in KEYWORDS:
            prop, table = KEYWORDS[name]
            out.append(f"{{{prop}, {{.kind = UI_VALUE_KEYWORD, .keyword = {table[value]}}}}}")
        elif name in LENGTHS:
            out.append(f"{{{LENGTHS[name]}, {length(value)}}}")
        elif name in NUMBERS:
            out.append(f"{{{NUMBERS[name]}, {number(value)}}}")
        elif name == "flex":
            g, s, b = value.split()
            out += [f"{{UI_STYLE_FLEX_GROW, {number(g)}}}", f"{{UI_STYLE_FLEX_SHRINK, {number(s)}}}",
                    f"{{UI_STYLE_FLEX_BASIS, {length(b)}}}"]
        elif name == "gap":
            v = value.split()
            row, col = (v[0], v[0]) if len(v) == 1 else (v[0], v[1])
            out += [f"{{UI_STYLE_ROW_GAP, {length(row)}}}", f"{{UI_STYLE_COLUMN_GAP, {length(col)}}}"]
        elif name in ("margin", "padding"):
            for side, v in zip(SIDES, boxes4(value)):
                out.append(f"{{UI_STYLE_{name.upper()}_{side}, {length(v)}}}")
        elif re.fullmatch(r"(margin|padding)-(top|right|bottom|left)", name):
            kind, side = name.split("-")
            out.append(f"{{UI_STYLE_{kind.upper()}_{side.upper()}, {length(value)}}}")
        elif name == "font-size":
            font_size = float(value.removesuffix("px"))
        else:
            raise SystemExit(f"unsupported property {name}")
    return out, font_size


class Tree(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.nodes, self.stack = [], []

    def handle_starttag(self, tag, attrs):
        if tag not in ("div", "p"):
            raise SystemExit(f"unsupported element {tag}")
        parent = self.stack[-1] if self.stack else -1
        a = dict(attrs)
        style = a.get("style", "")
        if "dir" in a:  # the dir attribute sets the direction (before the inline style, as in CSS)
            style = f"direction:{a['dir']};" + style
        self.nodes.append({"tag": tag, "style": style, "parent": parent, "text": ""})
        self.stack.append(len(self.nodes) - 1)

    def handle_endtag(self, tag):
        self.stack.pop()

    def handle_data(self, data):
        if self.stack and self.nodes[self.stack[-1]]["tag"] == "p":
            self.nodes[self.stack[-1]]["text"] += data


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


SCALES = [("expected", "100"), ("expected-125", "125"), ("expected-150", "150")]


def deviations():
    """Fixtures expected to fail: "name" (every scale) or "name@125" (one scale)."""
    out = {}
    path = HERE / "deviations.md"
    if path.exists():
        for m in re.finditer(r"^- `([a-z0-9_-]+(?:@(?:100|125|150))?)`: (.+)$",
                             path.read_text(encoding="utf-8"), re.M):
            out[m.group(1)] = m.group(2)
    return out


def main() -> int:
    xfail = deviations()
    lines = ["// Generated by tools/layout-fixtures/gen.py from the fixtures and Chrome's results.",
             "// Do not edit: change the fixtures and run make_fixtures.py, record.py and gen.py.",
             '#include "layout_fixtures.h"', ""]
    table = []
    names = sorted(p.stem for p in (HERE / "fixtures").glob("*.html"))
    for k, name in enumerate(names):
        tree = Tree()
        tree.feed((HERE / "fixtures" / f"{name}.html").read_text(encoding="utf-8"))
        results = [json.loads((HERE / d / f"{name}.json").read_text()) for d, _ in SCALES]
        expected = results[0]
        for r in results:
            if len(r["boxes"]) != len(tree.nodes):
                raise SystemExit(f"{name}: {len(tree.nodes)} nodes, {len(r['boxes'])} boxes")
        rows = []
        for i, n in enumerate(tree.nodes):
            d, size = decls(n["style"])
            if d:
                lines.append(f"static const ui_style_decl f{k}_{i}[] = {{{', '.join(d)}}};")
            text = c_string(n["text"]) if n["tag"] == "p" else "NULL"
            ref = f"f{k}_{i}" if d else "NULL"
            per_scale = ", ".join("{" + ", ".join(repr(float(v)) + "f" for v in r["boxes"][i]) + "}"
                                  for r in results)
            rows.append(f"    {{{n['parent']}, {ref}, {len(d)}, {text}, {float(size or 16)!r}f, {{{per_scale}}}}},")
        lines.append(f"static const layout_fixture_node f{k}[] = {{")
        lines += rows
        lines.append("};")
        has_text = any(n["tag"] == "p" for n in tree.nodes)
        w, h = expected["surface"]
        fails = ", ".join(str(name in xfail or f"{name}@{s}" in xfail).lower() for _, s in SCALES)
        table.append(f'    {{"{name}", {w}, {h}, f{k}, {len(tree.nodes)}, {str(has_text).lower()}, {{{fails}}}}},')
        lines.append("")
    unknown = {x.split("@")[0] for x in xfail} - set(names)
    if unknown:
        raise SystemExit(f"deviations.md names unknown fixtures: {', '.join(sorted(unknown))}")
    lines.append("const layout_fixture layout_fixtures[] = {")
    lines += table
    lines.append("};")
    lines.append(f"const uint32_t layout_fixture_count = {len(table)};")
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(table)} fixtures, {len(xfail)} expected to fail", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
