# Styles

How a node's paint (and, from the layout milestone on, its layout) is described: interned
styles applied by ID, per-node overrides, theme tokens, state variants and transitions.
The choices behind it are logged in `docs/decisions.md` (Milestone 2); the byte layout of
every op is below and in `docs/command-buffer.md`.

## Model

A **style** is a list of blocks of declarations, defined once by ID (`define_style`) and
applied to a node by listing IDs (`set_styles`). A block applies when the node has every
state the block requires; the first block usually requires none (the base block).

Every node resolves its values from, lowest precedence first:

1. **Initial values:** transparent background, no border, radius 0, opacity 1, no clip.
2. **`set_text`'s paragraph values:** the text's color, line height, alignment and max lines,
   for a node with text.
3. **The matching blocks of its styles**, ordered by:
   1. specificity: the number of states a block requires, so base blocks come first;
   2. the style's position in the node's list, so later styles win;
   3. the block's position in its style.

   So a hover block beats every base, a later style's hover beats an earlier style's
   hover, and a hover+pressed block beats any single-state block, whichever style it is
   in. This is how pseudo-class specificity works in CSS.
4. **Per-node overrides** (`set_props`), and `set_color`, `set_corner_radius`, `set_clip`
   (see [Older ops](#older-ops)).

A style ID that isn't defined (yet, or any more) contributes nothing. Redefining a style
resolves every node that lists it again, which is what hot reload needs. Nothing inherits:
a parent's text color doesn't reach its children's text. Tokens carry theming;
inheritance of text properties is planned for M2.1.

### Tokens

A declaration's value can be `token(id)`: the value of theme token `id`, set with
`set_token`. Changing a token resolves every node that uses it again, through styles or
overrides, so switching a theme is one op per token.

- **A token's value** is a length, a percentage, a number, a color, a keyword, auto, none,
  or another token. Chains resolve when used.
- **A declaration counts as absent**, and the next lower declaration shows through, when:
  - its token is missing;
  - the chain loops (Floyd's cycle detection, so a cycle of any length is found);
  - the value is one the property can't take (a length for a color, a negative border
    width).
- **Removing a token** (`set_token` with `unset`) is the same as never defining it.

### States

Each node has 8 state bits; five are named: hover 1, pressed 2, focused 4, disabled 8,
selected 16. Input sets them from milestone 4; until then `set_state(node, mask, value)`
sets the bits in `mask` to those in `value`. A block requiring a state the core doesn't
know yet simply never matches.

### Transitions

A declaration of `transition(P)` with a `spring(stiffness, damping)` value makes property P
animate whenever what it resolves to changes, for any reason: a different style list, a
token, a state, an override, a redefined style, or new text with another color.

- **A running spring retargets and keeps its velocity**, so interrupting it never jumps.
- **A node's first resolution never animates**: new nodes appear at their values.
- **Transitionable properties:** background color, border color, corner radius, opacity
  and text color. Colors spring in linear light.
- **Layout properties don't transition.** Their changes animate through layout animations
  (milestone 2, later step).
- **`none`** turns a transition off (from a later style or an override).

## Properties

| Id | Property | Values | Initial |
| --- | --- | --- | --- |
| 1 | `background_color` | color | transparent |
| 2 | `border_color` | color | transparent |
| 3 | `border_width` | px ≥ 0, all four sides | 0 |
| 4 | `corner_radius` | px ≥ 0, all four corners | 0 |
| 5 | `opacity` | number in [0, 1]: the node and its subtree, as one group | 1 |
| 6 | `clip` | keyword: 0 none, 1 rect (text and descendants inside the node's rect) | 0 |
| 7 | `text_color` | color: the text's default style; spans keep their own colors | `set_text`'s |
| 8 | `line_height` | number in (0, 100] (× the font size), or auto (from the fonts) | `set_text`'s |
| 9 | `text_align` | keyword: start, end, left, right, center, justify (0–5) | `set_text`'s |
| 10 | `max_lines` | whole number ≤ 65535 (0 = unlimited), or none | `set_text`'s |
| 0x8000 \| P | transition of P | spring, or none | none |

Layout properties ([`docs/layout.md`](layout.md)); keywords are the `ui_flex_*`,
`ui_display` and `ui_position` values in `include/ui/wire.h`:

| Id | Property | Values | Initial |
| --- | --- | --- | --- |
| 32 | `display` | keyword: flex, none | flex |
| 33 | `position` | keyword: relative, absolute | relative |
| 34 | `flex_direction` | keyword: row, row-reverse, column, column-reverse | row |
| 35 | `flex_wrap` | keyword: nowrap, wrap, wrap-reverse | nowrap |
| 36 | `justify_content` | keyword: normal, start, end, center, space-between, -around, -evenly | normal (start) |
| 37 | `align_items` | keyword: normal, start, end, center, baseline, stretch | normal (stretch) |
| 38 | `align_self` | keyword: auto, start, end, center, baseline, stretch | auto |
| 39 | `align_content` | keyword: normal, start, end, center, stretch, space-* | normal (stretch) |
| 40, 41 | `flex_grow`, `flex_shrink` | number ≥ 0 | 0, 1 |
| 42 | `flex_basis` | px, percent (≥ 0) or auto | auto |
| 43, 44 | `row_gap`, `column_gap` | px or percent, ≥ 0 | 0 |
| 45, 46 | `width`, `height` (border box) | px or percent (≥ 0), or auto | auto |
| 47, 48 | `min_width`, `min_height` | px or percent (≥ 0), or auto | auto |
| 49, 50 | `max_width`, `max_height` | px or percent (≥ 0), or none | none |
| 51–54 | margins (top, right, bottom, left) | px or percent (may be negative), or auto | 0 |
| 55–58 | padding (top, right, bottom, left) | px or percent, ≥ 0 | 0 |
| 59–62 | insets (top, right, bottom, left) | px or percent (may be negative), or auto | auto |
| 63 | `aspect_ratio` | number > 0 (width / height), or auto | auto |

Percentages of padding and margins resolve against the containing block's width, as in
CSS. `border_width` (3) also takes part in layout.

Every value property also accepts `token`. The text properties don't change shaping, so
setting them never reshapes text. Family, size, weight, slant and language stay on
`set_text`, which is prepared off the core thread (`docs/text.md`).

### Drawing

- **Borders** are drawn inside the node's edge, over the background, which covers the whole
  rect as with CSS `background-clip: border-box`. The inner corners have the outer radius
  minus the border width. A border at rest is a whole number of physical pixels, at least
  one, as browsers draw them.
- **Opacity** applies to the node and its subtree as one group: they are drawn into an
  offscreen layer, which is then composited at that opacity, as CSS does. Overlapping
  children inside a group therefore don't show through each other.
  - A node with nothing but a background needs no layer: its alpha is multiplied.
  - Opacity 0 skips the node and its subtree entirely.
- **Snapping:** a box at rest snaps to whole physical pixels, with its origin and its size
  each rounded, so a box never changes size depending on where it sits. A box that is
  moving (its rect springs), and everything inside it, is drawn at its exact position, and
  snaps again on the frame the spring settles. Text baselines always snap, so moving text
  steps vertically by whole pixels.

## Ops

All are in wire v0. Older readers drop them as unknown ops.

| Opcode | Op | Payload |
| --- | --- | --- |
| 12 | `define_style` | `u32 id` (not 0), `u16 header_bytes` (8), `u16 block_count`, then the blocks |
| 13 | `destroy_style` | `u32 id` |
| 14 | `set_styles` | node, `u16 count` (≤ 255), `u16 header_bytes` (12), `u32 ids[count]` (not 0) |
| 15 | `set_props` | node, `u16 header_bytes` (12), `u16 entry_count`, then the entries |
| 16 | `set_token` | `u32 id` (not 0), then one entry whose property is 0 |
| 17 | `set_state` | node, `u32 mask`, `u32 value` |

A **block** starts with an 8-byte header: `u16 block_bytes` (header included), `u8 states`,
`u8` reserved, `u16 entry_count`, `u16 header_bytes` (8; 0 = 8). Its entries follow at
`header_bytes`.

An **entry** is `u16 property`, `u8 kind`, `u8 words` (its size in 4-byte words, header
included), then the value:

| Kind | Value | Words |
| --- | --- | --- |
| 1 `unset` | none (overrides and tokens only: removes it) | 1 |
| 2 `px`, 3 `percent`, 6 `number` | `f32` | 2 |
| 4 `auto`, 5 `none` | none | 1 |
| 7 `color` | `f32 r, g, b, a`: sRGB, straight alpha, clamped to [0, 1] | 5 |
| 8 `keyword`, 9 `token` | `u32` | 2 |
| 10 `spring` | `f32 stiffness` (> 0), `f32 damping` (≥ 0) | 3 |

- **Newer writers.** A reader skips an entry whose property or kind it doesn't know, using
  `words`, and reads the fields it knows from a longer entry. Blocks and the op headers
  carry their size, so they can grow too.
- **Framing errors** reject the whole buffer: an entry of 0 words or one that runs past its
  block, a block shorter than its header or past the payload, ids or entries past the
  payload.
- **Invalid values** drop the op: a known property with a kind it doesn't take, a value out
  of range, `unset` in a style, a 0 id. The writer refuses these too
  (`UI_ERR_INVALID_ARGUMENT`).

## Older ops

`set_color`, `set_corner_radius` and `set_clip` set the node's overrides of background
color, corner radius and clip. `start_spring` on color or radius sets the override and
animates toward it with its own spring, whatever the styles declare. Without a declared
transition a `set_*` op jumps and stops a running spring, as it always did; with one, the
change animates like any other. `set_rect` and `start_spring` on the rect are unchanged in
this step (the layout step redefines them).

## Limitations

- One border width and color for all four sides, and one radius for all four corners.
- Clipping is rectangular; rounded clips need masks in the layers.
- Layers are drawn again every frame. Caching them comes with damage tracking.
- No inheritance (planned for M2.1), and no z-index: paint order is tree order.
