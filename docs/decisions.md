# Decisions log

Choices the architecture document leaves open, recorded as they are made. Newest
milestone first.

## Milestone 3b.3: hot reload

### The user's choices in review
- **Module-level signals are keyed too,** by the component rule, and stay the same object.
  `layout_cards`' TSX is split so `main.tsx` only mounts.
- **Each module evaluates under an owner.** Evaluating it again first disposes the previous
  version's effects, computeds, cleanups and timers. The entry stays a full reload; docs/ui.md
  recommends a small entry.
- **A component that throws while running again** rolls back: its previous version runs with the
  state its instances had, and the overlay shows the error.

### Differences from the plan
The approved design (3b plan, section 6), and what 3b.1 and 3b.2 changed:
- **The host had no module loading.** The engine's hooks existed, tested, but the host set neither.
  It gains a module table behind them, in `src/script/host/dev.c`, compiled only with `AXE_DEV`.
- **`import()` needs no engine change.** It runs through the same hooks. test_js_engine now checks
  it: two versions of a file, a shared import evaluated once, three kinds of failure, cycles.
- **"One prebuilt module" is one per specifier over shared chunks** (esbuild `--splitting`), not
  host-generated facades as the plan's first revision said: facades would need each package's
  export list, and splitting gives the same single state for free.
- **Template ids are per theme variant,** allocated lazily (3b.2), so "the same ids" means: a
  registry keeps the `Template` object across versions and redefines each live variant under its
  id. Themeless variants are counted in development only, so unused ones can be destroyed.
- **Tokens, themes and contexts need keys too.** Evaluating `context()` again makes a new symbol,
  which consumers can't find, and `token()` a new id.
- **A token's value is left alone unless its initializer changed,** as a signal's is: a running
  app changes tokens (`layout_cards` switches its theme), and an edit elsewhere must not undo that.
- **Inline style edits are template edits,** since 3b.2 compiles them into the template. Only
  `style()` edits are style-only.
- **`For`'s one-component rows** bypass `ComponentElement`, so a row registers as an instance.
- **The core gains a turn request.** Only the core sends event buffers. It's an atomic flag, not a
  queued message: a request can't be lost to a full queue, and several become one turn.

### What the tests and the example changed
- **A family runs again when its module was evaluated because of an import,** not only when its
  hash changed: its closures hold the old import. The brief's rule alone would leave it stale.
- **An update is a transaction.** A module evaluating can't know whether a later one will fail, so
  what it defines is staged and committed together. On a full reload the new app mounts before the
  old one goes, so a failure leaves the old one running.
- **The bootstrap.** If the entry doesn't build at startup, nothing has loaded `@axe/ui` to show
  the error. So the host always loads the hot runtime first, and the first batch is an ordinary
  update.
- **The import graph comes from the resolve hook,** not the transform: the engine resolves every
  static import of every version, so the edges are there for free.
- **A boundary may export plain objects of hot values.** `layout_cards`' `export const S = {...}`
  showed that a style-only edit otherwise evaluates every importer and runs its components.
- **A deleted file blocks an update only while something unchanged imports it,** or the edit that
  removes the import could never apply; and it clears when the file returns.
- **`AXE_DEV:` labels, not `if (AXE_DEV)`.** Dev hooks are labeled statements, which bundles drop
  with esbuild's `--drop-labels`. Without minifying, `if (false) instanceBegin(...)` stays in the
  text. `--minify-syntax` removed it, but the guard caught it costing 2.66% on
  `ui_for_mount_10k` (isolated by building the same tree without the flag), and that mount is a
  tracked target. Labels rewrite nothing else.

### CI
No job ran release tests, so `bench-callgrind`, the one release build, also runs
`axe_run_refuses_dev` and `bundle_no_dev_code`, with `--no-tests=error`: `ctest -R` passes when
nothing matches, and a renamed test would stop being checked without a sound.

### Tried, no gain
Stripping the inline source map from module source before the engine compiles it: the heap per
edit didn't change at all, because QuickJS doesn't keep the text. A plain module of 10 functions
measures 22 KB per version with no hot reload, so what grows is the engine's own module cost.

### Limits
- **Keys come from names:** renaming a signal resets it, as does moving it past another of the
  same name.
- **A signal made by a helper called twice at a module's top level isn't kept,** and neither are
  tokens made in a loop (`layout_cards/theme.ts`).
- **`surface.onFrame` runs outside the module's owner,** so timers it starts need
  `runWithOwner`. `layout_cards/main.tsx` does this.
- **Top-level await in an app module** can run past the update's checkpoint: it isn't tested.
- **A removed export** that an unchanged importer still uses keeps working until the importer is
  edited, since a boundary's importers aren't evaluated again.
- **inotify only.** Other platforms implement `watch.h` when they arrive.

### Measurements
Latency and heap: docs/ui.md, "Hot reload". A component edit is 33 ms at p50 and 39 ms at p95 in a
Debug build, against the 200 ms target. The Callgrind guard needs no new baselines: every case is
within 0.2%. No fuzz run: no fuzz target reaches what changed in the core (the turn request).

## Milestone 3b.2: the TSX transform, templates and the language benchmarks

### Toolchain and dependencies
- **oxc 0.157.0 on Rust 1.97** (3b.1 moved the whole build to 1.97). axe-tsx uses the parser,
  the AST and its visitor, scopes (`oxc_semantic`) and `oxc_sourcemap`: 90 crates, all
  permissive. `self_cell` is Apache-2.0 OR GPL-2.0-only, taken as Apache-2.0
  (third_party/VERSIONS.md).
- **One vendor directory,** `third_party/rust-vendor`, for both lockfiles (`cargo vendor
  --sync`); CI's cache keys cover both.
- **No LTO** for axe-tsx. The developer machine ran out of memory twice during 3b.2: once, a
  test looping forever (below); and it has 15 GiB with a browser and the desktop taking much of
  it. Builds there are capped (`CMAKE_BUILD_PARALLEL_LEVEL=4`, `CARGO_BUILD_JOBS=2`), and new
  tests run under a cgroup memory cap.

### The transform
- **Splicing, not regenerating.** axe-tsx keeps the source outside JSX as it is, types included
  (esbuild strips them next), and replaces each outermost JSX expression. The source map maps
  copied text line by line and generated text to the JSX it replaced.
- **Classification** (docs/ui.md, "How TSX compiles"):
  - constant: literals, module-level bindings, static members of module-level objects, and
    spreads of these;
  - local: no call or member access, but a local binding;
  - dynamic: calls, member accesses off locals, tagged templates.

  Functions are values. A static member of a module-level object (`S.card`) is read when the
  template is defined, which assumes it isn't a getter that changes.
- **A step before esbuild, not a plugin.** esbuild plugins need its JS or Go API, and builds
  run its native binary without Node. CMake mirrors an app's directory through axe-tsx when it
  holds TSX, then bundles the mirror. Package internals in tests go through the `@axe/ui/src/*`
  path, so a mirrored test shares the package's modules.
- **The leaf-text rule** is split three ways:
  - the type checker rejects strings in a View (TypeScript gives every JSX expression one type,
    `JSX.Element`, a brand that both a View's and a Text's children accept);
  - the transform reports misnesting it can see;
  - the runtime reports the rest.
- **Snapshots:** `tests/tsx/cases`, compared by `tools/axe-tsx/snapshots.py`
  (`tsx_snapshots`).

### Templates
- **Expanded on receipt, not applied in the scene.** The core rewrites each buffer's instances
  into their ops when it receives the buffer (docs/templates.md). Preparation, the scene and
  every check see plain ops, so equivalence holds by construction.
  - This replaces the plan's all-or-nothing check of an instance's handles: an instance is
    exactly its ops, bad handles included.
  - `test_template` checks the expansion byte for byte against hand-written ops: fixed
    templates and 300 random ones.
- **Wire:** ops 21–23 are new. Master's reader skips them (two fixtures). fuzz_commands expands
  as the core does; 5 minutes on 4 workers, about 460,000 inputs, clean.
- **In @axe/ui,** templates are defined lazily, once per theme in effect. A theme's variant is
  destroyed once no instance and no provider uses it (the user's choice); the themeless one
  stays, and its instances keep no count.
- **Constant inline styles** go into the template's `set_props` (the user's choice: inline
  styles are overrides).

### layout_cards in TSX
`layout_cards_equivalence` compares C, TypeScript and TSX: byte-identical at every time, under
both schedulers, and in dark right-to-left mode. `For` moves the fewest rows, so a shuffle
moves the same cards the imperative version does.

### The language benchmarks
- **The apps:** frozen TSX apps under `bench/apps`:
  - `screen1k`: 60 cards of 16 nodes, 300 paragraphs, 3 bindings a card;
  - `list10k`: a View, a bound label and a bound badge per row; `--stream` writes 100 items a
    frame, `--rows=N` sets the count;
  - `text1k`: 1,000 bound texts.
- **`bench_ui`** times the JS side of turns on the CPU, and its `--heap` mode takes the heap
  apart. **`bench_ui_live`** runs `list10k` in a window.
- **The hooks API is at version 2:** `UI_TRACE_COMMANDS_APPLIED` reports the frame that
  applied a buffer and when the logic thread sent it, which, with `PRESENTED`, gives the update
  delay.
- **The live runs** are in a headless sway (no window on screen) at a fixed rate. Its reported
  refresh isn't reliable when frames are slow, so the frame timings come from the trace.
- **Results against the agreed thresholds:** see docs/ui.md, "Benchmarks". The update path
  passes: the 100-write turn at p50 1.5 and p99 2.1 ms, 1,000 text updates in 7.2 ms, and the
  update delay at p99 1.75 frames. These fail:
  - the 10,000-row mount (about 450 ms against 300);
  - the heap per row (3.3 KB against 2);
  - dropped frames with 10,000 rows. The core's frame for 30,000 nodes takes about 40 ms (the
    display list and its encoding), whatever JS does; with 1,000 rows, 60 Hz holds with no
    misses.

### Making the runtime leaner
The first measurements: mount 836 ms, 6.7 KB a row, the 100-write turn at p50 4.3 ms, 1,000
texts at 25 ms. The measured QuickJS costs:

| Cost | Size or time |
| --- | --- |
| A closure with one captured variable | about 200 B |
| An empty object | about 140 B |
| An object of 5 fields | about 200 B |
| A short string | about 60 B |
| A property write | about 0.1 us |
| `setPrototypeOf` on an object | about 200 B more (the shape is unshared) |

What changed:
- **Text.**
  - Plain text (no options or Spans) gets a lean binding, written only on change.
  - `set_text` is encoded natively into a worst-case reservation, then the op is shrunk.
- **Raw writers** for `set_styles` and `instantiate_template`.
- **Bindings** are objects with a shared run function (no closure each), with a fast path for
  one style.
- **For rows.** A row is its owner, and a one-component row runs in place.
- **Owners** keep their region in a field.
- **A region with one node** keeps it in a field.
- **A signal** is its own getter, its methods assigned rather than inherited.
- **Fields.** Rarely used owner and computation fields are added lazily, and sources keep no
  observer tail.
- **Named closures.** None on hot paths: with `--keep-names` each costs a `__name` call.
- **Tried, no gain:** `useDefineForClassFields: false`.

### Bugs found on the way
- **An import cycle** (elements -> theme -> template -> elements) left the template module's
  class keyed by an undefined `MOUNT`. `MOUNT` moved to `regions.ts`, which imports nothing of
  the package.
- **A test helper looped forever** on `slotChildren(-1)`, filling an array until the machine ran
  out of memory. `slotChildren` now refuses a slot without a node, and axe-test-js limits the
  JS heap to 512 MiB.

## Milestone 3b.1: signals and components

### The user's choices in review
- **The split:** 3b.1 is signals and components; 3b.2 the TSX transform, templates and the
  language benchmarks; 3b.3 hot reload.
- **One Rust toolchain, 1.97,** for wgpu-native and, from 3b.2, axe-tsx on the current oxc.
  There is no second toolchain; oxc 0.133.0 on 1.93 would have been the fallback had wgpu-native
  broken.
- **The signal API:** `count()`, `count.set(v)`, `count.update(f)`.
- **No `batch`.** Effects always run at the end of the turn, so writes are batched already.
- **The benchmark thresholds** as proposed, plus a JS heap of at most 2 KB per row after
  mounting list10k (B2).
- **`destroy_template`** comes with the template ops (3b.2). `@axe/ui` releases a template variant
  once no instance and no provider uses it.
- **Inline styles are overrides,** constant or dynamic, as inline styles beat state rules in CSS.
  From 3b.2, constant inline values compile into the template's `set_props`.
- **Hot reload is in-process** (3b.3). The socket comes with the automation protocol.
- **Requested additions:**
  - the transform warns when a component destructures its props (3b.2);
  - `ts_typecheck`'s time is reported before and after the style types (below).

### Rust 1.97
- **`rust-toolchain.toml` is the one source.** CMake reads its channel and forces
  `Rust_TOOLCHAIN`, so build directories configured under 1.93 follow it. wgpu-native's own file
  still says 1.93; Corrosion runs the pinned toolchain.
- **Checked on 1.97.1:**
  - release, debug, asan and tsan suites: 46/46 each;
  - the fuzz preset's tests: 5/5;
  - the Callgrind guard in CI's image (with 1.97 added): every case within 0.3%.

  wgpu-native gains two unused-variable warnings in `conv.rs`, and nothing else changed.

### Signals
- **Push-pull with three states:** clean, check and dirty.
  - A write marks its direct observers dirty and everything downstream check, and queues the
    effects it reaches.
  - A computed in check refreshes its sources in order. It runs only if one of them changed,
    which a version per link tells.
  - This is glitch-free for diamonds, and an unchanged value stops propagation.
- **Links are reused across runs:** a run reading the same sources in the same order allocates
  nothing. A run links each source once, however often it reads it.
- **Scheduling:**
  - a new `setBeforeFlush` hook in `@axe/core` runs the flush after every `app.onTurnEnd`
    handler, so their writes land in the same buffer;
  - bindings run before effects, each in creation order (by id), and effects' writes queue
    further rounds;
  - after 100 rounds the flush reports "effects did not settle" and leaves the rest queued.
- **Bindings run when created,** so nodes get their initial values in the same ops. A binding
  that read nothing is dropped after its first run.
- **Errors:**
  - a computed's error is rethrown to its readers until a source changes;
  - an effect's or binding's error goes to the nearest boundary, or is reported, and the effect
    stays subscribed;
  - reports go through a new host function, `reportError`, as uncaught exceptions are, with the
    component stack.

### Ownership and regions
- **Owners:**
  - children are kept in an array, disposed newest first;
  - an owner disposed on its own is popped if newest, and otherwise left until half the array is
    disposed, then compacted, so a long list's rows go in O(1);
  - only the owner disposed first removes nodes (one `destroy_node` per top-level node); its
    descendants' nodes are inside them or beside them in its region.
- **Regions, not placeholder nodes.** Dynamic content lives in a region (a run of a parent's
  children). To insert, it finds the first node after it, so the scene equals the imperative
  one. A test compares a tree's ops against the same tree built with `Node` calls.
- **Elements are descriptions,** mounted where they are inserted. Children therefore mount under
  the component that places them and see its context. This is how `Provider` works with no
  special case.
- **`@axe/core/internal`:**
  - a slot-level node API (`slotCreate`, `slotMove`, `slotDestroy`, setters) with no `Node`
    objects and no leak registry: owners own the nodes;
  - `Node.adopt(slot)` gives refs a non-owning `Node`.

### For
- **Keyed by item, or `key(item)`;** repeated keys pair up in order.
- **The fewest moves.** Kept rows not in a longest increasing subsequence of their old positions
  move once each. The tests count the `move_node` ops: one for moving the last item to the
  front, n − 1 for a reversal.
- **Two passes, both linear:**
  1. from the end, the kept rows go into place (`following`, the first node after them, is
     carried along) and new rows get empty regions that remember their anchor node;
  2. front to back, the new rows mount before their anchors.

  So the first mount and rows added at the end need no `move_node`. A first version mounted new
  rows from the end with a move each. A second found each new row's place by walking past the
  empty regions after it: 10,000 rows took 16 s. The anchors fixed it.

### Styles
- **Value sugar is in `@axe/core`** (`sugar.ts`): `Token.create`, `with` and `set` take it too.
  `style()` and inline styles are in `@axe/ui`.
- **The generator emits what sugar needs:**
  - `stylePropKinds`, the value kinds each property takes, in the IDL's order;
  - `styleKeywords`, each keyword property's names in CSS spelling, subsets included.
- **`tests/ui/style_sugar_cases.json`** is the shared table that `@axe/ui`'s tests and, from
  3b.2, axe-tsx's must both pass.
- **Colors:** `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb(r, g, b)` and `rgba(r, g, b, a)` (`#rgba` isn't
  accepted).
  - **Types:** hex digits and lengths are checked at type level, through a homomorphic mapped
    parameter (`S extends CheckBlock<S> ? S : CheckBlock<S>`). The error names the fix: `invalid
    color "#ggg": use #rgb, #rrggbb or #rrggbbaa`. The `S & Check<S>` pattern only says "not
    assignable to never".
  - **rgb() and rgba()** are `rgb(${string})` to the type checker, checked at run time (and at
    compile time for constants, from 3b.2).
- **Static props apply once, with no binding.** A prop is dynamic when it is a getter or holds a
  function, and a Text when any prop or part is.
- **Themes:** `ThemeProvider` is a context provider plus a View whose style holds the theme's
  token declarations. View and Text put the theme's default first in their style list.

### The leaf-text rule
- **Types:** `View`'s children exclude strings, numbers and Spans; `Text`'s exclude nodes. The
  type tests check all four.
- **At run time:** the same misplacements throw, with a message pointing to `<Text>`. In 3b.2
  the transform reports intrinsic misnesting at compile time, since every JSX expression is one
  `JSX.Element` type.

### Type-checking time
`tsc` over the repository (15 runs each, local, with a browser busy):

| Tree | Best | Median |
| --- | --- | --- |
| master | 75 ms | 87 ms |
| this milestone | 107 ms | 112–134 ms |
| this milestone without the color and keyword checks | 95 ms | 98 ms |

The checks cost about 14 ms over roughly 30 `style()` calls in the tests, which isn't noticeable,
so they stay. The rest is the new package and its tests.

### First measurements
Release, `axe-run` headless, with a browser busy (load 5–6). These are preliminary; 3b.2's
benchmarks are the real ones.

| Case | Cost |
| --- | --- |
| A signal | 1.2 us |
| A computed, created and read with one source | 7.5 us |
| A binding with one source | 6.2 us |
| A tracked read (link included) | about 2.5 us |
| 10,000 For rows, each a View and a bound Text | about 600 ms |
| The same with static text | about 370 ms |
| 1,000 writes | 2–3 ms |
| Flushing 1,000 text bindings | 30–55 ms |
| Reversing 10,000 rows | about 65 ms |

- **QuickJS's pace:** about 0.1 us per property write, 0.9 us per small object. So the
  graph's objects are small:
  - computeds (`Memo`) carry their source fields, and bindings and effects don't;
  - flags, state and kind share one integer;
  - the run's cursor and id are module variables.
- **No named closures on hot paths.** `--keep-names` makes each one a `__name` call, about 1 us.
  The signal getter, Text's setter, `setText`'s relabel, the subtree walk and the color parser
  were rewritten to avoid them.
- **What 3b.2 must still win:**
  - `setText`'s encoding (about 16 us), which dominates the text cases: the lean `set_text` the
    plan has;
  - static structure: templates.

  B2 (mount at most 300 ms, update turns at p99 at most 4 ms) and B3 (at most 8 ms) aren't met
  by this runtime alone.

### Benchmark guard
In CI's image, on the final code:

| Case | Change | Why |
| --- | --- | --- |
| `bench_script/startup_source` | +5.40% | The frozen cards app bundles `@axe/core`, which grew (value sugar, the slot API), so there is more source to compile. Intended. |
| `bench_script/script_encode` | −5.32% | `setText` no longer creates a named closure per call. |
| `bench_script/script_dispatch` | +1.77% | Within the threshold. |
| `bench_script/startup_bytecode` | −0.40% | |
| Layout and text cases | unchanged | |

Per CONTRIBUTING.md, the baselines are updated from CI's artifact in a commit of their own.

## Milestone 3a.2: merge cleanup

### The TSan failure in script_host
- **Reproduced:** `test_script_host` from the tsan build, 4 at a time, while the rest of the
  TSan suite ran in a loop for load, each run's output kept. It failed 5 times in the first
  35 runs.
- **Not a race.** ThreadSanitizer reported nothing. `test_commands` (3 runs) and
  `test_text_globals` (2) failed their `hook_count == 0` checks, because a long-task hook had
  arrived: under TSan and load, evaluating the app or a turn building 4,000 ops took 51 to
  73 ms, past the default threshold of 50 ms of wall time.
- **Fixed in the test harness:** tests get no long-task reports unless they set a threshold
  (`test_long_task` sets 5 ms). The host's behavior is right: a slow task is a long task.
- **Regression test:** `test_slow_task_not_reported` spins 60 ms under the harness's default
  and expects no hooks; it fails with the old harness.
- **Checked:** after the fix, 1,000 runs under the same load passed, none failing (before
  it, 5 of 35 failed).
- **CI** now uploads, when a test fails:
  - `LastTest.log`, which holds every test's output, sanitizer reports included;
  - a JUnit file;
  - fuzz crash, leak and timeout inputs.

  A failed test's output is no longer truncated.

### Benchmarks independent of the examples
- **`bench/apps/cards`** is a frozen copy of the TypeScript `layout_cards` as of milestone 3a
  (cf963fc). `bench_script`'s startup, memory and first-frame cases run it instead of the
  example, whose growth in 3a.1 had moved the startup counts by +21% and +13%.
- **The startup counts now move only with the framework.** Against the 3a baselines, the
  startup cases measure +0.55% (bytecode) and +2.79% (source: 3a.1's larger `@axe/core` and
  `@axe/wire` to compile), in CI's image.
- **The rule is in CONTRIBUTING.md ("Bench apps").** Examples change freely. Bench apps
  change only on purpose, with the baselines updated from CI's artifact in a commit of their
  own.

### C API evolution
- **`docs/c-api.md`** sets the rules:
  - structs only gain fields at their end;
  - a new field's zero keeps the old behavior;
  - functions keep their signatures;
  - callers use designated initializers.
- **Positional initializers are unsupported,** except for four fixed pairs that will never
  grow, which the headers now mark: `ui_handle`, `ui_rect`, `ui_style_decl` and
  `ui_token_decl`. The repository writes thousands of `{property, value}` pairs, and they
  mirror wire records.
- **Tests:**
  - `c_api_designated_c11`, `c_api_designated_c17` and `c_api_designated_c23` compile a caller
    of every descriptor and config struct with `-Wall -Wextra -Wpedantic -Werror
    -Wmissing-field-initializers`;
  - `c_api_positional_rejected` expects a positional style block to fail on its missing
    `tokens` field.
- **The repository's own** positional `ui_text_span_desc` and `ui_time_source` initializers
  are designated now.

### CI on a branch
- **A manual run** (`workflow_dispatch`) has no base commit, so the `idl` job compared nothing.
  It now uses the branch's merge base with master for the evolution rules and the
  older-reader check.

## Milestone 3a.1: token scopes

### The user's choices in review
- **Strict kinds.** A token's kind is its global value's kind, followed through global
  aliases, and a declaration must have it. TypeScript matches the core exactly:
  `Token.create` infers one kind, and a union token type is a compile error. Kind classes
  wait for a real need.
- **Local and invalid tokens.** A token never set, or unset, is local: untyped, its
  declarations taken as they are. A token in a global cycle is invalid: its declarations and
  references are ignored like an undefined token, with a debug warning.
- **A new op,** `set_token_overrides` (20), rather than extending `set_props`.
- **The older-reader check** is a permanent part of CI's `idl` job, for every wire change.
- **The comparison with CSS custom properties** goes into `docs/styles.md` as proposed.

### Wire
- **Declarations are parallel arrays:** `u32 tokens[n]`, then `n` style entries of
  property 0, as `set_token` carries a value. A record of its own would have needed a second
  copy of the style-value codec, which the generator ties to `style_entry`'s 4-byte header.
- **Blocks grow from 8 to 12 bytes:** `token_count` (since `m3a.1`) and a reserved `u16`, the
  declarations after the entries. A block without declarations is still written with the
  8-byte header and `header_bytes` 0, so buffers that don't use scopes are byte for byte
  what they were: no fixture, golden or `layout_cards` image changed.
- **Generator changes**, each with tests:
  - an appended count is allowed for new variable data after the old, in a record with an
    original header field;
  - several arrays can share a count, and the encoders check their lengths;
  - `header(0)` writes the short header while every appended field is 0;
  - in TypeScript, the arrays an appended count counts are optional, so `@axe/wire` callers
    from before still compile;
  - `--compat-base` lets reserved padding follow an appended field, as the schema check
    already did;
  - the op history table lists the appended fields of self-sized records (style blocks).
- **Two of M1.1's fixtures** (`cmd-f71e5234b684363e`, `cmd-fz347fb8cb9b976ccf`) and
  `test_wire_compat`'s newer-writer block put a hypothetical newer writer's fields at block
  offset 8, which this version now owns. They were rewritten so that writer's fields follow
  this version's (header 16, `token_count` 0). Their dumps changed only in sizes and the
  header.
- **C API.** `ui_style_block_desc` gained `tokens` and `token_count` at its end. Positional
  initializers still compile, but `-Wmissing-field-initializers` now warns about them; the
  repository's own callers use designated initializers.
- **The older reader** (`tools/wire/old_reader.py`): `git archive` of the base, its
  generated codec and `wire_dump.c` compiled with the system C compiler, and the fixtures
  added since read through it and compared with this version's dumps minus the lines it
  can't know. A fixture with a block header written as 0 despite declarations fails it.

### Data model
- **A scope record per node that ever declared a token,** kept (perhaps empty) until the node
  is destroyed. Every node points at the nearest one, its own or its parent's. Records hold
  copies of the declarations, not pointers into definitions: previews read them, possibly
  between a redefinition and the resolution that gathers them again.
- **Pointers follow the tree at once:** creating a node copies its parent's, a node's first
  record re-points its subtree, and a move re-points the moved subtree (or re-links its
  record). These walks are stackless (pre-order over sibling links), since resolution's
  stack is in use while records are made.
- **Ownership is a flag on the node.** The first version asked the record for its owner,
  which read a freed ancestor's record while a subtree was torn down. The fuzzer found it on
  the first seed run.

### Resolution
- **Lookup per link:** the records from the nearest out, a filter test each, a binary search
  on a hit; then the global value. A token's kind is computed only when a declaration is
  found. Escaping a global cycle takes a declaration of one of its tokens, whose kind check
  then fails; without one, Floyd's walk finds the cycle.
- **What a node reads:** its filter holds its entries' own tokens (from the cascade, as in
  M2.1) and every later link and alias target that computing it followed. `check_resolved`
  also checks that a fresh computation reads nothing outside the filter.
- **A scope change** diffs the old and new declarations by value. It queues the subtree's
  readers of the changed tokens, minus those a scope between hides, and skips a subtree once
  everything is hidden. The subtree then resolves in tree order. Queuing happens where the
  cascade is gathered, so `resolve_node`'s common path didn't change.
- **Moves** queue by filter, the old and new chains' declarations together: more than
  needed at times, never too few.
- **Planted bugs** each failed `test_scopes`:
  - no subtree marking: 35 failed checks;
  - no re-pointing on a move: 5;
  - no kind check: 5;
  - no read recording: 56 (6 after recording moved to later links only);
  - no ordered walk: 49.

### Previews and preparation
- **Previews** build temporary scopes for the nodes they compute. These are allocated one by
  one, because the preview's node table moves when it grows. A node under an ancestor whose
  declarations differ is computed, not answered from the scene, and so is a scoped node when
  the buffer changes a token. Declarations make a buffer global (they can reach any text in
  the subtree).
- **Preparation missed the new op.** Buffers holding only `set_token_overrides` had no head
  step, so paragraphs it changed were shaped on the core. `test_prepare`'s scoped variant
  found it (a different frame, 3 paragraphs shaped on the core); the fuzzer checks previews
  but doesn't run the preparer.

### TypeScript
- **`Token<V extends TokenType & OneKind<V>>`.** `OneKind` is an interface whose `kind` is
  `never` for a union, so the constraint fails for one. A conditional type in the constraint
  is circular (TS2313); an interface is evaluated lazily. `AnyToken` lists the eight kinds.
- **`token.with(value)`** makes a declaration, typed by the token. `Token.local<V>()` makes a
  token with no global value; `node.setTokens` and `node.unsetTokens` set and remove a node's
  own; style blocks take `tokens`.

### Benchmarks
- **`bench_layout`'s scope case** switches a panel of 714 cards (5,000 nodes, each reading
  the background token the panel's scope declares) between two scopes. Over six release runs
  on 2026-11-04, with a browser and an editor busy: medians 0.41–1.10 ms, best 0.40–0.74 ms
  (target < 1 ms), 3.35M instructions. With the scope's text color too, which the subtree
  inherits: medians 1.06–1.94 ms, best 0.70–1.71 ms (reported, not a Callgrind case).
- **Global token changes didn't get slower.** The first version cost the token case +14% of
  its instructions and the inherited case +4.9%, so it was reworked:
  - the single-family test was inlined into the scan;
  - the common lookup got a straight path (no scope, one link), with the scoped walk out of
    line;
  - only links after the first are noted while computing;
  - the scope work moved into the block that gathers the cascade, with the subtree marked
    there;
  - the scene keeps one style view instead of building one per node.

  In CI's image the counts against the baselines are then: token -0.91%, inherited +0.56%,
  the other layout and text cases unchanged.
- **The startup cases grew because the example did.** `layout_cards`' first turn now builds
  a panel, a heading and two more cards (81 nodes instead of 61), sets 18 tokens instead of
  8 and defines a style more.
  - startup_bytecode: +20.9%; startup_source: +13.5%.
  - Before the example changed, the same core and `@axe/core` measured +0.58% and +2.81%
    (the larger bundle to compile).
  - The cost is intended: per CONTRIBUTING.md, the baselines are updated from CI's artifact in
    a commit of their own, with the new scope case's.

### Fuzzing
- **Seeds:** `seed16` and `seed17` are the blocks and panel fixtures, the panel's split so
  its second half changes a resolved scene. `seed18` is styled text under nested scopes,
  with states, own declarations, moves, a cycle and global changes in its second half.
  There is no dictionary to extend.
- **`fuzz_commands`** found the ownership use-after-free on the first seed run. Two 5-minute
  runs on 4 workers followed, about 246,000 and 192,000 inputs, with no findings; the second
  ran on the final code, its coverage up from 3,126 to 3,222 edges. `fuzz_set_text`, which
  covers `scene.c` too: 3 minutes on 4 workers, about 871,000 inputs, no findings.

### Example
- **`layout_cards`** gains nine inverse tokens, a panel whose scope declares each theme token
  as an alias of its inverse, and a hover block on cards: card, border and title alias the
  hover and accent tokens. Inside the panel those aliases resolve through the panel's scope.
- **A timer hovers the cards in turn** every 1.5 s, the panel's first, since input comes in
  milestone 4; `--no-hover` turns it off.
- **The TS version follows op for op;** token and style ids match by creation order, and
  `layout_cards_equivalence` stays byte-identical (both variants).

## After milestone 3a: fixes

### Moving boxes land without a snap
Reported in `layout_cards`: a card's border looked thicker while it moved, and the card
visibly snapped when it stopped. There were two causes.

- **Border widths snapped only at rest.** A moving node drew its border unsnapped, so a
  1 px border at 1.25× drew 1.25 device px, smeared over two pixels. It snapped back to 1
  when the card stopped. Layout already used the snapped width, so the moving border was
  also wider than the space layout gave it. Widths now always snap; only a moving box's
  position is subpixel.
- **Positions snapped when the spring settled.** A box glided toward its exact resting
  position (say 520.4 device px), then jumped to the rounded one (520) on the frame its
  spring stopped, its edges and text sharpening at once.
  - Now the build tracks where each box rests: its layout rect while a layout animation
    runs, plus what a frame's spring has left to travel. A box is drawn at its exact
    position plus its resting position's rounding error, so it arrives on that pixel. At
    rest this is the old rounding exactly.
  - Children and text keep their resting offsets inside a moving box, so nothing shifts
    when it lands.
  - In captures of a card settling, the landing frame changes nothing.
- **What remains:** glyphs are placed in quarter pixels horizontally and whole pixels
  vertically, so text steps in the slow tail of a move. Placing moving text exactly would
  be a change to how glyphs are positioned.
- **Tests:** two tests asserted that a child keeps its unsnapped offset from a moving
  parent (6.25 device px). They now assert its resting offset (6). The `test_paint_golden`
  case also checks the landing invariant: every moving frame, the box is drawn at its
  resting pixel plus what it has left to travel. One golden, `anim_content_mid_shrink@125`,
  was regenerated: a box mid-animation, now with crisp 1 px borders and text at its resting
  offset.

### A use-after-free in the head step
- **Found** while checking the snapping fix. `layout_cards_equivalence` failed once in
  three ASan runs: the TypeScript example, threaded scheduler, at 6.2 s. Reproduced about
  1 in 24 runs by running that case alone.
- **The race,** found with ThreadSanitizer: the head step read `tp->shaped_source` while a
  worker replaced it.
  - The head step of a buffer that loads fonts first registered them
    (`ui_prep_register_fonts`). Its own text had been shaped before the fonts existed, so
    registering sent those items back to the workers.
  - The very next loop read what each item was shaped as, to check its values, while
    `shape_item` was releasing and replacing it on a worker.
  - When the text took values from the node's styles, the shaped source was one derived
    for the node, and the worker's release freed it. The TS example's text does take its
    values from styles; the C example registers its font at startup, not in a buffer.
  - It could also submit a second job for an item already being shaped, when the check
    found its values wrong.
- **The fix is ordering.** The head step checks its text first, while the buffer's jobs
  are provably idle (it only runs then), and registers fonts after. An item sent back for
  new values isn't done, so registering doesn't send it twice. If the fonts land while it
  is shaped, `shape_item` sees the new generation under the lock and shapes again.
- **Test:** `test_font_loading`'s `test_load_font_with_text`. Under the threaded
  scheduler, four buffers each load a new font and set 48 paragraphs whose size comes from
  styles, and every paragraph must end up shaped with the fonts as they are. It repeats
  in six fresh apps.
  - Against the old order, ASan failed it in 8 of 10 runs with the same use-after-free, and
    TSan reports the race on every run.
  - With the fix: 0 of 20 ASan runs, TSan clean apart from Mesa's known `lp_fence_destroy`.
    The TS example's case: 0 of 24.
- **TSan found it on every run, where ASan needed luck,** so it became a preset and a CI
  job (next section).

### ThreadSanitizer in CI
- **A `tsan` preset and CI matrix entry** run the whole suite under ThreadSanitizer with
  `halt_on_error=1`: any race fails its test.
- **The C11 shim is committed** (`tools/tsan/c11_shim.h`). `cmake/Sanitizers.cmake`
  force-includes it into every C file when `UI_SANITIZE` contains `thread`. It maps
  `thrd_create`/`thrd_join`, `mtx_*` and `cnd_*` onto pthreads, which TSan intercepts
  (M1 decisions: glibc's C11 functions call pthreads out of TSan's sight). C++ (HarfBuzz)
  doesn't get it, and Rust (wgpu-native) isn't instrumented at all.
- **RelWithDebInfo, not Debug.** At `-O0` `text_golden` took 662 s under TSan; at `-O2`,
  14 s. The whole suite runs in about 90 s locally (4 jobs).
- **lavapipe only:** the test preset sets `VK_DRIVER_FILES` to lavapipe's ICD (both file
  names distributions use), since TSan segfaults inside NVIDIA's proprietary driver.
  `script_host` crashed that way locally before. CI has only lavapipe anyway.
- **Suppressions:** Mesa's `lp_fence_destroy` race (llvmpipe destroys a fence's mutex
  while its threads may still touch it). Races in our code are fixed, not suppressed.
  - **By library too.** The first CI run failed 15 GPU tests with 45 reports of this race.
    Ubuntu's Mesa has no symbols, so the frames read `libvulkan_lvp.so+0x...` and the
    function-name suppression didn't match; Arch resolves them, so it passed locally.
  - It now also suppresses `race:libvulkan_lvp.so`, any race with a frame inside lavapipe.
    Verified in the Ubuntu CI image: 41 of 41 pass. Our races still fail: with the old head
    step, `font_loading` still reports `core.c:482 in head_step`.
- **First run:** the whole suite passes with no reports in our code.
- **Proved:** with the head step's old order put back, the preset failed
  `font_loading` 3 times out of 3, reporting `data race ... core.c:482 in head_step`. With
  the fix it passes.

## Milestone 3a: language binding plumbing

### The user's choices in review
- **Time base:** a new `clock` event (7) comes first in every event buffer, and one buffer
  is sent at startup.
- **IDL format:** a custom line-based format with its grammar in `docs/idl.md`.
- **Errors:** the ops a failed task wrote are kept.
- **u64 is a `number`.**
  - Times (`u64 time` in the IDL) aren't rejected past 2^53: they decode rounded, and JS sees
    them in ms since the app started.
  - Exact counters (`frame_id`) throw past 2^53, and the encoder takes only safe integers for
    them.
- **Microtasks:** a checkpoint after each event and each timer callback.
- **Leaks:** a subtree is reported when its root's `Node` object is collected and no
  ancestor's object is alive.
- **`axe-run` options** (`--size`, `--title`, `--font`) configure the window: the surface
  exists before JS runs. Logged for later:
  - JS-created surfaces (a `create_surface` op) for multi-window apps;
  - a manifest, with packaging.
- **TypeScript 7 for type-checking only.** Nothing uses the compiler's programmatic API; the
  TSX transform (M3b) will be an esbuild plugin or a tool of its own.
- **Generated public enums** (`include/ui/wire_ids.h`).
- **`measure()` on a gone node** rejects with `StaleNodeError`.
- **`performance.now()` is wall time with web semantics.** The animation clock is `app.time`
  and the times on events. The test runner maps `performance.now()` to the animation clock.
- **`app.onSurface` replays** existing surfaces to a late handler.
- **`setText`'s `families` is a list of names** (`["Noto Sans", "sans-serif"]`), joined into
  one CSS-style list on the wire.

### IDL and generator
- **`original N` on every op and event, checked.** Reserved bytes taken over by a `since`
  field count as inside the original, as `header_bytes` did. Reserved padding past the
  original needs no `since`.
- **What the generator rejects:**
  - a `default` other than 0 on an appended field;
  - an appended enum without a value 0;
  - an appended count or size;
  - a header-less op growing its fixed part.
- **`--compat-base` compares two schemas** (CI: the PR's base, or the previous commit).
- **Checks in the IDL are the constraints the core drops on.** Context-dependent ones stay in
  the scene: `set_text`'s fields that may come from the node, handles, span order.
- **The C API's `ui_cmd_*` stay hand-written** as argument checks and conveniences over the
  generated encoders, with no offsets left in them. Their rejections are unchanged, including
  `load_font` not checking UTF-8 (the core drops such a path).
- **Generated C:**
  - fixed parts as structs, with `_read` (zero-filling) and `_write`;
  - `_tail_of`, size accessors, tail element accessors;
  - `_check` returning the failing field's name, which drop messages now print;
  - `_put`;
  - framing, and the style value validators.
- **The scene keeps `ui_scene_validate`** as a one-line wrapper of the generated validator.
- **The generated TypeScript:**
  - packs sub-word fields into 32-bit writes;
  - uses literal `case` values, because QuickJS evaluates case expressions in turn;
  - inlines integer checks and small copies.

### Migration proof
`tests/wire/proof.md` has the details.

- **Encoding:** a dual-check build ran the pre-IDL encoders on every call of the suite and the
  examples, byte for byte.
- **Decoding:** 11,759 recorded buffers and seeds, plus 5.4 million fuzzer inputs (301 s),
  decoded identically.
- **What stays:** 575 fixtures with their pre-IDL dumps, checked by `test_wire_fixtures`.
- **The fuzzer's one finding wasn't a bug.** A short type-7 event is malformed for the new
  reader (`clock`) and skipped by the old one; the dumps renumber type 7 before comparing.

### Engine and QuickJS-ng
- **QuickJS-ng v0.18.0**, built from its four engine sources. No `quickjs-libc`, so apps
  have no `std` or `os`.
- **Always `-O2`**, as HarfBuzz is; the sanitizers still instrument it.
- **The engine interface follows N-API:** scoped values, strong and weak refs, and a pending
  exception reported by an empty value or `false`. Weak refs go through `WeakRef`.
- **Compiling a module gives its imports empty stand-ins.** QuickJS loads imports even when
  only compiling, and bytecode binds them by name at evaluation. So `axe-jsc` uses an engine
  of its own.
- **QuickJS-ng quirks worked around:**
  - `JS_ThrowInternalError` is uncatchable, so a native that returns no value without
    throwing throws a TypeError instead;
  - a `FinalizationRegistry` target that is its own unregister token is never finalized, so
    nodes register a separate token.

### The host
- **One command buffer per turn:**
  - JS writes it in place, in host memory exposed as an ArrayBuffer;
  - the host takes that memory at the flush and appends its timer's re-arm;
  - a buffer is detached before its memory moves.
- **Events reach JS zero-copy** for the turn's length. The clock is dispatched too: it marks
  the turn's start, for `app.onTurnStart`.
- **JS timers share one wire timer** (id `0xFFFFFFFF`), armed for the earliest deadline:
  - deadlines are the turn's time plus the delay;
  - timers set during a batch wait for a later turn;
  - an interval re-arms from its deadline, or from now after a jump;
  - intervals are at least 1 ms.
- **The core fires timers again after a frame's apply.** A timer armed for a time already
  past fired one step later under the single scheduler than under the threaded one, which
  happens to take an extra step. Now both fire it in the same step.
- **Long tasks are measured per task** (an event with its checkpoint, a timer callback, the
  evaluation, the flush), 50 ms by default.
- **Hooks API version 1:** the `SCRIPT_*` kinds, plus `text` and `duration_ns`.
- **The prelude:** console, timers, the text codecs and `performance` are a JS prelude over
  a few natives, which are reachable only through its closures. A lone surrogate encodes as
  U+FFFD.
- **`ui_app_desc.script`:** the desc is copied, except the bundle's bytes.
  `ui_script_desc.internal` lets Axe's tools (the test runner) define modules.

### TypeScript, toolchain and tests
- **No Node.js at build time.** CMake runs esbuild's and tsc's native binaries from
  `ts/node_modules`, installed with `npm ci --ignore-scripts`; the install scripts aren't
  needed.
- **Debug and release app builds:** `axe_ts_app` builds read the bundle's source in debug
  builds and embed its bytecode in the others.
- **The handle allocator in JS** is the C one's: generations, a LIFO free list, subtrees
  freed in pre-order. The tree mirror lives in typed arrays per slot, so `Node` objects are
  held only by the app (which the leak rule needs).
- **The TS `layout_cards` follows the C one op for op**, `loadFont` instead of a startup font
  included. `layout_cards_equivalence` compares at 0, 1.3, 2.6, 4.1, 6.2 and 12.4 s under both
  schedulers, and again in dark right-to-left mode. Byte-identical in debug, release
  (bytecode) and asan.
- **The test runner (`axe-test-js`):**
  - `step(ms)` applies what the turn wrote, then steps the clock;
  - each step comes with a timer, since a stepped clock with nothing to draw sends no events.
- **Examples link `ui_keep_loaded`** in sanitizer builds, as the tests do (they run under
  `layout_cards_equivalence`).

### Benchmarks
Release, local, noisy machine (medians; best within a few percent):

- **Encoding through `@axe/core`, per op:**

  | Op | Time |
  | --- | --- |
  | `setRect` | 1.3 us |
  | `setColor` | 1.4 us |
  | `setProps` (3 entries) | 11 us |
  | `setStyles` | 2.7 us |
  | `setText` (short) | 16 us |
  | `Node.create` + `destroy` | 6.7 us |
  | bare `@axe/wire` `setRect` | 1.0 us |

  A mixed group comes to about 0.2 Mops/s, 5 MB/s. The calibration: a loop iteration with a
  call is 0.29 us. The cost is the interpreter's pace (roughly 20–50 ns a bytecode op), not
  a hotspot.
- **Startup** (host, evaluation and first turn of `layout_cards`): 5.9 ms from source,
  2.6 ms from bytecode. To first frame with a GPU: about 195 ms either way, dominated by the
  device and the first frame.
- **JS heap after the first turn:** 128 KiB for an empty app, 390 KiB for `layout_cards`.
- **Event to handler:** 5.3 us per turn of one `frame_committed`; 2.9 us per event in a turn
  of 64.
- **bench-callgrind** gets `script_encode`, `startup_source`, `startup_bytecode` and
  `script_dispatch`; their counts are deterministic.

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

### A bug the fuzzer found on the way
- **`start_spring` on an unknown property.** The check was `components(property) ==
  count`, and an unknown property has 0 components, so `count = 0` passed. The op then took
  the corner-radius branch and stored a target it never read (an uninitialized float). The
  nightly `fuzz (commands)` job hit it in run 41689458441 (`fuzz.yml` dispatched at
  acb05fe). The NaN radius tripped the resolution check, whose drawn-value comparison NaN
  fails. The bug dates from milestone 0's legacy ops, not M2.2.
- **Fixed in the core and the writer:** an op with an unknown property is dropped, as is a
  negative radius target, as `set_corner_radius` already did. The writer refuses both. Tests:
  `test_wire_compat`'s `test_start_spring_unknown`, a case in `test_runtime`, and the crash
  input as `fuzz/corpus/commands/regress-start-spring-unknown-property`. A 4-minute local
  run afterwards was clean (269k executions).

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
