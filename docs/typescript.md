# TypeScript

Apps are written in TypeScript against `@axe/core`, bundled by esbuild into one ES module,
and run by the script host on the logic thread ([`script.md`](script.md)). This milestone's
API is imperative and minimal; signals, TSX and hot reload (milestone 3b) build on it.

```ts
import { app, Node, Style, Token, loadFont, px, rgba, spring, keyword, FlexDirection, State } from "@axe/core";

const accent = Token.create(rgba(0.2, 0.45, 0.95));
const card = Style.define({
  decls: { width: px(208), flexDirection: keyword(FlexDirection.column), backgroundColor: rgba(1, 1, 1) },
  transitions: { backgroundColor: spring(90, 19), layout: spring(170, 24) },
  states: [{ when: State.hover, decls: { backgroundColor: accent } }],
});

void loadFont(`${AXE_SOURCE_DIR}/tests/fonts/NotoSans-VF.ttf`);

app.onSurface((surface) => {
  const n = Node.create(surface, { styles: [card] });
  n.setText("Card 1", { families: ["Noto Sans", "sans-serif"], size: 16, weight: 650 });
  n.measure().then((l) => console.log(l.layoutInSurface));
  setInterval(() => accent.set(rgba(0.38, 0.62, 1)), 4000);
});
```

`examples/spring_rect/main.ts` and `examples/layout_cards/main.ts` are ports of the C
examples, which stay as the C API's examples. The TypeScript `layout_cards` draws
byte-identical images to the C one at the same times under both schedulers
(`layout_cards_equivalence`).

## Packages

All in `ts/`, an npm workspace:

| Package | Contents |
| --- | --- |
| `@axe/wire` | Generated from `idl/wire.idl` ([`idl.md`](idl.md)): the enums, a `CommandWriter` with one method per op, `decodeEvent`, style value types and validators. Hand-written beside it: the writer's buffer, `EventReader`, u64 and UTF-8 helpers (`utf8OffsetFromUtf16`, `utf16OffsetFromUtf8`, `Utf8OffsetMap`). |
| `@axe/core` | The app, surfaces, nodes, styles, tokens, text, fonts, measurement; the binding to the host (`axe:host`). |
| `@axe/testing` | `test`, `expect`, and clock control for tests run by `axe-test-js`. |

`ts/types/host.d.ts` declares what the host provides (the globals in
[`script.md`](script.md#host-api), and the `axe:host` and `axe:test` modules); apps are
type-checked against it rather than the DOM's or Node's types.

## The API

### App and surfaces

- `app.onSurface(fn)` calls `fn` with each surface, existing ones first (replayed to a
  handler registered late), then each new one, its size already known.
- `surface.onResize`, `onFrame` and `onCloseRequested`, and `nextFrame()`.
- `app.time` is the animation clock at this turn, in ms since the app started
  ([`script.md`](script.md#time)). Times on events are in the same unit.
- `app.args` are the app's arguments (`axe-run ... -- args`).
- `app.onTurnStart` and `app.onTurnEnd` run at the start of every turn and before its
  commands are sent. M3b's signal scheduler batches updates there.

### Nodes

`Node.create(parent, { styles })` allocates the node's handle in JS (no round trip; the
allocator is the C one's, generations and LIFO free list) and writes `create_node`.

- **Ownership is explicit.** `destroy()` destroys the node and its subtree and frees their
  handles; any later use of them throws `StaleNodeError`. The tree is mirrored in JS (per
  handle slot, in typed arrays) only to free the right handles.
- **The leak safety net (debug builds).** A `FinalizationRegistry` watches `Node` objects. A
  subtree is reported (stderr and `UI_TRACE_SCRIPT_LEAK`) when its root's object is collected
  while the node lives and no ancestor's object is alive: nothing can destroy it any more. A
  child dropped while its parent's object lives isn't reported, since destroying the parent
  takes it along. Nothing is destroyed automatically.
- `moveTo(parent, before)`, `setStyles(...styles)`, `setProps(overrides, transitions)`
  (`unset` removes an override), `setState(mask, value)`, `setText(text, options)`, the older
  ops `setRect`, `setColor`, `setCornerRadius`, `setClip`, and `spring(property, target,
  { stiffness, damping })`.
- `measure()` resolves with the layout and presentation rects, relative to the parent and to
  the surface, once the frame that applies the request has laid the node out; answers come in
  request order. A node gone by then rejects with `StaleNodeError`.

### Styles and tokens

- **Values** are built by constructors: `px`, `percent`, `number`, `rgba`, `keyword` (with
  the generated enums: `keyword(FlexDirection.column)`), `string`, `spring`, and the
  constants `auto`, `none`, `inherit`, `initial`, `unset`.
- **`Style.define({ decls, transitions, states })`** defines an interned style: applying it
  costs one op. Per-property value types are generated, so `width: rgba(...)` fails to
  type-check; a value out of range throws a `RangeError` naming the property, as the core
  would drop it.
- **`Token.create(value)`** makes a theme token. Declarations refer to it directly
  (`backgroundColor: accent`), and `token.set(value)` updates every node using it, with their
  transitions.

### Text

`setText(text, options)` sets a node's paragraph. A field left out of the options comes from
the node's styles, or from what it inherits; for a span, from the paragraph.

- `families` is a fallback list (`["Noto Sans", "sans-serif"]`).
- `size`, `weight`, `slant`, `language` (null for none), `color`, `lineHeight`, `align`,
  `direction`, `maxLines`, and `ellipsis` (undefined for U+2026, null or "" for none).
- `spans` take UTF-16 indices, as JS strings count; the wire's offsets are UTF-8 bytes, and
  `setText` converts them. Hit testing will use `Utf8OffsetMap` the same way.
- Text longer than one op continues in `append_text` ops; all of it applies, or none.

### Fonts

`loadFont(path)` resolves when the font is registered, in the frame its commands apply, and
rejects with `FontLoadError` for a file that can't be read or isn't a font.

### Errors

The encoders check their arguments and throw before writing anything: a buffer never holds a
partial op. An uncaught error anywhere is reported by the host and the app goes on
([`script.md`](script.md#errors)).

## Building

Node.js and npm are build-time tools, needed once to install the pinned tools:

```sh
npm ci --ignore-scripts --prefix ts
```

| Tool | Version | Role |
| --- | --- | --- |
| esbuild | 0.28.2 | Bundles an app or a test into one ES module (`--format=esm --platform=neutral --target=es2023 --keep-names`). `axe:*` modules stay external. |
| TypeScript | 7.0.2 | Type-checking only (`tsc --noEmit`, strict). Nothing uses the compiler's programmatic API: M3b's TSX transform will be an esbuild plugin or a tool of its own. |
| `axe-jsc` | this build | Compiles a bundle to bytecode (`.axbc`, [`script.md`](script.md#bundles)). |

The lockfile pins both packages and their native binaries, which CMake calls directly
(`cmake/TypeScript.cmake`), so building needs no Node.js either.

In CMake:

```cmake
axe_ts_app(layout_cards_ts ENTRY ${CMAKE_CURRENT_SOURCE_DIR}/layout_cards/main.ts
           WIDTH 980 HEIGHT 640 SCALE 150 TITLE "layout_cards")
```

This builds an executable, `axe-run` with the app built in:

- **Debug builds** read the bundle's source from the build tree, so re-bundling needs no
  relink.
- **Other builds** embed its bytecode (`#embed`), for a faster start.

`axe_ts_bundle` and `axe_ts_bytecode` are the steps on their own.

## Running apps

```
axe-run [--scheduler single|threaded] [--headless [--at S] [--out f.ppm] [--scale N]]
        [--size WxH] [--title T] [--font F]... [--no-system-fonts] [--memory-limit MB]
        [--long-task MS] [--strict] bundle.{js,axbc} [-- app arguments]
```

`--headless` steps a paused clock the way the C examples do (two frames, then 60 Hz steps up
to `--at`) and writes the frame as a PPM. `--strict` makes the exit status 3 if the app
reported an uncaught error. An app built by `axe_ts_app` takes the same options, with its
own bundle by default.

The window's size, title and startup fonts are options for now, because the surface exists
before any JS runs. Apps creating their own surfaces (a `create_surface` op) come with
multi-window support; a manifest with packaging.

## Tests

`ts/packages/*/test/*.test.ts` are bundled and run by `axe-test-js` inside the script host,
headless, one CTest test each (`ts_wire`, `ts_core`, `ts_host`):

```ts
import { expect, step, test } from "@axe/testing";

test("a timer fires at its deadline", async () => {
  let fired = false;
  setTimeout(() => (fired = true), 100);
  await step(100); // what the test wrote applies, then the clock moves 100 ms
  expect(fired).toBe(true);
});
```

- **Determinism.** The clock is paused, and `performance.now()` reads it too.
- **`step(ms)`** applies what the test wrote, moves the clock by `ms`, and resolves in the
  turn at that time. `nextTurn()` waits for a later turn without moving it.
- **`hooks()`** lists the uncaught errors, rejections, long tasks and leaks reported so far.
- **`gc()`** collects garbage now.
- **`cBuffer` and `cEvents`** give buffers encoded by the C codec, which the encoder tests
  compare byte for byte.

`ts_typecheck` runs `tsc` over the packages, the tests and the examples.
