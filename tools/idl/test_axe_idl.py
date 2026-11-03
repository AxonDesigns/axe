#!/usr/bin/env python3
"""Tests of the wire IDL's parser, rule checks, compatibility check and generator
(CTest: idl_generator). Run with python3 -I tools/idl/test_axe_idl.py."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import axe_idl  # noqa: E402
import idl_c  # noqa: E402
import idl_model  # noqa: E402
import idl_ts  # noqa: E402
from idl_model import IdlError, compat, parse  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))

PRELUDE = """\
wire 0
magic commands 0x42434955 "UICB"
magic events 0x42454955 "UIEB"
changes
  c1  "First change"
  c2  "Second change"
enum op : u16
  ping = 1
enum event_type : u16
  pong = 1
"""

PING = """\
command ping = 1  original 8
  node  node
event pong = 1  original 4
  id  u32
"""


def schema(body: str = "", ops: str = PING, prelude: str = PRELUDE):
    return parse(prelude + textwrap.dedent(body) + ops, "test.idl")


class Parsing(unittest.TestCase):
    def test_minimal(self):
        s = schema()
        self.assertEqual([r.name for r in s.commands], ["ping"])
        self.assertEqual(s.commands[0].size, 8)
        self.assertEqual(s.events[0].original, 4)
        self.assertEqual(s.changes, {"c1": "First change", "c2": "Second change"})

    def test_comments_strings_and_escapes(self):
        s = schema(ops='command ping = 1  original 8  "a \\"quoted\\" # not a comment"  # comment\n'
                       '  node  node  "doc"\nevent pong = 1  original 4\n  id  u32\n')
        self.assertEqual(s.commands[0].doc, 'a "quoted" # not a comment')
        self.assertEqual(s.commands[0].fields[0].doc, "doc")

    def test_layout_offsets(self):
        s = schema("""
        enum kind : u8
          a = 0
        record pair
          x  u16
          y  u16
        """, ops="""\
command ping = 1  original 28
  node  node
  a     u8
  b     kind
  c     u16
  d     f32
  e     u64 time
  p     pair
event pong = 1  original 4
  id  u32
""")
        r = s.commands[0]
        self.assertEqual([(f.name, f.offset, f.size) for f in r.fields],
                         [("node", 0, 8), ("a", 8, 1), ("b", 9, 1), ("c", 10, 2), ("d", 12, 4), ("e", 16, 8),
                          ("p", 24, 4)])
        self.assertTrue(r.field("e").type.time)
        self.assertEqual(r.size, 28)

    def test_tail_kinds(self):
        s = schema(ops="""\
command ping = 1  original 8
  count   u16 = count(values)
  len     u16 = count(name)
  header  u16 = header
  pad     u16
  tail
  values  f32[count]  finite
  name    bytes[len] pad 4  utf8 no_nul
event pong = 1  original 4
  id  u32
""")
        r = s.commands[0]
        self.assertEqual([(t.name, t.kind, t.pad) for t in r.tail], [("values", "array", 1), ("name", "bytes", 4)])

    def test_real_idl(self):
        with open(os.path.join(ROOT, "idl/wire.idl"), encoding="utf-8") as f:
            s = parse(f.read(), "idl/wire.idl")
        sizes = {r.name: (r.original, r.size) for r in s.messages()}
        # The sizes docs/command-buffer.md documents.
        self.assertEqual(sizes["set_text"], (64, 72))
        self.assertEqual(sizes["start_spring"], (20, 20))
        self.assertEqual(sizes["layout_measured"], (88, 88))
        self.assertEqual(sizes["clock"], (8, 8))
        st = next(r for r in s.commands if r.name == "set_text")
        self.assertEqual(st.field("from_node").offset, 70)
        self.assertEqual(st.field("style").offset, 36)
        self.assertEqual(s.records["text_span"].size, 36)
        self.assertEqual(s.style_prop("width").id, 45)


class Errors(unittest.TestCase):
    def rejects(self, text: str, message: str, prelude: str = PRELUDE, ops: str | None = None):
        with self.assertRaises(IdlError) as cm:
            parse(prelude + textwrap.dedent(text) + (PING if ops is None else ops), "test.idl")
        self.assertIn(message, str(cm.exception))
        self.assertRegex(str(cm.exception), r"^test\.idl:\d+: ")
        return cm.exception

    def test_tabs(self):
        self.rejects("record r\n\tx  u32\n", "tabs are not allowed")

    def test_unterminated_string(self):
        self.rejects('record r  "doc\n  x u32\n', "unterminated string")

    def test_line_numbers(self):
        e = self.rejects("record r\n  x  nosuchtype\n", "unknown type")
        self.assertEqual(e.line, PRELUDE.count("\n") + 2)

    def test_unknown_statement(self):
        self.rejects("struct r\n", "unknown statement")

    def test_names(self):
        self.rejects("record Bad\n  x u32\n", "lower_snake_case")

    def test_duplicates(self):
        self.rejects("record r\n  x u32\nrecord r\n  y u32\n", "already defined")
        self.rejects("record r\n  x u32\n  x u32\n", "defined twice")
        self.rejects("", "opcode 1 is already command", ops=PING + "command pang = 1  original 8\n  node node\n")

    def test_enum_values(self):
        self.rejects("enum e : u8\n  a = 256\n", "doesn't fit u8")
        self.rejects("enum e : u8\n  a = 1\n  b = 1\n", "used twice")
        self.rejects("enum e : u64\n  a = 1\n", "storage must be")

    def test_opcode_enum_must_match(self):
        self.rejects("", "must list exactly the opcodes",
                     ops=PING + "command pang = 2  original 8\n  node node\n")

    def test_original_size_required_and_checked(self):
        self.rejects("", "must state its original size",
                     ops="command ping = 1\n  node node\nevent pong = 1 original 4\n  id u32\n")
        self.rejects("", "original 12 is larger than its fields",
                     ops="command ping = 1  original 12\n  node node\nevent pong = 1 original 4\n  id u32\n")
        self.rejects("", "not a multiple of 4",
                     ops="command ping = 1  original 8\n  node node\n  x u16 since c1\n"
                         "event pong = 1 original 4\n  id u32\n")

    def test_appended_field_needs_since(self):
        self.rejects("", "an appended field needs `since <change>`",
                     ops="command ping = 1  original 8\n  node node\n  x u32\n"
                         "event pong = 1 original 4\n  id u32\n")

    def test_since_must_name_a_change(self):
        self.rejects("", "not in `changes`",
                     ops="command ping = 1  original 8\n  node node\n  x u32 since nope\n"
                         "event pong = 1 original 4\n  id u32\n")

    def test_appended_default_must_be_zero(self):
        self.rejects("", "its default must be 0",
                     ops="command ping = 1  original 8\n  node node\n  x u32 since c1 default 3\n"
                         "event pong = 1 original 4\n  id u32\n")
        self.rejects("", "`default` is for appended fields",
                     ops="command ping = 1  original 8\n  node node default 1\n"
                         "event pong = 1 original 4\n  id u32\n")

    def test_appended_enum_needs_zero(self):
        self.rejects("enum e : u32\n  a = 1\n", "needs a value 0",
                     ops="command ping = 1  original 8\n  node node\n  x e since c1\n"
                         "event pong = 1 original 4\n  id u32\n")

    def test_appended_count_has_no_zero_default(self):
        self.rejects("", "has no zero default",
                     ops="command ping = 1  original 8\n  node node\n  n u16 = count(xs) since c1\n"
                         "  header u16 = header since c1\n  tail\n  xs u32[n]\n"
                         "event pong = 1 original 4\n  id u32\n")

    def test_taking_reserved_bytes_is_allowed(self):
        s = schema(ops="command ping = 1  original 12\n  node node\n  x u16 since c1\n  reserved u16\n"
                       "event pong = 1 original 4\n  id u32\n")
        self.assertEqual(s.commands[0].field("x").offset, 8)

    def test_reserved_fields(self):
        self.rejects("", "plain u8, u16 or u32",
                     ops="command ping = 1  original 16\n  node node\n  reserved f32\n  reserved u32\n"
                         "event pong = 1 original 4\n  id u32\n")
        self.rejects("", "has no `since`",
                     ops="command ping = 1  original 12\n  node node\n  reserved u32 since c1\n"
                         "event pong = 1 original 4\n  id u32\n")

    def test_alignment(self):
        self.rejects("", "not 4-byte aligned",
                     ops="command ping = 1  original 16\n  a u8\n  b u32\n  c u32\n  d u16\n  e u8\n"
                         "event pong = 1 original 4\n  id u32\n")

    def test_fixed_part_multiple_of_4(self):
        self.rejects("", "not a multiple of 4",
                     ops="command ping = 1  original 10\n  node node\n  x u16\n"
                         "event pong = 1 original 4\n  id u32\n")

    def test_counts_must_be_declared(self):
        self.rejects("", "must be `= count(xs)`",
                     ops="command ping = 1  original 8\n  n u32\n  header u16 = header\n  r u16\n"
                         "  tail\n  xs u32[n]\nevent pong = 1 original 4\n  id u32\n")
        self.rejects("", "no such tail item",
                     ops="command ping = 1  original 4\n  n u16 = count(xs)\n  h u16 = header\n"
                         "  tail\n  ys u32[n]\nevent pong = 1 original 4\n  id u32\n")

    def test_stride_needs_records(self):
        self.rejects("", "stride() applies to an array of records",
                     ops="command ping = 1  original 8\n  n u16 = count(xs)\n  s u16 = stride(xs)\n"
                         "  h u16 = header\n  r u16\n  tail\n  xs u32[n]\nevent pong = 1 original 4\n  id u32\n")

    def test_no_header_means_no_growth(self):
        self.rejects("", "can't grow its fixed part",
                     ops="command ping = 1  original 4\n  n u16 = count(xs)\n  r u16\n  x u32 since c1\n"
                         "  tail\n  xs u32[n]\nevent pong = 1 original 4\n  id u32\n")

    def test_self_sized_last(self):
        self.rejects("record e\n  words u8 = size/4\n  r u8\n  r2 u16\n", "must come last",
                     ops="command ping = 1  original 8\n  n u16 = count(es)\n  m u16 = count(xs)\n"
                         "  h u16 = header\n  r u16\n  tail\n  es e[n]\n  xs u32[m]\n"
                         "event pong = 1 original 8\n  id u32\n  r u32\n")

    def test_record_tail_needs_size(self):
        self.rejects("record r\n  n u16 = count(xs)\n  q u16\n  tail\n  xs u32[n]\n", "has a tail but no size")

    def test_check_kinds(self):
        bad = {"finite": "x u32 finite", "known": "x u32 known", "utf8": "x u32 utf8"}
        for word, line in bad.items():
            with self.subTest(word):
                self.rejects("", word,
                             ops=f"command ping = 1  original 8\n  node node\n  {line} since c1\n"
                                 "event pong = 1 original 4\n  id u32\n")

    def test_ranges(self):
        self.rejects("", "is empty",
                     ops="command ping = 1  original 4\n  x f32 3..1\nevent pong = 1 original 4\n  id u32\n")
        self.rejects("", "not a range",
                     ops="command ping = 1  original 4\n  x f32 ..\nevent pong = 1 original 4\n  id u32\n")
        s = schema(ops="command ping = 1  original 4\n  x f32 finite 0<..<1\nevent pong = 1 original 4\n  id u32\n")
        c = s.commands[0].fields[0].checks[1]
        self.assertEqual((c.lo, c.hi, c.lo_open, c.hi_open), (0, 1, True, True))

    def test_components_needs_enum(self):
        self.rejects("", "must be an enum",
                     ops="command ping = 1  original 8\n  p u32\n  n u16 = count(xs) components(p)\n"
                         "  h u16 = header\n  tail\n  xs f32[n]\nevent pong = 1 original 4\n  id u32\n")

    def test_magic_letters(self):
        self.rejects("", "is not \"UICB\"", prelude=PRELUDE.replace("0x42434955", "0x42434956"))

    def test_missing_pieces(self):
        self.rejects("", "missing `wire", prelude=PRELUDE.replace("wire 0\n", ""))
        self.rejects("", "missing `magic events`", prelude=PRELUDE.replace('magic events 0x42454955 "UIEB"\n', ""))


STYLE = """\
enum value_kind : u8
  unset = 1
  px = 2
  number = 6
  keyword = 8
  token = 9
  spring = 10
  string = 11
  inherit = 12
  initial = 13
enum shape : u32
  a = 0
  b = 1
union style_value  by value_kind  c ui_value
  px       number f32
  number   number f32
  keyword  keyword u32
  token    token u32
  spring   spring.stiffness f32 finite 0<.., spring.damping f32 finite >= 0
  string   string str16 1..1014
"""


class Styles(unittest.TestCase):
    def props(self, lines: str, extra: str = "token_values px | token\ntransition_values spring\n"):
        return schema(STYLE + "style_properties c ui_style_prop UI_STYLE_\n" + lines + extra)

    def test_values(self):
        s = self.props("  1  width  px >= 0 | number 0..1 whole  layout\n  2  form  keyword shape[a]\n")
        w = s.style_prop("width")
        self.assertEqual([v.kind for v in w.values], ["px", "number"])
        self.assertTrue(w.values[1].whole)
        self.assertEqual(w.flags, {"layout"})
        self.assertEqual(s.style_prop("form").values[0].allowed, ["a"])

    def test_rejections(self):
        cases = {
            "  1 w  bogus\n": "unknown value kind",
            "  1 w  keyword nope\n": "no such enum",
            "  1 w  keyword shape[c]\n": "is not a value of shape",
            "  1 w  transition-only\n": "transition-only stands alone",
            "  1 w  px\n  1 v  px\n": "clashes with",
            "  0 w  px\n": "property ids are 1 to 32767",
            "  1 w  keyword any\n": "for token values only",
        }
        for lines, message in cases.items():
            with self.subTest(message):
                with self.assertRaises(IdlError) as cm:
                    self.props(lines)
                self.assertIn(message, str(cm.exception))

    def test_needs_token_and_transition_values(self):
        with self.assertRaises(IdlError):
            self.props("  1 w  px\n", extra="")


class Compatibility(unittest.TestCase):
    BASE = """\
enum shape : u32
  a = 0
  b = 1
"""
    OPS = """\
command ping = 1  original 16
  node   node
  x      u32
  reserved u32
event pong = 1  original 4
  id  u32
"""

    def check(self, body: str, ops: str) -> list[str]:
        old = schema(self.BASE, self.OPS)
        new = schema(body, ops)
        return compat(old, new)

    def test_identical(self):
        self.assertEqual(self.check(self.BASE, self.OPS), [])

    def test_append_with_since(self):
        ops = self.OPS.replace("  reserved u32\n", "  reserved u32\n  y u32 since c1\n")
        self.assertEqual(self.check(self.BASE, ops), [])

    def test_take_reserved(self):
        ops = self.OPS.replace("  reserved u32\n", "  y u32 since c1\n")
        self.assertEqual(self.check(self.BASE, ops), [])

    def test_breaks(self):
        cases = {
            "removed": (self.BASE, self.OPS.replace("  x      u32\n", "  reserved2 u32\n")),
            "changed type": (self.BASE, self.OPS.replace("  x      u32\n", "  x      f32\n")),
            "was removed, moved or renamed": (self.BASE, self.OPS.replace("  x      u32\n  reserved u32\n",
                                                                          "  reserved u32\n  x      u32\n")),
            "changed opcode": (self.BASE, self.OPS.replace("command ping = 1", "command ping = 2")),
            "removed or renumbered": (self.BASE.replace("b = 1", "b = 2"), self.OPS),
        }
        for message, (body, ops) in cases.items():
            with self.subTest(message):
                if message == "changed opcode":
                    body = body + ""
                    new_prelude = PRELUDE.replace("ping = 1", "ping = 2")
                    old = schema(self.BASE, self.OPS)
                    errors = compat(old, schema(body, ops, prelude=new_prelude))
                else:
                    try:
                        errors = self.check(body, ops)
                    except IdlError as e:  # the new schema itself breaks a rule
                        errors = [str(e)]
                self.assertTrue(any(message in e for e in errors), errors)

    def test_removed_op(self):
        old = schema(self.BASE, self.OPS + "command pang = 2  original 8\n  node node\n",
                     prelude=PRELUDE.replace("  ping = 1\n", "  ping = 1\n  pang = 2\n"))
        errors = compat(old, schema(self.BASE, self.OPS))
        self.assertTrue(any("was removed" in e for e in errors), errors)

    def test_original_changed(self):
        ops = self.OPS.replace("original 16", "original 12")  # the reserved word becomes padding
        errors = self.check(self.BASE, ops)
        self.assertTrue(any("original size changed" in e for e in errors), errors)


class Generator(unittest.TestCase):
    def test_outputs_are_stable(self):
        with open(os.path.join(ROOT, "idl/wire.idl"), encoding="utf-8") as f:
            text = f.read()
        a = axe_idl.outputs(parse(text, "idl/wire.idl"), ROOT)
        b = axe_idl.outputs(parse(text, "idl/wire.idl"), ROOT)
        self.assertEqual(a, b)
        self.assertIn("include/ui/wire_ids.h", a)
        self.assertIn("ts/packages/wire/src/gen/commands.ts", a)

    def test_committed_outputs_up_to_date(self):
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "axe_idl.py"), "--check"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_check_reports_stale_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            for d in ("idl", "docs"):
                os.makedirs(os.path.join(tmp, d))
            with open(os.path.join(ROOT, "idl/wire.idl"), encoding="utf-8") as f:
                open(os.path.join(tmp, "idl/wire.idl"), "w", encoding="utf-8").write(f.read())
            with open(os.path.join(ROOT, "docs/command-buffer.md"), encoding="utf-8") as f:
                open(os.path.join(tmp, "docs/command-buffer.md"), "w", encoding="utf-8").write(f.read())
            run = lambda *a: subprocess.run([sys.executable, "-I", os.path.join(HERE, "axe_idl.py"), "--root", tmp, *a],
                                            capture_output=True, text=True)
            r = run("--check")
            self.assertEqual(r.returncode, 1)
            self.assertIn("src/wire/gen/wire_gen.c", r.stderr)
            self.assertEqual(run().returncode, 0)
            self.assertEqual(run("--check").returncode, 0)

    def test_generated_c_mentions_every_op(self):
        with open(os.path.join(ROOT, "idl/wire.idl"), encoding="utf-8") as f:
            s = parse(f.read(), "idl/wire.idl")
        h, c = idl_c.gen_h(s), idl_c.gen_c(s)
        for r in s.messages():
            self.assertIn(f"ui_wire_{r.name}_read", h)
            self.assertIn(f"ui_result ui_wire_{r.name}_put", c)
        ts = idl_ts.commands_ts(s)
        for r in s.commands:
            self.assertIn(f"  {idl_ts.camel(r.name)}(", ts)

    def test_compat_base_against_git(self):
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "axe_idl.py"), "--check", "--compat-base", "HEAD"],
                           capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
