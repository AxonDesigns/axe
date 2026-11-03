#!/usr/bin/env python3
"""Writes the layout fixtures (fixtures/*.html) from the compact specs below.

Each fixture covers one property or interaction. A fixture is the children of a surface:
<div> nodes and <p> text leaves with inline styles. record.py records Chrome's boxes for
them and gen.py turns them into a C test. Edit the specs here, then run all three:

    python3 -I tools/layout-fixtures/make_fixtures.py
    python3 -I tools/layout-fixtures/record.py
    python3 -I tools/layout-fixtures/gen.py
"""
import html
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
FIXTURES = {}


def div(style="", *children):
    return ("div", style, list(children))


def rtl(style="", *children):
    """A div with dir="rtl": it and its subtree lay out right to left."""
    return ("div", style, list(children), {"dir": "rtl"})


def ltr(style="", *children):
    return ("div", style, list(children), {"dir": "ltr"})


def p(text, style=""):
    return ("p", style, text)


def fx(name, w, h, *nodes):
    if name in FIXTURES:
        raise SystemExit(f"duplicate fixture {name}")
    FIXTURES[name] = (w, h, nodes)


def render(node, depth):
    tag, style, body = node[:3]
    pad = "  " * depth
    attr = "".join(f' {k}="{v}"' for k, v in (node[3] if len(node) > 3 else {}).items())
    attr += f' style="{html.escape(style)}"' if style else ""
    if tag == "p":
        return f"{pad}<p{attr}>{html.escape(body)}</p>\n"
    inner = "".join(render(c, depth + 1) for c in body)
    return f"{pad}<div{attr}>\n{inner}{pad}</div>\n" if inner else f"{pad}<div{attr}></div>\n"


def box(w, h, extra=""):
    return div(f"width:{w}px;height:{h}px;{extra}")


# ---- Direction and justify-content ------------------------------------------------------
JUSTIFY = ["flex-start", "flex-end", "center", "space-between", "space-around", "space-evenly"]
for d in ["row", "row-reverse", "column", "column-reverse"]:
    for j in JUSTIFY:
        fx(f"justify_{d}_{j}", 320, 320,
           div(f"flex-direction:{d};justify-content:{j};width:300px;height:300px",
               box(50, 30), box(40, 20), box(60, 40)))
fx("justify_overflow_center", 320, 120,
   div("justify-content:center;width:100px;height:100px",
       box(80, 30, "flex-shrink:0"), box(80, 30, "flex-shrink:0")))
fx("justify_overflow_space_between", 320, 120,
   div("justify-content:space-between;width:100px;height:100px",
       box(80, 30, "flex-shrink:0"), box(80, 30, "flex-shrink:0")))
fx("justify_single_space_around", 320, 120,
   div("justify-content:space-around;width:300px;height:100px", box(80, 30)))

# ---- align-items and align-self ---------------------------------------------------------
ALIGN = ["stretch", "flex-start", "flex-end", "center", "baseline"]
for d in ["row", "column"]:
    for a in ALIGN:
        kids = [box(50, 30), div("width:40px;height:40px;margin:5px"),
                div("width:60px" if d == "row" else "height:60px"), div("min-width:20px;min-height:20px")]
        fx(f"align_{d}_{a}", 320, 320,
           div(f"flex-direction:{d};align-items:{a};width:300px;height:200px", *kids))
for d in ["row", "column"]:
    kids = [div(f"align-self:{a};width:40px;height:40px") for a in
            ["auto", "flex-start", "flex-end", "center", "baseline"]]
    kids.append(div("align-self:stretch;min-width:10px;min-height:10px"))
    fx(f"align_self_{d}", 320, 320,
       div(f"flex-direction:{d};align-items:center;width:300px;height:300px", *kids))

# ---- flex-grow ----------------------------------------------------------------------------
fx("grow_equal", 320, 120, div("width:300px;height:100px",
   div("flex:1 1 0px"), div("flex:1 1 0px"), div("flex:1 1 0px")))
fx("grow_weighted", 320, 120, div("width:300px;height:100px",
   div("flex-grow:1;width:20px"), div("flex-grow:2;width:20px"), div("flex-grow:3;width:20px")))
fx("grow_with_basis", 320, 120, div("width:300px;height:100px",
   div("flex:1 1 50px"), div("flex:1 1 100px")))
fx("grow_max_clamp", 320, 120, div("width:300px;height:100px",
   div("flex-grow:1;max-width:60px"), div("flex-grow:1"), div("flex-grow:1")))
fx("grow_min_clamp", 320, 120, div("width:300px;height:100px",
   div("flex:1 1 0px;min-width:150px"), div("flex:1 1 0px"), div("flex:1 1 0px")))
fx("grow_fraction", 320, 120, div("width:300px;height:100px",
   div("flex-grow:0.2;width:50px"), div("flex-grow:0.3;width:50px")))
fx("grow_column", 120, 320, div("flex-direction:column;width:100px;height:300px",
   div("flex-grow:1"), div("flex-grow:2;height:20px"), box(30, 30)))
fx("grow_zero", 320, 120, div("width:300px;height:100px", box(50, 20), box(60, 20)))

# ---- flex-shrink --------------------------------------------------------------------------
fx("shrink_equal", 320, 120, div("width:300px;height:100px",
   div("width:150px"), div("width:150px"), div("width:150px")))
fx("shrink_weighted", 320, 120, div("width:300px;height:100px",
   div("width:150px;flex-shrink:1"), div("width:150px;flex-shrink:2"), div("width:150px;flex-shrink:3")))
fx("shrink_zero", 320, 120, div("width:300px;height:100px",
   div("width:200px;flex-shrink:0"), div("width:200px")))
fx("shrink_min_clamp", 320, 120, div("width:300px;height:100px",
   div("width:200px;min-width:180px"), div("width:200px")))
fx("shrink_by_basis", 320, 120, div("width:300px;height:100px",
   div("flex-basis:300px"), div("flex-basis:100px")))
fx("shrink_padding", 320, 120, div("width:300px;height:100px",
   div("width:200px;padding:0 40px"), div("width:200px")))
fx("shrink_none_overflow", 320, 120, div("width:300px;height:100px",
   div("width:200px;flex-shrink:0"), div("width:200px;flex-shrink:0")))
fx("shrink_column", 120, 320, div("flex-direction:column;width:100px;height:200px",
   div("height:150px"), div("height:150px;flex-shrink:3")))

# ---- flex-basis ---------------------------------------------------------------------------
fx("basis_px", 320, 120, div("width:300px;height:100px", div("flex-basis:80px"), div("flex-basis:40px")))
fx("basis_percent", 320, 120, div("width:300px;height:100px", div("flex-basis:50%"), div("flex-basis:25%")))
fx("basis_over_width", 320, 120, div("width:300px;height:100px", div("flex-basis:120px;width:30px")))
fx("basis_auto_width", 320, 120, div("width:300px;height:100px", div("flex-basis:auto;width:70px")))
fx("basis_auto_content", 320, 120, div("width:300px;height:100px", div("", box(60, 10), box(30, 10))))
fx("basis_percent_indefinite", 120, 320, div("flex-direction:column;width:100px",
   div("flex-basis:50%", box(20, 30)), box(20, 20)))
fx("basis_zero_grow", 320, 120, div("width:300px;height:100px",
   div("flex:1 1 0px", box(100, 10)), div("flex:1 1 0px")))

# ---- flex-wrap and align-content --------------------------------------------------------
five = [box(100, 30), box(100, 40), box(100, 20), box(100, 30), box(100, 30)]
fx("wrap_basic", 320, 220, div("flex-wrap:wrap;width:300px;height:200px", *five))
fx("wrap_reverse", 320, 220, div("flex-wrap:wrap-reverse;width:300px;height:200px", *five))
fx("wrap_column", 320, 220, div("flex-direction:column;flex-wrap:wrap;width:300px;height:100px", *five))
fx("wrap_gap", 320, 220, div("flex-wrap:wrap;gap:10px 20px;width:300px;height:200px", *five))
fx("wrap_grow", 320, 220, div("flex-wrap:wrap;width:300px;height:200px",
   *[box(100, 30, "flex-grow:1") for _ in range(5)]))
fx("wrap_nowrap_overflow", 320, 220, div("width:300px;height:200px", *[box(100, 30, "flex-shrink:0") for _ in range(4)]))
fx("wrap_auto_height", 320, 220, div("flex-wrap:wrap;width:300px", *five))
for ac in ["flex-start", "flex-end", "center", "space-between", "space-around", "space-evenly", "stretch"]:
    fx(f"align_content_{ac}", 320, 220,
       div(f"flex-wrap:wrap;align-content:{ac};width:300px;height:200px",
           box(100, 30), div("width:100px"), box(100, 20), box(100, 30)))
fx("align_content_single_line", 320, 220,
   div("flex-wrap:wrap;align-content:center;width:300px;height:200px", box(100, 30), box(100, 20)))

# ---- gap ------------------------------------------------------------------------------------
fx("gap_row", 320, 120, div("column-gap:15px;width:300px;height:100px", box(50, 20), box(50, 20), box(50, 20)))
fx("gap_column", 120, 320, div("flex-direction:column;row-gap:15px;width:100px;height:300px",
   box(50, 20), box(50, 20), box(50, 20)))
fx("gap_percent", 320, 120, div("column-gap:10%;width:300px;height:100px", box(50, 20), box(50, 20)))
fx("gap_with_grow", 320, 120, div("gap:20px;width:300px;height:100px",
   div("flex-grow:1"), div("flex-grow:1"), div("flex-grow:1")))

# ---- min and max sizes --------------------------------------------------------------------
fx("min_over_max", 320, 120, div("width:300px;height:100px", div("width:80px;min-width:100px;max-width:50px")))
fx("max_percent", 320, 120, div("width:300px;height:100px", div("flex-grow:1;max-width:25%")))
fx("min_height_column_grow", 120, 320, div("flex-direction:column;width:100px;height:300px",
   div("flex-grow:1;min-height:200px"), div("flex-grow:1")))
fx("max_height_stretch", 320, 220, div("width:300px;height:200px", div("width:50px;max-height:60px")))
fx("min_height_stretch", 320, 220, div("align-items:flex-start;width:300px;height:200px",
   div("width:50px;min-height:60px")))
fx("max_width_shrink_content", 320, 120, div("width:300px;height:100px",
   div("max-width:40px", box(80, 20)), box(50, 20)))
fx("min_width_zero_overflow", 320, 120, div("width:100px;height:100px",
   div("min-width:0", box(150, 20)), box(50, 20)))
fx("min_max_column", 120, 320, div("flex-direction:column;width:100px;height:300px",
   div("height:400px;max-height:120px"), div("height:10px;min-height:50px")))

# ---- margins ------------------------------------------------------------------------------
fx("margin_px", 320, 120, div("width:300px;height:100px", box(50, 20, "margin:5px 10px 15px 20px"), box(50, 20)))
fx("margin_percent", 320, 120, div("width:300px;height:100px", box(50, 20, "margin:10%"), box(50, 20)))
fx("margin_auto_main", 320, 120, div("width:300px;height:100px", box(50, 20, "margin-left:auto;margin-right:auto")))
fx("margin_auto_push", 320, 120, div("width:300px;height:100px", box(50, 20), box(50, 20, "margin-left:auto"), box(50, 20)))
fx("margin_auto_cross", 320, 120, div("width:300px;height:100px", box(50, 20, "margin-top:auto;margin-bottom:auto")))
fx("margin_auto_over_justify", 320, 120, div("justify-content:center;width:300px;height:100px",
   box(50, 20, "margin-right:auto"), box(50, 20)))
fx("margin_negative", 320, 120, div("width:300px;height:100px", box(50, 20), box(50, 20, "margin-left:-20px;margin-top:-5px")))
fx("margin_stretch", 320, 120, div("width:300px;height:100px", div("width:50px;margin:10px 0")))

# ---- padding and border ---------------------------------------------------------------------
fx("padding_px", 320, 220, div("padding:10px 20px 30px 40px;width:300px;height:200px", box(50, 20), div("flex-grow:1")))
fx("padding_percent", 320, 220, div("padding:5%;width:300px;height:200px", div("flex-grow:1")))
fx("border_box", 320, 220, div("border-width:7px;padding:3px;width:300px;height:200px", div("flex-grow:1")))
fx("padding_floor", 320, 120, div("width:300px;height:100px", div("width:10px;padding:0 20px;border-width:5px")))
fx("padding_nested_percent", 320, 220, div("width:300px;height:200px", div("width:200px;padding:10%", div("flex-grow:1"))))
fx("border_column", 120, 320, div("flex-direction:column;border-width:4px;width:100px;height:300px",
   div("flex-grow:1;border-width:2px"), box(30, 30, "border-width:6px")))

# ---- absolute positioning -------------------------------------------------------------------
def parent(*children, extra=""):
    return div(f"width:300px;height:200px;{extra}", box(40, 40), *children)

fx("abs_left_top", 320, 220, parent(box(50, 30, "position:absolute;left:10px;top:20px")))
fx("abs_right_bottom", 320, 220, parent(box(50, 30, "position:absolute;right:10px;bottom:20px")))
fx("abs_all_insets", 320, 220, parent(div("position:absolute;left:10px;right:30px;top:20px;bottom:40px")))
fx("abs_overconstrained", 320, 220, parent(box(50, 30, "position:absolute;left:10px;right:10px")))
fx("abs_auto_margins", 320, 220, parent(box(50, 30, "position:absolute;left:0;right:0;top:0;bottom:0;margin:auto")))
fx("abs_static_start", 320, 220, parent(box(50, 30, "position:absolute")))
fx("abs_static_center", 320, 220, parent(box(50, 30, "position:absolute"),
   extra="justify-content:center;align-items:center"))
fx("abs_static_end", 320, 220, parent(box(50, 30, "position:absolute"), extra="justify-content:flex-end;align-items:flex-end"))
fx("abs_static_column", 320, 220, parent(box(50, 30, "position:absolute"),
   extra="flex-direction:column;justify-content:center"))
fx("abs_percent", 320, 220, parent(div("position:absolute;left:10%;top:25%;width:50%;height:20%")))
fx("abs_padded_parent", 320, 220, parent(box(50, 30, "position:absolute;left:10px;top:10px"),
   extra="padding:20px;border-width:5px"))
fx("abs_static_padded", 320, 220, parent(box(50, 30, "position:absolute"), extra="padding:20px;border-width:5px"))
fx("abs_shrink_to_fit", 320, 220, parent(div("position:absolute;left:10px;top:10px", box(60, 20), box(30, 40))))
fx("abs_min_max", 320, 220, parent(div("position:absolute;left:0;right:0;top:0;max-width:100px;min-height:50px")))
fx("abs_nested", 320, 220, parent(div("position:absolute;left:50px;top:50px;width:100px;height:100px",
   box(20, 20, "position:absolute;right:0;bottom:0"))))

# ---- display: none -------------------------------------------------------------------------
fx("none_child", 320, 120, div("width:300px;height:100px", box(50, 20), box(50, 20, "display:none"), box(50, 20)))
fx("none_with_grow", 320, 120, div("width:300px;height:100px", div("flex-grow:1"), div("flex-grow:1;display:none"), div("flex-grow:1")))
fx("none_nested", 320, 120, div("width:300px;height:100px", div("display:none", box(50, 20)), box(50, 20)))

# ---- aspect-ratio -----------------------------------------------------------------------------
fx("ar_width", 320, 220, div("align-items:flex-start;width:300px;height:200px", div("width:100px;aspect-ratio:2")))
fx("ar_height", 320, 220, div("align-items:flex-start;width:300px;height:200px", div("height:50px;aspect-ratio:2")))
fx("ar_column_stretch", 320, 220, div("flex-direction:column;width:300px;height:200px", div("aspect-ratio:4")))
fx("ar_basis", 320, 220, div("align-items:flex-start;width:300px;height:200px", div("height:40px;aspect-ratio:1.5;flex-shrink:0")))
fx("ar_max", 320, 220, div("align-items:flex-start;width:300px;height:200px", div("width:200px;max-height:50px;aspect-ratio:1")))

# ---- nesting ----------------------------------------------------------------------------------
fx("nest_row_in_column", 320, 220, div("flex-direction:column;width:300px;height:200px",
   div("height:50px", div("flex-grow:1"), box(50, 20)), div("flex-grow:1", box(30, 30))))
fx("nest_column_in_row", 320, 220, div("width:300px;height:200px",
   div("flex-direction:column;flex-grow:1", box(50, 30), div("flex-grow:1")), box(80, 50)))
fx("nest_percent_definite", 320, 220, div("width:300px;height:200px",
   div("width:50%;height:50%", div("width:50%;height:50%"))))
fx("nest_percent_indefinite", 320, 220, div("align-items:flex-start;width:300px;height:200px",
   div("width:100px", div("height:50%;width:20px"), box(20, 40))))
fx("nest_deep", 320, 220, div("width:300px;height:200px;padding:5px",
   div("flex-grow:1;padding:5px", div("flex-grow:1;padding:5px;flex-direction:column",
       div("flex-grow:1;padding:5px", div("flex-grow:1")), box(10, 10)))))
fx("nest_stretch_chain", 320, 220, div("width:300px;height:200px",
   div("width:100px", div("width:50px", div("width:20px")))))
fx("nest_shrink_to_fit", 320, 220, div("flex-direction:column;align-items:flex-start;width:300px;height:200px",
   div("", box(60, 20), box(40, 30)), div("flex-direction:column", box(70, 10), box(50, 10))))
fx("nest_grow_shrink_mix", 320, 220, div("width:300px;height:200px",
   div("flex:2 1 100px", box(150, 10)), div("flex:1 2 200px"), div("flex:0 0 50px")))

# ---- text ---------------------------------------------------------------------------------------
LOREM = "Flexible boxes lay out text that wraps across several lines when the room runs short."
fx("text_single_line", 420, 120, div("width:400px;height:100px", p("Hello flexbox")))
fx("text_wrap_column", 320, 220, div("flex-direction:column;width:200px;height:200px", p(LOREM)))
fx("text_shrink_row", 320, 220, div("width:200px;height:200px", p("Two paragraphs side by side"), p("share the row width")))
fx("text_min_content", 320, 220, div("width:40px;height:200px", p("Unbreakable")))
fx("text_abs_shrink", 320, 220, div("width:300px;height:200px", p("Absolute text", "position:absolute;left:10px;top:10px")))
fx("text_padding", 320, 220, div("flex-direction:column;width:200px;height:200px",
   p(LOREM, "padding:6px 10px;border-width:2px")))
fx("text_sizes", 420, 220, div("align-items:flex-start;width:400px;height:200px",
   p("Large", "font-size:32px"), p("Small", "font-size:12px"), p("Body")))
fx("text_center_align", 320, 120, div("flex-direction:column;width:300px;height:100px",
   p("Centered lines", "text-align:center")))
fx("text_height_for_width", 320, 320, div("flex-direction:column;align-items:flex-start;width:300px;height:300px",
   p(LOREM, "width:120px"), p(LOREM, "width:250px")))
fx("text_grow", 420, 120, div("width:400px;height:100px", p("Grows", "flex-grow:1"), p("Fixed")))
fx("text_baseline", 420, 120, div("align-items:baseline;width:400px;height:100px",
   p("Big", "font-size:32px"), p("small", "font-size:12px"), box(30, 20), p("Body text")))
fx("text_max_width", 320, 220, div("flex-direction:column;align-items:flex-start;width:300px;height:200px",
   p(LOREM, "max-width:100px")))
fx("text_align_items_center", 320, 120, div("align-items:center;justify-content:center;width:300px;height:100px",
   p("Label")))
fx("text_newlines", 320, 220, div("flex-direction:column;width:300px;height:200px", p("First line\nSecond line\n\nFourth")))
fx("text_in_wrap", 320, 220, div("flex-wrap:wrap;width:200px;height:200px",
   p("One"), p("Two words"), p("Three more words here"), p("Four")))
fx("text_basis_zero", 320, 120, div("width:300px;height:100px", p("Equal share", "flex:1 1 0px"), p("of the row width", "flex:1 1 0px")))

# ---- intrinsic sizes -------------------------------------------------------------------------
fx("intr_row_in_flex_start", 320, 220, div("flex-direction:column;align-items:flex-start;width:300px;height:200px",
   div("gap:5px", box(50, 20), box(70, 30), box(30, 10))))
fx("intr_column_width", 320, 220, div("align-items:flex-start;width:300px;height:200px",
   div("flex-direction:column", box(50, 20), box(90, 30), box(30, 10))))
fx("intr_wrap_container", 320, 220, div("flex-direction:column;align-items:flex-start;width:300px;height:200px",
   div("flex-wrap:wrap", box(80, 20), box(80, 20), box(80, 20))))
fx("intr_text_row", 420, 220, div("flex-direction:column;align-items:flex-start;width:400px;height:200px",
   div("", p("Left words"), p("Right words"))))
fx("intr_abs_row", 320, 220, div("width:300px;height:200px",
   div("position:absolute;left:0;top:0;padding:5px", box(40, 20), box(60, 30))))
fx("intr_percent_in_auto", 320, 220, div("flex-direction:column;align-items:flex-start;width:300px;height:200px",
   div("", div("width:50%;height:20px"), box(80, 20))))
fx("intr_min_width_container", 320, 220, div("width:100px;height:200px",
   div("", box(60, 20, "flex-shrink:0"), box(60, 20, "flex-shrink:0")), box(30, 20)))
fx("intr_nested_text", 320, 220, div("flex-direction:column;align-items:center;width:300px;height:200px",
   div("padding:8px;border-width:2px", p("Button"))))

# ---- baselines ----------------------------------------------------------------------------------
fx("baseline_boxes", 320, 120, div("align-items:baseline;width:300px;height:100px",
   box(50, 20), box(50, 40), box(50, 30, "margin-top:10px")))
fx("baseline_nested", 420, 120, div("align-items:baseline;width:400px;height:100px",
   div("padding-top:10px", p("Inside")), p("Outside", "font-size:24px")))
fx("baseline_column_ignored", 120, 220, div("flex-direction:column;align-items:baseline;width:100px;height:200px",
   box(50, 20), p("Text")))
fx("baseline_wrap", 320, 220, div("flex-wrap:wrap;align-items:baseline;width:200px;height:200px",
   p("A", "font-size:24px"), p("b"), box(100, 30), p("C", "font-size:32px")))
# A container's baseline comes from its items: an empty first item, a first item of fixed
# height (whose size needs no measuring, but its baseline does), in a row and a column.
fx("baseline_first_empty", 420, 120, div("align-items:baseline;width:400px;height:100px",
   div("", box(20, 10), div("height:30px", p("Text"))), p("Outside", "font-size:24px")))
fx("baseline_first_fixed", 420, 120, div("align-items:baseline;width:400px;height:100px",
   div("", div("height:30px;padding-top:5px", p("Inner")), box(20, 10)), p("Outside", "font-size:24px")))
fx("baseline_first_fixed_column", 420, 120, div("align-items:baseline;width:400px;height:100px",
   div("flex-direction:column", div("height:30px;padding-top:5px", p("Inner")), p("Next")),
   p("Outside", "font-size:24px")))

# ---- Right to left ------------------------------------------------------------------------
# dir="rtl" on the fixture's root: main-start of a row is on the right; start and end
# alignment, auto margins, absolute insets and logical properties follow (CSS Writing Modes).
for j in JUSTIFY:
    fx(f"rtl_justify_row_{j}", 320, 120, rtl(f"justify-content:{j};width:300px;height:100px",
                                              box(50, 30), box(40, 20), box(60, 40)))
for j in ["flex-start", "flex-end"]:
    fx(f"rtl_justify_row_reverse_{j}", 320, 120,
       rtl(f"flex-direction:row-reverse;justify-content:{j};width:300px;height:100px", box(50, 30), box(40, 20)))
for a in ["flex-start", "flex-end", "center", "stretch"]:
    fx(f"rtl_column_align_{a}", 220, 220, rtl(f"flex-direction:column;align-items:{a};width:200px;height:200px",
                                               box(50, 30), div("height:20px"), box(80, 40)))
fx("rtl_column_align_self", 220, 220, rtl("flex-direction:column;width:200px;height:200px",
   box(50, 30, "align-self:flex-start"), box(50, 30, "align-self:flex-end"), box(50, 30, "align-self:center")))
fx("rtl_wrap", 320, 220, rtl("flex-wrap:wrap;width:200px;height:200px",
   box(80, 30), box(80, 40), box(80, 20), box(60, 30), box(90, 30)))
fx("rtl_wrap_reverse", 320, 220, rtl("flex-wrap:wrap-reverse;width:200px;height:200px",
   box(80, 30), box(80, 40), box(80, 20)))
fx("rtl_wrap_align_content", 320, 220, rtl("flex-wrap:wrap;align-content:space-between;width:200px;height:200px",
   box(120, 30), box(120, 40), box(50, 20)))
fx("rtl_gap", 320, 120, rtl("column-gap:12px;width:300px;height:100px", box(50, 30), box(40, 20), box(60, 40)))
fx("rtl_grow_shrink", 320, 120, rtl("width:300px;height:100px",
   div("flex:1 1 0px;height:20px"), div("flex:2 1 0px;height:20px"), box(40, 20, "flex-shrink:0")))
fx("rtl_baseline", 320, 120, rtl("align-items:baseline;width:300px;height:100px",
   p("Big", "font-size:32px"), box(40, 20), p("small", "font-size:12px")))
# Auto margins and physical sides
fx("rtl_margin_left_auto", 320, 120, rtl("width:300px;height:100px", box(50, 30), box(40, 20, "margin-left:auto")))
fx("rtl_margin_right_auto", 320, 120, rtl("width:300px;height:100px", box(50, 30, "margin-right:auto"), box(40, 20)))
fx("rtl_margin_both_auto", 320, 120, rtl("width:300px;height:100px", box(50, 30, "margin:0 auto")))
fx("rtl_margins_physical", 320, 120, rtl("width:300px;height:100px",
   box(50, 30, "margin-left:10px;margin-right:30px"), box(40, 20, "margin-left:5px")))
fx("rtl_padding_physical", 320, 120, rtl("padding-left:10px;padding-right:30px;border-width:4px;width:300px;height:100px",
   box(50, 30), box(40, 20)))
# Absolute positioning
fx("rtl_abs_left_top", 320, 220, rtl("width:300px;height:200px", box(40, 40),
   box(50, 30, "position:absolute;left:10px;top:20px")))
fx("rtl_abs_right", 320, 220, rtl("width:300px;height:200px;padding:7px", box(40, 40),
   box(50, 30, "position:absolute;right:10px;top:20px")))
fx("rtl_abs_static_row", 320, 220, rtl("justify-content:center;width:300px;height:200px", box(40, 40),
   box(50, 30, "position:absolute")))
fx("rtl_abs_static_start", 320, 220, rtl("padding-left:6px;padding-right:14px;width:300px;height:200px",
   box(50, 30, "position:absolute")))
fx("rtl_abs_static_column", 320, 220, rtl("flex-direction:column;align-items:flex-start;width:300px;height:200px",
   box(50, 30, "position:absolute")))
fx("rtl_abs_auto_margins", 320, 220, rtl("width:300px;height:200px",
   box(50, 30, "position:absolute;left:10px;right:40px;margin-left:auto;margin-right:auto")))
# Logical properties, both directions
for d, make in [("rtl", rtl), ("ltr", ltr)]:
    fx(f"{d}_margin_inline", 320, 120, make("width:300px;height:100px",
       box(50, 30, "margin-inline-start:10px;margin-inline-end:30px"), box(40, 20)))
    fx(f"{d}_padding_inline", 320, 120, make("padding-inline-start:12px;padding-inline-end:4px;width:300px;height:100px",
       box(50, 30), box(40, 20)))
    fx(f"{d}_inset_inline_start", 320, 220, make("width:300px;height:200px",
       box(50, 30, "position:absolute;inset-inline-start:10px;top:5px")))
    fx(f"{d}_inset_inline_end", 320, 220, make("width:300px;height:200px",
       box(50, 30, "position:absolute;inset-inline-end:10px;top:5px")))
# Within one declaration block the later of a logical and a physical property wins.
fx("rtl_logical_after_physical", 320, 120, rtl("width:300px;height:100px",
   box(50, 30, "margin-right:40px;margin-inline-start:10px")))
fx("rtl_physical_after_logical", 320, 120, rtl("width:300px;height:100px",
   box(50, 30, "margin-inline-start:10px;margin-right:40px")))
# Text and nesting
fx("rtl_text_align_start", 320, 120, rtl("flex-direction:column;width:300px;height:100px",
   p("Start aligned"), p("End aligned", "text-align:end")))
fx("rtl_text_wrap", 220, 220, rtl("flex-direction:column;align-items:flex-start;width:200px;height:200px",
   p("A paragraph that wraps over several lines", "width:120px")))
fx("rtl_nested_ltr", 320, 220, rtl("width:300px;height:200px",
   box(40, 40), ltr("width:150px;height:60px", box(30, 30), box(20, 20))))
fx("ltr_nested_rtl", 320, 220, div("width:300px;height:200px",
   box(40, 40), rtl("width:150px;height:60px", box(30, 30), box(20, 20))))


def main() -> int:
    out = HERE / "fixtures"
    out.mkdir(exist_ok=True)
    for old in out.glob("*.html"):
        if old.stem not in FIXTURES:
            old.unlink()
    for name, (w, h, nodes) in FIXTURES.items():
        body = "".join(render(n, 0) for n in nodes)
        (out / f"{name}.html").write_text(f"<!-- surface {w}x{h} -->\n{body}", encoding="utf-8")
    print(f"{len(FIXTURES)} fixtures", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
