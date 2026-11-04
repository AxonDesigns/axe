"""Parser, layout and rule checks for the wire IDL (grammar: docs/idl.md).

Everything here is pure: parse() turns text into a Schema, or raises IdlError naming the
file and line. The emitters (idl_c, idl_ts, idl_docs) only read a checked Schema.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field as dc_field


class IdlError(Exception):
    def __init__(self, path: str, line: int, message: str):
        super().__init__(f"{path}:{line}: {message}")
        self.path, self.line, self.message = path, line, message


SCALARS = {"u8": 1, "u16": 2, "u32": 4, "u64": 8, "f32": 4}
HANDLES = ("node", "handle")  # 8 bytes: u32 index, u32 generation
INT_LIMITS = {"u8": 0xFF, "u16": 0xFFFF, "u32": 0xFFFFFFFF, "u64": 0xFFFFFFFFFFFFFFFF}
NAME = re.compile(r"[a-z][a-z0-9_]*$")


# ---- Model ------------------------------------------------------------------------------


@dataclass
class EnumValue:
    name: str
    value: int
    doc: str = ""
    components: int | None = None  # enum property: float components of the value
    min: float | None = None       # enum property: smallest value of each component
    line: int = 0


@dataclass
class Enum:
    name: str
    storage: str           # u8, u16 or u32
    bitmask: bool
    values: list[EnumValue]
    c_type: str = ""
    c_prefix: str = ""
    doc: str = ""
    line: int = 0

    def value(self, name: str) -> EnumValue | None:
        return next((v for v in self.values if v.name == name), None)


@dataclass
class Check:
    """A value constraint. kind: finite, nonzero, known, utf8, no_nul, whole, range, min_of,
    components_of."""
    kind: str
    lo: float | None = None
    hi: float | None = None
    lo_open: bool = False
    hi_open: bool = False
    ref: str = ""  # min_of, components_of: the field it refers to


@dataclass
class Type:
    """A field's type. base: a scalar, a handle kind, or the name of an enum, bitmask or record.
    storage: the scalar it is stored as (enums and bitmasks may override theirs)."""
    base: str
    storage: str = ""
    count: int = 1         # fixed arrays: f32[4]
    time: bool = False     # u64 holding animation-clock nanoseconds (may exceed 2^53)


@dataclass
class Field:
    name: str
    type: Type
    size: int = 0
    offset: int = 0
    assign: str = ""       # "", count, header, header0, stride, size, words
    assign_ref: str = ""   # the tail element count/stride refer to
    checks: list[Check] = dc_field(default_factory=list)
    since: str = ""
    default: float = 0
    doc: str = ""
    line: int = 0
    record: "Record | None" = None  # nested record type
    enum: Enum | None = None

    @property
    def reserved(self) -> bool:
        return self.name == "reserved"


@dataclass
class TailItem:
    """Variable data after a fixed part, in order.
    kind: array (count_field elements of a scalar or fixed record, or of a self-sized record),
    bytes (count_field bytes, padded to `pad`, or required to be a `multiple` of 4), single
    (one self-sized record), union (a union selected by kind_field)."""
    name: str
    kind: str
    elem: str = ""
    count_field: str = ""
    pad: int = 1
    multiple: int = 1
    checks: list[Check] = dc_field(default_factory=list)
    doc: str = ""
    line: int = 0
    record: "Record | None" = None
    union: "Union | None" = None


@dataclass
class Record:
    name: str
    kind: str              # record, command, event
    fields: list[Field]
    tail: list[TailItem]
    opcode: int = 0
    original: int | None = None
    doc: str = ""
    line: int = 0
    size: int = 0          # fixed part, this version

    def field(self, name: str) -> Field | None:
        return next((f for f in self.fields if f.name == name), None)

    def tail_item(self, name: str) -> TailItem | None:
        return next((t for t in self.tail if t.name == name), None)

    @property
    def header_field(self) -> Field | None:
        return next((f for f in self.fields if f.assign in ("header", "header0")), None)

    @property
    def size_field(self) -> Field | None:
        return next((f for f in self.fields if f.assign in ("size", "words")), None)


@dataclass
class UnionVariant:
    kind: str              # a value of the union's enum
    fields: list[Field]
    line: int = 0


@dataclass
class Union:
    name: str
    enum: Enum
    variants: list[UnionVariant]
    c_type: str = ""
    line: int = 0

    def variant(self, kind: str) -> UnionVariant | None:
        return next((v for v in self.variants if v.kind == kind), None)


@dataclass
class ValueSpec:
    """One alternative a style property takes: kind (a value_kind name, or transition-only),
    a range, whole, an enum and the keywords allowed (keyword), a byte limit (string)."""
    kind: str
    check: Check | None = None
    whole: bool = False
    enum: str = ""
    allowed: list[str] = dc_field(default_factory=list)
    max_bytes: int | None = None


@dataclass
class StyleProp:
    id: int
    name: str
    values: list[ValueSpec]
    flags: set[str]
    doc: str = ""
    section: str = ""      # a comment heading the property in generated enums
    line: int = 0

    @property
    def declarable(self) -> bool:
        return not any(v.kind == "transition-only" for v in self.values)


@dataclass
class Schema:
    path: str
    version: int = 0
    magic: dict[str, tuple[int, str]] = dc_field(default_factory=dict)
    changes: dict[str, str] = dc_field(default_factory=dict)
    enums: dict[str, Enum] = dc_field(default_factory=dict)
    records: dict[str, Record] = dc_field(default_factory=dict)
    unions: dict[str, Union] = dc_field(default_factory=dict)
    commands: list[Record] = dc_field(default_factory=list)
    events: list[Record] = dc_field(default_factory=list)
    style_props: list[StyleProp] = dc_field(default_factory=list)
    style_c: tuple[str, str] = ("", "")
    style_doc: str = ""
    token_values: list[ValueSpec] = dc_field(default_factory=list)
    transition_values: list[ValueSpec] = dc_field(default_factory=list)

    def messages(self) -> list[Record]:
        return self.commands + self.events

    def style_prop(self, name: str) -> StyleProp | None:
        return next((p for p in self.style_props if p.name == name), None)


# ---- Tokenizer ----------------------------------------------------------------------------


@dataclass
class Line:
    number: int
    indent: int
    tokens: list[str]   # strings keep their quotes, so they stay distinguishable
    doc: str            # the line's last quoted string, if it ends with one


def tokenize(path: str, text: str) -> list[Line]:
    lines = []
    for number, raw in enumerate(text.splitlines(), 1):
        if "\t" in raw:
            raise IdlError(path, number, "tabs are not allowed; indent with spaces")
        tokens, i, n = [], 0, len(raw)
        while i < n:
            c = raw[i]
            if c == "#":
                break
            if c == " ":
                i += 1
                continue
            if c == '"':
                j, out = i + 1, []
                while True:
                    if j >= n:
                        raise IdlError(path, number, "unterminated string")
                    if raw[j] == "\\" and j + 1 < n and raw[j + 1] in '"\\':
                        out.append(raw[j + 1])
                        j += 2
                        continue
                    if raw[j] == '"':
                        break
                    out.append(raw[j])
                    j += 1
                tokens.append('"' + "".join(out))
                i = j + 1
                continue
            if c in ",|":
                tokens.append(c)
                i += 1
                continue
            j = i
            while j < n and raw[j] not in ' ",|#':
                j += 1
            tokens.append(raw[i:j])
            i = j
        if not tokens:
            continue
        doc = ""
        if tokens[-1].startswith('"'):
            doc = tokens.pop()[1:]
        indent = len(raw) - len(raw.lstrip(" "))
        lines.append(Line(number, indent, tokens, doc))
    return lines


def is_string(tok: str) -> bool:
    return tok.startswith('"')


# ---- Parser -------------------------------------------------------------------------------


class Parser:
    def __init__(self, path: str, text: str):
        self.path = path
        self.lines = tokenize(path, text)
        self.pos = 0
        self.schema = Schema(path)

    def error(self, line: int | Line, message: str):
        raise IdlError(self.path, line.number if isinstance(line, Line) else line, message)

    def block(self) -> list[Line]:
        """The indented lines after the current top-level line."""
        out = []
        while self.pos < len(self.lines) and self.lines[self.pos].indent > 0:
            out.append(self.lines[self.pos])
            self.pos += 1
        return out

    def parse(self) -> Schema:
        seen_wire = False
        while self.pos < len(self.lines):
            line = self.lines[self.pos]
            self.pos += 1
            if line.indent:
                self.error(line, "unexpected indented line")
            t = line.tokens
            key = t[0]
            if key == "wire":
                self.expect_len(line, 2)
                self.schema.version = self.integer(line, t[1])
                seen_wire = True
            elif key == "magic":
                self.expect_len(line, 3)
                if t[1] not in ("commands", "events"):
                    self.error(line, "magic is for `commands` or `events`")
                if not line.doc or len(line.doc) != 4:
                    self.error(line, "magic needs its four ASCII letters as a string")
                value = self.integer(line, t[2])
                if value.to_bytes(4, "little").decode("latin-1") != line.doc:
                    self.error(line, f"magic {t[2]} is not \"{line.doc}\" in little-endian order")
                self.schema.magic[t[1]] = (value, line.doc)
            elif key == "changes":
                self.expect_len(line, 1)
                for item in self.block():
                    if len(item.tokens) != 1 or not item.doc:
                        self.error(item, "a change is `<id> \"description\"`")
                    if item.tokens[0] in self.schema.changes:
                        self.error(item, f"change {item.tokens[0]} listed twice")
                    self.schema.changes[item.tokens[0]] = item.doc
            elif key in ("enum", "bitmask"):
                self.parse_enum(line, key == "bitmask")
            elif key == "record":
                self.parse_record(line, "record")
            elif key in ("command", "event"):
                self.parse_record(line, key)
            elif key == "union":
                self.parse_union(line)
            elif key == "style_properties":
                self.parse_style_props(line)
            elif key == "token_values":
                self.schema.token_values = self.parse_values(line, t[1:], allow_any=True)
            elif key == "transition_values":
                self.schema.transition_values = self.parse_values(line, t[1:])
            else:
                self.error(line, f"unknown statement `{key}`")
        if not seen_wire:
            raise IdlError(self.path, 1, "missing `wire <version>`")
        for which in ("commands", "events"):
            if which not in self.schema.magic:
                raise IdlError(self.path, 1, f"missing `magic {which}`")
        return self.schema

    # -- helpers --

    def expect_len(self, line: Line, n: int):
        if len(line.tokens) != n:
            self.error(line, f"expected {n} word(s), got {len(line.tokens)}")

    def integer(self, line: Line, tok: str) -> int:
        try:
            return int(tok, 0)
        except ValueError:
            self.error(line, f"`{tok}` is not an integer")

    def number(self, line: Line, tok: str) -> float:
        try:
            v = float(int(tok, 0)) if re.fullmatch(r"-?(0x[0-9a-fA-F]+|\d+)", tok) else float(tok)
        except ValueError:
            self.error(line, f"`{tok}` is not a number")
        if not math.isfinite(v):
            self.error(line, f"`{tok}` is not a finite number")
        return v

    def name(self, line: Line, tok: str, what: str) -> str:
        if not NAME.match(tok):
            self.error(line, f"{what} `{tok}` must be lower_snake_case")
        return tok

    def define(self, line: Line, name: str):
        s = self.schema
        if name in s.enums or name in s.records or name in s.unions or name in SCALARS or name in HANDLES:
            self.error(line, f"`{name}` is already defined")

    # -- enums --

    def parse_enum(self, line: Line, bitmask: bool):
        t = line.tokens
        if len(t) < 4 or t[2] != ":":
            self.error(line, f"expected `{t[0]} <name> : <u8|u16|u32> [c <type> <PREFIX>]`")
        name = self.name(line, t[1], t[0])
        self.define(line, name)
        storage = t[3]
        if storage not in ("u8", "u16", "u32"):
            self.error(line, f"{t[0]} storage must be u8, u16 or u32, not `{storage}`")
        c_type = c_prefix = ""
        rest = t[4:]
        if rest:
            if len(rest) != 3 or rest[0] != "c":
                self.error(line, "expected `c <c_type> <C_PREFIX_>` after the storage")
            c_type, c_prefix = rest[1], rest[2]
        e = Enum(name, storage, bitmask, [], c_type, c_prefix, line.doc, line.number)
        limit = INT_LIMITS[storage]
        for item in self.block():
            it = item.tokens
            if len(it) < 3 or it[1] != "=":
                self.error(item, "an enum value is `<name> = <integer> [components N] [min X]`")
            vname = self.name(item, it[0], "value")
            if e.value(vname):
                self.error(item, f"value `{vname}` listed twice")
            v = EnumValue(vname, self.integer(item, it[2]), item.doc, line=item.number)
            if not 0 <= v.value <= limit:
                self.error(item, f"{v.value} doesn't fit {storage}")
            if not bitmask and any(x.value == v.value for x in e.values):
                self.error(item, f"value {v.value} is used twice")
            k = 3
            while k < len(it):
                if it[k] == "components" and k + 1 < len(it):
                    v.components = self.integer(item, it[k + 1])
                elif it[k] == "min" and k + 1 < len(it):
                    v.min = self.number(item, it[k + 1])
                else:
                    self.error(item, f"unexpected `{it[k]}`")
                k += 2
            e.values.append(v)
        if not e.values:
            self.error(line, f"{t[0]} `{name}` has no values")
        self.schema.enums[name] = e

    # -- types and checks --

    def parse_type(self, line: Line, toks: list[str], k: int) -> tuple[Type, int]:
        """Reads a type at toks[k]: `u32`, `f32[4]`, `u64 time`, `<enum>`, `<enum> : u8`."""
        tok = toks[k]
        m = re.fullmatch(r"([a-z0-9_]+)(?:\[(\d+)\])?", tok)
        if not m:
            self.error(line, f"`{tok}` is not a type")
        ty = Type(m.group(1), count=int(m.group(2)) if m.group(2) else 1)
        k += 1
        if ty.count < 1:
            self.error(line, "a fixed array needs at least one element")
        if k + 1 < len(toks) and toks[k] == ":":
            ty.storage = toks[k + 1]
            if ty.storage not in ("u8", "u16", "u32"):
                self.error(line, f"storage must be u8, u16 or u32, not `{ty.storage}`")
            k += 2
        if k < len(toks) and toks[k] == "time":
            ty.time = True
            k += 1
        return ty, k

    CHECK_WORDS = ("finite", "nonzero", "known", "utf8", "no_nul", "whole")

    def parse_checks(self, line: Line, toks: list[str], k: int, stop=()) -> tuple[list[Check], int]:
        checks = []
        while k < len(toks) and toks[k] not in stop:
            tok = toks[k]
            if tok in self.CHECK_WORDS:
                checks.append(Check(tok))
                k += 1
            elif tok in (">=", ">", "<=", "<"):
                if k + 1 >= len(toks):
                    self.error(line, f"`{tok}` needs a value")
                arg = toks[k + 1]
                m = re.fullmatch(r"min\(([a-z0-9_]+)\)", arg)
                if m:
                    if tok != ">=":
                        self.error(line, "only `>= min(<field>)` is supported")
                    checks.append(Check("min_of", ref=m.group(1)))
                else:
                    v = self.number(line, arg)
                    c = Check("range")
                    if tok in (">=", ">"):
                        c.lo, c.lo_open = v, tok == ">"
                    else:
                        c.hi, c.hi_open = v, tok == "<"
                    checks.append(c)
                k += 2
            elif re.fullmatch(r"components\([a-z0-9_]+\)", tok):
                checks.append(Check("components_of", ref=tok[11:-1]))
                k += 1
            elif ".." in tok:
                checks.append(self.parse_range(line, tok))
                k += 1
            else:
                break
        return checks, k

    def parse_range(self, line: Line, tok: str) -> Check:
        m = re.fullmatch(r"(-?[0-9.]+(?:e-?\d+)?)?(<)?\.\.(<)?(-?[0-9.]+(?:e-?\d+)?)?", tok)
        if not m or (m.group(1) is None and m.group(4) is None):
            self.error(line, f"`{tok}` is not a range (`A..B`, `A<..B`, `A..<B`, `A<..`, `..B`)")
        c = Check("range", lo_open=bool(m.group(2)), hi_open=bool(m.group(3)))
        if m.group(1) is not None:
            c.lo = self.number(line, m.group(1))
        elif c.lo_open:
            self.error(line, f"`{tok}`: `<` needs a lower bound")
        if m.group(4) is not None:
            c.hi = self.number(line, m.group(4))
        elif c.hi_open:
            self.error(line, f"`{tok}`: `<` needs an upper bound")
        if c.lo is not None and c.hi is not None and c.lo > c.hi:
            self.error(line, f"`{tok}` is empty")
        return c

    # -- records, commands, events --

    def parse_record(self, line: Line, kind: str):
        t = line.tokens
        k = 2
        if len(t) < 2:
            self.error(line, f"expected `{kind} <name>`")
        name = self.name(line, t[1], kind)
        r = Record(name, kind, [], [], doc=line.doc, line=line.number)
        if kind != "record":
            if len(t) < 4 or t[2] != "=":
                self.error(line, f"expected `{kind} <name> = <opcode> original <bytes>`")
            r.opcode = self.integer(line, t[3])
            if not 1 <= r.opcode <= 0xFFFF:
                self.error(line, "opcodes are 1 to 65535")
            k = 4
        else:
            self.define(line, name)
        while k < len(t):
            if t[k] == "original" and k + 1 < len(t):
                r.original = self.integer(line, t[k + 1])
                k += 2
            else:
                self.error(line, f"unexpected `{t[k]}`")
        if kind != "record" and r.original is None:
            self.error(line, f"{kind} `{name}` must state its original size (`original <bytes>`)")
        in_tail = False
        for item in self.block():
            if item.tokens == ["tail"]:
                if in_tail:
                    self.error(item, "`tail` given twice")
                in_tail = True
                continue
            if in_tail:
                r.tail.append(self.parse_tail_item(item))
            else:
                r.fields.append(self.parse_field(item))
        if not r.fields and not r.tail:
            self.error(line, f"{kind} `{name}` has no fields")
        if kind == "record":
            self.schema.records[name] = r
        else:
            group = self.schema.commands if kind == "command" else self.schema.events
            for other in group:
                if other.name == name:
                    self.error(line, f"{kind} `{name}` is defined twice")
                if other.opcode == r.opcode:
                    self.error(line, f"opcode {r.opcode} is already {kind} `{other.name}`")
            group.append(r)

    def parse_field(self, item: Line) -> Field:
        toks = item.tokens
        if len(toks) < 2:
            self.error(item, "a field is `<name> <type> [= assign] [checks] [since <change>]`")
        name = self.name(item, toks[0], "field")
        ty, k = self.parse_type(item, toks, 1)
        f = Field(name, ty, doc=item.doc, line=item.number)
        if k < len(toks) and toks[k] == "=":
            if k + 1 >= len(toks):
                self.error(item, "`=` needs what the field holds")
            a = toks[k + 1]
            m = re.fullmatch(r"(count|stride)\(([a-z0-9_]+)\)", a)
            if m:
                f.assign, f.assign_ref = m.group(1), m.group(2)
            elif a in ("header", "size"):
                f.assign = a
            elif a == "header(0)":
                f.assign = "header0"
            elif a == "size/4":
                f.assign = "words"
            else:
                self.error(item, f"`= {a}`: a field can hold count(<tail>), stride(<tail>), header, "
                                 "header(0), size or size/4")
            k += 2
        f.checks, k = self.parse_checks(item, toks, k)
        while k < len(toks):
            if toks[k] == "since" and k + 1 < len(toks):
                f.since = toks[k + 1]
                k += 2
            elif toks[k] == "default" and k + 1 < len(toks):
                f.default = self.number(item, toks[k + 1])
                k += 2
            else:
                self.error(item, f"unexpected `{toks[k]}`")
        return f

    def parse_tail_item(self, item: Line) -> TailItem:
        toks = item.tokens
        if len(toks) < 2:
            self.error(item, "a tail item is `<name> <type>[<count field>]` or `<name> <record>`")
        name = self.name(item, toks[0], "tail item")
        m = re.fullmatch(r"([a-z0-9_]+)(?:\[([a-z0-9_]+)\]|\(([a-z0-9_]+)\))?", toks[1])
        if not m:
            self.error(item, f"`{toks[1]}` is not a tail type")
        elem, count, kind_field = m.group(1), m.group(2), m.group(3)
        if kind_field:
            ti = TailItem(name, "union", elem, kind_field)
        elif count:
            ti = TailItem(name, "bytes" if elem == "bytes" else "array", elem, count)
        else:
            ti = TailItem(name, "single", elem)
        ti.doc, ti.line = item.doc, item.number
        k = 2
        while k < len(toks):
            if toks[k] in ("pad", "multiple") and k + 1 < len(toks):
                v = self.integer(item, toks[k + 1])
                if v not in (1, 2, 4):
                    self.error(item, f"{toks[k]} must be 1, 2 or 4")
                if ti.kind != "bytes":
                    self.error(item, f"`{toks[k]}` applies to bytes only")
                setattr(ti, toks[k], v)
                k += 2
                continue
            checks, k2 = self.parse_checks(item, toks, k)
            if k2 == k:
                self.error(item, f"unexpected `{toks[k]}`")
            ti.checks += checks
            k = k2
        return ti

    # -- unions --

    def parse_union(self, line: Line):
        t = line.tokens
        if len(t) != 6 or t[2] != "by" or t[4] != "c":
            self.error(line, "expected `union <name> by <enum> c <c_type>`")
        name = self.name(line, t[1], "union")
        self.define(line, name)
        enum = self.schema.enums.get(t[3])
        if not enum or enum.bitmask:
            self.error(line, f"`{t[3]}` is not an enum")
        u = Union(name, enum, [], t[5], line.number)
        for item in self.block():
            kind = item.tokens[0]
            if not enum.value(kind):
                self.error(item, f"`{kind}` is not a value of {enum.name}")
            if u.variant(kind):
                self.error(item, f"variant `{kind}` listed twice")
            fields, cur = [], []
            for tok in item.tokens[1:] + [","]:
                if tok == ",":
                    if not cur:
                        self.error(item, "empty field in a variant")
                    if not re.fullmatch(r"[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)?", cur[0]):
                        self.error(item, f"`{cur[0]}` is not a member name")
                    ty, k = self.parse_type(item, cur, 1)
                    f = Field(cur[0], ty, line=item.number)
                    f.checks, k = self.parse_checks(item, cur, k)
                    if k != len(cur):
                        self.error(item, f"unexpected `{cur[k]}`")
                    fields.append(f)
                    cur = []
                else:
                    cur.append(tok)
            u.variants.append(UnionVariant(kind, fields, item.number))
        self.schema.unions[name] = u

    # -- style properties --

    FLAGS = ("inherited", "transition", "layout", "shaping")

    def parse_values(self, line: Line, toks: list[str], allow_any=False) -> list[ValueSpec]:
        if not toks:
            self.error(line, "expected the values the property takes")
        alts, cur = [], []
        for tok in toks + ["|"]:
            if tok == "|":
                if not cur:
                    self.error(line, "empty alternative")
                alts.append(cur)
                cur = []
            else:
                cur.append(tok)
        specs = []
        for alt in alts:
            kind = alt[0]
            spec = ValueSpec(kind)
            rest = alt[1:]
            if kind == "keyword":
                if not rest:
                    self.error(line, "keyword needs an enum (`keyword <enum>` or `keyword <enum>[a b]`)")
                m = re.fullmatch(r"([a-z0-9_]+)(?:\[(.*)\])?", " ".join(rest))
                if not m:
                    self.error(line, f"`{' '.join(rest)}` is not `<enum>[values]`")
                spec.enum = m.group(1)
                spec.allowed = m.group(2).split() if m.group(2) else []
                if spec.enum == "any":
                    if not allow_any:
                        self.error(line, "`keyword any` is for token values only")
                    spec.enum = ""
                rest = []
            elif kind == "string":
                if rest[:1] == ["<="]:
                    spec.max_bytes = self.integer(line, rest[1])
                    rest = rest[2:]
            elif kind in ("px", "percent", "number"):
                checks, k = self.parse_checks(line, rest, 0)
                for c in checks:
                    if c.kind == "range":
                        if spec.check:
                            self.error(line, "one range per alternative")
                        spec.check = c
                    elif c.kind == "whole" and kind == "number":
                        spec.whole = True
                    else:
                        self.error(line, f"`{c.kind}` doesn't apply to {kind}")
                rest = rest[k:]
            elif kind not in ("color", "auto", "none", "token", "spring", "transition-only"):
                self.error(line, f"unknown value kind `{kind}`")
            if rest:
                self.error(line, f"unexpected `{' '.join(rest)}` after {kind}")
            specs.append(spec)
        return specs

    def parse_style_props(self, line: Line):
        t = line.tokens
        if len(t) != 4 or t[1] != "c":
            self.error(line, "expected `style_properties c <c_type> <C_PREFIX_>`")
        self.schema.style_c = (t[2], t[3])
        self.schema.style_doc = line.doc
        section = ""
        for item in self.block():
            it = item.tokens
            if it == ["section"]:
                section = item.doc
                continue
            if len(it) < 3:
                self.error(item, "a property is `<id> <name> <values> [flags]`")
            pid = self.integer(item, it[0])
            name = self.name(item, it[1], "property")
            k = len(it)
            while k > 2 and it[k - 1] in self.FLAGS:
                k -= 1
            p = StyleProp(pid, name, self.parse_values(item, it[2:k]), set(it[k:]), item.doc, section,
                          item.number)
            section = ""
            if not 1 <= pid <= 0x7FFF:
                self.error(item, "property ids are 1 to 32767 (bit 15 marks transitions)")
            for other in self.schema.style_props:
                if other.id == pid or other.name == name:
                    self.error(item, f"property {pid} `{name}` clashes with {other.id} `{other.name}`")
            self.schema.style_props.append(p)


# ---- Layout and rules ---------------------------------------------------------------------


def scalar_size(s: Schema, f: Field, path: str) -> int:
    ty = f.type
    base = ty.base
    if base in SCALARS:
        if ty.storage:
            raise IdlError(path, f.line, f"`: {ty.storage}` applies to enums and bitmasks only")
        if ty.time and base != "u64":
            raise IdlError(path, f.line, "`time` applies to u64 only")
        return SCALARS[base]
    if ty.time:
        raise IdlError(path, f.line, "`time` applies to u64 only")
    if base in HANDLES:
        return 8
    if base in s.enums:
        f.enum = s.enums[base]
        if not ty.storage:
            ty.storage = f.enum.storage
        return SCALARS[ty.storage]
    if base in s.records:
        f.record = s.records[base]
        if f.record.tail:
            raise IdlError(path, f.line, f"record `{base}` has a tail, so it can't be nested")
        return f.record.size
    raise IdlError(path, f.line, f"unknown type `{base}`")


def layout(s: Schema, r: Record):
    path = s.path
    offset = 0
    names = set()
    for f in r.fields:
        if f.name in names and not f.reserved:
            raise IdlError(path, f.line, f"field `{f.name}` is defined twice")
        names.add(f.name)
        size = scalar_size(s, f, path)
        if f.type.count > 1:
            if f.type.base not in SCALARS:
                raise IdlError(path, f.line, "fixed arrays hold scalars only")
            size *= f.type.count
        f.size = size
        align = 4 if f.record or f.type.base in HANDLES else min(SCALARS[f.type.storage or f.type.base], 4)
        if offset % align:
            raise IdlError(path, f.line, f"`{f.name}` at offset {offset} is not {align}-byte aligned")
        f.offset = offset
        offset += size
    r.size = offset
    if r.original is None:
        r.original = _original_floor(r)


def _original_floor(r: Record) -> int:
    """The end of the last field that isn't appended: where appended fields may start."""
    end = 0
    for f in r.fields:
        if not f.since:
            end = f.offset + f.size
    return end


def appended_count_ok(s: Schema, r: Record, f: Field):
    """An appended count must count only arrays that follow every item an original field
    sizes, in a record whose header says where its variable data starts."""
    path = s.path
    counted = [i for i, t in enumerate(r.tail) if t.count_field == f.name]
    if not counted:
        raise IdlError(path, f.line, f"`{f.name}` was appended, but counts nothing")
    if not r.header_field or r.header_field.since:
        raise IdlError(path, f.line, f"`{f.name}` was appended to a record without an original header field")
    by_name = {x.name: x for x in r.fields}
    for i, t in enumerate(r.tail):
        cf = by_name.get(t.count_field)
        if cf is not None and not cf.since and i > min(counted):
            raise IdlError(path, f.line,
                           f"`{f.name}` was appended, so what it counts must come after `{t.name}`")


def check_record(s: Schema, r: Record):
    path = s.path
    what = f"{r.kind} `{r.name}`"
    if r.kind != "record" and r.size % 4:
        raise IdlError(path, r.line, f"{what}: its fixed part is {r.size} bytes, not a multiple of 4")
    if r.original > r.size:
        raise IdlError(path, r.line, f"{what}: original {r.original} is larger than its fields "
                                     f"({r.size} bytes)")
    if r.kind != "record" and r.original % 4:
        raise IdlError(path, r.line, f"{what}: original {r.original} is not a multiple of 4")
    for f in r.fields:
        end = f.offset + f.size
        if f.since and f.since not in s.changes:
            raise IdlError(path, f.line, f"`since {f.since}`: not in `changes`")
        if end > r.original and not f.since and not f.reserved:
            raise IdlError(path, f.line,
                           f"`{f.name}` (offset {f.offset}) lies past {what}'s original size "
                           f"{r.original}: an appended field needs `since <change>`")
        if f.since and f.offset < r.original and end > r.original:
            raise IdlError(path, f.line, f"`{f.name}` straddles the original size {r.original}")
        if f.reserved:
            if f.type.base not in ("u8", "u16", "u32") or f.checks or f.assign:
                raise IdlError(path, f.line, "a reserved field is a plain u8, u16 or u32")
            if f.since:
                raise IdlError(path, f.line, "a reserved field has no `since`")
        if f.since:
            # Appended fields and fields that took reserved bytes default to zero bytes: older
            # writers sent zeros, or nothing, which readers fill with zeros.
            if f.default != 0:
                raise IdlError(path, f.line,
                               f"`{f.name}` was appended ({f.since}), so its default must be 0, "
                               f"not {f.default:g}")
            if f.enum and not f.enum.bitmask and not any(v.value == 0 for v in f.enum.values):
                raise IdlError(path, f.line,
                               f"`{f.name}` was appended, so {f.enum.name} needs a value 0 as its default")
            if f.assign in ("size", "words"):
                raise IdlError(path, f.line,
                               f"`{f.name}` was appended, but a {f.assign} field has no zero default")
            if f.assign == "count":
                # An appended count is 0 from an older writer: no elements, which is what it
                # meant, as long as what it counts is new variable data after the old.
                appended_count_ok(s, r, f)
        elif f.default != 0:
            raise IdlError(path, f.line, "`default` is for appended fields, and must be 0")
        check_field_checks(s, r, f)
    # Assignments refer to tail items; tail items refer to fields.
    for f in r.fields:
        if f.assign in ("count", "stride"):
            ti = r.tail_item(f.assign_ref)
            if not ti:
                raise IdlError(path, f.line, f"`{f.assign}({f.assign_ref})`: no such tail item")
            if f.assign == "stride" and not (ti.kind == "array" and ti.elem in s.records):
                raise IdlError(path, f.line, "stride() applies to an array of records")
        if f.assign and f.type.base not in ("u8", "u16", "u32"):
            raise IdlError(path, f.line, f"`{f.name}` holds a size or count, so it must be u8, u16 or u32")
        if f.assign in ("header", "header0") and not r.tail:
            raise IdlError(path, f.line, f"`= header` needs a tail after the header")
    if sum(1 for f in r.fields if f.assign in ("header", "header0")) > 1:
        raise IdlError(path, r.line, f"{what} has two header sizes")
    if sum(1 for f in r.fields if f.assign in ("size", "words")) > 1:
        raise IdlError(path, r.line, f"{what} has two sizes")
    if r.size_field and r.kind != "record":
        raise IdlError(path, r.size_field.line, "only records carry their own size (= size)")
    if r.tail and not r.header_field and r.size > r.original:
        raise IdlError(path, r.line, f"{what} has no header size, so its variable data starts at its "
                                     "original size: it can't grow its fixed part")
    for ti in r.tail:
        check_tail_item(s, r, ti)
    names = [ti.name for ti in r.tail]
    for f in r.fields:
        if f.name in names:
            raise IdlError(path, f.line, f"`{f.name}` is both a field and a tail item")
    variable = [ti for ti in r.tail if ti.kind in ("single", "union") or
                (ti.kind == "array" and ti.record and ti.record.size_field)]
    if r.tail and not r.header_field and r.kind == "record" and not r.size_field:
        raise IdlError(path, r.line, f"{what} has a tail but no size: add `= size` or `= header`")
    if variable and variable[-1] is not r.tail[-1]:
        raise IdlError(path, variable[0].line, "a self-sized item must come last in a tail")


def check_field_checks(s: Schema, r: Record, f: Field):
    path = s.path
    for c in f.checks:
        if c.kind in ("finite", "whole") and f.type.base != "f32":
            raise IdlError(path, f.line, f"`{c.kind}` applies to f32 fields")
        if c.kind == "known" and not f.enum:
            raise IdlError(path, f.line, "`known` applies to enum and bitmask fields")
        if c.kind == "nonzero" and f.type.base not in SCALARS:
            raise IdlError(path, f.line, "`nonzero` applies to numbers")
        if c.kind in ("utf8", "no_nul"):
            raise IdlError(path, f.line, f"`{c.kind}` applies to bytes")
        if c.kind == "components_of":
            ref = r.field(c.ref)
            if not ref or not ref.enum or not all(v.components for v in ref.enum.values):
                raise IdlError(path, f.line, f"components({c.ref}): `{c.ref}` must be an enum whose "
                                             "values all give `components`")
            if f.assign != "count":
                raise IdlError(path, f.line, "components() checks a count")
        if c.kind == "min_of":
            raise IdlError(path, f.line, "`>= min(...)` applies to tail arrays")


def check_tail_item(s: Schema, r: Record, ti: TailItem):
    path = s.path
    count = r.field(ti.count_field) if ti.count_field else None
    if ti.kind in ("array", "bytes"):
        if not count:
            raise IdlError(path, ti.line, f"`{ti.name}`: no field `{ti.count_field}` holds its count")
        # Several arrays may share a count (parallel arrays): it names the first of them.
        first = r.tail_item(count.assign_ref) if count.assign == "count" else None
        if not first or (first is not ti and (ti.kind != "array" or first.kind != "array" or
                                              first.count_field != ti.count_field)):
            raise IdlError(path, count.line,
                           f"`{count.name}` counts `{ti.name}`, so it must be `= count({ti.name})`")
    if ti.kind == "array":
        if ti.elem in s.records:
            ti.record = s.records[ti.elem]
        elif ti.elem not in ("u32", "f32", "u16", "u8"):
            raise IdlError(path, ti.line, f"`{ti.elem}` is not a scalar or record")
    elif ti.kind == "single":
        ti.record = s.records.get(ti.elem)
        if not ti.record or not ti.record.size_field:
            raise IdlError(path, ti.line, f"`{ti.name}`: a single item must be a record carrying its size")
    elif ti.kind == "union":
        ti.union = s.unions.get(ti.elem)
        kf = r.field(ti.count_field)
        if not ti.union:
            raise IdlError(path, ti.line, f"`{ti.elem}` is not a union")
        if not kf or kf.enum is not ti.union.enum:
            raise IdlError(path, ti.line, f"`{ti.count_field}` must be a {ti.union.enum.name} field")
        if not r.size_field:
            raise IdlError(path, ti.line, "a union tail needs its record to carry its size")
    for c in ti.checks:
        if c.kind in ("utf8", "no_nul") and ti.kind != "bytes":
            raise IdlError(path, ti.line, f"`{c.kind}` applies to bytes")
        if c.kind in ("finite", "min_of") and not (ti.kind == "array" and ti.elem == "f32"):
            raise IdlError(path, ti.line, f"`{c.kind}` applies to f32 arrays")
        if c.kind == "min_of":
            ref = r.field(c.ref)
            if not ref or not ref.enum:
                raise IdlError(path, ti.line, f"min({c.ref}): `{c.ref}` must be an enum field")
        if c.kind == "nonzero" and not (ti.kind == "array" and ti.elem in SCALARS):
            raise IdlError(path, ti.line, "`nonzero` applies to arrays of numbers")


def check_union(s: Schema, u: Union):
    for v in u.variants:
        for f in v.fields:
            size = scalar_size(s, f, s.path) if f.type.base != "str16" else 0
            f.size = size * f.type.count
            for c in f.checks:
                if c.kind == "range" and f.type.base != "str16":
                    pass
                elif c.kind not in ("finite", "range"):
                    raise IdlError(s.path, f.line, f"`{c.kind}` doesn't apply in a union")


def check_style(s: Schema):
    path = s.path
    if not s.style_props:
        return
    kinds = s.enums.get("value_kind")
    if not kinds:
        raise IdlError(path, s.style_props[0].line, "style properties need the value_kind enum")
    if not s.token_values or not s.transition_values:
        raise IdlError(path, s.style_props[0].line, "style properties need token_values and transition_values")
    for p in s.style_props + [StyleProp(0, "token values", s.token_values, set(), line=0),
                              StyleProp(0, "transition values", s.transition_values, {"transition"}, line=0)]:
        for v in p.values:
            if v.kind == "transition-only":
                if len(p.values) != 1 or "transition" not in p.flags or not p.id:
                    raise IdlError(path, p.line, "transition-only stands alone, with the transition flag")
                continue
            if not kinds.value(v.kind):
                raise IdlError(path, p.line, f"`{v.kind}` is not a value_kind")
            if v.kind == "keyword" and v.enum:
                e = s.enums.get(v.enum)
                if not e or e.bitmask:
                    raise IdlError(path, p.line, f"keyword `{v.enum}`: no such enum")
                for a in v.allowed:
                    if not e.value(a):
                        raise IdlError(path, p.line, f"`{a}` is not a value of {e.name}")
                    if e.value(a).value >= 32:
                        raise IdlError(path, p.line, f"keyword values must be below 32")
                if any(x.value >= 32 for x in e.values) and not v.allowed:
                    raise IdlError(path, p.line, f"keyword values must be below 32")


def check(s: Schema):
    # Records are laid out in definition order; a nested record must come first.
    for r in s.records.values():
        layout(s, r)
    for r in s.messages():
        layout(s, r)
    for r in list(s.records.values()) + s.messages():
        check_record(s, r)
    for u in s.unions.values():
        check_union(s, u)
    for group, name in ((s.commands, "op"), (s.events, "event_type")):
        e = s.enums.get(name)
        if not e:
            raise IdlError(s.path, 1, f"missing `enum {name}` listing the opcodes")
        listed = {v.name: v.value for v in e.values}
        mine = {r.name: r.opcode for r in group}
        if listed != mine:
            missing = sorted(set(listed.items()) ^ set(mine.items()))
            raise IdlError(s.path, e.line, f"enum {name} must list exactly the opcodes defined: {missing}")
    check_style(s)


def parse(text: str, path: str = "<idl>") -> Schema:
    s = Parser(path, text).parse()
    check(s)
    return s


# ---- Compatibility with an earlier schema -------------------------------------------------


def compat(old: Schema, new: Schema) -> list[str]:
    """What breaks the evolution rules between two versions of the schema."""
    errors = []
    if old.version != new.version:
        return []  # a version bump may change anything

    def fields_compat(what: str, o: Record, n: Record):
        if n.original != o.original:
            errors.append(f"{what}: original size changed from {o.original} to {n.original}")
        by_offset = {}
        for f in n.fields:
            by_offset.setdefault(f.offset, []).append(f)
        for of in o.fields:
            nf = next((f for f in by_offset.get(of.offset, []) if f.name == of.name), None)
            if nf:
                if (nf.size, nf.type.base, nf.type.storage, nf.type.count) != (
                        of.size, of.type.base, of.type.storage, of.type.count):
                    errors.append(f"{what}: `{of.name}` changed type or size")
                continue
            if of.reserved:
                took = [f for f in n.fields if f.offset >= of.offset and
                        f.offset + f.size <= of.offset + of.size]
                if took and all(f.since or f.reserved for f in took):
                    continue
            errors.append(f"{what}: `{of.name}` (offset {of.offset}) was removed, moved or renamed")
        for nf in n.fields:
            if nf.offset >= o.size and not nf.since and not nf.reserved:
                errors.append(f"{what}: `{nf.name}` was appended without `since`")
        if [t.name for t in o.tail] != [t.name for t in n.tail][:len(o.tail)]:
            errors.append(f"{what}: its variable data changed order")

    for group in ("commands", "events"):
        olds = {r.name: r for r in getattr(old, group)}
        news = {r.name: r for r in getattr(new, group)}
        for name, o in olds.items():
            n = news.get(name)
            if not n:
                errors.append(f"{group[:-1]} `{name}` was removed")
                continue
            if n.opcode != o.opcode:
                errors.append(f"{group[:-1]} `{name}` changed opcode {o.opcode} -> {n.opcode}")
            fields_compat(f"{group[:-1]} `{name}`", o, n)
    for name, o in old.records.items():
        n = new.records.get(name)
        if not n:
            errors.append(f"record `{name}` was removed")
        else:
            fields_compat(f"record `{name}`", o, n)
    for name, o in old.enums.items():
        n = new.enums.get(name)
        if not n:
            errors.append(f"{'bitmask' if o.bitmask else 'enum'} `{name}` was removed")
            continue
        for v in o.values:
            nv = n.value(v.name)
            if not nv or nv.value != v.value:
                errors.append(f"`{name}.{v.name}` = {v.value} was removed or renumbered")
    for o in old.style_props:
        n = new.style_prop(o.name)
        if not n or n.id != o.id:
            errors.append(f"style property {o.id} `{o.name}` was removed or renumbered")
    return errors
