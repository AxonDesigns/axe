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

## Layout animations

- **A node has two rects.** Its *layout rect* is what layout produced, relative to its
  parent's border box. Its *presentation rect* is where it is drawn, relative to its
  parent's presentation rect. They are the same unless a layout animation is running.
- **A node opts in with `transition(layout)`**, in a style or an override:
  `{UI_STYLE_TRANSITION(UI_STYLE_LAYOUT), ui_transition(stiffness, damping)}`. Whenever its
  layout rect changes, for whatever reason, its presentation rect springs from where it is
  drawn to the new layout rect. The reason can be its own style, a sibling, its parent's
  size, a reorder or the surface's size. Position and size are the four components of one
  spring.
  - A running animation retargets and keeps its velocity, so an interrupted move never
    jumps.
  - New nodes, and nodes shown again after `display: none`, appear at their layout rect.
  - Removing the transition ends a running animation at the layout rect.
- **Children are placed in their parent's presentation rect** at their own rects, so a
  subtree moves with its root. A node whose size animates draws its background and border
  at the animated size; its text and children keep their final layout. Nothing is scaled, as
  with FLIP without the scale.
- **`move_node` keeps a node where it is drawn.** Its presentation rect, and a running
  spring, are re-expressed relative to the new parent, and the next layout animates it into
  its new place. Moved again mid-flight, it carries on with its velocity. A node moved to
  another surface appears at its new layout rect.
- **A moving node and its subtree draw unsnapped**, and snap again on the frame the spring
  settles (`docs/styles.md`).
- **Layout itself never waits for an animation.** The layout rect is final at once: the
  next layout, and the measurement op, see it. A spring starts at the animation time of the
  frame whose layout changed the rect, and later frames advance it.

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
  first line, else its first item's, synthesized from that item's bottom border edge when
  it has none (§8.5): an empty first box gives its container its bottom edge, even when a
  later item has text.

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
- **Work is skipped where the result can't depend on it:**
  - The automatic minimum size is computed only when the line shrinks, or when an explicit
    `flex-basis` could be below it.
  - Baselines are computed only on request: a parent aligning by baseline asks for its
    items', and a container asked for its own measures its first item for it. A
    measurement without one is cached as such, and a later request for a baseline misses.
  - A min-content and a max-content height constraint share cache entries: a box's
    min-content and max-content heights are both its height at the width it gets.
  - An intrinsic measurement stops once the measured axis is known.
- **Layout runs as a surface draws**, after apply, styles and springs, and before the
  display list is built. `ui_layout_stats` counts computations, cache hits and passes.

In `test_layout`, a tree of 221 nodes lays out with about 840 computations. Changing one
leaf recomputes its row (whose items all changed size) and its ancestors, 26 computations;
the other rows come from their caches.

## Benchmarks

`bench/bench_layout` (release build) lays out a wrapping grid of 1,429 cards on a
1600×900 surface. Each card is a column of a title, a paragraph and a footer row (a label,
a button, an icon), with padding, gaps and a border: 10,005 nodes, 4,287 of them text.
Text is shaped beforehand, as the workers do before a buffer applies.

| Case | Median | Best | Work |
| --- | --- | --- | --- |
| Full layout, cold: every paragraph measured for the first time (one run) | 17–26 ms | | 22,872 computations |
| Full layout, every node's cache dropped, text measurements kept | 9–13 ms | 8.2 ms | |
| Incremental: one paragraph's text changes (target < 1 ms) | 0.68–1.4 ms | 0.46–0.60 ms | 34 computations, 4,419 cache hits |
| Style resolution of all 10,005 nodes after a token change | 4.0–5.9 ms | 3.8 ms | |

Ranges are over three runs of the benchmark on 2026-11-02, on a Ryzen 5 3400G (4 cores, 8
threads) while a browser kept about three threads busy, so medians varied by up to 2×; the
best run of each case is the steadier figure. The incremental case is dominated by the grid: the edited card's height
changes its row, so the grid lays out its 1,429 cards again, each from its cache (about
three hits per card). Making that cheaper needs a grid that relays out only the lines
after a change, which flexbox's algorithm doesn't offer.

## Conformance

`tools/layout-fixtures` holds 165 fixtures. Each covers one property or interaction:
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
| Result (2026-11-02) | **165 of 165 match** (100%) |
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
