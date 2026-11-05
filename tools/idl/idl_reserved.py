"""Every IDL name against each target's reserved words (test_axe_idl.py, CTest idl_generator).

A name a target can't use as it is has to be handled by that target's generator, so a new
collision fails the test until someone has looked at it and said why it's safe here: ALLOWED."""

from __future__ import annotations

from idl_luau import KEYWORDS as LUAU
from idl_model import Schema
from idl_ts import camel

# C23.
C = {"alignas", "alignof", "auto", "bool", "break", "case", "char", "const", "constexpr", "continue", "default",
     "do", "double", "else", "enum", "extern", "false", "float", "for", "goto", "if", "inline", "int", "long",
     "nullptr", "register", "restrict", "return", "short", "signed", "sizeof", "static", "static_assert", "struct",
     "switch", "thread_local", "true", "typedef", "typeof", "typeof_unqual", "union", "unsigned", "void",
     "volatile", "while"}

# ECMAScript's reserved words, strict mode's (generated code is modules) and `await`. TypeScript's
# own (`type`, `number`) are contextual: they work as identifiers.
TYPESCRIPT = {"await", "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete",
              "do", "else", "enum", "export", "extends", "false", "finally", "for", "function", "if",
              "implements", "import", "in", "instanceof", "interface", "let", "new", "null", "package", "private",
              "protected", "public", "return", "static", "super", "switch", "this", "throw", "true", "try",
              "typeof", "var", "void", "while", "with", "yield"}

# target -> name -> why it is safe there.
ALLOWED: dict[str, dict[str, str]] = {
    "c": {
        "auto": "an enum value, written UI_VALUE_AUTO and UI_TEXT_DIR_AUTO",
    },
    "typescript": {},
    "luau": {
        "end": "idl_luau quotes keywords: [\"end\"] in tables, v[\"end\"] in fields, end_ as a parameter",
    },
}

SPELLING = {"c": (C, str), "typescript": (TYPESCRIPT, camel), "luau": (set(LUAU), str)}


def names(s: Schema) -> dict[str, set[str]]:
    """Every name the schema gives something, with what it names."""
    out: dict[str, set[str]] = {}

    def add(name: str, what: str):
        out.setdefault(name, set()).add(what)

    for e in s.enums.values():
        add(e.name, "enum")
        for v in e.values:
            add(v.name, f"value of {e.name}")
    for r in [*s.records.values(), *s.commands, *s.events]:
        add(r.name, r.kind)
        for f in r.fields:
            add(f.name, f"field of {r.name}")
        for t in r.tail:
            add(t.name, f"tail of {r.name}")
    for u in s.unions.values():
        add(u.name, "union")
        for v in u.variants:
            for f in v.fields:
                for part in f.name.split("."):
                    add(part, f"member of {u.name}")
    for p in s.style_props:
        add(p.name, "style property")
    return out


def collisions(s: Schema) -> dict[str, dict[str, set[str]]]:
    """target -> IDL name -> what it names, for each name that target reserves."""
    found: dict[str, dict[str, set[str]]] = {}
    for target, (reserved, spell) in SPELLING.items():
        found[target] = {n: w for n, w in names(s).items() if spell(n) in reserved}
    return found
