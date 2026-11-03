# Layout

How Axe sizes and places nodes: its own flexbox engine with CSS semantics, checked against
Chrome. The choices behind it are logged in `docs/decisions.md` (Milestone 2); layout
properties are style properties (`docs/styles.md`).

## Model

- **Every node is a flex container** (`display: flex`), with CSS's initial values:
  - `flex-direction: row`, `flex-wrap: nowrap`, `flex: 0 1 auto`;
  - `align-items` normal (stretch), `justify-content` normal (flex-start);
  - `min-width` and `min-height` auto (the automatic minimum size of flex items, §4.5);
  - sizes are **border-box**: `width` and `height` include padding and border, and a box is
    never smaller than its padding plus border.
- **The surface is a column container** the size of the surface; top-level nodes are its
  items. A top-level node with `flex-grow: 1` fills the window.
- **A node with text is a text leaf.** Its paragraph fills its content box, as in a CSS block
  holding only text (or React Native's, Flutter's, SwiftUI's text):
  - Its min-content width is its widest grapheme cluster. Lines break anywhere when nothing
    else fits, as with `overflow-wrap: anywhere`.
  - Its max-content width is its widest line unwrapped.
  - Its height comes from the lines at the width it gets.
  - `text-align` and right-to-left start alignment work at any width.
  - In-flow children of a text node lay out in the same content box, over the text.
- **`position: absolute`** takes a node out of the flow and places it in its parent's
  padding box. Every node is a containing block, as if every element were
  `position: relative`.
  - Insets place it, and both insets on an axis give its size.
  - Auto margins center it between two insets.
  - With no inset on an axis, it sits where a sole flex item would (its static position):
    `justify-content` places it on the main axis and `align-self` on the cross axis.
- **`display: none`** removes a node and its subtree from layout and drawing.
- Supported properties: direction, wrap, `justify-content`, `align-items`, `align-self`,
  `align-content`, grow, shrink, basis, gaps, sizes with min and max, margins (auto
  included), padding, border width, insets, `aspect-ratio`, `display`, `position`. Lengths
  are px, percent or auto (`docs/styles.md`).

## set_rect

`set_rect(x, y, w, h)` sets the node's frame as overrides: `position: absolute`, `left: x`,
`top: y`, `width: w`, `height: h`, in px. With no padding or border on the parent, that is
exactly what `set_rect` always did, so code written before layout keeps working.
- `start_spring` on the rect moves the frame; the node and its subtree are laid out again
  on every frame of the spring, so text reflows as it did before layout.
- To return a node to the flow, unset those overrides (`set_props` with `unset` for
  `position`, `left`, `top`, `width` and `height`).

## The algorithm

`src/layout/flex.c` follows CSS Flexible Box Layout Level 1, §9:
1. Base sizes and hypothetical main sizes.
2. The container's main size.
3. Lines.
4. Resolving flexible lengths with the freeze loop. Shrinking is weighted by the inner
   (content-box) base size.
5. Hypothetical cross sizes.
6. Line cross sizes, with baseline alignment.
7. `align-content: stretch`, stretched items.
8. Main and cross axis alignment, with auto margins.
9. Absolutely positioned children.

Two details decide most results:
- **Content measures ignore the node's own constraints in the measured axis.** A flex
  item's content-based base size and its automatic minimum size measure its content, not
  its own `width`, `min-height` and so on in that axis. Constraints in the other axis still
  apply: a `max-width` decides where text wraps, and so the item's height.
- **Only definite sizes resolve percentage heights (§9.8).**
  - A main size is definite when the container's main size is.
  - A cross size is definite when it is specified, or when the item stretches.
  - Widths are settled once laid out, so percentage widths always resolve then.
  - While measuring intrinsic sizes, percentages are cyclic and count as auto.
  - Only an `auto` cross size stretches. A percentage height that can't resolve doesn't.

Intrinsic sizes of flex containers follow Chrome rather than the specification's §9.9.1:
- A row's max-content width is the sum of its items' contributions plus gaps.
- A row's min-content width is that sum too, or, when wrapping, the largest contribution.
- A column's width is its widest item.

Baselines:
- **In a row,** an item's baseline is its first line's, else its bottom border edge.
- **In a column,** items have no baseline in the cross axis, so `baseline` lines up their
  start border edges.
- **A container's first baseline** is that of its first baseline-aligned item on the
  first line, else its first item's.

## Caching and invalidation

- **Each node keeps up to six measurements and its last full layout**, keyed by:
  - what its parent fixed (`known` sizes);
  - the space available: a size, min-content or max-content;
  - which axis's own constraints were ignored;
  - its parent's content size, but only for a node with percentage sizes or padding.
- **A clean node asked with the same inputs answers from its cache.** When its full layout
  is reused, its whole subtree is skipped.
- **Changes mark a node dirty, and every ancestor up to the surface:**
  - a resolved layout value or the border width changing;
  - new text, or the line height or max lines changing;
  - creating, destroying or moving a node;
  - a rect spring moving the frame;
  - the surface's size or scale changing (a new scale re-measures every text node).
- **Paint changes don't lay out.** Neither does a frame with nothing dirty.
- **The automatic minimum size is computed only when it can matter:** when the line
  shrinks, or when an explicit `flex-basis` could be below it.
- **Layout runs as a surface draws**, after apply, styles and springs, and before the
  display list is built. `ui_layout_stats` counts computations, cache hits and passes.

In `test_layout`, a tree of 221 nodes lays out with about 1,060 computations. Changing one
leaf recomputes its row (whose items all changed size) and its ancestors, under 50
computations; the other rows come from their caches.

## Conformance

`tools/layout-fixtures` holds 162 fixtures. Each covers one property or interaction:
- direction × `justify-content`, `align-items` and `align-self`;
- grow, shrink and basis;
- wrap and `align-content`, gaps;
- min/max interactions, margins (auto included), padding and border;
- absolute positioning;
- `display: none`, `aspect-ratio`;
- nesting, intrinsic sizes, baselines;
- text measurement and wrapping.

The pipeline:
- **`make_fixtures.py`** writes the fixtures from compact specs. Each fixture is a fragment
  of `<div>` nodes and `<p>` text leaves with inline styles.
- **`fetch_chrome.py`** downloads `chrome-headless-shell` 156.0.8078.80 from Chrome for
  Testing, pinned by sha256, into the gitignored `third_party/chrome-headless-shell/`.
- **`record.py`** renders each fixture in a page whose prelude matches the model above and
  records every element's border box relative to its parent into `expected/*.json`:
  - every `div` is `display: flex; position: relative; box-sizing: border-box`;
  - every `p` is a block in the test font (Noto Sans regular, static) at line-height 1.25,
    with `overflow-wrap: anywhere` and `white-space: pre-wrap`;
  - Chrome runs with no font hinting, at scale 1.
- **`gen.py`** turns fixtures and results into `tests/layout/fixtures_generated.c`.
- **`test_layout_conformance`** builds each tree through the wire, lays it out with the
  same font, and compares every box. CI never runs Chrome; the results are committed.

| | |
| --- | --- |
| Tolerance, boxes without text | 1/64 px (Chrome's LayoutUnit) |
| Tolerance, fixtures with text | 0.5 px |
| Result (2026-11-02) | **162 of 162 match** (100%) |
| Known deviations | none (`tools/layout-fixtures/deviations.md`) |

Text matches within the tolerance because both sides shape with HarfBuzz at the same size,
and the fixtures use line heights that are whole pixels: 1.25 × 12, 16, 24 and 32 px.

To regenerate after changing fixtures:

```sh
python3 -I tools/layout-fixtures/fetch_chrome.py   # once
python3 -I tools/layout-fixtures/make_fixtures.py
python3 -I tools/layout-fixtures/record.py
python3 -I tools/layout-fixtures/gen.py
```

`UI_LAYOUT_FIXTURE=<name> build/debug/tests/test_layout_conformance` prints every box of
one fixture next to Chrome's.

## Limitations

- Left-to-right only: `direction: rtl` doesn't mirror rows yet (text inside is bidi).
- No `order`, no `flex-basis: content`, no `visibility: collapse`, no grid (milestone 2.1).
- One border width for all sides.
- Intrinsic sizes follow Chrome's simpler algorithm, not §9.9.1's flex fractions; the
  fixtures pin Chrome's behavior.
- Fixtures run at scale 1; at other scales layout is the same in logical px, and only text
  measures in physical pixels.
