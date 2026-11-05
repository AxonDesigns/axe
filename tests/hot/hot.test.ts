// Hot reload, headless and with no watcher (docs/ui.md, "Hot reload"): files are rebuilt by the
// tool axe-run's watcher uses, and each update applies at a turn's start. Each test has a
// directory of its own; pointing the entry at it is a full reload, which starts it clean.
//
// Apps report to the test through globalThis.probe.

import { expect, hooks, nextTurn, step, test } from "@axe/testing";
import { hotEdit } from "axe:test";
import { hotState } from "@axe/ui/hot";

interface Sig<T> {
  (): T;
  set(v: T): void;
  update(f: (v: T) => T): void;
}
const probe = {} as Record<string, unknown> & { log: string[] };
probe.log = [];
(globalThis as unknown as { probe: typeof probe }).probe = probe;
const sig = <T>(name: string): Sig<T> => probe[name] as Sig<T>;

async function edit(files: Record<string, string | null>): Promise<void> {
  const before = hotState().updates;
  hotEdit(files);
  while (hotState().updates === before) await nextTurn();
  await nextTurn();
}

const entry = (dir: string): string => `
import { app, mount } from "@axe/ui";
import { App } from "./${dir}/app";
app.onSurface((s) => mount(App, s));
`;

const P = "declare const probe: any;\n";

function counter(label: string, init = "0"): string {
  return `${P}import { Text, View, signal, style } from "@axe/ui";
const card = style({ padding: 4 });
export function Counter(props: { label: string }) {
  const n = signal(${init});
  const other = signal(10);
  probe.n = n;
  probe.other = other;
  return <View style={card}><Text>${label} {props.label} {n()}</Text></View>;
}
export function App() {
  return <Counter label="a" />;
}
`;
}

test("a component edit keeps its signals' values", async () => {
  await edit({ "c1/app.tsx": counter("one"), "main.tsx": entry("c1") });
  expect(hotState().overlay).toBe(null);
  const before = hotState().runs("c1/app.tsx", "Counter");
  sig<number>("n").set(5);
  await nextTurn();
  await edit({ "c1/app.tsx": counter("two") });
  expect(hotState().runs("c1/app.tsx", "Counter")).toBe(before + 1);
  expect(sig<number>("n")()).toBe(5);
  // App's own source didn't change, so it didn't run.
  expect(hotState().runs("c1/app.tsx", "App")).toBe(1);
});

test("a changed initializer resets only that signal", async () => {
  sig<number>("other").set(7);
  await nextTurn();
  await edit({ "c1/app.tsx": counter("two", "100") });
  expect(sig<number>("n")()).toBe(100);
  expect(sig<number>("other")()).toBe(7);
});

test("a style-only edit re-runs no component", async () => {
  const v = (padding: number): string => `${P}import { View, style, signal } from "@axe/ui";
const card = style({ padding: ${padding} });
export function App() {
  probe.s = signal(1);
  return <View style={card} />;
}
`;
  await edit({ "c2/app.tsx": v(4), "main.tsx": entry("c2") });
  const runs = hotState().runs("c2/app.tsx", "App");
  const s = sig<number>("s");
  s.set(9);
  await edit({ "c2/app.tsx": v(8) });
  expect(hotState().runs("c2/app.tsx", "App")).toBe(runs);
  expect(probe["s"]).toBe(s); // the same instance: nothing ran
  expect(s()).toBe(9);
});

test("a style-only edit of a styles module re-runs no component", async () => {
  const styles = (padding: number): string => `import { style } from "@axe/ui";
export const S = { card: style({ padding: ${padding} }), nested: { other: style({ gap: 2 }) } };
`;
  const view = `${P}import { View } from "@axe/ui";
import { S } from "./styles";
export function App() {
  return <View style={S.card} />;
}
`;
  await edit({ "c15/styles.ts": styles(4), "c15/app.tsx": view, "main.tsx": entry("c15") });
  const runs = hotState().runs("c15/app.tsx", "App");
  await edit({ "c15/styles.ts": styles(8) });
  expect(hotState().runs("c15/app.tsx", "App")).toBe(runs);
  expect(hotState().overlay).toBe(null);
});

test("a template edit keeps state", async () => {
  const v = (extra: string): string => `${P}import { Text, View, signal } from "@axe/ui";
export function App() {
  const n = signal(0);
  probe.t = n;
  return <View><Text>{n()}</Text>${extra}</View>;
}
`;
  await edit({ "c3/app.tsx": v(""), "main.tsx": entry("c3") });
  sig<number>("t").set(3);
  await nextTurn();
  await edit({ "c3/app.tsx": v("<View /><View />") });
  expect(sig<number>("t")()).toBe(3);
  expect(hotState().overlay).toBe(null);
});

test("descendants get their state back by path, rows by key", async () => {
  const v = (mark: string): string => `${P}import { For, Text, View, signal } from "@axe/ui";
function Row(props: { id: string }) {
  const n = signal(0);
  probe["row" + props.id] = n;
  return <Text>{props.id} {n()}</Text>;
}
function Inner() {
  const n = signal(0);
  probe.inner = n;
  return <View />;
}
const items = signal(["a", "b"]);
probe.items = items;
export function App() {
  return <View>${mark}<Inner /><For each={items()}>{(id: string) => <Row id={id} />}</For></View>;
}
`;
  await edit({ "c4/app.tsx": v(""), "main.tsx": entry("c4") });
  sig<number>("inner").set(4);
  sig<number>("rowa").set(1);
  sig<number>("rowb").set(2);
  sig<string[]>("items").set(["b", "a"]); // rows move, and keep their keys
  await nextTurn();
  await edit({ "c4/app.tsx": v("<View />") });
  expect(sig<number>("inner")()).toBe(4);
  expect(sig<number>("rowa")()).toBe(1);
  expect(sig<number>("rowb")()).toBe(2);
  expect(sig<string[]>("items")()).toEqual(["b", "a"]);
});

test("a broken module shows the overlay; fixing it removes it, with state intact", async () => {
  await edit({ "c5/app.tsx": counter("ok"), "main.tsx": entry("c5") });
  sig<number>("n").set(8);
  await nextTurn();
  const runs = hotState().runs("c5/app.tsx", "Counter");
  // The transform's error, with the original file and line.
  await edit({ "c5/app.tsx": `export function App( {\n` });
  expect(hotState().overlay ?? "").toContain("c5/app.tsx:");
  expect(hotState().runs("c5/app.tsx", "Counter")).toBe(runs);
  // An evaluation error, located through the source map.
  await edit({ "c5/app.tsx": `export const a = 1;\n\nthrow new Error("at the top level");\n` });
  expect(hotState().overlay ?? "").toContain("at the top level");
  expect(hotState().overlay ?? "").toContain("c5/app.tsx:3");
  await edit({ "c5/app.tsx": counter("fixed") });
  expect(hotState().overlay).toBe(null);
  expect(sig<number>("n")()).toBe(8);
});

test("an error in one component's re-run rolls that component back, and nothing else", async () => {
  const v = (body: string): string => `${P}import { Text, View, signal } from "@axe/ui";
export function Bad() {
  const n = signal(0);
  probe.bad = n;
  ${body}
  return <Text>bad</Text>;
}
export function Good() {
  const n = signal(0);
  probe.good = n;
  return <Text>good</Text>;
}
export function App() {
  return <View><Bad /><Good /></View>;
}
`;
  await edit({ "c6/app.tsx": v(""), "main.tsx": entry("c6") });
  sig<number>("bad").set(1);
  sig<number>("good").set(2);
  await nextTurn();
  await edit({ "c6/app.tsx": v(`throw new Error("in the re-run");`) });
  expect(hotState().overlay ?? "").toContain("in the re-run");
  expect(sig<number>("bad")()).toBe(1);
  expect(sig<number>("good")()).toBe(2);
  await edit({ "c6/app.tsx": v("") });
  expect(hotState().overlay).toBe(null);
  expect(sig<number>("bad")()).toBe(1);
});

test("a nested component that throws is the one rolled back, not the parent running it", async () => {
  const v = (title: string, body: string): string => `${P}import { Text, View, signal } from "@axe/ui";
function Child() {
  const n = signal(0);
  probe.child = n;
  ${body}
  return <Text>child</Text>;
}
export function App() {
  const n = signal(0);
  probe.parent = n;
  probe.title = "${title}";
  return <View><Text>${title}</Text><Child /></View>;
}
`;
  await edit({ "c16/app.tsx": v("one", ""), "main.tsx": entry("c16") });
  sig<number>("child").set(1);
  sig<number>("parent").set(2);
  await nextTurn();
  await edit({ "c16/app.tsx": v("two", `throw new Error("in the child");`) });
  expect(hotState().overlay ?? "").toContain("in the child");
  expect(probe["title"]).toBe("two"); // the parent's new version stands
  expect(sig<number>("child")()).toBe(1);
  expect(sig<number>("parent")()).toBe(2);
  await edit({ "c16/app.tsx": v("three", "") });
  expect(hotState().overlay).toBe(null);
  expect(sig<number>("child")()).toBe(1);
});

test("a non-component module re-evaluates its importers up to a boundary", async () => {
  const lib = (n: number): string => `export const value = ${n};\n`;
  const view = `${P}import { Text } from "@axe/ui";
import { value } from "./lib";
export function Shows() {
  probe.value = value;
  return <Text>{value}</Text>;
}
`;
  const app = `import { View } from "@axe/ui";
import { Shows } from "./view";
export function App() {
  return <View><Shows /></View>;
}
`;
  await edit({ "c7/lib.ts": lib(1), "c7/view.tsx": view, "c7/app.tsx": app, "main.tsx": entry("c7") });
  expect(probe["value"]).toBe(1);
  const reloads = hotState().fullReloads;
  await edit({ "c7/lib.ts": lib(2) });
  expect(probe["value"]).toBe(2);
  expect(hotState().fullReloads).toBe(reloads);
});

test("reaching the entry is a full reload", async () => {
  const lib = (n: number): string => `export const value = ${n};\n`;
  const app = `${P}import { Text, View, app, mount } from "@axe/ui";
export function App() {
  return <View />;
}
`;
  await edit({ "c8/app.tsx": app, "c8/lib.ts": lib(1), "main.tsx": `${P}import { app, mount } from "@axe/ui";
import { App } from "./c8/app";
import { value } from "./c8/lib";
probe.entry = value;
app.onSurface((s) => mount(App, s));
` });
  const reloads = hotState().fullReloads;
  await edit({ "c8/lib.ts": lib(2) });
  expect(hotState().fullReloads).toBe(reloads + 1);
  expect(probe["entry"]).toBe(2);
});

test("cycles terminate, through a boundary and without one", async () => {
  const a = (n: number): string => `import { Text } from "@axe/ui";
import { b } from "./b";
export function A() {
  return <Text>{${n} + b()}</Text>;
}
`;
  const b = (n: number): string => `import { A } from "./a";
export const b = () => (A ? ${n} : 0);
`;
  const app = `import { View } from "@axe/ui";
import { A } from "./a";
export function App() {
  return <View><A /></View>;
}
`;
  await edit({ "c9/a.tsx": a(1), "c9/b.ts": b(1), "c9/app.tsx": app, "main.tsx": entry("c9") });
  await edit({ "c9/b.ts": b(2) }); // a boundary (a.tsx) in the cycle
  expect(hotState().overlay).toBe(null);
  await edit({ "c9/a.tsx": a(2) });
  expect(hotState().overlay).toBe(null);

  const x = (n: number): string => `import { y } from "./y";
export const x = () => ${n} + y();
`;
  const y = `import { x } from "./x";
export const y = () => (x ? 1 : 0);
`;
  const app2 = `${P}import { View } from "@axe/ui";
import { x } from "./x";
export function App() {
  probe.x = x();
  return <View />;
}
`;
  await edit({ "c10/x.ts": x(1), "c10/y.ts": y, "c10/app.tsx": app2, "main.tsx": entry("c10") });
  expect(probe["x"]).toBe(2);
  await edit({ "c10/x.ts": x(2) }); // a cycle with no boundary: on to App's module
  expect(probe["x"]).toBe(3);
  expect(hotState().overlay).toBe(null);
});

test("re-evaluating a module stops what its previous version started", async () => {
  const v = (n: number): string => `${P}import { computed, effect, onCleanup, signal, app } from "@axe/ui";
export const tick = signal(0);
const twice = computed(() => tick() * 2);
effect(() => {
  probe.log.push("effect ${n} " + twice());
  onCleanup(() => probe.log.push("cleanup ${n}"));
});
onCleanup(() => probe.log.push("module ${n} gone"));
const id = setInterval(() => probe.log.push("timer ${n}"), 100);
const stop = app.onTurnEnd(() => { probe.ends${n} = (probe.ends${n} ?? 0) + 1; });
`;
  const app = `import { View } from "@axe/ui";
import { tick } from "./state";
export function App() {
  return <View>{tick() < 0 ? <View /> : null}</View>;
}
`;
  await edit({ "c11/state.ts": v(1), "c11/app.tsx": app, "main.tsx": entry("c11") });
  await step(0);
  expect(probe.log).toContain("effect 1 0");
  probe.log.length = 0;
  await step(100);
  expect(probe.log).toContain("timer 1");
  await edit({ "c11/state.ts": v(2) });
  probe.log.length = 0;
  await step(100);
  expect(probe.log).toContain("timer 2");
  expect(probe.log.includes("timer 1")).toBe(false);
  const ends = probe["ends1"];
  await nextTurn();
  expect(probe["ends1"]).toBe(ends);
});

test("a module-level signal keeps its value and its identity", async () => {
  const state = (n: number): string => `${P}import { signal } from "@axe/ui";
export const count = signal(0);
export const version = ${n};
probe.count = count;
`;
  const app = `${P}import { View } from "@axe/ui";
import { count, version } from "./state";
export function App() {
  probe.seen = count;
  probe.version = version;
  return <View />;
}
`;
  await edit({ "c12/state.ts": state(1), "c12/app.tsx": app, "main.tsx": entry("c12") });
  const count = sig<number>("count");
  count.set(6);
  await nextTurn();
  await edit({ "c12/state.ts": state(2) });
  expect(probe["version"]).toBe(2);
  expect(probe["count"]).toBe(count);
  expect(probe["seen"]).toBe(count);
  expect(count()).toBe(6);
});

test("context: a provider's new value reaches consumers; the context stays the same", async () => {
  const ctx = `import { context, signal } from "@axe/ui";
export const Label = context("default");
`;
  const v = (value: string): string => `${P}import { Text, View, use } from "@axe/ui";
import { Label } from "./ctx";
function Reads() {
  probe.label = use(Label);
  return <Text>x</Text>;
}
export function App() {
  return <View><Label.Provider value="${value}"><Reads /></Label.Provider></View>;
}
`;
  await edit({ "c13/ctx.ts": ctx, "c13/app.tsx": v("one"), "main.tsx": entry("c13") });
  expect(probe["label"]).toBe("one");
  await edit({ "c13/app.tsx": v("two") });
  expect(probe["label"]).toBe("two");
});

test("a new file, and a deleted one still imported", async () => {
  const lib = `export const value = 1;\n`;
  const app = (imports: string): string => `${P}import { View } from "@axe/ui";
${imports}
export function App() {
  return <View />;
}
`;
  await edit({ "c14/app.tsx": app(""), "main.tsx": entry("c14") });
  await edit({ "c14/lib.ts": lib }); // nothing imports it yet
  expect(hotState().overlay).toBe(null);
  await edit({ "c14/app.tsx": app(`import { value } from "./lib"; probe.lib = value;`) });
  expect(probe["lib"]).toBe(1);
  await edit({ "c14/lib.ts": null });
  expect(hotState().overlay ?? "").toContain("still imports it");
  await edit({ "c14/app.tsx": app("") });
  expect(hotState().overlay).toBe(null);
  // Or the file comes back, with a new value.
  await edit({ "c14/lib.ts": lib, "c14/app.tsx": app(`import { value } from "./lib"; probe.lib = value;`) });
  await edit({ "c14/lib.ts": null });
  expect(hotState().overlay ?? "").toContain("still imports it");
  await edit({ "c14/lib.ts": `export const value = 2;\n` });
  expect(hotState().overlay).toBe(null);
  expect(probe["lib"]).toBe(2);
});

test("signals and computeds are stable, so their module is a boundary", async () => {
  const state = (init: number, factor: number): string => `import { computed, signal } from "@axe/ui";
export const count = signal(${init});
export const scaled = computed(() => count() * ${factor});
`;
  const app = `${P}import { Text, View, signal } from "@axe/ui";
import { count, scaled } from "./state";
function Shows() {
  probe.shown = scaled;
  return <Text>{scaled()}</Text>;
}
export function App() {
  probe.count = count;
  return <View><Shows /></View>;
}
`;
  await edit({ "c17/state.ts": state(1, 2), "c17/app.tsx": app, "main.tsx": entry("c17") });
  const scaled = probe["shown"] as () => number;
  const runs = hotState().runs("c17/app.tsx", "Shows");
  sig<number>("count").set(3);
  await nextTurn();
  expect(scaled()).toBe(6);
  const reloads = hotState().fullReloads;
  await edit({ "c17/state.ts": state(1, 10) });
  expect(hotState().runs("c17/app.tsx", "Shows")).toBe(runs); // nothing below it ran
  expect(hotState().fullReloads).toBe(reloads);
  expect(sig<number>("count")()).toBe(3);
  expect(scaled()).toBe(30); // the importer's reference reaches the new version
  sig<number>("count").set(4);
  await nextTurn();
  expect(scaled()).toBe(40);
});

test("a module exporting a plain function still propagates", async () => {
  const util = (n: number): string => `export function twice(x: number) { return x * ${n}; }\n`;
  const app = `${P}import { Text, View } from "@axe/ui";
import { twice } from "./util";
export function App() {
  probe.result = twice(2);
  return <View><Text>x</Text></View>;
}
`;
  await edit({ "c18/util.ts": util(2), "c18/app.tsx": app, "main.tsx": entry("c18") });
  expect(probe["result"]).toBe(4);
  const runs = hotState().runs("c18/app.tsx", "App");
  await edit({ "c18/util.ts": util(3) });
  expect(probe["result"]).toBe(6);
  expect(hotState().runs("c18/app.tsx", "App")).toBe(runs + 1);
});

test("what starts with no owner is warned about once per place, with its location", async () => {
  const app = `${P}import { View, effect } from "@axe/ui";
export function App() {
  return <View />;
}
function later() {
  setTimeout(() => {}, 1);
  setInterval(() => {}, 1000);
  effect(() => {});
}
probe.later = later;
`;
  await edit({ "c19/app.tsx": app, "main.tsx": entry("c19") });
  const before = hotState().warnings.length;
  const later = probe["later"] as () => void;
  later(); // here there is no owner: this test runs in a promise continuation
  later();
  await nextTurn();
  const new_ = hotState().warnings.slice(before);
  expect(new_.length).toBe(3);
  expect(new_[0] ?? "").toContain("c19/app.tsx:7");
  expect(new_[0] ?? "").toContain("setTimeout with no owner");
  expect(new_[0] ?? "").toContain("runWithOwner");
  expect(new_[1] ?? "").toContain("setInterval");
  expect(new_[2] ?? "").toContain("effect");
});

test("a top-level await at startup: the app waits, and the module keeps its owner", async () => {
  const data = (n: number, delay: number): string => `${P}import { effect, onCleanup, signal, style } from "@axe/ui";
await new Promise((r) => setTimeout(r, ${delay}));
export const data = signal(${n});
export const version = ${n};
export const card = style({ padding: 2 });
effect(() => { probe.log.push("effect ${n} " + data()); });
onCleanup(() => probe.log.push("gone ${n}"));
const id = setInterval(() => probe.log.push("timer ${n}"), 100);
`;
  const app = `${P}import { Text, View } from "@axe/ui";
import { card, data, version } from "./data";
export function App() {
  probe.version = version;
  probe.data = data;
  return <View style={card}><Text>{data()}</Text></View>;
}
`;
  probe.log.length = 0;
  const before = hotState().updates;
  probe["version"] = 0;
  hotEdit({ "c20/data.ts": data(1, 30), "c20/app.tsx": app, "main.tsx": entry("c20") });
  while (hotState().updates === before) await nextTurn();
  await nextTurn();
  expect(probe["version"]).toBe(0); // the await hasn't finished: the previous app is still up
  await step(40);
  await nextTurn();
  expect(probe["version"]).toBe(1);
  expect(hotState().overlay).toBe(null);
  await step(0);
  expect(probe.log).toContain("effect 1 1"); // created after the await

  // Across a hot reload: same thing, and the signal keeps its value and object.
  const held = sig<number>("data");
  held.set(5);
  await nextTurn();
  probe.log.length = 0;
  await edit({ "c20/data.ts": data(2, 30).replace("signal(2)", "signal(1)") });
  expect(probe["version"]).toBe(1); // still waiting
  await step(40);
  await nextTurn();
  expect(probe["version"]).toBe(2);
  expect(sig<number>("data")).toBe(held);
  expect(held()).toBe(5);
  expect(probe.log).toContain("gone 1"); // the old version's owner went, at the commit
  probe.log.length = 0;
  await step(100);
  expect(probe.log).toContain("timer 2"); // a timer made after the await is owned
  expect(probe.log.includes("timer 1")).toBe(false);
});

test("a rejected top-level await keeps the last version, and a catch still has its owner", async () => {
  const data = (body: string): string => `${P}import { onCleanup } from "@axe/ui";
${body}
export const version = "ok";
`;
  const app = `${P}import { View } from "@axe/ui";
import { version } from "./data";
export function App() {
  probe.version = version;
  return <View />;
}
`;
  await edit({ "c21/data.ts": data("await null;"), "c21/app.tsx": app, "main.tsx": entry("c21") });
  expect(probe["version"]).toBe("ok");
  await edit({ "c21/data.ts": data(`await Promise.reject(new Error("no data"));`) });
  expect(hotState().overlay ?? "").toContain("no data");
  expect(probe["version"]).toBe("ok");
  probe.log.length = 0;
  await edit({ "c21/data.ts": data(`try { await Promise.reject(new Error("x")); } catch { onCleanup(() => probe.log.push("caught")); }`) });
  expect(hotState().overlay).toBe(null);
  await edit({ "c21/data.ts": data("await null;") });
  expect(probe.log).toContain("caught");
});

test("a top-level await that never settles doesn't hold later saves back", async () => {
  const data = (body: string, n: number): string => `${body}
export const version = ${n};
`;
  const app = `${P}import { View } from "@axe/ui";
import { version } from "./data";
export function App() {
  probe.version = version;
  return <View />;
}
`;
  await edit({ "c22/data.ts": data("", 1), "c22/app.tsx": app, "main.tsx": entry("c22") });
  await edit({ "c22/data.ts": data("await new Promise(() => {});", 2) });
  expect(probe["version"]).toBe(1);
  await edit({ "c22/data.ts": data("", 3) });
  await nextTurn();
  expect(probe["version"]).toBe(3);
  expect(hotState().overlay).toBe(null);
  // And one that finishes after it was replaced stops where it is.
  await edit({ "c22/data.ts": data(`await new Promise((r) => setTimeout(r, 20)); (globalThis as any).probe.zombie = 1;`, 4) });
  await edit({ "c22/data.ts": data("", 5) });
  await nextTurn();
  expect(probe["version"]).toBe(5);
  await step(30);
  await nextTurn();
  expect(probe["zombie"]).toBe(undefined);
  expect(probe["version"]).toBe(5);
});

test("for await at a module's top level is a build error", async () => {
  await edit({ "c23/bad.ts": `for await (const x of [1]) {}\n`, "c23/app.tsx": `import { View } from "@axe/ui";
import "./bad";
export function App() {
  return <View />;
}
`, "main.tsx": entry("c23") });
  expect(hotState().overlay ?? "").toContain("for await");
  await edit({ "c23/bad.ts": `async function f() { for await (const x of [1]) { void x; } }\nawait f();\n` });
  expect(hotState().overlay).toBe(null);
});

test("no update reported an uncaught error", () => {
  const uncaught = hooks().filter((h) => h.kind === 0 && !h.text.includes("hot reload"));
  expect(uncaught.map((h) => h.text)).toEqual([]);
});
