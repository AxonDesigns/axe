# Decisions log

Choices the architecture document leaves open, recorded as they are made. Newest
milestone first.

## Milestone 2.2: the benchmark guard

### Baselines from CI
- **The artifact is a baselines file.** `callgrind.py --out` writes `bench/baselines.json`'s
  format, stamped with the CI run that measured it (run ID, attempt, workflow, event, ref,
  commit, URL). Outside CI the stamp is empty, and `--update` refuses such counts.
  Baselines can only be copied from CI. Locally the same code measured 1–6% off CI, enough
  to fail three cases against CI's baselines.
- **Baselines:** from CI run 41687697393 (`ci`, a push to master at 2927f9e, the guard in its
  final form), committed alone (a1be2c5). Every count equals the M2.1 baselines (run
  41675753054), as expected: M2.2 changed no framework code. The file now records the run
  that measured it.

### The guard
- **Behavior:** a case more than 2% above its baseline fails the job, as does a baseline
  case with no count. A case more than 2% below passes and is reported, with a note to
  update the baseline. A case without a baseline passes as "new".
- **The log:** the job lists every case as baseline, measured and change, then a verdict
  naming the cases and what to do. The same table goes in the run's summary.
- **Running the guard alone:** `ci` takes a `jobs` input (`all` or `bench`) when started
  by hand, so checks like the ones below run only the guard.

### Proving it
Both checks ran on a throwaway branch, `tmp-guard-check` (deleted afterwards), with
`jobs=bench`, against the baselines from CI run 41675753054:

- **Regression (run 41678755948, failed as intended).** A volatile 40-iteration loop in
  `resolve_node` gave `bench_layout/token` +7.46% and `bench_layout/inherited` +8.89%. The
  job failed with "FAILED: 2 case(s) more than 2% above their baselines: ...". Every other
  case stayed within 0.01%.
- **Improvement (run 41679348724, passed).** Skipping the background and text paint
  updates in partial resolutions (wrong on screen, less work) gave token −21.30% and
  inherited −24.92%. The job passed and reported "Improved by more than 2%: ... update the
  baselines from this run's bench-callgrind artifact".
- **A misfire:** run 41679271921 re-ran the regression commit by mistake and was
  cancelled.

### actionlint
- **A job in `ci.yml`** downloads actionlint's release binary (v1.7.12), pinned by version
  and sha256 and checked with `sha256sum --check`; no Docker.
- **Its first run (41678692413) found two issues,** both shellcheck SC2012 in `fuzz.yml`
  (the corpus counted with `ls | wc -l`), now counted with `find corpus -type f`. Nothing
  in `ci.yml` or the setup action.
- **Not from actionlint:** the runner warns that `actions/checkout@v4` (and the other v4
  actions) target the deprecated Node.js 20. They still run, forced onto Node 24; moving
  to newer major versions is left for a separate change.

## Milestone 2.1: inheritance, direction, borders and polish

### Test infrastructure
- **ASan tests keep shared objects loaded** (`tests/keep_loaded.c`, linked only when
  `UI_SANITIZE` has `address`). GPU drivers leak a few one-time allocations, which
  `tools/lsan.supp` suppresses by library name. Since 2026-11-03 the leaks on the
  development machine came from libraries the Vulkan loader had already unloaded, which
  LeakSanitizer reports as `<unknown module>`: unmatched, so 9 GPU tests failed under
  ASan, on M2's code as much as on new code. A no-op `dlclose` in the executable keeps every
  library mapped until exit, and the suppressions match again. The cause on this machine
  wasn't pinned down (no driver or loader update; a reboot in between).

### Example
- **`examples/layout_cards` turns right-to-left** every 6 s (`--rtl` to start so,
  `--no-direction` to stay). The page's `direction` changes, and the cards slide to their
  mirrored places through their layout transitions. There are no input events yet, so a
  timer stands in for a toggle key, as it does for the theme.
- **Cards have a 4 px inline-start border** in the accent color, and **buttons rounder start
  corners** (logical properties), so both visibly follow the direction. Paragraphs take
  their direction from the node (`set_text` leaves it out of `.set`).
- **Checked** headless (captures in both directions) and in a nested headless sway at 1.25
  under both schedulers: 9 s each, through shuffles, a theme switch and a direction flip.
  Frames presented on refresh (about 6.2 ms), none late, no warnings.

### Benchmark reliability
- **Every case prints median and best.** `bench_text` now does too. On this desktop,
  medians move 10–20% between runs; the best run is steadier.
- **Instruction counts in CI** (job `bench-callgrind`, CONTRIBUTING.md "Benchmarks"):
  - Each CPU case is a noinline `bench_case_*` function holding exactly what is timed.
    That covers `bench_layout`'s five cases and `bench_text`'s shaping, re-layout and
    rasterization.
  - `--callgrind` runs each once and skips the GPU cases.
  - `tools/bench/callgrind.py` runs each benchmark under Callgrind with
    `--toggle-collect=bench_case_*` and reads each case's inclusive count. It compares
    the counts with `bench/baselines.json` and uploads them as an artifact.
  - Both benchmarks use only the fonts in `tests/fonts` (system fonts are off by default),
    so counts don't depend on the machine's fonts.
- **Deterministic:** two runs gave identical counts, both locally and in the
  `axe-ci-ubuntu:24.04` image.
- **Toolchain-dependent:** the image's counts differ from this machine's by 0.3–6%. That is
  why the plan has baselines come from CI's artifact.
- **The threshold is 2%,** as proposed. A case over its baseline by more fails the job; one
  under by more is reported as an improvement to record.
- **The first baselines were provisional:** no CI run existed yet, so they were counted in
  the Docker image (Ubuntu 24.04, Clang 22, valgrind 3.22 from apt, as CI) and marked
  `provisional`: reported, never failing. CI's first `bench-callgrind` run (41675753054, at
  ccf3131) matched them exactly except `bench_text/shape` (+0.08%), and its counts replaced
  them in a commit of their own. They are enforced since.
- **Valgrind** comes from apt in CI (3.22); locally it is 3.25. Baselines are only compared
  with counts from the same CI image, so the difference doesn't matter.
- **`bench_layout`'s token case** used to set, on its first run, the value the scene was
  built with. With one run under Callgrind it measured a change that changed nothing. The
  alternation now starts with a real change.

### Style resolution performance
- **Profiled first** (Callgrind on `bench_layout`'s token case: one token that all 10,005
  nodes read for their background, through one style): 28.9M instructions and 6.4 ms. Of
  them, `update_paint` 22%, `resolve_node` 10%, computing 8.6%, gathering cascades again
  8.4% (every user's, although a token never changes which entries apply), sRGB to linear
  8% (`powf` on every color, on every computation), the depth sort 8%, copying and
  comparing whole computed values 11%, and the scan for users 5%.
- **Property families** (docs/styles.md, "Resolution"): background, border colors, radii,
  opacity, text (inherited text values but direction) and other (layout, direction,
  transitions, the rest). A node can be queued for some families; computing them resets
  only their values (the parent's for text), applies only their entries, and runs only
  their effects. Other means everything, since direction maps every family's logical
  properties. The plan said "per property"; families are as precise for themes (a theme
  token feeds one or two families) with far less bookkeeping.
- **Token changes queue families, not cascades.** A user is queued with the families of
  its entries reading a changed token (closed over chains); its cascade isn't gathered
  again. A token that makes a declaration invalid needs nothing special: computing treats
  the entry as absent and the lower one shows, so the plan's "cascade again when validity
  changes" wasn't needed. Redefined and destroyed styles still gather their users'
  cascades again.
- **Finding users without indices.** Each node keeps a 64-bit filter of the token ids its
  cascade reads and the families of those entries, rebuilt whenever it is computed
  entirely. A token change scans nodes' filters (0.06 ms for 10k) and reads a cascade only
  when the filter matches and more than one family reads tokens. The planned style → nodes
  and token → nodes indices would save part of that scan but need upkeep in `set_styles`,
  overrides and destruction; not worth it at this cost.
- **Inherited changes** queue children with the text family only (everything on a
  direction change, or for a child taking `inherit` on a non-inherited property). The
  paragraph is keyed again only if a shaping value changed, and its line parameters set
  only if line height or alignment did.
- **One walk instead of a sort** for queues over 64 nodes and an eighth of the scene: the
  tree in order, resolving queued nodes and what they reach. What the walk doesn't reach
  (nothing while every node is under a surface) is sorted as before.
- **Colors are linear from decoding:** entries, tokens and the overrides `set_color` and
  `start_spring` write convert once, so computing copies them.
- **Memory decides the rest.** 10k nodes take 10.5 MB, beyond this machine's 4 MB L3, so
  after the above the time followed cache lines touched, not instructions. `ui_node`'s
  fields are now grouped by use (the tree and style bookkeeping, what is drawn, the computed
  values with the paint ones first), entries carry their family and logical side, and
  computing no longer reads the direction unless a logical entry needs it, nor `set_text`'s
  source unless the text family is computed. The last two alone took the token case from
  about 1.06 to 0.76 ms. Prefetching the next nodes in the walk didn't help.
- **Results** (release, Ryzen 5 3400G, medians of 15 runs over several runs of the
  benchmark on 2026-11-03; 10,005 nodes):

  | Case | Before | After | Instructions (Callgrind) |
  | --- | --- | --- | --- |
  | Token change (a background) | 6.4 ms | 0.76–0.90 ms | 28.9M → 5.6M |
  | Inherited text color at the root | 5.0 ms | 1.8–2.0 ms | 4.7M after |

  The token case meets the < 1 ms target with little margin; it is bound by memory (about
  three cache misses per node), so a slower memory system would miss it. Going further
  means splitting `ui_node` into hot and cold arrays. The inherited case's remaining misses
  are the text values themselves and each text node's `set_text` source.
- **Checked against resolving everything:** `ui_scene_check_resolved` computes every node
  from scratch and compares (and that what is drawn follows the values, or a spring heads
  there). `test_style`, `test_layout` and `test_layout_anim` run it after every resolution;
  `fuzz_commands` traps on a difference. Planting two bugs (text changes not reaching
  children; a partial background not drawn) failed it each time.
- **`fuzz_commands` can split its input** (bits 3–7 of the first byte, unused before): the
  ops before the split apply and resolve, then the rest, then the whole buffer twice as
  before. Applying the same buffer twice re-applies every node's own ops, so changes to an
  already resolved scene (a token under styled nodes, the walk) were nearly unreachable.
  A zero split is the old behavior, so the corpus keeps its meaning; `seed15` splits after
  80 styled nodes and changes their tokens.

### Conformance at fractional scales
- **Every fixture is recorded at 1, 1.25 and 1.5** (`record.py`, `expected-125/`,
  `expected-150/`; the 1.0 results didn't change on re-recording) and
  `test_layout_conformance` runs all three, reporting a pass rate per scale. Deviations can
  name a scale. Tolerances are unchanged: 1/64 px for boxes, 0.5 px with text.
- **First run: 209, 202 and 206 of 209.** Every failure was a baseline-aligned fixture off
  by exactly one device pixel. Chrome rounds a font's ascent and descent to whole device
  pixels and floors the half-leading; we rounded the exact baseline instead. Fixed-height
  lines now place their baseline Chrome's way: **209 of 209 at every scale.** One text
  golden moved (`justify@125`, lines one pixel higher).
- Borders snapping in layout (the per-side border step) was already in place; without it
  bordered fixtures would differ at fractional scales.

### Text nodes with in-flow children
- **A mistake, reported, not refused:** layout keeps placing such children over the text
  (as in M2), but a debug warning names the node once (a node flag) and the new stat
  `text_nodes_with_children` counts such nodes. Refusing them would break apps that do it
  today; the warning tells them how to fix it. Absolutely positioned children are exempt.
- Detected where layout lays the text leaf out, so a hidden child or one added later is
  seen exactly when it would be drawn over the text (`test_layout`'s `test_text_children`).

### Content during layout animations
- **Clip by default** (as planned): while a node's presentation size differs from its
  layout size, its text and children are clipped to its padding box as drawn. The clip is
  rectangular (rounded clipping is out of scope), and nothing is clipped at rest.
- **`content_animation: reflow`** (property 17, not inherited; decided in review: the
  node's own text only, still clipped) wraps the paragraph to the presentation's content
  width each frame. It only breaks lines again, never shapes. Children keep their final
  layout in both modes, so measurement and layout never see an intermediate size.
- **Tests:** `test_layout_anim`'s `test_content_clip` (the child's batch is clipped while
  the box shrinks, unclipped at rest) and the golden `anim_content_mid_shrink@125` (both
  modes mid-shrink, identical under both schedulers).

### Per-side borders and per-corner radii
- **Longhands** 66–77 (side widths and colors, corner radii) and logical ones 86–93; the
  existing `border_width`, `border_color` and `corner_radius` are shorthands that set all
  four at their place in the cascade. Logical borders and radii map with the logical side
  machinery (seven groups: margins, padding, insets, border widths, border colors, top
  corners, bottom corners), with the shorthands counting as setting both sides.
- **Transitions stay on the shorthands:** border colors are four springs under one
  transition, radii one four-component spring. `ui_node.jump` grew to 16 bits.
- **Layout snaps border widths** to whole device pixels at the surface's scale (decided in
  review: as Chrome). A scale change marks every bordered node dirty. Goldens regenerated:
  `paint_borders` and `paint_flex_text` at 1.25 and 1.5, whose bordered boxes are up to a
  pixel smaller now (a 1 px border is 0.8 px of layout at 1.25×).
- **Two shaders.** Boxes with one radius, width and color keep `rrect.wgsl` and its 64-byte
  instances; the rest use `border.wgsl` with 128-byte `ui_dl_border` instances in their own
  display-list batches, keeping paint order.
- **The shader** picks the corner whose region (within its radii of both edges) holds the
  pixel and measures the implicit ellipse over its gradient, bounded by the edges; picking
  by quadrant first drew notches when a radius exceeded half the box. Distances are in
  physical pixels, so the anti-aliasing ramp is one pixel wide: `fwidth` of a distance that
  switches formula at a region boundary left light ticks. A pixel's side is the one with
  the smallest distance-to-edge ÷ width, which puts the boundary on the outer-to-inner
  corner diagonal; the seam is blended over one pixel measured across that line (the
  ratio difference × w₁w₂ / |(w₁, w₂)|). A first version blended over `fwidth` of the ratio,
  which faded seams over several pixels (the user spotted it).
- **Radii are scaled on the CPU** (CSS's factor: the smallest side ÷ sum of its radii).
- **Compared with Chrome** by `tools/border-compare/compare.py`: 1.0–1.4% of pixels differ
  by more than 8 levels, along curve and seam anti-aliasing (`docs/styles.md`, "Borders").

### Direction and logical properties
- **`direction`** is an inherited style property (16) and a surface setting
  (`ui_surface_desc.direction`, what top-level nodes inherit). It lays out the node's
  children (rows mirror) and is its paragraph's base direction unless `set_text` sets one.
- **Mirroring by swapping sides.** A right-to-left container lays out in a mirrored frame:
  its own and its items' left and right padding, borders, margins and insets are swapped,
  the algorithm runs unchanged, and x positions are mirrored back. Sizes don't depend on
  direction, so the layout cache needs no direction in its key; a direction change marks
  the node dirty like a layout value.
- **Logical properties** 80–85 (start/end margins, padding, insets) map onto the physical
  slots with the node's own direction, after the cascade has run, and win where they came
  later than the physical entry for that side. The computation keeps the cascade position
  of each side's last physical and logical entry, so a direction change remaps them
  without gathering the cascade again. `inherit` on a logical property takes the parent's
  value of the physical side it maps to (CSS maps with the parent's direction; documented).
  Logical border widths, colors and radii come with per-side borders.
- **Fixtures:** 44 right-to-left fixtures (`dir="rtl"` on the fixture root, also `ltr` for
  logical properties). All 209 match Chrome; the rtl group matched on its first run.
  `test_layout_conformance` leaves each paragraph's direction to its node, as `<p>`
  inherits it in HTML.

### Shaping properties in styles, through preparation
- **`set_text` leaves fields to the node** (decided in review: the C API inherits by
  default). `ui_text_desc.set` / `ui_text_span_desc.set` name the explicit fields; the rest
  go on the wire as `from_node` bits, a u16 at header offset 70 and a u8 at span offset 13,
  both formerly reserved, so 0 (every field set) is what older writers meant. Fields left to
  the node are written as valid placeholders, which an older reader shows instead of
  dropping the op. Every existing caller (tests, examples, benches, tools) now passes
  `UI_TEXT_SET_ALL`; no golden changed.
- **A paragraph's key** is its node's text context (what its styles decide, and its
  parent's values for the rest; `ui_text_ctx`) combined with its `set_text`
  (`ui_text_ctx_key`). One function computes it for the scene, for previews and for
  workers, so they can't disagree. When the key differs from the `set_text`'s own values,
  the paragraph shows a **derived source**: the same text (shared by reference), with the
  key in its base style and in every span field left to the node.
- **Prepared shaping is matched, not assumed.** Whatever preparation shaped is offered to
  the node when its buffer applies, and adopted when the node resolves if its source
  shapes as the node's key then (`ui_text_source_shapes_as`). A miss shapes on the core and
  counts in `text_shaped_on_core`.
- **Previews** (`src/core/preview.h`) read the scene through an overlay of the ops that can
  change text values (creating, destroying and moving nodes; style lists, overrides,
  states, `set_text`; definitions and tokens, in a private `ui_styles`). They compute only
  the nodes asked about and their ancestors, iteratively; untouched nodes under unchanged
  parents answer from the scene. A buffer that only sets text, or changes styles in ways
  that can't touch text values, doesn't scan the scene's other paragraphs.
- **Guesses are made against the head-time state.** When a buffer arrives, its text is
  shaped with the values a preview of the scene plus every pending buffer up to it gives,
  which is exactly what its head step will see. So the head step almost never prepares text
  again, and what it does is the same under both schedulers.
- **Head steps may now run inside the apply phase** (changed from M2, where they ran only at
  the end of a core step): a buffer that reaches the head while the frame applies gets its
  head step there, and applying stops if that gave the workers work. Without this, a
  `set_text` buffer queued behind another would always wait a frame. Font loads follow the
  same rule; `test_font_loading` and `test_prepare` still pass unchanged.
- **The fuzzer checks previews against reality:** `fuzz_commands` previews every input
  buffer, applies it, and traps if any node's text context differs from the preview's.
  `fuzz_set_text` now resolves after applying, so derived sources are fuzzed too. Within
  minutes it found two ways a preview disagreed with applying, both fixed with a seed:
  - a parent's `set_text` that applying drops (an invalid span, say) was read from its
    header alone, so its children were predicted to inherit from it. Previews now decode a
    `set_text` exactly as applying does (lazily: only a text node with children needs it),
    falling back to an earlier one in the buffer or the scene's when it is dropped
    (`regress-preview-dropped-set-text`);
  - `create_node` was predicted to succeed with a handle whose generation the slot had
    already used, which the handle table refuses, and to fail on a slot freed by destroying
    an ancestor (`regress-preview-stale-create`);
  - `set_styles` with a 0 id, which applying drops, was taken (`regress-preview-invalid-styles`).
    Reviewing every op against its apply function then also found `move_node`'s `before`
    check missing and stale surfaces for nodes under one moved to another surface.
- **Tests:** `test_prepare`'s `test_styled_text` (both schedulers: styled text shaped on
  workers, a token change reshaping three paragraphs there, no core shaping, identical
  frames; mutations that drop the guess or the reshape fail it), `test_layout`'s
  `test_styled_text`, `test_text_wire`'s `test_from_node`. New stat `text_reshaped`.

### Inheritance
- **Inherited properties, as in CSS:** text color, font family, size, weight, slant,
  language, line height, text alignment and direction. `max_lines` doesn't inherit. New
  properties 11–16 (`font_family`, `font_size`, `font_weight`, `font_slant`, `language`,
  `direction`); new value kinds 11 `string`, 12 `inherit`, 13 `initial`.
- **`inherit` and `initial` work on every property and transition** (CSS-wide keywords).
  `inherit` on a non-inherited property copies the parent's computed value whole (a
  percentage stays a percentage); such a node follows every change of its parent, not only
  inherited ones. Tokens can't hold either: a token is a value, not a cascade keyword.
- **Strings** (family lists, language tags) are interned in `ui_styles`: equal strings are
  one object, so computed values compare by pointer. Entries, tokens and nodes (for
  `set_text`'s family and language) hold references; computed values don't, so a string
  whose last reference goes away waits in a garbage list until the end of the frame's
  resolution, after every computed value that pointed at it has been replaced. At most
  1014 bytes (what fits an entry of 255 words); languages at most 64.
- **`set_text`'s values sit between the inherited value and the styles.** They replace the
  inherited values for the node (and max lines' initial value), so the M2 rule that styles
  beat `set_text` still holds, and nothing changes for existing apps. They are part of the
  node's computed values, so they inherit to its children.
- **Two-step resolution.** Each node keeps its cascade, the entries that apply to it in
  order of precedence (pointers into style definitions and its overrides). It is gathered
  again only when the node's styles, state or overrides change, or a style it lists is
  redefined; adding or removing an override marks it too, because that moves the overrides.
  Computing runs the cascade over starting values (initial, inherited, `set_text`'s) and
  reads tokens then.
- **Incremental, top-down.** The frame's dirty nodes are sorted by depth and computed
  parents first. After a node is computed, its children are computed only if its inherited
  values changed (or a child takes `inherit` on another property and anything changed), so
  a change stops at descendants that declare their own values. `test_style` counts the
  computations (`ui_scene_stats.styles_computed`).
- **Interim cost:** `bench_layout`'s token change over 10,005 nodes went from 3.0–3.8 ms to
  about 4.8 ms (best of 15): every user of a changed token gathers its cascade again, and
  the dirty queue is sorted by depth. The style-performance step replaces this path.

## Milestone 2: layout and styles

### Example and goldens
- **`examples/layout_cards`**: a wrapping grid of cards (a title, a paragraph, a footer row
  with a label and a button), all colors from eight theme tokens with color transitions,
  cards with `transition(layout)`. A timer inserts, moves or removes a card every 2.5 s
  (`move_node`, with a seeded generator so runs repeat), another switches light and dark
  every 4 s. `--headless --at S` steps a paused clock to S seconds and writes a PPM, which
  shows a slide or a theme change mid-flight.
- **Checked in a nested headless sway** at 1.25× under both schedulers: about 61 fps
  while animating, idle at rest, no late frames, a resize through `swaymsg` reflows the
  grid, and closing the window exits cleanly.
- **New golden `paint_flex_text`** (1.0, 1.25, 1.5): a flex layout with text built only
  through styles, with wrapping, `space-between`, centering, baseline alignment and
  `align-self: end`. Checked in the `axe-ci-ubuntu:24.04` image against CI's Mesa.

### Layout engine
- **Own flexbox, CSS semantics** (`src/layout/flex.c`, `docs/layout.md`):
  - every node is a flex container with CSS's initial values, sized as border-box;
  - the surface is a column container of its size;
  - every node is a containing block for absolute children (as if all were
    `position: relative`).
- **Text is a text leaf (changed from the plan, user's decision).** The plan made a node's
  text an anonymous flex item, as Chrome does with text inside a flex div. In practice that
  shrink-wraps single-line text in a row, so `text-align` and RTL start alignment did
  nothing (the Hebrew golden moved to the left edge). Now:
  - a node with text lays it out across its content box, like a CSS block holding only
    text (or React Native, Flutter and SwiftUI text);
  - in-flow children of a text node lay out over it;
  - fixtures model text nodes as `display: block` paragraphs;
  - to center a label in a bigger box, make the label its own node.
- **Text min-content is the widest grapheme cluster** (CSS `overflow-wrap: anywhere`). Our
  line breaker already breaks inside a word that doesn't fit, so text never overflows its
  box, and M1's emergency-break golden is unchanged.
- **Zero means CSS's initial value for most lengths.** `ui_length` zero is 0 px (margins,
  padding, gaps); `ui_layout_style_init` sets sizes, basis, min sizes and insets to auto and
  maxima to none. The first version made zero mean auto, which gave every node auto margins
  and centered everything; the borders golden caught it.
- **Content measurement ignores the measured axis's own constraints.**
  - Content-based flex base sizes, automatic minimums and intrinsic contributions measure
    a node in a mode that ignores its own size, min and max in that axis
    (`INTRINSIC_X`/`_Y`), as the spec's content sizes do. Two failing fixtures came from
    applying them.
  - The other axis keeps its constraints: a `max-width` decides where text wraps
    (`text_max_width`).
- **Definite sizes (§9.8).**
  - Children get a size as `known` only when it is definite: the container's main size is
    definite, the cross size is specified, or the item stretches. Otherwise they get it as
    available space.
  - Percentage heights resolve only against definite heights; percentage widths always
    resolve once laid out.
  - Only an `auto` cross size stretches.
  - Fixtures `nest_percent_indefinite` and `intr_percent_in_auto` pinned these.
- **Baseline in a column** lines up the items' start border edges (Chrome's synthesized
  baseline), so the largest start margin offsets the others.
- **Intrinsic sizes of flex containers follow Chrome**, not §9.9.1: a row's max-content width
  is the sum of contributions plus gaps; its min-content is the sum, or the largest when
  wrapping; a column's width is its widest item.
- **set_rect is an absolute frame** (proposed in the plan): overrides of `position`, `left`,
  `top`, `width` and `height`, which keeps every older test, example and fuzz seed working.
  `start_spring` on the rect animates that frame and writes the computed values directly, so
  the spring needs no style resolution each frame.
- **`move_node`** (opcode 18): reorders, reparents and moves between surfaces. It refuses a
  move under the node itself or a descendant, and a `before` that isn't a sibling under the
  new parent. A subtree that changes surface gets its root updated.
- **Caching:**
  - Six measurements and the last full layout per node, keyed by the known sizes, the
    available space, the intrinsic axis, and the parent's content size only for nodes with
    percentage sizes or padding.
  - Dirty flags clear a node's cache and propagate to the surface.
  - Optimizations found with the incremental test, which went from 18 computations per
    node to under 5:
    - the automatic minimum size is computed only when the line shrinks, or for an
      explicit `flex-basis`;
    - measuring a container no longer re-measures its items for a baseline;
    - the parent size left out of most keys.
  - A cache-key bug (a `bool` storing the axis) made every Y-axis intrinsic entry miss.
- **Layout runs while a surface draws**, after apply, resolve and animate, before building.
  It doesn't mark the root dirty itself: what made layout dirty already did. Doing it
  cost one extra frame per layout, which the idle-frame tests caught.
- **Conformance:**
  - 165 fixtures (162 at first; three baseline fixtures came with the next step), recorded by `chrome-headless-shell` 156.0.8078.80 (Chrome for Testing,
    sha256-pinned, downloaded by `fetch_chrome.py` into a gitignored directory).
  - Tolerances are 1/64 px without text and 0.5 px with text.
  - **All 165 match.** `deviations.md` lists expected failures (none), and a listed fixture
    that passes fails the test.
  - The fixtures are generated by `make_fixtures.py`. Python scripts use only the standard
    library and run as `python3 -I`.
- **Fuzzing found one hang:** with extreme sizes the freeze loop's total violation became
  NaN (inf − inf), so no item ever froze. Each round now freezes everything if no rule froze
  anything (`regress-flex-nan-violation`). `fuzz_commands` lays out every pass (seed08 has
  flex properties, wrap, absolute, display none and `move_node`). `fuzz_set_text` checks
  that min-content ≤ max-content and that narrowing never lowers the height.
- **Goldens:** `paint_borders` was regenerated. The 30 px-border box in a 40 px frame now
  takes 60 px, because a border box can't be smaller than its border, and the styled box's
  text sits inside its border. Both are CSS-correct.

### Measurement
- **Opcode 19 `request_layout`** (node, `u32 request_id`) and **event 6
  `layout_measured`** with four named rects (review change 4): layout and presentation,
  each relative to the parent and to the surface. The event also carries the node and its
  surface, so logic can match answers without keeping the request.
- **Answered after layout, in the frame that applies the request**, in request order. The
  core keeps requests until the node's surface layout is current: nothing under it dirty,
  laid out at the surface's size and scale. That is right after the surface's layout when
  it draws, and at once when it doesn't (nothing changed). Requests apply only in frame
  steps, so the answers are the same under both schedulers (`test_measure`).
- **A request marks nothing dirty**, so it never makes a frame; `test_measure` checks the
  frame count.
- **Stale handles get an answer** (`UI_ERR_STALE_HANDLE`), unlike every other op on a stale
  handle, which is dropped: logic waiting for the reply must not hang.
- **Hidden nodes report zero rects**: their layout rects are stale or zero.
- **`ui_event` gained `ui_rect`** in the public header. A `switch` over `ui_event_type`
  without a `default` now needs the new case (the `spring_rect` example did).

### Layout animations
- **`transition(layout)` on a pseudo-property** (64, `UI_STYLE_LAYOUT`) that can't be
  declared, only transitioned. It reuses the transition entry, so no new op or value kind,
  and it works from styles, state blocks and overrides like every other transition.
- **Layout writes only layout rects.** The scene places a node whose layout rect changed,
  or that was never placed (`ui_scene_place`): its presentation rect (`ui_node.rect`, what
  the build draws) jumps there, or a 4-component spring (x, y, width, height) starts or
  retargets toward it, keeping its velocity. `ui_scene_layout` takes the frame's animation
  time for that.
- **Children follow their parent's presentation** at their own rects, and nothing scales:
  a node's background and border animate their size, its text and children don't (FLIP
  without scale, as planned).
- **Reparenting keeps the node where it is drawn** (as planned): `move_node`
  re-expresses the presentation rect and a running spring relative to the new parent. A
  move to another surface, where the old position means nothing, appears in place.
- **New nodes, and nodes shown again after `display: none`, appear in place**; removing
  the transition ends a running animation at the layout rect.
- **Bug found by the tests:** a hidden node is never computed, so it stayed dirty, and
  showing it again didn't reach its parent (dirty propagation stops at the first dirty
  node); the parent's cached layout then skipped it. Layout now clears the cache and the
  flag of a hidden node it passes over. `test_layout`'s `test_display_none` checks it.
- **Tests:** `test_layout_anim` checks the closed-form position while moving and after a
  retarget, children, snapping, reparenting mid-flight, and that the single and threaded
  schedulers draw byte-identical frames mid-animation. Fuzz seed `seed09` starts and
  retargets layout springs on the harness's second pass.

### Layout properties in styles, bench_layout
- **Layout properties go through the style machinery unchanged:** tokens, state blocks,
  overrides and redefinitions re-resolve the node, and a changed layout value marks it
  dirty. A transition on a layout property or `border_width` is refused by the writer and
  dropped by the reader; geometry animates through layout animations.
- **`bench_layout`** (`docs/layout.md#benchmarks`): a 10,005-node grid of cards. The
  incremental case started at 1.9 ms, with 66 computations and about 21,700 cache hits per
  edit. It is now 0.46–0.60 ms at best and 0.68–1.4 ms median on a busy machine (34
  computations, 4,419 hits), after the memo fix below and these changes:
  - **Baselines on request.** A plain measurement no longer measures a container's first
    item for a baseline nobody asked for; the run mode says when one is wanted, and cache
    entries record whether they carry one.
  - **Heights' min-content and max-content constraints share cache entries** (CSS Sizing:
    both block sizes are the height at the given width; nothing reads a min-content
    constraint in y). The root's automatic minimum no longer lays the grid out again.
  - Intrinsic measurements stop once the measured axis is known, and whether a node uses
    percentages is remembered between computations instead of recomputed per lookup.
- **New fixtures found a baseline bug.** A container's baseline came from its first item
  *with* a baseline; CSS (§8.5) and Chrome take the first item's, synthesized from its
  bottom border edge when it has none. `baseline_first_empty`, `baseline_first_fixed` and
  `baseline_first_fixed_column` pin it; they were written to check that baselines on
  request measure the right item.
- **A paragraph measurement memo bug:** new text reset the entry count but not the next
  slot, so new entries were written after stale ones, and a width measured before the
  change answered with the old text's size. The benchmark's edits exposed it;
  `test_text_layout`'s `test_measure_memo` fails without the fix.
- **Style resolution** of 10,005 nodes after a token change takes 3.8–5.9 ms. No target was
  set; it is the next candidate for work (each node's blocks are merged afresh).

### Styles (paint properties)
- **Wire:** opcodes 12–17 (`define_style`, `destroy_style`, `set_styles`, `set_props`,
  `set_token`, `set_state`); `docs/styles.md` has the layout. Entries are self-sized
  (`u16 property, u8 kind, u8 words`), so readers skip what they don't know and newer
  writers can grow values. Blocks and op headers carry their size. IDs are chosen by
  logic, as handles are, so defining needs no round trip.
- **The writer takes descriptor arrays** (`ui_style_block_desc`, `ui_style_decl`) with
  value constructors (`ui_px`, `ui_rgba`, `ui_token_ref`, `ui_transition`, ...) instead of
  the builder object the plan mentioned. It is simpler in C and fits a generated binding
  just as well. The writer validates every declaration against the same table the core
  uses (`src/style/props.c`).
- **Precedence:** initial values, `set_text`'s paragraph values, the matching blocks
  ordered by specificity (number of states), then list order, then declaration order, then
  overrides (review change). A hover+pressed block beats a later style's hover, as
  pseudo-class specificity would; `test_style` checks it.
- **Tokens** may refer to tokens (review change). Chains resolve when used, with Floyd's
  cycle detection. A missing token, a cycle or a value of the wrong kind makes the
  declaration count as absent, so the lower one shows; CSS would use the initial value.
  Changing a token closes over the tokens that refer to it, finds the styles using any of
  them, and resolves only the nodes that list those styles or use the tokens in overrides.
- **Resolution runs once per frame**, after apply and before animate, for the nodes queued
  by ops (a dirty list, not a scan). A style or token change scans the nodes once to queue
  their users.
- **Transitions** exist for background, border color, radius, opacity and text color.
  - They start whenever the resolved value changes, for any reason; `test_style` starts
    one from each source.
  - A node's first resolution never animates.
  - The change is detected against the running spring's target, or the drawn value, so no
    old state is kept.
  - Border width and layout properties don't transition; geometry animates through layout
    animations.
- **`set_color`, `set_corner_radius` and `set_clip` are overrides.**
  - Without a declared transition they jump and stop a spring, exactly as in M0. With one,
    they animate.
  - `start_spring` on color or radius sets the override and starts its own spring on the
    next resolution, ahead of any declared transition.
  - `set_rect` and the rect spring are untouched until the layout step.
- **Text:** styles carry only properties that don't reshape (decided before the
  milestone): text color, line height, alignment, max lines. They override `set_text`'s
  values, which sit just above the initial values; spans keep their explicit colors.
  `set_text` queues the node for resolution, so new text keeps the styles' values.
- **No inheritance, no z-index (review):** a parent's text color doesn't reach its
  children, and paint order is tree order. Inheritance of text properties is planned for
  M2.1; tokens carry theming until then.
- **Node storage:** springs moved into a block allocated the first time a property animates
  (`ui_node_anim`), so a still node doesn't carry six springs (768 bytes). Each node keeps
  its computed style (about 120 bytes).

### Borders, opacity layers and snapping
- **Rect instances grew to 64 bytes** (border width and color); glyph instances stay at 48,
  each with its own stride. The border is a second signed distance inside the first, with
  inner radius r − w. The background covers the whole rect under it, as CSS's default
  `background-clip: border-box` does.
- **Group opacity is a layer** (decided before the milestone):
  - **Display list:** a node below opacity 1 brackets itself and its subtree with
    `LAYER_BEGIN`/`LAYER_END` batches. The bounds are the union of what was drawn inside,
    with a 1 px anti-aliasing margin around rects, intersected with the clip at the begin
    batch and the surface, rounded out to whole pixels. The composite quad is an ordinary
    `ui_dl_rect` record, so no new buffer is needed.
  - **Render:** ends the pass, draws the layer into a pooled texture (the target's format,
    sizes in 64 px steps, released after 120 unused frames), then resumes the target pass
    and composites with `textureLoad` at exact texels, times the opacity.
  - **Per-pass state:** each pass reads its viewport and origin from one uniform buffer at
    its own dynamic offset (256-byte slots), so every pipeline draws into a layer
    translated.
  - **Reuse:** a texture is free again once its composite is encoded; passes run in order,
    so later layers of the same frame reuse it.
  - **Cheaper cases:** a node with nothing but a background multiplies its alpha instead,
    and opacity 0 skips the subtree.
  - **What makes a layer empty:** content entirely clipped or off the surface. Its batches
    draw nothing.
- **Snapping at rest (review change):** a box at rest rounds its origin and its size
  separately, to physical pixels, so sizes never wobble. A box whose rect springs, and
  everything under it, draws unsnapped, and snaps on the frame the spring settles. Layout
  animations will count as moving too.
  - Borders at rest are whole pixels, at least one (`floor`, as browsers do).
  - Text baselines keep snapping, so moving text steps vertically (a known limitation).
  - `test_paint_golden` checks subpixel positions during a slow spring and whole pixels
    once it settles.
  - It changed the 1.25× text goldens by one row: the card's bottom edge at 167.5 px now
    snaps to 168. They were regenerated; no text pixel changed. `test_headless` now expects
    the snapped edge where it expected a half-covered row.
- **Goldens:** `paint_borders`, `paint_opacity`, `paint_clipping` at 1.0, 1.25 and 1.5. The
  opacity case also checks a pixel where two overlapping children of a 50% group meet:
  blue over light gray with no red, which per-primitive alpha would show. The golden
  comparison moved to `tests/golden.h`, shared with the text goldens.
- **Fuzzing found one bug in the new code within minutes:** a layer with nothing visible
  kept an infinite extent, which `end_layer` converted to an integer (undefined
  behavior). Bounds are now clamped into the surface first, NaN included
  (`regress-layer-infinite-extent`, `test_layer_bounds`). New seeds `seed06` and `seed07`
  cover styles, tokens, states and layers, written by this writer and as a newer writer
  would (unknown properties and kinds, longer entries and headers).

### Preparation off the core thread
- **A worker pool owned by the app** (`src/runtime/jobs.h`): N threads, where
  `ui_app_desc.worker_threads` 0 means auto = hardware threads − 3 (core, logic, render),
  clamped to 1–4. `UI_WORKERS` overrides it.
  - Jobs are short and FIFO, and they never wait for each other.
  - Jobs belong to groups. A group counts its unfinished jobs and calls `done` once it is
    sealed and the count reaches 0. A job may submit follow-ups to its own group, and the
    owner may reopen a sealed group.
  - Workers never touch the scene, the atlas or the GPU.
  - **Under the single scheduler the pool has no threads and jobs run inline** when
    submitted, on the main thread. A core made without a pool (unit tests) gets an inline
    one of its own.
- **Preparation starts when the core receives a buffer**, in `drain()` at every core
  step, not at the frame boundary (`src/core/prepare.h`).
  - Framing is validated first, so a malformed buffer is never prepared.
  - **`set_text`:** decoding (assembling continuations, sanitizing, building the source)
    moved from `scene.c` into the pure `ui_text_op_decode` (`src/core/text_op.c`). A job
    decodes, then shapes at every distinct configured surface scale (usually one), then
    submits glyph jobs.
  - **Glyph jobs** rasterize every distinct glyph of the result at **all 4 subpixel
    bins** into a new CPU glyph cache, 64 glyphs per job. The bin depends on pen
    positions, which are known only after layout. That is 4× the rasterization work,
    but it is done on workers.
  - **`load_font`:** the file is read and parsed on a worker.
- **The atomicity rule:**
  - A buffer applies whole at the first frame boundary where it and every buffer before it
    are prepared. The first unready buffer and everything after it wait.
  - Animations, timers and frames continue meanwhile. Pending buffers wake idle surfaces
    only once the head buffer is ready, so waiting never costs an empty frame.
  - When a group finishes, the worker wakes the core (platform wake, then `progress`).
- **Font loads have a second, scene-dependent step.** When a buffer with `load_font`
  reaches the head of the queue and its own jobs are done:
  - Its faces are registered, and the registry generation goes up.
  - Every existing paragraph is shaped again in the buffer's group.
  - Text in it and in later pending buffers that was shaped at an older generation is
    prepared again.
  - Because every earlier buffer is applied by then, the scene can't change before this
    buffer applies, so all reshaped paragraphs swap in during the frame that applies it.
    The frame that applies `load_font` still shows the new font, as in M1.
  - This step runs only before or after a frame, never inside its apply phase, so it
    happens at the same point under both schedulers.
  - A lock orders "this text is done at generation g" against registration, so every item
    is either found stale by the registration or sees the new generation itself.
- **Font discovery initializes on a worker** at startup (`defer_discovery`), from the core's
  first step. Received buffers start preparing only once it is done, so nothing is shaped
  without the system fonts. Until then buffers wait; frames don't. (It first started while
  the core was created, before the platform: an app whose platform failed then crashed
  when the job's completion woke it. Found with the `layout_cards` example; covered by
  `test_prepare`'s `test_failed_start`.)
- **Determinism:**
  - `ui_app_frame` and `ui_app_step_core` first wait until every queued buffer is prepared
    (`wait_prepared`), and "idle" now includes no preparation in flight. A buffer queued
    before `ui_app_frame` therefore applies in that frame under both schedulers, and the
    test captures stay byte-identical.
  - `ui_app_frame_without_logic` waits for neither logic nor preparation, which is what
    the stall test needs.
- **The font registry is thread-safe:**
  - The face table is append-only, in chunks, and published with a release store, so
    lookups by id and coverage checks take no lock.
  - Matching, fallback, instances and the font table take a mutex. The font table became
    a hash map; it was a linear search.
  - Fontconfig calls (behind their own mutex) and face loading happen outside that mutex,
    with a re-check when inserting.
  - The cluster scratch moved into a parameter.
  - **Registered faces are kept apart from discovered ones.** Only registered faces take
    part in family matching and in the registration-order fallback step, and only the
    core registers. Discovery results are cached per family, weight and style, and per
    fallback code point. Before, discovered faces joined the registration list, so with
    concurrent discovery the fallback order could have depended on which worker
    discovered what first.
  - A **generation** counts changes to the registered set. Shaping records it, and a text
    context treats older shaping as stale (`ui_text_context.generation`).
- **Paragraph input is an immutable, refcounted `ui_text_source`.** The paragraph and
  the jobs that prepare it each hold a reference. The paragraph's old fields now point
  into it. Styles will override max lines, alignment and line height through
  `ui_paragraph_set_params`.
- **Nothing on the frame path takes the registry lock:**
  - Shaped and laid-out runs carry their font pointer.
  - Shaping records the base font's metrics, so line layout no longer matches a family,
    which could have run a Fontconfig query on the core.
- **CPU glyph cache** (`src/text/glyph_cache.h`): 16 shards with a lock each, 16 MiB by
  default (`ui_text_config.glyph_cache_bytes`), least recently used out first.
  - Entries carry the sequence number of the buffer that prepared them, and are protected
    until the core releases it. The core releases a buffer's glyphs once every surface
    has drawn since it applied, or has nothing to draw.
  - Glyphs that fail to rasterize are cached as failures, so the core doesn't retry them.
  - On a miss, the atlas copies from this cache before rasterizing itself.
- **Fallbacks stay correct, only slower:**
  - A buffer that couldn't be prepared (out of memory) applies unprepared: decoded on the
    core, shaped at layout, and `load_font` read on the core.
  - A scale no preparation covered, such as moving to a monitor with another scale, also
    shapes on the core.
  - The counters `text_shaped_on_core` and `glyphs_rasterized_on_core` show this. The
    tests require both to stay 0 on the prepared path.
- **Stats:**
  - `texts_prepared`, `glyphs_prepared`;
  - `buffers_deferred`: buffers that missed at least one frame at the head;
  - `max_head_wait_ns`, with a debug warning above 100 ms.

  `glyphs_rasterized` keeps counting glyphs that enter the atlas.
- **Tracepoints** `UI_TRACE_PREPARE_BEGIN/END` fire on the worker, around each job, with
  the buffer's sequence number. `test_prepare` holds a worker in one, as
  `test_atlas_upload` holds the render thread. The `UI_TRACE` printer reports preparation
  jobs and their time per second.
- **ThreadSanitizer.** A local TSan build (not a preset) found nothing in our code. glibc's
  C11 `thrd_create` and `mtx_*` call pthreads internally where TSan doesn't intercept them,
  so the build force-includes a small shim mapping C11 threads onto pthreads. The only
  reports left come from Mesa's llvmpipe (`lp_fence_destroy`).

## Milestone 1.1: wire compatibility and fuzzing

### Wire evolution
- **Ops grow by appending; most changes need no version bump.** The rules are in
  `docs/command-buffer.md` ("Evolving the format"), along with each op's original size and
  every appended field. A reader accepts a known op whose payload is at least its
  original size and ignores bytes past the fields it knows. Only a smaller payload
  rejects the buffer. Events follow the same rules (`ui_event_next`).
- **Appended fields default to zero bytes.** A reader copies the fields the writer sent
  into a buffer of the size it knows and zero-fills the rest (`ui_wire_read_fields`), so
  "missing" needs no code per field. A field whose natural default isn't 0 has to be
  encoded so that 0 means the default.
- **Size fields went into bytes every earlier writer left 0**, so adding them needed no
  bump either:
  - `set_text` `header_bytes` takes the high half of the old u32 `chunk_bytes` (a chunk is
    at most 65 532 bytes), and `span_bytes` takes the default style's `start`, which was
    always 0 and never read;
  - `append_text` and `load_font` `header_bytes` take the high halves of their u32 lengths.

  A size of 0 means the op's original size. That makes buffers from the original 64-byte
  `set_text` writer decode correctly, and `tests/test_wire_compat.c` checks it.
- **Buffers from the writers between 62d0e10 and this change don't decode.** They sent the
  72-byte header without `header_bytes`, which is now read as 64. Inferring 72 from the op
  size would be possible, but none of these buffers exist: writer and core ship in one
  library and buffers are never stored.
- **`set_text` spans carry their size too**, which the brief didn't ask for. Styles are the
  record most likely to grow (decorations, letter spacing, features), and without a
  stride, growing them would have needed a bump. A field appended to styles is appended to
  each span, and to the header for the default style.
- **Flags stay strict.** `set_clip` still drops an op with a flag bit it doesn't know: a
  newer writer asking for, say, rounded clipping must not get a rectangular clip without
  noticing. Behavior an old reader must not skip goes behind a new opcode or a checked
  flag.
- **The old `set_text` fuzz seeds were 72-byte-era buffers** and were patched to carry
  `header_bytes = 72`. Patching them showed that `regress-raster-negative-x` had silently
  stopped reproducing at 62d0e10. It was a 64-byte-era input, the 72-byte reader rejected
  its sizes, so it never reached the rasterizer. The fuzzer had also mutated the unused
  default-style `start`, which now reads as `span_bytes`; with those bytes zeroed it
  decodes as an older-writer buffer again. With the rasterizer's x clamp removed, it
  crashes ASan again, so it guards the bug once more.

### Continuous fuzzing
- **A nightly workflow** (`.github/workflows/fuzz.yml`, 03:17 UTC, also run by hand with a
  time per target) runs every target for 10 minutes. Targets run as parallel matrix jobs,
  each building only its own fuzzer, with `-max_len=8192` like the CTest smoke tests,
  `-timeout=30` and `-rss_limit_mb=4096`. Scheduled runs only start from the default
  branch, so it begins once this reaches `master`.
- **Corpus cache:** each target restores the newest `fuzz-corpus-<target>-*` cache, fuzzes
  with it plus the committed seeds, then shrinks it with `-merge=1` and saves it under a
  new key (cache keys are immutable). The committed corpus is only read, never written.
- **Crash, leak, timeout and OOM inputs** are written to `crashes/` and uploaded as the
  `fuzz-crashes-<target>` artifact of the failed job. CONTRIBUTING.md says how to replay
  one and commit it as a `regress-*` seed.
- **Shared setup:** the toolchain, packages, lavapipe and wgpu-native cache restore moved
  into a composite action (`.github/actions/setup`) used by both workflows. Saving the
  caches stays in each job, so a failing step still saves them.
- **New target `fuzz_commands`:** arbitrary op streams through `ui_scene_apply`, then
  springs and display-list building on two surfaces. The harness writes the buffer header
  and op count so inputs don't have to guess them. `fuzz_set_text` only ever built
  `set_text` and `append_text` ops, so the framing checks that this change loosened (every
  op now accepts longer payloads) had no fuzzer. Its seeds are scenes encoded by this
  writer, by an older writer and by a newer one.
- **It found two bugs within seconds**, both fixed with a regression seed and a unit test:
  - **Unknown short ops were read past their end** (from M0). `apply_op` read a node
    handle from every op not handled before it, unknown opcodes included, before
    dropping those. A newer writer's op with a payload under 8 bytes at the end of a
    buffer made an older reader read past it, which is exactly the forward-compatibility
    case. Unknown opcodes are now dropped before anything is read
    (`regress-unknown-short-op`, `test_wire_compat`).
  - **Huge clip rects overflowed the scissor** (from M1's `set_clip`). A clip at
    x = 3e21, from a large but finite rect, was converted to `int32_t`. Scissor
    coordinates are now clamped to [0, 2³⁰], NaN included, before the conversion
    (`regress-scissor-huge-rect`, `test_clip`).
- **Fuzz before committing:** a change to code a target covers is fuzzed locally for a few
  minutes first (CONTRIBUTING.md has the target-to-code table). The CTest smoke test's
  10,000 runs are too few. The language change in M1 went in while its fuzz smoke run was
  failing, which is what prompted the rule.

## Milestone 1: text pipeline

### CI
- **GitHub Actions, Linux only** (`.github/workflows/ci.yml`): `ubuntu-24.04`, one job per
  preset (`debug`, `asan`). Every test runs headless on lavapipe. `VK_DRIVER_FILES` names
  only the lavapipe ICD, so a test can never pick up a GPU by accident.
- **Clang 22 from apt.llvm.org** is pinned in CI (`LLVM_VERSION`). It is the oldest version
  the code was developed with. Local development may use newer versions.
- **What is cached.** Two caches, both keyed on the wgpu-native submodule commit:
  - the vendored crates, `third_party/wgpu-native-vendor/`;
  - Corrosion's Cargo target dir, `build/<preset>/cargo`, whose key also includes the Rust
    and LLVM versions.
- **What a warm run rebuilds.** Vendored crates are fingerprinted by version, so they never
  rebuild. Fresh checkouts give the `wgpu-native` crate itself new mtimes, so only that one
  crate rebuilds.
- On failure, the test log and any golden-diff PNGs are uploaded as artifacts.

### Follow-ups after the first review
- **HarfBuzz is always built with `-O2 -g`**, even in debug and asan presets, as wgpu-native
  is always built in release. Unoptimized, the first shaping of the example paragraph took
  21.7 ms; optimized it takes 1.5 ms. The "about 60 ms first frame" seen in the windowed
  debug run was mostly this.
- **Controls draw nothing.** Fonts map tab, line feed and other C0/C1 controls to
  `.notdef` (glyph 0, with an advance), so every line ending in `\n` showed a box. After
  shaping, characters with general category Cc, Zl or Zp become `UI_GLYPH_INVISIBLE`
  with no advance; the scene skips them.
- **Tabs** advance to the next stop, every 8 space widths of the tab's font from the line
  start. Line breaking and positioning use the same rule, and trailing tabs hang like
  spaces (CSS `pre-wrap`). In RTL lines, stops are measured from the line's left edge.
  There are no custom tab stops yet. Golden case `controls` covers this.
- **Large glyphs:** a glyph may be as large as a page (1024 px by default) instead of a
  quarter page. A 300 px heading at 1.5× used to be rejected and drawn as nothing. Larger
  glyphs still need a larger `atlas_page_size`.
- **Pressure eviction is least recently used first.** Unprotected glyphs not used in this
  frame are sorted by last use and evicted in batches (8, or an eighth of them) until the
  new glyph fits, instead of all at once.
- **Synthetic bold and italic** (CSS `font-synthesis`), through HarfBuzz's synthetic bold
  and slant, which change outlines, advances and extents together.
  - Bold when the request is ≥ 600 but the face reaches at most 500 (through its `wght`
    axis, or its static weight). Emboldening is 0.025 em, and advances grow with it.
  - A slant of 0.2 (about 11°) when italic or oblique is asked of a face with no italic
    design and no `ital` or `slnt` axis.
  - The synthesis is part of the interned instance, so glyph caching is keyed correctly.
  - Tests use `NotoSans-Regular-Static.ttf`, a 15 KB static Latin instance made by
    `tools/font_subset --static`. Golden case `styles` shows synthetic italic with axis
    weights.
- **Alignment and line height** (the `set_text` header grows from 64 to 72 bytes; no
  client outside this repo writes it yet, so the layout changed in place and the wire
  stays v0).
  - Align is start, end, left, right, center or justify. Start and end follow each line's
    paragraph direction.
  - Justify spreads the extra room evenly over the line's spaces (general category Zs, not
    the hanging trailing ones). The last line of a paragraph (before a hard break or at the
    end of the text), ellipsized lines and lines without spaces are start-aligned. There is
    no inter-character justification for CJK yet.
  - `line_height` > 0 makes every line box `round(line_height × size × scale)` px, with
    half-leading around the default style's font, so a fallback font's tall metrics no
    longer push lines apart. 0 keeps the metric-based lines.
  - Golden cases `justify` and `center` cover this.
- **Per-span language** (BCP 47), passed to HarfBuzz for `locl` forms: Romanian ș and
  Serbian б differ in Noto Sans, and tests check both.
  - Languages share `set_text`'s string table with the family lists (now
    `string_count` / `strings_bytes`, at most 64 strings in all).
  - A style's language sits in what were reserved bytes, as string index + 1, so 0 (and a
    zero-initialized C struct) means the default language.
  - The writer accepts tags of ASCII letters, digits and hyphens, at most 35 bytes.
  - Language is part of the shaping style: a change splits runs.
- **Clipping** (`set_clip`, opcode 10): a node's text and descendants are drawn only inside
  its rect, intersected with its ancestors' clips.
  - The scene keeps a clip per tree level while it walks, and every batch carries its
    clip. The renderer turns it into a scissor rect (whole pixels, rounded outward, clamped
    to the target), so the result is rectangular: rounded clipping needs the compositing
    layers that come later.
  - Rects and glyphs entirely outside the clip are culled before reaching the display
    list. Culled glyphs are never rasterized, so a long clipped paragraph costs only its
    visible lines.
  - The node's own background is not clipped (it lies inside its rect anyway).
  - `test_clip` checks pixels against the same scene without the clip.
- **Runtime font loading:** `load_font(id, path)` (opcode 11) and the `font_loaded(id,
  result)` event (5), following the timer pattern: a request ID completed by an event, with
  no callback into the language.
  - It is applied at the frame boundary on the core thread. HarfBuzz maps the file and
    reads tables lazily, so this is cheap for local files. Loading off-thread belongs to
    the asset system.
  - Success invalidates every paragraph's shaping (matches and fallbacks may change) and
    marks the roots dirty, so the frame that applies it already shows the new font.
  - Loading a file that is already registered is a no-op that still succeeds.

### Unicode layer
- **Version: Unicode 18.0.0.** This is the UCD that the pinned HarfBuzz 15.0.0 ships
  (`hb-ucd-table.hh`). HarfBuzz's built-in UCD drives shaping and our tables drive
  segmentation, bidi and script runs, so the two never disagree about a character.
  `tools/unicode/fetch.sh` pins the source files by sha256.
- **Tables are generated and committed:** `tools/unicode/gen_tables.py` (Python 3, stdlib,
  run as `python3 -I`) writes `src/unicode/ucd_tables.{h,c}`.
  - Each code point maps to one 8-byte record: line break class, bidi class, grapheme
    break, general category, script, flags (Extended_Pictographic, East Asian, InCB,
    Default_Ignorable) and a Script_Extensions index.
  - 1,154 distinct records are found through a two-stage table with blocks of 128, about
    110 KB in all.
  - Enum values are fixed in the generator, not taken from file order, so a version bump
    doesn't renumber them.
- **The algorithms work on code point arrays.** Callers keep a parallel byte-offset array,
  so every later stage still speaks UTF-8 byte offsets.
  - Line breaking and bidi take a reusable context and allocate only when they grow.
  - Grapheme breaking allocates nothing.
- **Conformance: 100%, no deviations, nothing skipped.** BidiCharacterTest (91,707 cases),
  LineBreakTest (19,346) and GraphemeBreakTest (853) run as the CTest tests
  `unicode_bidi`, `unicode_linebreak` and `unicode_grapheme`. The files are committed under
  `tests/unicode/` (about 10 MB, Unicode License v3) so tests run offline.
- **Changes in 18.0 that the code follows:**
  - LB12a no longer allows a break between BA and GL.
  - U+00AD is HH, and U+2012/U+2013 are BA.
  - GB9c is `\p{InCB=Linker} \p{InCB=Extend}* × \p{InCB=Consonant}`.
- **Line breaking groups LB9 units first.** A base character plus the combining marks it
  absorbs is one unit, and the rules are then evaluated in order between adjacent units.
  - A CM that starts a unit becomes AL with the properties of U+0041 (LB10).
  - SA characters resolve by general category (LB1). There is no dictionary breaking for
    Thai, Lao, Khmer or Myanmar, so those lines break only at spaces and punctuation.
- **Bidi keeps removed (X9) characters in place and skips them.** Level runs and isolating
  run sequences are lists of kept indices.
  - I1–I2 run once per paragraph after all sequences, so sos and eos see explicit levels.
  - Removed characters render at the level of the character before them.
  - BD16 pairs are unbounded per sequence; only the 63-deep opening stack is limited, as
    the spec requires.
- **Script runs (UAX #24):**
  - Common and Inherited join the surrounding run; leading ones join the first real script.
  - Script_Extensions narrow a run's candidate set. A character starts a new run only when
    the intersection would be empty.
  - A closing bracket takes its opener's script.
  - All-Common text resolves to Zyyy.
- **UTF-8 policy:** one U+FFFD per maximal subpart (Unicode §3.9, as in WHATWG).
  `ui_utf8_sanitize` returns the replacement count, which the wire counts in stats.

### HarfBuzz, Fontconfig and the test fonts
- **HarfBuzz 15.0.0** is a git submodule. A project-owned `third_party/harfbuzz.cmake`
  compiles the amalgamated `harfbuzz.cc` with Clang++, using upstream meson's flags
  (`-fno-exceptions -fno-rtti -fno-threadsafe-statics`).
  - `HAVE_PTHREAD` makes it use pthread mutexes rather than `std::mutex`.
  - There is no FreeType, glib, ICU, graphite or platform shaper: HarfBuzz's own OpenType
    functions load fonts, shape and draw outlines.
  - `harfbuzz-subset.cc` is a separate target, used only by `tools/font_subset`.
  - C++ is enabled project-wide (`CMAKE_CXX_COMPILER=clang++` in the presets) but compiles
    only HarfBuzz. The sanitizer presets instrument it too.
- **One module per dependency.** `src/text/hb/` is the only code that includes `hb.h`, and
  `src/text/fontconfig/` the only code that includes `fontconfig.h`. Neither leaks a type
  into another header.
- **Shaping settings.**
  - Cluster level `MONOTONE_GRAPHEMES`, so HarfBuzz clusters align with grapheme
    boundaries, which hit testing needs.
  - Clusters are UTF-8 byte offsets into the whole paragraph. The surrounding text is
    passed as context, so joining across run boundaries is correct.
  - Fonts are scaled to `size_px * 64` with no ppem, so there is no hinting.
  - Language comes from the span (see the follow-ups above), else HarfBuzz's default.
- **Fontconfig** (minimum 2.13) is used only with `text.system_fonts`, or with extra font
  directories.
  - Tests build an isolated configuration (`FcConfigCreate` + `FcConfigAppFontAddDir`)
    that knows only `tests/fonts`. It sets the XDG cache directory so Fontconfig doesn't
    warn.
  - `ui_fc_match` accepts a result only if its family really matches, because
    `FcFontMatch` always returns something. Generic families (sans-serif, serif,
    monospace, system-ui) resolve to whatever Fontconfig picks.
  - Variable fonts' named-instance index bits are dropped: axes are set from the span's
    weight and style.
- **Font matching** follows CSS Fonts 4 §5.2.
  - The weight distance: 400–500 looks up to 500, then down, then up; lighter weights
    look down first; bolder weights look up first. A variable `wght` axis matches any
    weight in its range.
  - A style mismatch costs more than any weight difference.
  - An italic request uses an `ital` axis, else `slnt` (down to -12), else an italic face,
    else a synthetic slant (see the follow-ups above).
- **Fallback per grapheme cluster.** The primary face is tried first, then the span's
  family list, then registered families in registration order (each at its best weight
  and style), then Fontconfig fallback (cached by the cluster's first non-ignorable code
  point). If nothing covers the cluster, the primary face draws `.notdef`.
  - Default-ignorable code points need no coverage.
  - **Stickiness:** a Common or Inherited cluster (space, punctuation, digits) keeps the
    previous cluster's font when that font covers it. Otherwise every space inside fallback
    text would split the run and lose kerning.
- **Runs** split where the bidi level, script, resolved face or shaping style (family,
  weight, style, size) changes. Color is not a split: glyph color comes from the style of
  the glyph's cluster.
- **Test fonts:** `tests/fonts/`, 3.8 MB, OFL, pinned to a google/fonts commit with sha256
  (`tests/fonts/SOURCES.md`).
  - Noto Sans (variable wght/wdth; it also covers Devanagari), Noto Sans Hebrew, Arabic and
    Devanagari.
  - Noto Sans SC subset to `cjk_chars.txt` (249 KB instead of 17.8 MB). The subset tool
    keeps every name record: `hb-subset` normally keeps only IDs 0–6, which renamed the
    font "Noto Sans SC Thin".
  - The subset is a Modified Version under the OFL. Its name doesn't use the declared
    Reserved Font Name "Source".

### Rasterizer and atlas
- **Coverage rasterizer** (`src/text/raster.c`). Outlines come from HarfBuzz's draw API
  and are flattened adaptively: the segment count bounds the chord error |B''|h²/8 by
  1/16 px, up to 128 segments per curve.
  - Each line segment adds its exact trapezoid area to an accumulation buffer, in the
    scheme of font-rs and stb_truetype 2. A row prefix sum then gives coverage =
    min(1, |winding area|).
  - This is exact for non-overlapping contours and for same-direction overlaps (variable
    fonts overlap contours this way). Opposite-direction overlaps cancel.
  - There is no supersampling. Tests check a 10×10 square at quarter-pixel offsets to
    within 8-bit quantization, and a circle (cubic and quadratic) to within 0.5% of πr².
  - Bounds come from the flattened segments, not from HarfBuzz extents, so a bitmap is
    never clipped.
  - Bitmaps larger than 4096 px per side are refused.
- **Subpixel positioning:** 4 horizontal bins (0, ¼, ½, ¾ px), each rasterized at its
  offset. There is no vertical bin because baselines snap to whole pixels.
- **Cache key:** face, interned variation instance, physical size in 1/64 px, glyph ID and
  bin. The table uses open addressing (linear probing with tombstones) and rehashes at
  75% load.
- **Pages:** 1024² R8 by default, up to 8 (`ui_atlas_desc`). Each page op carries a
  format, so RGBA pages for color glyphs need no protocol change.
  - Shelf packing: shelf heights round up to 4 px, and a glyph takes the shortest shelf
    within +25% of its height. Freed columns are kept sorted and merged, and space next
    to a shelf's bump pointer returns to it.
  - An empty trailing shelf gives its rows back to the page.
  - Glyphs need no padding because the shader reads texels with `textureLoad` at exact
    integer positions.
  - Glyphs larger than a page are rejected and counted.
- **Eviction.** A glyph is evicted once it has gone unused for `evict_frames` epochs
  (default 300). An epoch is one core frame that draws, so an idle app doesn't age glyphs.
  - A glyph used at or after the protect epoch is never evicted. The protect epoch is the
    oldest epoch among the surfaces' newest unfinished display lists.
  - Under pressure (no space and no new page allowed), glyphs that are not used this frame
    and not protected are evicted least recently used first, in batches, until placement
    succeeds.
  - A glyph that still doesn't fit is cached as empty (drawn as nothing, counted as
    rejected), so it isn't retried every frame. It ages out like any other entry.
  - Pages after the first are released once empty for the eviction window.
- **Uploads survive mailbox replacement.** Atlas changes travel in `UI_MSG_ATLAS_UPDATE`
  messages (one per frame, increasing `seq`) through the render actor's inbox. That inbox
  is now **unbounded**, so it can't refuse or drop one.
  - The core pushes a frame's update before publishing that frame's lists, and stamps
    each list with `atlas_seq`.
  - The render actor handles its inbox only between lists. After taking a list, it applies
    queued updates until `atlas_seq` is reached, then draws.
  - An update that belonged to a replaced (undrawn) list is still in the FIFO and is
    applied before the next list.
  - Every texture write is therefore queued after any submitted draw that could read the
    old texels. With the protect epoch, no list still to be drawn references an evicted
    region.
  - A list whose `atlas_seq` the inbox can't satisfy is a protocol violation. It is logged
    and drawn anyway.

### Paragraph layout
- **Greedy line breaking** over UAX #14 opportunities, measured with the shaped advances.
  - Each code point's advance is the sum of its cluster's glyphs; with grapheme-level
    clusters, the advance sits on the cluster's first code point.
  - A line ends at a mandatory break, else at the last opportunity before the content
    overflows, else (no opportunity: a word wider than the line) at the last grapheme
    boundary before the overflow, with at least one grapheme per line.
  - A width within 1/64 px of the limit counts as fitting.
- **Trailing whitespace hangs.** SP, the hard break characters and ZWSP at the logical end
  of a line are laid out but excluded from the line's width, so they never cause a break.
  In RTL lines they sit left of the content.
- **Pieces and reordering.** A line is cut into pieces wherever the shaped run or the L1
  level changes, and L2 then orders the pieces.
  - Within a piece, glyphs keep HarfBuzz's visual order. A piece whose L1 level parity
    differs from its run's (whitespace reset to the paragraph level) is reversed.
  - A piece that starts or ends mid-run is sliced by cluster when HarfBuzz marks the
    boundary safe. Otherwise it is reshaped, with the paragraph as context, so cursive
    joining continues across an intra-word break (CSS Text 3 §7.3).
- **Metrics:**
  - The line box height is `round(max ascent + max descent + max line gap)` over the
    line's runs, with the gap split evenly above and below.
  - Baselines are `round(top + gap/2 + ascent)`, and line tops accumulate rounded
    heights, so spacing is uniform and every baseline is a whole physical pixel.
- **Alignment** (start by default; see the follow-ups above for the others) uses each
  line's own paragraph level, since texts with paragraph separators can mix directions.
  Unbounded layouts align to the widest line.
- **Max lines and the ellipsis.**
  - When `max_lines` cuts text off, the last kept line is shortened to the longest prefix
    that fits with the ellipsis, ending at a grapheme boundary and never on whitespace.
  - The ellipsis is shaped in the font and script of the run before it, at the paragraph
    direction, as a piece at the paragraph level at the logical end (on the right in LTR,
    on the left in RTL).
  - The default is U+2026; if that font lacks it, "..." is used. An empty ellipsis cuts
    without one.
- **Empty lines:** empty text is one empty line in the base font, and text ending in a
  hard break gets an empty last line. Carets need both once editing exists.
- **Cache:** `ui_paragraph` keeps the shaped paragraph (keyed by text, spans, direction
  and scale) and the line layout (also width and max lines). A width change, such as a
  resize or a spring, only re-breaks lines. `shape_count` and `layout_count` let tests
  check this.

### Wire and rendering
- **`set_text` (opcode 8) and `append_text` (9)**; the format is in
  `docs/command-buffer.md`.
  - Text longer than one op (64 KiB) continues in `append_text` ops for the same node,
    which must follow directly in the same buffer. The op framing is unchanged and the wire
    stays v0. Buffers are atomic, so long text changes atomically too.
  - Family names travel as a NUL-terminated string table inside the op. Interned style IDs
    come with the styling milestone.
  - The core assembles the continuations while applying: it reads ahead from the
    `set_text` and consumes them. A missing continuation drops the `set_text` (and what it
    consumed), and the node keeps its old text.
- **Invalid UTF-8** is replaced per maximal subpart. Span offsets are remapped onto the
  sanitized text, so a span stays on its characters; boundaries inside a sequence snap to
  its start. Replacements are counted in `ui_app_stats.text_invalid_sequences`.
- **Fonts come from app creation** (`ui_app_desc.text.font_files`, `system_fonts`), and at
  runtime from `load_font` (see the follow-ups above).
- **Scene:** a node may own a `ui_paragraph`.
  - It is laid out at the node's width × scale when its surface's list is built.
  - Draw order is background, then text, then children. Overflowing text is drawn unless
    the node clips (see the follow-ups above).
  - Destroying a node frees its paragraph.
- **Glyph placement:** pen x is floored to a pixel and the remainder picks one of 4 bins
  (rounding to the nearest bin; bin 4 carries into the next pixel). Baselines round to
  whole physical pixels, including when the node itself sits at a fractional position.
- **Display list:** glyph instances (48 bytes, the same stride as rects) plus ordered
  batches. Each batch covers a contiguous range of rects or of glyphs on one atlas page, so
  paint order holds while drawing stays instanced.
- **Glyph pipeline:** one quad per glyph at whole pixels, reading coverage with
  `textureLoad`. This means no sampler, no filtering and no bleeding, and the result is the
  same on every driver. The pipelines for both target formats are compiled at startup,
  next to the rect pipelines. There is one bind group per atlas page.
- **Coverage contrast:** blending stays in linear light (sRGB targets). The fragment shader
  uses `c' = c + k·c·(1−c)·(1−L)`, where L is the luminance of the text color.
  - Dark text thickens; white text is unchanged.
  - k must stay ≤ 1 to keep c' monotonic.
  - An optional `pow(c', 1/γ)` follows (γ = 1, off, by default).
  - The **default is k = 0.9**, measured rather than picked by eye. For the same coverage,
    total ink of dark-on-light text is 1,346 at k = 0, 1,550 at 0.5 and 1,819 at 1.0,
    against 1,784 for sRGB-space blending (the weight people are used to); 0.9 matches it.
    Light-on-dark text is already about that heavy with linear blending.
  - Images are in `docs/images/`, generated by `tools/text_contrast_shots`.
  - `ui_text_config.contrast` and `gamma` set it (0 = default); the `UI_TEXT_CONTRAST`
    and `UI_TEXT_GAMMA` environment variables override it, including to 0.
- **Image codecs** (`src/image/`): own QOI encoder and decoder, used for goldens, and a PNG
  writer with stored deflate blocks, used for diffs and docs images. No dependency.
- **Goldens:** 7 cases at 1.0, 1.25 and 1.5, 400×150 logical px each, 332 KB of QOI in
  `tests/goldens/`, generated under lavapipe (Mesa 26.2) with `UI_UPDATE_GOLDENS=1`.
  - The single and threaded schedulers must produce byte-identical images.
  - Against a golden, at most 0.05% of pixels may differ by more than 2/255 in any
    channel. That tolerance absorbs sRGB blending differences between Mesa versions, since
    coverage itself is computed on the CPU and read exactly.
  - Failures write actual, expected and diff PNGs next to the test binary, and CI uploads
    them.
- **Upload survival test** (`test_atlas_upload`). `ui_app_step_core`, test-only, produces a
  frame on the core alone.
  - Single scheduler: a frame that introduces 19 px glyphs is replaced before render runs.
    Later the same text is drawn from cache, with no new upload, and must match a fresh
    app's render byte for byte.
  - Threaded scheduler: a hook holds the render thread inside encode while the core
    publishes 20 frames with new glyphs each. The final text uses glyphs from a replaced
    frame.
  - A deliberately broken render, keeping only the newest queued update as a mailbox
    would, fails both variants.

### Hit testing
- **Internal C API** (`src/text/hittest.h`) over a paragraph's last layout. It becomes
  public, and asynchronous for languages, when editing lands in the core.
- **Data model:** per line, grapheme clusters in visual order with their byte range,
  extent and direction.
  - A cluster's logical end is the next cluster in logical order: the next glyph group to
    the right in LTR runs, to the left in RTL runs.
  - A glyph group covering several graphemes (a ligature) is split per grapheme, at GDEF
    ligature carets when the font has them for that glyph, evenly otherwise. The "ffi"
    ligature in "office" still gets a caret between each letter.
  - Hard line breaks are not clusters, so a caret never sits after a newline on its own
    line.
  - Built lazily on the first query after a layout, and cached with it.
- **Affinity:** DOWNSTREAM attaches the caret to the character after the offset, UPSTREAM
  to the one before it.
  - At a soft wrap, upstream is the end of the earlier line. After a hard break the next
    line is used whatever the affinity.
  - At a direction change, upstream is the trailing edge of the preceding character's run
    and downstream the leading edge of the following one. Offset 4 in "abc אבג def" has
    one caret at each end of the Hebrew run.
- **Point to offset:** the line by y (clamped), then the cluster by x (clamped to the line
  ends). The leading half (left in LTR, right in RTL) gives the cluster's start with
  downstream affinity; the trailing half gives its end with upstream affinity.
- **Selection rectangles:** per line, the clusters overlapping the range, merged where they
  touch in visual order, at the full line-box height. A range across a direction change
  gives several rectangles on one line.
- **Carets** are centered on the boundary with the caller's width, spanning the line box.
  An offset inside a cluster snaps to its start, and an empty line puts the caret at its
  start edge.

### Fuzzing, benchmarks and the example
- **Fuzz preset** (`cmake --preset fuzz`): libFuzzer + ASan + UBSan, RelWithDebInfo.
  Everything is built with `-fsanitize=fuzzer-no-link`, HarfBuzz included. There are four
  targets in `fuzz/`, and each checks invariants as well as surviving its input:
  - `fuzz_utf8`: output always valid, replacement count equal to the maximal subparts, and
    valid input unchanged.
  - `fuzz_linebreak`: LB2/LB3, mandatory breaks exactly after hard breaks, no break before
    a space except after one, GB1/GB2.
  - `fuzz_bidi`: paragraph levels 0 or 1 and honoring a forced direction, levels within
    max depth + 1, trailing whitespace at the paragraph level, and L2 a permutation.
  - `fuzz_set_text`: input bytes become a `set_text` (and `append_text`) payload, then
    apply, layout at an input-chosen scale and width, rasterization into a small atlas,
    and hit tests.
- **Seed corpora** are committed in `fuzz/corpus/` (multilingual and malformed samples;
  the `set_text` seeds come from the real writer). CTest runs each target for 10,000 runs
  in CI; new inputs go to the build directory.
- **First finding:** after 488k runs, `fuzz_set_text` found a heap overflow in the
  rasterizer, not the wire code. Bitmap x is computed as `x + (dx − left)` while the bounds
  use `floor(min_x + dx)`, and the two round differently, so a point could land at x =
  −1e−7 and write column −1. Points are now clamped into the bitmap (also when stepping
  per row), and non-finite outline points are ignored. The input is in the corpus as
  `regress-raster-negative-x`.
- **Benchmarks:** `bench/bench_text`, built in every preset and not run by CTest. It
  measures shaping plus layout of a 10,000-character paragraph (cold and re-layout),
  rasterizing 1,000 new glyphs, and frame time with 2,000 glyphs split into core commit
  and render encode through the hook API. Results are in `docs/text.md`. CI budgets come
  with the vertical slice's numbers.
- **Example:** `examples/text_paragraph` shows a card with a multilingual paragraph.
  - The card follows the window; a timer every 3 s springs it between 45% and 100% of the
    width, and the text node inside springs with it, so the text reflows on every frame
    of the animation.
  - Fonts are the bundled test fonts plus system fonts for fallback.
  - `--headless` writes a PPM.
  - Run under both schedulers in nested headless sway at 1.25×: about 159 fps on a 160 Hz
    output while reflowing, with a median core commit of 0.26 ms.

## Milestone 0.2: hardening before M1

### Idle frames
- **A surface draws only when it has work.** Work is any of:
  - an applied op on a node under it;
  - a spring under it that was running at the start of the frame (the settling frame still
    draws);
  - a size or scale change;
  - a new configure;
  - a forced redraw.
  A ready surface with none of these commits nothing, increments
  `ui_app_stats.frames_skipped_idle` and emits the `UI_TRACE_FRAME_IDLE` hook.
- **"Forced" means the renderer lost the frame** (no swapchain image), so the content must
  be redrawn even though nothing changed. `ui_app_frame` is *not* a force. It gives every
  surface a frame opportunity, like a vblank, so the headless test can show that frames
  without commands commit nothing.
- **Three pacing states per surface:** *ready* (may draw now), *awaiting* (a frame
  callback is outstanding) and *idle* (neither). A drawn frame asks for its next callback
  in its commit. An idle frame asks for none, so an idle window costs the compositor,
  the GPU and us nothing.
- **Waking up:** when an idle surface gets work, the core calls `surface_request_frame`
  once. On Wayland that is `wl_surface_frame` plus a `wl_surface_commit` with no new
  buffer, issued from the main thread. That is safe because an idle surface has no frame in
  flight and the render thread has nothing pending for it. The next frame is paced by the
  compositor's callback, as before the idle period.
- **Pending command buffers count as work for every idle surface.** Which surface a buffer
  touches is only known once it is applied at a frame boundary, so all of them are woken.
  A surface that turns out unaffected skips one frame. That costs one buffer-less commit
  per flush while idle, which is cheap next to an always-on loop.
- **A configure that changes only window state still draws one frame.** That is what acks
  it, so the compositor never waits on an unacked configure.

### Timers (new wire op and event)
- **Why:** idle frames remove the only clock logic had. Logic used `frame_committed` to time
  its every-2-seconds spring, so an idle window would stop the demo.
- **What:** command `set_timer(id, deadline_ns)`, opcode 7, and event
  `timer_fired(id, deadline_ns, now_ns)`, opcode 4, both on the animation clock. This is the
  architecture's pattern for async results: a request ID completed by an event, with no
  callbacks into the language. Old readers skip the new opcodes, so the wire version stays
  0.
- **Determinism:** timers are on the animation clock, so a paused or stepped clock drives
  them in tests. They fire in `ui_core_step` before the frame, earliest deadline first,
  ties by id.
  - In run mode the main thread sleeps in the platform wait exactly until the next due
    timer (`ui_core_wait_timeout_ns`, using the clock's inverse mapping), or forever while
    paused. The wait rounds up to whole milliseconds so it never spins early.
- **`set_timer` applies at the frame boundary like every command.** While idle it is one
  of the pending buffers that wake the surfaces (above).
- **Limit:** 4096 armed timers per app; more are dropped and counted as dropped ops.
- The demo arms its timer from the first `frame_committed` and re-arms from each
  deadline, so the period doesn't drift.

### Backpressure
- **Logic → core is a separate, unbounded queue** (`ui_core.from_logic`). It grows by
  doubling and never returns `UI_ERR_FULL`. The core's own bounded queue now carries only
  render → core messages (frame done, render failed), and core → logic stays bounded with
  coalescing.
  - **Why unbounded rather than blocking:** it needs no cross-scheduler wait protocol, and
    the single scheduler, where core and logic share a thread, can't block at all.
  - **Why memory is still bounded in practice:** logic can only run ahead by as many
    flushes as it makes between two core steps.
- **The sleep-polling retry loop is gone.** The only remaining failure is out of memory,
  and it loses nothing:
  - the logic actor keeps an ordered outbox and retries it before handling new events;
  - the core reserves room for every queued buffer before taking any, and on failure
    leaves them queued until the next step.
  So order and completeness hold even under memory pressure.
- **The flood test** pushes into `from_logic` directly, from another thread under the
  threaded scheduler. That is the exact channel the logic actor uses, and the logic host
  can't yet flush several buffers in one callback.

### Refresh-aware retry
- **A skipped frame now retries after the surface's own period**
  (`surface_period_ns`, a new platform call), instead of a hard-coded 16.67 ms. The period
  is:
  - on Wayland, the refresh from presentation feedback, else the measured present
    interval, else 60 Hz;
  - on headless, a nominal 60 Hz.
  A retry is also a forced redraw.

### Test-only frame without logic
- **`ui_app_frame_without_logic`** (internal, in `runtime/app_internal.h`) is a scheduler op
  that produces a frame and waits only for render to finish it, not for logic. The stall
  test uses it to keep frames coming while logic is held, without real sleeps.
  - Under the single scheduler it steps core and render and skips logic.
- **The stall test holds logic on a condition variable** inside its event callback for 32
  frames × 16 ms (512 ms of stepped clock time). Every frame must commit and the rect must
  follow the closed-form spring within 1.5 px. After release, the `set_color` written
  during the stall must apply.

### Tooling
- **The `UI_TRACE` printer is idle-aware.** It restarts interval measurement after an idle
  period, so an idle gap is no longer reported as a long frame or a late present, and it
  prints the number of idle frames.
- **`spring_rect --period S`** sets the time between springs, so idle behavior can be
  observed (long idle stretches with `--period 20`).

## Milestone 0.1: runtime skeleton

### Dependencies and build
- **wgpu-native v29.0.1.1** is pinned as a git submodule. Its crates are vendored by
  `tools/vendor-rust.sh` into the gitignored `third_party/wgpu-native-vendor/`, about
  215 MB. `.cargo/config.toml` makes every Cargo build offline against them, and
  `Cargo.lock` pins versions and checksums.
- **Corrosion v0.6.1** is copied in-tree; it is pure CMake.
- **Rust 1.93** is pinned to match wgpu-native's toolchain file.
- **wgpu-native is always built in release**, even in debug presets: a debug wgpu cannot
  hold frame rate, and we don't debug it. Only the `vulkan` and `wgsl` features are
  enabled on Linux.
- `_DEFAULT_SOURCE` is defined for the library because strict `-std=c23` hides POSIX
  interfaces such as `clock_gettime`.
- The WGSL shader is embedded with C23 `#embed`, so no build step is needed.

### Handles and wire
- **The logic side allocates node handles.** The core mirrors the generational scheme
  and validates each handle: `create_node` needs a free slot with a newer generation, and
  every other op needs an exact match. This avoids a round trip ("the core never waits on
  JS") and keeps stale-handle detection exact.
- **The top 4 bits of a handle's index carry its kind** (node or surface). A surface handle
  can never be confused with a node handle.
- **There is no `insert_child` yet.** `create_node` takes a parent, which is either a node
  or a surface (meaning a root-level node). Surfaces reach logic through `surface_resized`.
- **`destroy_node` destroys the subtree.** The logic side is responsible for freeing the
  descendants' handles in its allocator.
- **Malformed framing rejects the whole buffer**, which is what keeps flushes atomic.
  Semantically invalid ops (stale handles, NaN) are dropped one by one.
- **At most two event buffers are in flight to logic**; later events coalesce. A stalled
  logic actor therefore can't grow an unbounded queue.

### Threading and schedulers
- **"Identical behavior" between schedulers is defined for `ui_app_frame`**, which
  produces one frame and then waits until render has finished it and logic has handled
  its events. Under a paused clock the headless test requires byte-identical images from
  both schedulers.
  - In free-running `ui_app_run` mode, a threaded flush can land one frame later than in
    single mode. The Visibility guarantee ("no earlier than the next commit") allows this.
- **Single-scheduler order is core → logic → render.** Commands that logic produces in
  response to frame N apply at frame N+1 in both modes.
- **Closing a window ends the app (v0 policy).** Logic still receives
  `surface_close_requested` first. A real close protocol comes with the language runtime.
- **The render-thread check uses a thread-local scope counter**, set while the render
  actor's start, step or stop runs. Any `wgpu*` call outside it aborts in debug builds.
  This is the only thread-local state; it holds no app data.

### Rendering
- **Swapchain format:** BGRA8 sRGB (preferred) or RGBA8 sRGB, so blending happens in
  linear light. Pipelines for both are precompiled at startup.
- **Present mode is FIFO.** At most one frame is queued, through wgpu-native's
  `desiredMaximumFrameLatency = 1` (an extension, behind a capability) and through pacing
  on frame callbacks.
- **Headless targets are RGBA8 sRGB.** Captures read back the last rendered frame.
- **Display lists hold physical pixels and premultiplied linear colors.** The renderer
  uploads them directly as instances.
- **One animation time per frame**: the earliest predicted present among the surfaces
  drawing. Sampling per surface arrives with multi-monitor work.

### Wayland
- **Per-commit state is attached on the render thread** right before present: the
  configure ack, the viewport destination, the frame callback and presentation feedback.
  It must share the commit with the buffer of the matching size, which only the render
  thread knows. libwayland requests are thread-safe, and the callbacks are dispatched on
  the main thread.
- **Callbacks are created through a `wl_surface` proxy wrapper pinned to the default
  queue.** The NVIDIA Vulkan WSI moves the original `wl_surface` proxy to its private queue.
  Without the wrapper, our frame callbacks were never dispatched.
- **Predicted present time** = the first vblank after the frame callback (from the last
  presentation timestamp and the refresh period) plus a learned number of periods. That
  number moves after 4 consecutive presents that are more than half a period late or
  early.
  - With refresh 0 (VRR, or wlroots' headless backend), the period is measured from
    presents.
- **Versions bound:** seat v7 and compositor v4. This needs no handlers for events newer
  than libwayland 1.20 (pointer `axis_value120`, `warp`, `preferred_buffer_scale`).
- **The first frame renders at scale 1.** The compositor only sends `preferred_scale` after
  the surface is mapped on an output; the second frame is correct.
- The app ID is `dev.axe.app` for now.

### Testing
- The demo logic is a library shared by the example and the headless test. The test
  drives the real logic rather than a copy.
- The ASan test preset sets `VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1`. Without it,
  driver leaks show up as `<unknown module>` frames that `tools/lsan.supp` can't match.
