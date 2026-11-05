# Writing apps with @axe/ui

`@axe/ui` is how Axe apps are written: TSX components with fine-grained signals, over the
retained scene of [`@axe/core`](typescript.md).
- **Components run once.** Only the code that reads a signal runs again when the signal
  changes, and it changes exactly the properties that depend on it. There is no virtual DOM and
  no diffing.
- **Static structure is nearly free.** Each subtree's static part is a template the core
  instantiates with one op ([`templates.md`](templates.md)).

This guide is for people and for AI tools writing apps. Every rule is stated outright, and
each one that is enforced shows the error you'd get.

```tsx
import { For, Text, View, app, mount, signal, style } from "@axe/ui";

const card = style({ padding: 12, gap: 6, backgroundColor: "#fff", cornerRadius: 8 });
const count = signal(0);
const items = signal(["Apples", "Pears"]);

function App() {
  return (
    <View style={card}>
      <Text>Clicked {count()} times</Text>
      <For each={items()}>{(item: string) => <Text>{item}</Text>}</For>
    </View>
  );
}

app.onSurface((surface) => mount(App, surface));
setInterval(() => count.update((n) => n + 1), 1000);
```

## Rules at a glance

1. **Components run once.** Code in a component's body runs when it mounts, never again. What
   must follow a signal goes in JSX (a prop or child that reads it), a `computed` or an
   `effect`.
2. **Read a signal where it is used.** `{count()}` in JSX, or inside a computed or an effect,
   subscribes that spot. `const n = count()` in a component's body reads it once.
3. **Props that change are getters.** Read `props.title` where you need it. Don't destructure
   props (`function Card({ title })`): that reads every prop once, when the component starts.
   The transform warns:
   `Card destructures its props: props that change won't update it; read them as props.x where they're used`.
   Use `mergeProps` for defaults and `splitProps` to forward a subset.
4. **Text goes only in `<Text>`.** Text in a `<View>` is a compile error that points to
   `<Text>`. A `<Span>` goes only in a `<Text>`, and a `<Text>` holds no other element.
5. **Effects run at the end of the turn.** A write never runs anything right away. Bindings and
   effects run once at the end of the turn, however many writes happened, so a turn writes one
   atomic command buffer.
6. **Owners dispose what they create.** When a component, a branch of `<Show>` or a row of
   `<For>` goes away, its effects stop, its cleanups run (newest first) and its nodes are
   destroyed.
7. **Tags are components.** `<View>`, `<Text>` and `<Span>` come from `@axe/ui`. There are no
   lowercase elements: `<div>` is an error.

## Signals

| Function | What it does |
| --- | --- |
| `signal(v)` | A value. `s()` reads it, `s.set(v)` and `s.update(fn)` change it, `s.peek()` reads it without subscribing. |
| `computed(fn)` | A derived value: computed when read, and again only after something it read changed. |
| `effect(fn)` | Runs `fn` at the end of the turn that created it, then again whenever something it read changes. |
| `untrack(fn)` | Runs `fn` without subscribing to what it reads. |
| `onCleanup(fn)` | Runs `fn` when the current owner is disposed or, inside an effect or computed, before its next run. |
| `root(fn)` | A new root owner, disposed by the function `fn` receives. `mount` makes one for you. |
| `getOwner()`, `runWithOwner(o, fn)` | Keep ownership and context across `await` or a timer. |

**When things run.**
- **Writes** mark what depends on them and queue the effects they reach. No user code runs
  during a write.
- **Computeds** are lazy and always current: reading one after a write gives the new value, in
  the same turn.
- **At the end of each turn:** first every `app.onTurnEnd` handler, then the bindings, in
  creation order, then the effects. Effects that write signals queue another round. After 100
  rounds the flush stops and reports "effects did not settle"; the remaining effects wait for
  the next turn.
- **There is no `batch`:** every write in a turn is already batched.
- **Microtasks:** a promise that an effect resolves continues after the flush. What it writes
  goes into the next turn.

**Glitch-free.** A computed runs at most once per change, after every source it reads has
settled, so it never sees a mix of old and new values. With `b = a + 1`, `c = a * 2` and
`d = b + c`, `d` runs once per write to `a` and always sees a consistent `b` and `c`. A computed
whose value didn't change (`Object.is`, or its `equals` option) stops the propagation there.

**Dependencies are dynamic.** Each run subscribes to what that run read. In
`() => (useA() ? a() : b())`, a write to `b` does nothing while `useA()` is true.

**Errors.**
- **A throwing computed:** its readers get the error, until a source changes.
- **A throwing effect or binding:** the error goes to the nearest `<ErrorBoundary>`. Without
  one, it is reported (stderr and the hook API, with the component stack: `in a binding of Card
  < List < mount`). The effect stays subscribed, and the rest of the app keeps running.
- **A throwing cleanup:** reported the same way; the other cleanups still run.

## Components

```tsx
function Badge(props: { label: string; count: () => number }) {
  return (
    <View style={badge}>
      <Text>{props.label}: {props.count()}</Text>
    </View>
  );
}

<Badge label="Inbox" count={unread} />;
```

- **A component is a function from props to JSX.** It runs once, under an owner of its own,
  where it is mounted.
- **Elements are descriptions.** They create nodes only where they are inserted. So children
  passed to a component mount where that component puts them, and they see its context.
  Mounting the same element twice makes two copies.
- **What a component may return:** JSX, arrays of it, `null`, `undefined`, `false`, or a
  function returning any of these (dynamic content, mounted again whenever what it read
  changes).
- **No placeholder nodes.** Dynamic content keeps its place among its siblings by itself, so
  the scene is exactly what the same app written with `Node` calls builds. The TSX
  `layout_cards` draws byte-identical images to the C and TypeScript versions.

### Built-in elements

| Element | Props | Children |
| --- | --- | --- |
| `<View>` | `style`, `tokens`, `state`, `ref` | elements |
| `<Text>` | the same, plus paragraph options: `families`, `size`, `weight`, `slant`, `language`, `color`, `lineHeight`, `align`, `direction`, `maxLines`, `ellipsis` | text, `{expressions}` and `<Span>`s |
| `<Span>` | `families`, `size`, `weight`, `slant`, `language`, `color` | text and `{expressions}` |

- **`style`:** styles and inline styles ([Styles](#styles)).
- **`tokens`:** the node's own token declarations for its subtree (`accent.with("#f63")`).
- **`state`:** state bits the app sets (`State.selected`). From milestone 4, input sets hover,
  pressed and focus.
- **`ref`:** called with the node once it exists, for `measure()`. The node belongs to the UI:
  don't destroy or move it yourself.

**Text.** A `<Text>`'s children are its paragraph: all its parts become one `set_text`, whatever
their number. A `<Span>` styles a run of it. Spans don't nest.

```tsx
<Text size={15}>Hello, <Span weight={700}>{name()}</Span>!</Text>
```

**Compile errors from the leaf-text rule** (axe-tsx; the type checker also rejects text in a
View):

| Code | Error |
| --- | --- |
| `<View>hi</View>` | `text in a View must be inside <Text>: <Text>hi</Text>` |
| `<View>{"a string"}</View>` | the same |
| `<View><Span>x</Span></View>` | `a Span must be inside <Text>` |
| `<Text><View /></Text>` | `a Text holds only text and Spans: put other nodes beside it, in a View` |
| `<Text><Span>a <Span>b</Span></Span></Text>` | `Spans don't nest: close one before opening the next` |
| `<div />` | `<div> isn't an element: Axe has no lowercase elements; use View, Text or a component` |
| `<View colour="red" />` | `View has no prop `colour`` |

TypeScript gives every JSX expression one type, so a component that returns a `<View>` and is
placed in a `<Text>` is caught at run time, in debug builds.

## Control flow

| Element | Mounts |
| --- | --- |
| `<Show when={…} fallback={…} keyed>` | Its children while `when` is truthy, otherwise `fallback`. Without `keyed`, the branch mounts again only when truthiness flips; a function child, `{(value) => …}`, reads the current value through its accessor. With `keyed`, each new truthy value mounts it again. |
| `<Switch fallback={…}><Match when={…}>…</Match>…</Switch>` | The first `Match` whose `when` is truthy. A branch mounts again only when another `Match` is selected. |
| `<For each={…} key={…} fallback={…}>{(item, index) => …}</For>` | A row per item, keyed by the item, or by `key(item)`. |
| `<Index each={…}>{(item, i) => …}</Index>` | A row per position. When the list changes, each row's `item()` accessor changes; rows are added or removed at the end, and none moves. |
| `<Dynamic component={…} {...props} />` | `component` with the other props, mounted again when `component` changes. |
| `<ErrorBoundary fallback={(error, reset) => …}>` | Its children; after one of them throws, `fallback`. An error inside the fallback goes to the next boundary up. |

**`<For>` keeps rows and moves the fewest.** When the list changes:
- rows of items that stay are kept, with their state;
- rows of new items are mounted, and rows of items that went are disposed;
- the rows kept in place are a longest increasing subsequence of their old positions; every
  other kept row moves once, with `move_node`, so layout animations play. Moving the last item
  to the front is one move;
- rows added at the end, the first mount included, need no moves at all.

Use `<For>` when items have identity (a list of records). Use `<Index>` when position matters
more than identity (a fixed grid of cells).

## Context

```tsx
const Locale = context("en");

function Price(props: { amount: number }) {
  const locale = use(Locale); // read once, when Price starts
  return <Text>{format(props.amount, locale)}</Text>;
}

<Locale.Provider value="ro">
  <Price amount={3} />
</Locale.Provider>;
```

`use(ctx)` returns the nearest provider's value, or the default. It is read once, where it is
called. For a value that changes, put a signal in the context.

## Styles

`style()` defines an interned style: applying it to a node costs one op.

```ts
const card = style({
  width: 208,                    // numbers are logical px
  maxWidth: "50%",               // percentages are strings
  padding: [14, 14, 12, 14],     // padding, margin and inset take 1, 2 or 4 values
  gap: 8,                        // both gaps
  flexDirection: "column",       // keywords by name
  backgroundColor: "#fff",       // #rgb, #rrggbb, #rrggbbaa, rgb(r, g, b), rgba(r, g, b, a)
  borderStartColor: accent,      // tokens directly
  transitions: { backgroundColor: spring(90, 19), layout: spring(170, 24) },
  hover: { backgroundColor: "#eef3ff", tokens: [accent.with("#f63")] },
  states: [{ when: [State.hover, State.pressed], opacity: 0.9 }],
});
```

**Values.**
- **Properties** are the core's ([`styles.md`](styles.md)), in camelCase.
- **A number** is logical px, or a plain number for properties that take one (`opacity`,
  `flexGrow`, `lineHeight`, `fontWeight`, `aspectRatio`, `maxLines`).
- **Strings** are colors, percentages (`"50%"`), `"auto"`, `"none"`, `"inherit"`, `"initial"`,
  keyword names in CSS's spelling (`"row-reverse"`, `"space-between"`), or the string itself
  for `fontFamily` and `language`.
- **Value objects** (`px(4)`, `rgba(0.2, 0.4, 1)`) work too.

**Typed.** These are compile errors:
- a hex color of the wrong form (`"#12345"`, `"#ggg"`): `invalid color "#ggg": use #rgb, #rrggbb
  or #rrggbbaa`;
- a unit string (`width: "12px"`);
- a color where a length goes;
- an unknown keyword or property;
- a token of another kind;
- a transition of a property that has none.

What the types can't see (`rgb(300, 0, 0)`, `opacity: 2`) throws a `RangeError` naming the
property.

**State blocks.** `hover`, `pressed`, `focused`, `disabled` and `selected` are blocks for one
state. `states` takes blocks for combinations. The core resolves them, with no JS involved
([`styles.md`](styles.md), "States").

**On a node.** `style` takes a style, an inline object, or a list of them. Falsy entries are
skipped and later entries win.

```tsx
<View style={[card, selected() && cardSelected, { width: width() }]} />
```

- **Styles** (`style()`) apply in list order.
- **Inline objects**, constant or not, are the node's **overrides**: they win over every
  style, state blocks included, as inline styles beat state rules in CSS. A constant one goes
  into the template's `set_props`; a dynamic one writes only the overrides that changed, and
  unsets one that disappears.

**Tokens.** `token(value)` makes a theme token from sugar: `token(4)` is a length token,
`token("#36f")` a color token. `Token.local()` makes a token with no global value.
`token.set(value)` changes it everywhere it is used, and `token.with(value)` declares it for a
subtree, in a style's `tokens` or a node's `tokens` prop ([`styles.md`](styles.md), "Token
scopes").

## Themes

```tsx
const ink = token("#111");
const dark = theme({
  tokens: [ink.with("#eee"), page.with("#121418")],
  components: {
    Text: style({ textColor: ink, fontFamily: "Noto Sans, sans-serif" }),
    View: style({ flexDirection: "column" }),
  },
});

<ThemeProvider theme={dark}>
  <App />
</ThemeProvider>;
```

`<ThemeProvider>` renders a `<View>` whose style declares the theme's tokens. It also gives
every `<View>` and `<Text>` below it the theme's default style for that element, first in its
list: a style the element sets itself comes later and wins.

## Mounting

`mount(App, surface)` mounts `App` on a surface, after the surface's other root-level nodes,
and returns `{ unmount() }`. Unmounting stops the app's effects, runs its cleanups and destroys
its nodes.

## How TSX compiles

`axe-tsx` (`tools/axe-tsx`, a Rust tool on oxc) runs before esbuild. It keeps your source as it
is outside JSX, types included, and replaces each JSX expression.

**What an expression is.**

| Class | What | How it is used |
| --- | --- | --- |
| Constant | Literals, module-level bindings and their static members (`card`, `S.card`, `"#fff"`, `[card, { gap: 4 }]`), spreads of them | Goes into the template, the same for every instance |
| Local | No call and no member access, but a local binding (`style={mine}`, `{n}` in a `For` row) | Applied once per instance |
| Dynamic | A call, a member access off a local (`props.title`), a tagged template | A binding (or a getter, as a component prop) |

Functions are values: what they read doesn't matter here. `/*@once*/` isn't needed: a value
read once is simply not a call.

**`<View>` and `<Text>`** become nodes of a template:
- **Constant props go into the template:** styles, inline styles (as `set_props`), token
  declarations, states, and a Text's paragraph when every part is constant.
- **The rest attaches to the instance's nodes:** a binding per dynamic prop, one call per local
  one, one binding per dynamic paragraph. A paragraph with no options and no Spans gets a lean
  binding that writes the text only when it changes.
- **Dynamic children** of a `<View>` (components, expressions, control flow) go in regions,
  each placed before the next static child or region.

**Components** become `createComponent(Tag, props)`. Dynamic props become getters, children a
getter, and spreads `mergeProps`.

For example:

```tsx
const card = style({ padding: 8 });

function Card(props: { title: string; count: () => number }) {
  return (
    <View style={card}>
      <Text>{props.title}</Text>
      <Text>Count: {props.count()}</Text>
    </View>
  );
}
```

compiles to:

```ts
const _tmpl$1 = /*#__PURE__*/ _$template([{ p: -1, props: () => ({ style: card }) }, { p: 0, t: 1 }, { p: 0, t: 1 }]);

function Card(props) {
  return _$instance(_tmpl$1, (_n) => {
    _$bindPlainText(_n[1], () => props.title);
    _$bindPlainText(_n[2], () => ["Count: ", props.count()]);
  });
}
```

Mounting a `Card` is then one `instantiate_template` op for its three nodes, and two `set_text`
ops.

**Speed.** About 0.3 ms per file: parse 0.1 ms, scopes 0.04 ms, transform 0.14 ms for a 30-line
file. `axe-tsx --timings` reports each file.

**Source maps.** Each file carries an inline map, and esbuild composes it into the bundle's.

## Building

`axe_ts_app(name ENTRY main.tsx ...)` builds a TSX app.
- **The mirror:** when the app's directory holds TSX, `axe-tsx` compiles it into a mirror in
  the build tree, which esbuild bundles.
- **Imports leaving the app's directory** go through package names (`@axe/ui`), not relative
  paths.
- **Type-checking** uses `"jsx": "preserve"` and `"jsxImportSource": "@axe/ui"`, so `tsc` checks
  TSX against `@axe/ui/jsx-runtime`'s JSX types.

## How it works

- **Ownership.** Every computation and component has an owner. Disposing an owner:
  1. disposes its children, newest first;
  2. runs its cleanups in reverse order;
  3. unsubscribes its computations;
  4. destroys the nodes it put in the scene, one `destroy_node` per top-level node.
- **Regions.** Dynamic content lives in a region: a run of a parent's children. To insert, a
  region finds the node that follows it, so siblings keep their order with no placeholders.
- **The graph.** It is push-pull. A write marks its observers and queues effects. A computed
  re-checks its sources, in order, when read, and runs only if one of them actually changed.
  Each dependency link remembers the version of the source it saw, and links are reused across
  runs, so a stable run allocates nothing.
- **Kept small for QuickJS,** which pays for every object (about 140 bytes empty, 200 with a
  few fields), every captured variable and every property write (about 0.1 us):
  - a binding is one object running a shared function;
  - a `<For>` row is its own owner, and a row that is one component runs it in place;
  - a region holding one node keeps it in a field;
  - a signal is its own getter.

## Benchmarks

The language benchmarks of milestone 3b, with thresholds agreed before they ran.
- **The apps** are frozen TSX apps under `bench/apps`.
- **The CPU part** is `bench_ui`: release builds, one machine, median of several runs while a
  browser was busy.
- **The live part** is `bench_ui_live`: `list10k` streaming in a headless sway at a fixed
  refresh rate.

| | Measure | Result | Threshold |
| --- | --- | --- | --- |
| B1 | `screen1k` (about 1,000 nodes) from bytecode: JS for host, evaluation, mount and first flush | 7.7 ms | ≤ 25 ms ✓ |
| B1 | First frame, over an empty app's | +23 ms | ≤ +60 ms ✓ |
| B2 | `list10k`: mount 10,000 rows through `For` (JS) | about 450 ms | ≤ 300 ms ✗ |
| B2 | JS heap per row after mounting | 3.3 KB | ≤ 2 KB ✗ |
| B2 | A turn writing 100 items' signals | p50 1.5 ms, p99 2.1 ms | ≤ 2, ≤ 4 ms ✓ |
| B2 | Update delay, a turn's writes to the screen | p99 1.75 frames | ≤ 2 frames ✓ |
| B2 | Missed refreshes, 10,000 rows streaming | about 24 fps whatever the refresh rate | 0 at 60 Hz, ≤ 0.5% at 144 Hz ✗ |
| B2 | The same with 1,000 rows | 0 at 60 Hz; about 26% at 144 Hz | |
| B3 | 1,000 bound texts updated in one turn | 7.2 ms | ≤ 8 ms ✓ |

**What fails, and why.**
- **Mount and memory.** These are the cost of creating 10,000 rows' worth of JS objects in an
  interpreter. Each row is a row owner, a region, two bindings and their dependency links,
  plus the app's own item and two signals (about 1 KB of the 3.3). The mount profile: the
  interpreter's loop 44%, creating properties 15%, garbage collection 9%.
- **Missed refreshes.** These are the core's, not JS's: frames never wait for JS. With 30,000
  nodes, the core rebuilds the display list (about 18 ms) and the renderer encodes it (about
  20 ms) every frame. With 1,000 rows, those take about 4.7 ms a frame, near 144 Hz's 6.9 ms
  budget. Culling, an incremental display list and layer caching come in M3c, the core's
  frame cost ([`architecture.md`](architecture.md), "Roadmap").
- **The update path itself passes**: the turn with 100 writes, 1,000 text updates in a turn,
  and the update delay.

So, by the architecture's rule ("Language bindings"), TypeScript on QuickJS-ng stays. The
failures become [tracked targets](#tracked-targets) for later milestones.

Run them:

```sh
build/release/bench/bench_ui                       # B1, B2's CPU part, B3
build/release/bench/bench_ui --heap BUNDLE.axbc... # the JS heap of any app after its first turn
WAYLAND_DISPLAY=<headless sway> build/release/bench/bench_ui_live [--rows=N] [--seconds S]
```

`bench-callgrind` counts the CPU cases on every CI run: `ui_startup_1k`, `ui_for_mount_10k`,
`ui_for_update`, `ui_for_reorder` and `ui_text_1k`.

### Tracked targets

The failed thresholds aren't relaxed. Each is a target a later milestone must meet, with the
same benchmark:

| Measure | Current result | Threshold | Expected in |
| --- | --- | --- | --- |
| B2 mount, 10,000 rows (`list10k`, JS) | about 450 ms | ≤ 300 ms | M4: virtualized lists and lighter row objects |
| B2 JS heap per row after mounting | 3.3 KB | ≤ 2 KB | M4: lighter row objects, and a store that creates per-item signals lazily, only for values that are read |
| B2 missed refreshes, 10,000 rows streaming | about 24 fps whatever the refresh rate | 0 missed at 60 Hz, ≤ 0.5% at 144 Hz | M3c: the core's frame cost |
| B2 missed refreshes, 1,000 rows at 144 Hz | about 26% missed | ≤ 0.5% | M3c: the core's frame cost |
