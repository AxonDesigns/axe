// TSX through axe-tsx: a template is defined once and instantiated with one op; bindings
// patch single properties; dynamic children keep their place among a template's nodes; For in
// TSX; theme variants are defined per theme and released with their provider; spread props.

import { For, Node, Show, Text, ThemeProvider, View, mount, signal, style, theme, token, type Element } from "@axe/ui";
import { expect, test } from "@axe/testing";
import { slotChildren } from "@axe/core/internal";
import { capture, captureFlush, only, surface } from "./ops.js";

const card = style({ padding: 8, backgroundColor: "#fff" });
const label = style({ textColor: "#333" });

function Card(props: { title: string; count: () => number }): Element {
  return (
    <View style={card}>
      <Text style={label}>{props.title}</Text>
      <Text>Count: {props.count()}</Text>
    </View>
  );
}

// Mounts under a root View and returns its slot, the ops written, and the unmount.
function mountIn(app: () => Element) {
  let root = -1;
  let m: { unmount(): void } | null = null;
  const ops = capture(() => {
    m = mount(() => <View ref={(n: Node) => (root = n.handle.index)}>{app()}</View>, surface());
  });
  return { root, ops, unmount: () => m!.unmount() };
}

const names = new Map<number, string>();
function Named(props: { name: string }): Element {
  return <View ref={(n: Node) => names.set(n.handle.index, props.name)} />;
}
const order = (slot: number) => slotChildren(slot).map((s) => names.get(s) ?? "?");

test("a template is defined once and instantiated with one op", () => {
  const count = signal(1);
  const a = mountIn(() => <Card title="A" count={count} />);
  expect(only(a.ops, "defineTemplate").length).toBe(2); // the root View's, and Card's
  expect(only(a.ops, "instantiateTemplate").length).toBe(2);
  expect(only(a.ops, "createNode").length).toBe(0);
  // Card's dynamic texts: one set_text each, after the instance.
  expect(only(a.ops, "setText").map((o) => o.text)).toEqual(["A", "Count: 1"]);
  const b = mountIn(() => <Card title="B" count={count} />);
  expect(only(b.ops, "defineTemplate").length).toBe(0); // already defined
  expect(only(b.ops, "instantiateTemplate").length).toBe(2);
  // A binding patches only its paragraph.
  const ops = captureFlush(() => count.set(2));
  expect(ops.map((o) => o.op)).toEqual(["setText", "setText"]);
  expect(ops.map((o) => o.text)).toEqual(["Count: 2", "Count: 2"]);
  a.unmount();
  b.unmount();
});

test("a template's nodes are the tree TSX describes", () => {
  const m = mountIn(() => (
    <View>
      <Named name="a" />
      <View>
        <Named name="inner" />
      </View>
      <Text>static</Text>
    </View>
  ));
  const outer = slotChildren(m.root)[0]!;
  const kids = slotChildren(outer);
  expect(kids.length).toBe(3);
  expect(names.get(kids[0]!)).toBe("a");
  expect(order(kids[1]!)).toEqual(["inner"]);
  m.unmount();
});

test("dynamic children keep their place among static ones", () => {
  const show = signal(false);
  const m = mountIn(() => (
    <View>
      <Named name="first" />
      {show() && <Named name="maybe" />}
      <Text>between</Text>
      <Show when={show()}>
        <Named name="shown" />
      </Show>
      <Named name="last" />
    </View>
  ));
  const outer = slotChildren(m.root)[0]!;
  expect(order(outer)).toEqual(["first", "?", "last"]);
  captureFlush(() => show.set(true));
  expect(order(outer)).toEqual(["first", "maybe", "?", "shown", "last"]);
  captureFlush(() => show.set(false));
  expect(order(outer)).toEqual(["first", "?", "last"]);
  m.unmount();
});

test("For in TSX keeps rows and moves the fewest", () => {
  const list = signal(["a", "b", "c", "d"]);
  const m = mountIn(() => (
    <View>
      <For each={list()}>{(s: string) => <Named name={s} />}</For>
    </View>
  ));
  const outer = slotChildren(m.root)[0]!;
  expect(order(outer)).toEqual(["a", "b", "c", "d"]);
  const ops = captureFlush(() => list.set(["d", "a", "b", "c"]));
  expect(order(outer)).toEqual(["d", "a", "b", "c"]);
  expect(only(ops, "moveNode").length).toBe(1);
  expect(only(ops, "instantiateTemplate", "createNode", "destroyNode").length).toBe(0);
  m.unmount();
});

test("a theme's variant is defined under its provider and released with it", () => {
  const ink = token("#111");
  const dark = theme({ tokens: [ink.with("#eee")], components: { Text: style({ textColor: ink }) } });
  const on = signal(true);
  const m = mountIn(() => (
    <Show when={on()}>
      <ThemeProvider theme={dark}>
        <Card title="themed" count={() => 0} />
      </ThemeProvider>
    </Show>
  ));
  // Card's template again, in the theme's variant (its texts get the theme's default style).
  expect(only(m.ops, "defineTemplate").length).toBeTruthy();
  const ops = captureFlush(() => on.set(false));
  expect(only(ops, "destroyTemplate").length).toBe(1);
  m.unmount();
});

test("spread props go through the component path", () => {
  const extra = { style: card };
  const m = mountIn(() => <View {...extra}><Named name="x" /></View>);
  const outer = slotChildren(m.root)[0]!;
  expect(order(outer)).toEqual(["x"]);
  expect(only(m.ops, "setStyles").some((o) => o.ids?.[0] === card.id)).toBe(true);
  m.unmount();
});
