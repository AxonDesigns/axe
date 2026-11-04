// @axe/ui's components and control flow: components run once, bindings patch single
// properties, regions keep order without placeholder nodes, For moves the fewest rows, context,
// error boundaries, mount and unmount, text and spans, and the same ops as imperative code.

import {
  Dynamic,
  ErrorBoundary,
  For,
  Index,
  Match,
  Node,
  Show,
  Span,
  State,
  Style,
  Switch,
  Text,
  View,
  context,
  createComponent,
  mergeProps,
  mount,
  onCleanup,
  signal,
  splitProps,
  style,
  use,
  type Element,
} from "@axe/ui";
import { Hook, expect, hooks, test } from "@axe/testing";
import { slotAlive, slotChildren } from "@axe/core/internal";
import { longestIncreasing } from "../src/flow.js";
import { flush } from "../src/reactive.js";
import { capture, captureFlush, only, surface } from "./ops.js";

const exceptions = () => hooks().filter((h) => h.kind === Hook.exception);

/** Mounts `app` under a root View and returns that View's slot and the unmount. */
function mountIn(app: () => Element): { root: number; unmount: () => void; ops: ReturnType<typeof capture> } {
  let rootSlot = -1;
  let m: { unmount(): void } | null = null;
  texts.clear();
  const ops = capture(() => {
    m = mount(() => View({ ref: (n: Node) => (rootSlot = n.handle.index), get children() { return app(); } }), surface());
  });
  return { root: rootSlot, unmount: () => m!.unmount(), ops };
}

// A View carrying a label, to tell nodes apart through the mirror: its text is its name.
const texts = new Map<number, string>();
function Item(props: { name: string }): Element {
  return View({
    ref: (n: Node) => texts.set(n.handle.index, props.name),
  });
}
const names = (slot: number) => slotChildren(slot).map((s) => texts.get(s) ?? "?");

test("a component runs once; a binding patches only its property", () => {
  const n = signal(1);
  let runs = 0;
  function Counter(): Element {
    runs++;
    return Text({ get children() { return ["count ", () => n()]; } });
  }
  const m = mountIn(() => createComponent(Counter, {}));
  expect(only(m.ops, "setText").map((o) => o.text)).toEqual(["count 1"]);
  const ops = captureFlush(() => n.set(2));
  expect(ops.map((o) => o.op)).toEqual(["setText"]);
  expect(ops[0]!.text).toBe("count 2");
  expect(runs).toBe(1);
  m.unmount();
});

test("static content leaves no bindings behind; dynamic props are getters", () => {
  const color = signal<"#fff" | "#000">("#fff");
  let ops = capture(() => void 0);
  const m = mountIn(() =>
    View({
      get style() {
        return { backgroundColor: color() };
      },
      children: View({ style: { width: 10 } }),
    }),
  );
  ops = captureFlush(() => color.set("#000"));
  expect(ops.map((o) => o.op)).toEqual(["setProps"]);
  expect(ops[0]!.entries).toBe(1);
  m.unmount();
});

test("children keep their order around dynamic regions, with no placeholder nodes", () => {
  const show = signal(false);
  const m = mountIn(() => [
    Item({ name: "a" }),
    () => (show() ? [Item({ name: "b1" }), Item({ name: "b2" })] : null),
    () => null, // an empty region next to another
    Item({ name: "c" }),
  ]);
  expect(names(m.root)).toEqual(["a", "c"]);
  const ops = captureFlush(() => show.set(true));
  expect(names(m.root)).toEqual(["a", "b1", "b2", "c"]);
  // Created before c, directly: create_node then move_node into place.
  expect(only(ops, "createNode").length).toBe(2);
  captureFlush(() => show.set(false));
  expect(names(m.root)).toEqual(["a", "c"]);
  m.unmount();
});

test("Show mounts its branch only when truthiness flips; the branch reads the value", () => {
  const user = signal<{ name: string } | null>(null);
  let branchRuns = 0;
  function Greeting(props: { name: () => string }): Element {
    branchRuns++;
    return Text({ get children() { return ["hi ", props.name()]; } });
  }
  const m = mountIn(() =>
    createComponent(Show<{ name: string } | null>, {
      get when() { return user(); },
      fallback: Text({ children: "nobody" }),
      children: (u: () => { name: string }) => createComponent(Greeting, { name: () => u().name }),
    }),
  );
  let ops = captureFlush(() => user.set({ name: "ana" }));
  expect(only(ops, "setText").map((o) => o.text)).toEqual(["hi ana"]);
  expect(only(ops, "destroyNode").length).toBe(1); // the fallback
  ops = captureFlush(() => user.set({ name: "bo" }));
  expect(ops.map((o) => o.op)).toEqual(["setText"]); // same branch, its binding only
  expect(branchRuns).toBe(1);
  ops = captureFlush(() => user.set(null));
  expect(only(ops, "setText").map((o) => o.text)).toEqual(["nobody"]);
  m.unmount();
});

test("For keeps rows by key and moves the fewest", () => {
  const list = signal(["a", "b", "c", "d", "e"]);
  let created = 0;
  const m = mountIn(() =>
    createComponent(For<string>, {
      get each() { return list(); },
      children: (s: string) => {
        created++;
        return Item({ name: s });
      },
    }),
  );
  expect(names(m.root)).toEqual(["a", "b", "c", "d", "e"]);
  // Move e to the front: one move (a..d stay).
  let ops = captureFlush(() => list.set(["e", "a", "b", "c", "d"]));
  expect(names(m.root)).toEqual(["e", "a", "b", "c", "d"]);
  expect(only(ops, "moveNode").length).toBe(1);
  expect(only(ops, "createNode", "destroyNode").length).toBe(0);
  // Reverse: n - 1 moves.
  ops = captureFlush(() => list.set(["d", "c", "b", "a", "e"]));
  expect(names(m.root)).toEqual(["d", "c", "b", "a", "e"]);
  expect(only(ops, "moveNode").length).toBe(4); // old positions 4 3 2 1 0: one row stays
  // Insert, remove and move at once.
  ops = captureFlush(() => list.set(["x", "c", "a", "e", "y"]));
  expect(names(m.root)).toEqual(["x", "c", "a", "e", "y"]);
  expect(only(ops, "destroyNode").length).toBe(2); // d, b
  expect(only(ops, "createNode").length).toBe(2); // x, y
  expect(created).toBe(7);
  m.unmount();
});

test("the longest increasing subsequence", () => {
  expect([...longestIncreasing([4, 0, 1, 2, 3])].sort()).toEqual([1, 2, 3, 4]);
  expect([...longestIncreasing([3, 2, 1, 0])].length).toBe(1);
  expect([...longestIncreasing([-1, 0, -1, 1])].sort()).toEqual([1, 3]);
  expect([...longestIncreasing([])].length).toBe(0);
});

test("For rows keep their state and get index accessors; fallback when empty", () => {
  const list = signal([{ id: 1 }, { id: 2 }]);
  const indices = new Map<number, () => number>();
  const m = mountIn(() =>
    createComponent(For<{ id: number }>, {
      get each() { return list(); },
      key: (x: { id: number }) => x.id,
      fallback: Item({ name: "empty" }),
      children: (x: { id: number }, i: () => number) => {
        indices.set(x.id, i);
        return Item({ name: `row${x.id}` });
      },
    }),
  );
  captureFlush(() => list.set([{ id: 2 }, { id: 1 }])); // new objects, same keys
  expect(names(m.root)).toEqual(["row2", "row1"]);
  expect(indices.get(1)!()).toBe(1);
  expect(indices.get(2)!()).toBe(0);
  captureFlush(() => list.set([]));
  expect(names(m.root)).toEqual(["empty"]);
  captureFlush(() => list.set([{ id: 3 }]));
  expect(names(m.root)).toEqual(["row3"]);
  m.unmount();
});

test("Index updates items in place and adds or removes at the end", () => {
  const list = signal(["a", "b"]);
  const m = mountIn(() =>
    createComponent(Index<string>, {
      get each() { return list(); },
      children: (item: () => string) => Text({ get children() { return item(); } }),
    }),
  );
  let ops = captureFlush(() => list.set(["b", "a", "c"]));
  // The new row's binding runs as it mounts; the others' at the flush.
  expect(only(ops, "setText").map((o) => o.text).sort()).toEqual(["a", "b", "c"]);
  expect(only(ops, "moveNode").length).toBe(0);
  ops = captureFlush(() => list.set(["b"]));
  expect(only(ops, "destroyNode").length).toBe(2);
  m.unmount();
});

test("Switch mounts the first matching case, again only when another is selected", () => {
  const n = signal(1);
  const m = mountIn(() =>
    createComponent(Switch, {
      fallback: Item({ name: "other" }),
      get children() {
        return [
          createComponent(Match<boolean>, { get when() { return n() < 10; }, children: Item({ name: "small" }) }),
          createComponent(Match<boolean>, { get when() { return n() < 100; }, children: Item({ name: "medium" }) }),
        ];
      },
    }),
  );
  expect(names(m.root)).toEqual(["small"]);
  let ops = captureFlush(() => n.set(5));
  expect(ops.length).toBe(0);
  captureFlush(() => n.set(50));
  expect(names(m.root)).toEqual(["medium"]);
  ops = captureFlush(() => n.set(500));
  expect(names(m.root)).toEqual(["other"]);
  m.unmount();
});

test("Dynamic mounts the component it is given, with the other props", () => {
  const A = (p: { name: string }) => Item({ name: `A:${p.name}` });
  const B = (p: { name: string }) => Item({ name: `B:${p.name}` });
  const which = signal<typeof A | null>(A);
  const m = mountIn(() =>
    createComponent(Dynamic<{ name: string }>, {
      get component() { return which(); },
      name: "x",
    }),
  );
  expect(names(m.root)).toEqual(["A:x"]);
  captureFlush(() => which.set(B));
  expect(names(m.root)).toEqual(["B:x"]);
  captureFlush(() => which.set(null));
  expect(names(m.root)).toEqual([]);
  m.unmount();
});

test("context: the default, a provider, the nearest one, read once", () => {
  const Theme = context("light", "Theme");
  const seen: string[] = [];
  function Leaf(): Element {
    seen.push(use(Theme));
    return null;
  }
  const m = mountIn(() => [
    createComponent(Leaf, {}),
    createComponent(Theme.Provider, {
      value: "dark",
      get children() {
        return [createComponent(Leaf, {}), createComponent(Theme.Provider, { value: "dim", children: createComponent(Leaf, {}) })];
      },
    }),
  ]);
  expect(seen).toEqual(["light", "dark", "dim"]);
  m.unmount();
});

test("an error boundary takes a component's error, shows its fallback, and resets", () => {
  const fail = signal(true);
  let reset: (() => void) | null = null;
  function Fragile(): Element {
    if (fail()) throw new Error("broken");
    return Item({ name: "fine" });
  }
  const before = exceptions().length;
  const m = mountIn(() =>
    createComponent(ErrorBoundary, {
      fallback: (e: unknown, r: () => void) => {
        reset = r;
        return Item({ name: `fallback: ${(e as Error).message}` });
      },
      children: createComponent(Fragile, {}),
    }),
  );
  flush();
  expect(names(m.root)).toEqual(["fallback: broken"]);
  fail.set(false);
  captureFlush(() => reset!());
  expect(names(m.root)).toEqual(["fine"]);
  expect(exceptions().length).toBe(before); // taken, not reported
  m.unmount();
});

test("an error in a binding goes to the boundary; without one it is reported", () => {
  const bad = signal(false);
  const m = mountIn(() =>
    createComponent(ErrorBoundary, {
      fallback: Item({ name: "caught" }),
      children: Text({
        get children() {
          if (bad()) throw new Error("binding failed");
          return "ok";
        },
      }),
    }),
  );
  captureFlush(() => bad.set(true));
  expect(names(m.root)).toEqual(["caught"]);
  m.unmount();

  const before = exceptions().length;
  const bad2 = signal(false);
  function Named(): Element {
    return Text({
      get children() {
        if (bad2()) throw new Error("no boundary");
        return "ok";
      },
    });
  }
  const m2 = mountIn(() => [createComponent(Named, {}), Item({ name: "sibling" })]);
  captureFlush(() => bad2.set(true));
  const reported = exceptions().slice(before);
  expect(reported.length).toBe(1);
  expect(reported[0]!.text).toContain("no boundary");
  expect(reported[0]!.text).toContain("Named");
  expect(names(m2.root)).toEqual(["?", "sibling"]); // the rest keeps running
  m2.unmount();
});

test("a component that throws while starting mounts nothing; its siblings do", () => {
  const before = exceptions().length;
  function Broken(): Element {
    onCleanup(() => void 0);
    Item({ name: "never" }); // not mounted: elements mount where they are inserted
    throw new Error("setup failed");
  }
  const m = mountIn(() => [Item({ name: "a" }), createComponent(Broken, {}), Item({ name: "b" })]);
  expect(names(m.root)).toEqual(["a", "b"]);
  expect(exceptions().length).toBe(before + 1);
  m.unmount();
});

test("unmount destroys the nodes and runs the cleanups", () => {
  let cleaned = 0;
  function Leaf(): Element {
    onCleanup(() => cleaned++);
    return Item({ name: "leaf" });
  }
  const m = mountIn(() => [createComponent(Leaf, {}), View({ children: createComponent(Leaf, {}) })]);
  const rootSlot = m.root;
  const ops = capture(() => m.unmount());
  expect(cleaned).toBe(2);
  expect(only(ops, "destroyNode").length).toBe(1); // the root View takes its subtree
  expect(slotAlive(rootSlot)).toBe(false);
});

test("text: parts and spans become one set_text; text outside Text is an error", () => {
  const name = signal("Ana");
  const m = mountIn(() =>
    Text({
      size: 14,
      get children() {
        return ["Hello ", Span({ weight: 700, get children() { return name(); } }), "!"];
      },
    }),
  );
  const set = only(m.ops, "setText");
  expect(set.length).toBe(1);
  expect(set[0]!.text).toBe("Hello Ana!");
  expect(set[0]!.spans).toBe(1);
  const ops = captureFlush(() => name.set("Bo"));
  expect(ops.map((o) => o.text)).toEqual(["Hello Bo!"]);
  m.unmount();

  const before = exceptions().length;
  function Wrong(): Element {
    return View({ children: "loose text" as never });
  }
  const m2 = mountIn(() => createComponent(Wrong, {}));
  const reported = exceptions().slice(before);
  expect(reported.length).toBe(1);
  expect(reported[0]!.text).toContain("must be inside <Text>");
  m2.unmount();
});

test("mergeProps and splitProps keep getters", () => {
  const s = signal(1);
  const props = { get n() { return s(); }, label: "x" };
  const merged = mergeProps({ n: 0, size: 3 }, props);
  expect(merged.n).toBe(1);
  expect(merged.size).toBe(3);
  s.set(2);
  expect(merged.n).toBe(2);
  const [picked, rest] = splitProps(props, ["n"]);
  s.set(3);
  expect(picked.n).toBe(3);
  expect(rest.label).toBe("x");
});

test("the same ops as the imperative API", () => {
  const card = style({ width: 100, backgroundColor: "#fff" });
  const label = style({ textColor: "#333" });
  const m = mountIn(() =>
    View({
      style: card,
      state: State.selected,
      children: [Text({ style: label, size: 16, children: "Title" }), View({ style: [card, { height: 4 }] })],
    }),
  );
  const ui = m.ops.slice(1); // after the create_node of the test's root View
  m.unmount();
  const parent = Node.create(surface());
  const handMade = capture(() => {
    const v = Node.create(parent, { styles: [card] });
    v.setState(State.selected, State.selected);
    const t = Node.create(v, { styles: [label] });
    t.setText("Title", { size: 16 });
    const w = Node.create(v, { styles: [card] });
    w.setProps({ height: { kind: 2, number: 4 } });
  });
  parent.destroy();
  const shape = (ops: typeof ui) => ops.map((o) => [o.op, o.text ?? "", (o.ids ?? []).join(","), o.entries ?? 0, o.mask ?? 0].join("|"));
  expect(shape(ui)).toEqual(shape(handMade));
  void Style;
});

test("keyed Show mounts its branch again for each new truthy value", () => {
  const v = signal<string | null>("a");
  let runs = 0;
  const m = mountIn(() =>
    createComponent(Show<string | null>, {
      get when() { return v(); },
      keyed: true,
      children: (x: () => string) => {
        runs++;
        return Item({ name: x() });
      },
    }),
  );
  captureFlush(() => v.set("b"));
  expect(names(m.root)).toEqual(["b"]);
  expect(runs).toBe(2);
  m.unmount();
});

test("For pairs repeated keys in order", () => {
  const list = signal(["x", "y", "x"]);
  const m = mountIn(() =>
    createComponent(For<string>, {
      get each() { return list(); },
      children: (s: string, i: () => number) => Item({ name: `${s}${i()}` }),
    }),
  );
  expect(names(m.root)).toEqual(["x0", "y1", "x2"]);
  const ops = captureFlush(() => list.set(["x", "x", "y"]));
  expect(only(ops, "createNode", "destroyNode").length).toBe(0);
  expect(only(ops, "moveNode").length).toBe(1);
  m.unmount();
});

test("mounting twice on a surface appends; each unmount removes only its own", () => {
  const a = mountIn(() => Item({ name: "a" }));
  const b = mountIn(() => Item({ name: "b" }));
  a.unmount();
  expect(slotAlive(b.root)).toBe(true);
  b.unmount();
});
