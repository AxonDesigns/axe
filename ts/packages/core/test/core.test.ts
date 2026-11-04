// @axe/core: text encodes as ui_cmd_set_text does, handles follow the C allocator, the tree
// mirror, measurement and its order, fonts, and the checks on styles and tokens.

import {
  CommandWriter,
  StyleProp,
  ValueKind,
  WIRE_VERSION,
  type AutoValue,
  type ColorValue,
  type Handle,
  type PxValue,
  type StyleValue,
} from "@axe/wire";
import {
  FontLoadError,
  Node,
  StaleNodeError,
  State,
  Style,
  TextAlign,
  Token,
  app,
  auto,
  loadFont,
  number,
  percent,
  px,
  rgba,
  spring,
  unset,
  type Surface,
} from "@axe/core";
import { cBuffer, expect, step, test } from "@axe/testing";
import { commands } from "../src/runtime.js";
import { setText } from "../src/text.js";

const surface = (): Surface => {
  const s = app.surfaces[0];
  if (!s) throw new Error("no surface yet");
  return s;
};

// Encodes into a writer of its own, by swapping the turn's: setText writes to `commands`.
function encodeAlone(write: () => void): Uint8Array {
  const saved = commands.mark();
  const before = commands.size;
  write();
  const bytes = commands.u8.slice(before, commands.size);
  const ops = commands.opCount - saved.ops;
  commands.rollback(saved);
  const w = new CommandWriter();
  const out = new Uint8Array(16 + bytes.length);
  out.set(bytes, 16);
  new Uint32Array(out.buffer, 0, 4).set([w.magic, WIRE_VERSION, out.length, ops]);
  return out;
}

test("setText encodes as ui_cmd_set_text", () => {
  const a: Handle = { index: 1, generation: 1 }, b: Handle = { index: 2, generation: 3 };
  const bytes = encodeAlone(() => {
    setText(a, "héllo \u{1F600} world", {
      families: ["Noto Sans", "sans-serif"],
      size: 16,
      weight: 400,
      align: TextAlign.center,
      lineHeight: 1.4,
      maxLines: 2,
      ellipsis: "...",
      spans: [
        { start: 1, end: 2, weight: 700 }, // UTF-16 indices: the é
        { start: 6, end: 8, families: ["Noto Serif"], language: "ro", size: 20 }, // the emoji
      ],
    });
    setText(b, "Open", {});
  });
  expect(bytes).toEqual(new Uint8Array(cBuffer("text")));
});

test("long text continues in append_text ops", () => {
  let big = "";
  for (let i = 0; i < 200000; i++) big += String.fromCharCode(97 + (i % 26));
  const bytes = encodeAlone(() => setText({ index: 1, generation: 1 }, big, { ellipsis: "" }));
  expect(bytes).toEqual(new Uint8Array(cBuffer("long_text")));
});

test("setText checks its options and writes nothing when they're wrong", () => {
  const n = Node.create(surface());
  const before = commands.opCount;
  expect(() => n.setText("abc", { spans: [{ start: 2, end: 1 }] })).toThrow(RangeError, "spans");
  expect(() => n.setText("abc", { spans: [{ start: 0, end: 2 }, { start: 1, end: 3 }] })).toThrow(RangeError, "spans");
  expect(() => n.setText("abc", { size: 0 })).toThrow(RangeError, "size");
  expect(() => n.setText("abc", { language: "no spaces" })).toThrow(RangeError, "language");
  expect(() => n.setText("abc", { families: [] })).toThrow(RangeError, "families");
  expect(commands.opCount).toBe(before);
  n.destroy();
});

test("handles follow the C allocator: generations, and a LIFO free list in pre-order", () => {
  const s = surface();
  const used = new Set<number>();
  const root = Node.create(s);
  const kids = [Node.create(root), Node.create(root)];
  const grandchild = Node.create(kids[0]!);
  for (const n of [root, ...kids, grandchild]) used.add(n.handle.index);
  expect(used.size).toBe(4);
  const slots = [root, kids[0]!, grandchild, kids[1]!].map((n) => n.handle.index); // pre-order
  const gens = [root, kids[0]!, grandchild, kids[1]!].map((n) => n.handle.generation);
  root.destroy(); // frees root, kid 0, grandchild, kid 1, in that order
  for (const n of [root, ...kids, grandchild]) expect(n.alive).toBe(false);
  // The last slot freed is reused first, each with a newer generation.
  const again = [Node.create(s), Node.create(s), Node.create(s), Node.create(s)];
  expect(again.map((n) => n.handle.index)).toEqual([...slots].reverse());
  again.forEach((n, i) => expect(n.handle.generation).toBe(gens[3 - i]! + 1));
  for (const n of again) n.destroy();
});

test("the tree's rules", () => {
  const s = surface();
  const a = Node.create(s), b = Node.create(a), c = Node.create(a);
  expect(() => a.moveTo(b)).toThrow(RangeError, "under itself");
  expect(() => b.moveTo(a, b)).toThrow(RangeError, "before");
  expect(() => b.moveTo(s, c)).toThrow(RangeError, "before"); // c isn't a child of the surface
  b.moveTo(a, c); // already first: reorders nothing, still fine
  c.moveTo(s);
  a.destroy();
  expect(c.alive).toBe(true); // it moved out first
  expect(() => b.setColor(1, 0, 0)).toThrow(StaleNodeError);
  expect(() => b.destroy()).toThrow(StaleNodeError);
  expect(() => Node.create(b)).toThrow(StaleNodeError);
  c.destroy();
});

test("measure resolves in request order, after the frame lays the node out", async () => {
  const box = Style.define({ decls: { width: px(120), height: px(30), marginLeft: px(10) } });
  const n = Node.create(surface(), { styles: [box] });
  const order: string[] = [];
  const first = n.measure().then((l) => (order.push("first"), l));
  const second = n.measure().then((l) => (order.push("second"), l));
  const l = await first;
  await second;
  expect(order).toEqual(["first", "second"]);
  expect(l.layoutInSurface).toEqual({ x: 10, y: 0, width: 120, height: 30 });
  expect(l.presentationInParent).toEqual(l.layoutInParent);
  n.destroy();
  box.destroy();
});

test("measure rejects with StaleNodeError for a node gone by the frame", async () => {
  const n = Node.create(surface());
  const pending = n.measure();
  n.destroy();
  await expect(pending).toReject(StaleNodeError);
  await expect(n.measure()).toReject(StaleNodeError);
});

test("fonts load, or reject with FontLoadError", async () => {
  await loadFont(`${AXE_SOURCE_DIR}/tests/fonts/NotoSansHebrew-VF.ttf`);
  await expect(loadFont(`${AXE_SOURCE_DIR}/tests/fonts/missing.ttf`)).toReject(FontLoadError);
  await expect(loadFont("")).toReject(RangeError); // refused before it reaches the core
});

test("styles and tokens refuse what the core would drop", () => {
  expect(() => Style.define({ decls: { width: px(-1) } })).toThrow(RangeError, "width");
  expect(() => Style.define({ decls: { opacity: number(2) } })).toThrow(RangeError, "opacity");
  expect(() => Style.define({ decls: { maxWidth: percent(NaN) } })).toThrow(RangeError);
  expect(() => Style.define({ decls: { width: unset } as never })).toThrow(RangeError); // unset is for overrides
  expect(() => Style.define({ decls: { nonsense: px(1) } as never })).toThrow(RangeError, "nonsense");
  expect(() => Style.define({ transitions: { layout: spring(0, 1) } })).toThrow(RangeError, "transition of layout");
  expect(() => Style.define({ states: [{ when: 0 }] })).toThrow(RangeError, "state bits");
  const t = Token.create(rgba(1, 0, 0));
  expect(() => t.set(spring(1, 1) as never)).toThrow(RangeError, "can't hold");
  const n = Node.create(surface());
  n.setProps({ width: unset, backgroundColor: t });
  n.destroy();
  t.unset();
});

test("token scopes encode as their wire ops", () => {
  const accent = Token.create(rgba(1, 0, 0));
  const size = Token.create(px(4));
  const local = Token.local<ColorValue>();
  const n = Node.create(surface());
  // A style's blocks: the base and a state block declare tokens; a block without keeps the
  // short header (no tokens field at all).
  let defined: Style | null = null;
  const style = encodeAlone(() => {
    defined = Style.define({
      decls: { backgroundColor: accent },
      tokens: [accent.with(rgba(0, 1, 0)), size.with(px(8))],
      states: [{ when: State.hover, tokens: [accent.with(local)] }, { when: State.pressed, decls: { opacity: number(0.5) } }],
    });
  });
  const body = (w: CommandWriter): number[] => Array.from(w.finish(WIRE_VERSION)!);
  const w = new CommandWriter();
  const decl = (value: StyleValue) => ({ property: 0, value });
  w.defineStyle((defined as Style | null)!.id, [
    {
      states: 0,
      entries: [{ property: StyleProp.backgroundColor, value: { kind: ValueKind.token, token: accent.id } }],
      tokens: [accent.id, size.id],
      tokenValues: [decl(rgba(0, 1, 0)), decl(px(8))],
    },
    { states: State.hover, entries: [], tokens: [accent.id], tokenValues: [decl({ kind: ValueKind.token, token: local.id })] },
    { states: State.pressed, entries: [{ property: StyleProp.opacity, value: number(0.5) }] },
  ]);
  expect(Array.from(style)).toEqual(body(w));
  // A node's own, and removing them.
  const own = encodeAlone(() => n.setTokens(accent.with(rgba(0, 0, 1)), local.with(rgba(1, 1, 1))));
  const w2 = new CommandWriter();
  w2.setTokenOverrides(n.handle, [accent.id, local.id], [decl(rgba(0, 0, 1)), decl(rgba(1, 1, 1))]);
  expect(Array.from(own)).toEqual(body(w2));
  const gone = encodeAlone(() => n.unsetTokens(accent, local));
  const w3 = new CommandWriter();
  w3.setTokenOverrides(n.handle, [accent.id, local.id], [decl(unset), decl(unset)]);
  expect(Array.from(gone)).toEqual(body(w3));
  // What a token can't hold throws before anything is written.
  expect(() => accent.with(spring(1, 1) as never)).toThrow(RangeError, "can't hold");
  n.destroy();
  accent.unset();
  size.unset();
});

// Compile-time checks only (never called): a token has one kind, and scopes must keep it.
function tokenKinds(): void {
  const color = Token.create(rgba(1, 0, 0));
  const length = Token.create(px(1));
  color.with(rgba(0, 0, 1));
  color.with(Token.local<ColorValue>());
  // @ts-expect-error a token type is one kind, not a union
  const both: Token<PxValue | AutoValue> | null = null;
  void both;
  // @ts-expect-error a token's kind is inferred from its value: one kind
  Token.create(Math.random() > 0.5 ? px(1) : auto);
  // @ts-expect-error a color token's declarations are colors
  color.with(px(1));
  // @ts-expect-error or color tokens
  color.with(length);
  // @ts-expect-error a style property takes tokens of its kinds only
  Style.define({ decls: { width: color } });
}
void tokenKinds;

test("springs and the older ops", async () => {
  const n = Node.create(surface());
  n.setRect(0, 0, 10, 10);
  n.spring(0x1, [50, 50, 20, 20], { stiffness: 170, damping: 26 });
  await step(1000);
  const l = await n.measure();
  expect(l.presentationInSurface.x).toBeCloseTo(50, 0.01);
  n.destroy();
});

test("lean set_text and set_styles encode as the general encoders do", () => {
  const n = Node.create(surface());
  const slot = n.handle.index;
  for (const s of ["", "abc", "héllo · ✓", "😀 x", "lone \ud800 surrogate", "x".repeat(5000)]) {
    const general = encodeAlone(() => setText(n.handle, s, {}));
    const lean = encodeAlone(() => expect(commands.setTextPlain(slot, n.handle.generation, s)).toBe(true));
    expect(Array.from(lean)).toEqual(Array.from(general));
  }
  // Too long for the lean path's reservation: it declines and writes nothing.
  const before = commands.size;
  expect(commands.setTextPlain(slot, n.handle.generation, "x".repeat(30000))).toBe(false);
  expect(commands.size).toBe(before);
  const style = Style.define({ decls: { opacity: number(0.5) } });
  expect(Array.from(encodeAlone(() => commands.setStylesRaw(slot, n.handle.generation, [style.id, style.id]))))
    .toEqual(Array.from(encodeAlone(() => n.setStyles(style, style))));
  n.destroy();
});
