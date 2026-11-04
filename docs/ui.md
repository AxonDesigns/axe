# Writing apps with @axe/ui

`@axe/ui` is how Axe apps are written: fine-grained signals over the retained scene of
[`@axe/core`](typescript.md). Components run once. Only the code that reads a signal runs again
when the signal changes, and it changes exactly the properties that depend on it: there is no
virtual DOM and no diffing.

This guide is for people and for AI tools writing apps. Every rule is stated outright, and
each one that the type checker or the runtime enforces says what the error looks like.

> **Status (milestone 3b.1).** Everything below works today through plain function calls:
> `View({ style, children })`, with getters for props that change. Milestone 3b.2 adds TSX,
> which compiles `<View style={card}>...</View>` to these same calls and turns static subtrees
> into templates. The semantics don't change.

```ts
import { For, Text, View, mount, signal, style, app } from "@axe/ui";

const card = style({ padding: 12, gap: 6, backgroundColor: "#fff", cornerRadius: 8 });
const items = signal(["Apples", "Pears"]);
const count = signal(0);

function App() {
  return View({
    style: card,
    children: [
      Text({ get children() { return ["Clicked ", count(), " times"]; } }),
      For({ get each() { return items(); }, children: (item: string) => Text({ children: item }) }),
    ],
  });
}

app.onSurface((surface) => mount(App, surface));
setInterval(() => count.update((n) => n + 1), 1000);
```

## Rules at a glance

1. **Components run once.** Code in a component body runs when it mounts, never again. What
   must follow a signal goes in a binding (a prop or child that reads the signal), a
   `computed` or an `effect`.
2. **Read a signal where it is used.** `count()` inside a getter, a child function, a computed
   or an effect subscribes that code. `count()` in a component's body reads it once.
3. **Props that change are getters.** Read `props.title` where you need it. Don't destructure
   props (`function Card({ title })`): that reads every prop once, when the component starts.
   Use `mergeProps` for defaults and `splitProps` to forward a subset.
4. **Text goes only in `Text`.** A string or a number in a `View` is an error that points to
   `Text`. A `Span` goes only in a `Text`, and a `Text` holds no nodes.
5. **Effects run at the end of the turn.** A write never runs anything right away. Bindings and
   effects run once at the end of the turn, however many writes happened, so a turn writes one
   atomic command buffer.
6. **Owners dispose what they create.** When a component, a branch of `Show` or a row of `For`
   goes away, its effects stop, its cleanups run (newest first) and its nodes are destroyed.

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
- **A throwing effect or binding:** the error goes to the nearest `ErrorBoundary`. Without one,
  it is reported (stderr and the hook API, with the component stack: `in a binding of Card <
  List < mount`). The effect stays subscribed, and the rest of the app keeps running.
- **A throwing cleanup:** reported the same way; the other cleanups still run.

## Components

```ts
function Badge(props: { label: string; count: () => number }) {
  return View({
    style: badge,
    children: Text({ get children() { return [props.label, ": ", props.count()]; } }),
  });
}

createComponent(Badge, { label: "Inbox", count: unread });
```

- **A component is a function from props to elements.** It runs once, under an owner of its
  own, when its element is mounted. TSX's `<Badge ... />` compiles to `createComponent`.
- **Elements are descriptions.** They create nodes only where they are inserted. So children
  passed to a component mount where that component puts them, and they see its context.
  Mounting the same element twice makes two copies.
- **What a component may return:** elements, arrays of them, `null`, `undefined`, `false`,
  or a function returning any of these (dynamic content, mounted again whenever what it read
  changes).
- **No placeholder nodes.** Dynamic content keeps its place among its siblings by itself, so
  the scene is exactly what the same app written with `Node` calls builds.

### Built-in elements

| Element | Props | Children |
| --- | --- | --- |
| `View` | `style`, `tokens`, `state`, `ref` | elements |
| `Text` | the same, plus paragraph options: `families`, `size`, `weight`, `slant`, `language`, `color`, `lineHeight`, `align`, `direction`, `maxLines`, `ellipsis` | strings, numbers, `Span`s, and functions giving them |
| `Span` | `families`, `size`, `weight`, `slant`, `language`, `color` | strings, numbers, and functions giving them |

- **`style`:** styles and inline styles ([Styles](#styles)).
- **`tokens`:** the node's own token declarations for its subtree (`accent.with("#f63")`).
- **`state`:** state bits the app sets (`State.selected`). From milestone 4, input sets hover,
  pressed and focus.
- **`ref`:** called with the node once it exists, for `measure()`. The node belongs to the UI:
  don't destroy or move it yourself.

**Text.** A `Text`'s children are its paragraph: all its parts become one `set_text`, whatever
their number. A `Span` styles a run of it. Spans don't nest.

```ts
Text({ size: 15, get children() { return ["Hello, ", Span({ weight: 700, children: name() }), "!"]; } });
```

These are errors, from the type checker and at run time alike:

| Code | Error |
| --- | --- |
| `View({ children: "hi" })` | `text in a View must be inside <Text>: <Text>hi</Text>` |
| `View({ children: Span(...) })` | `a Span must be inside <Text>` |
| `Text({ children: View(...) })` | `a Text holds only text and Spans: put other nodes beside it, in a View` |

## Control flow

| Element | Mounts |
| --- | --- |
| `Show({ when, fallback, keyed, children })` | Its children while `when` is truthy, otherwise `fallback`. Without `keyed`, the branch mounts again only when truthiness flips, and a children function reads the current value through its accessor. With `keyed`, each new truthy value mounts it again. |
| `Switch({ fallback, children: [Match({ when, children }), ...] })` | The first `Match` whose `when` is truthy. A branch mounts again only when another `Match` is selected. |
| `For({ each, key, fallback, children: (item, index) => ... })` | A row per item, keyed by the item, or by `key(item)`. |
| `Index({ each, fallback, children: (item, i) => ... })` | A row per position. When the list changes, each row's `item()` accessor changes; rows are added or removed at the end, and none moves. |
| `Dynamic({ component, ...props })` | `component` with the other props, mounted again when `component` changes. |
| `ErrorBoundary({ fallback, children })` | Its children; after one of them throws, `fallback` (elements, or `(error, reset) => elements`). An error inside the fallback goes to the next boundary up. |

**`For` keeps rows and moves the fewest.** When the list changes:
- rows of items that stay are kept, with their state;
- rows of new items are mounted, and rows of items that went are disposed;
- the rows kept in place are a longest increasing subsequence of their old positions; every
  other kept row moves once, with `move_node`, so layout animations play. Moving the last item
  to the front is one move.

Use `For` when items have identity (a list of records). Use `Index` when position matters more
than identity (a fixed grid of cells).

## Context

```ts
const Locale = context("en");

function Price(props: { amount: number }) {
  const locale = use(Locale); // read once, when Price starts
  return Text({ children: format(props.amount, locale) });
}

createComponent(Locale.Provider, { value: "ro", children: createComponent(Price, { amount: 3 }) });
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
property. From milestone 3b.2, the transform reports it at compile time for constants.

**State blocks.** `hover`, `pressed`, `focused`, `disabled` and `selected` are blocks for one
state. `states` takes blocks for combinations. The core resolves them, with no JS involved
([`styles.md`](styles.md), "States").

**On a node.** `style` takes a style, an inline object, or a list of them. Falsy entries are
skipped and later entries win.

```ts
View({ get style() { return [card, selected() && cardSelected, { width: width() }]; } });
```

- **Styles** (`style()`) apply in list order.
- **Inline objects**, constant or not, are the node's **overrides**: they win over every
  style, state blocks included, as inline styles beat state rules in CSS. Only overrides that
  changed are written; one that disappears is unset.

**Tokens.** `token(value)` makes a theme token from sugar: `token(4)` is a length token,
`token("#36f")` a color token. `Token.local()` makes a token with no global value.
`token.set(value)` changes it everywhere it is used, and `token.with(value)` declares it for a
subtree, in a style's `tokens` or a node's `tokens` prop ([`styles.md`](styles.md), "Token
scopes").

## Themes

```ts
const ink = token("#111");
const dark = theme({
  tokens: [ink.with("#eee"), page.with("#121418")],
  components: {
    Text: style({ textColor: ink, fontFamily: "Noto Sans, sans-serif" }),
    View: style({ flexDirection: "column" }),
  },
});

createComponent(ThemeProvider, { theme: dark, children: createComponent(App, {}) });
```

`ThemeProvider` renders a `View` whose style declares the theme's tokens. It also gives every
`View` and `Text` below it the theme's default style for that element, first in its list: a
style the element sets itself comes later and wins.

## Mounting

`mount(App, surface)` mounts `App` on a surface, after the surface's other root-level nodes,
and returns `{ unmount() }`. Unmounting stops the app's effects, runs its cleanups and destroys
its nodes.

## How it works

- **Ownership.** Every computation and component has an owner. Disposing an owner:
  1. disposes its children, newest first;
  2. runs its cleanups in reverse order;
  3. unsubscribes its computations;
  4. destroys the nodes it put in the scene, one `destroy_node` per top-level node.
- **Regions.** Dynamic content lives in a region: a run of a parent's children. To insert, a
  region finds the node that follows it, so siblings keep their order with no placeholders.
- **Bindings.** A prop or child that reads signals becomes a binding. A binding that read no
  signal on its first run is dropped, so static content keeps nothing alive.
- **The graph.** It is push-pull. A write marks its observers and queues effects. A computed
  re-checks its sources, in order, when read, and runs only if one of them actually changed.
  Each dependency link remembers the version of the source it saw, and links are reused across
  runs, so a stable run allocates nothing.
