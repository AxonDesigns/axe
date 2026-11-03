// @axe/wire against the C codec: the TypeScript encoder writes the bytes ui_cmd_* writes for
// the same ops (the runner's reference cases), the decoder reads what the core's encoder
// writes, and the UTF-8 helpers agree with UTF-8.

import {
  CommandWriter,
  EventReader,
  EventType,
  FlexDirection,
  MAGIC_COMMANDS,
  Property,
  State,
  StyleProp,
  Utf8OffsetMap,
  ValueKind,
  WIRE_VERSION,
  WireError,
  encodeUtf8,
  transitionOf,
  utf16OffsetFromUtf8,
  utf8Length,
  utf8OffsetFromUtf16,
  type Handle,
  type StyleEntry,
} from "@axe/wire";
import { cBuffer, cEvents, expect, test } from "@axe/testing";

const node = (index: number, generation: number): Handle => ({ index, generation });
const a = node(1, 1), b = node(2, 3);
const surface = node(1 << 28, 1);
const NULL = node(0, 0);

const finish = (w: CommandWriter): Uint8Array => w.finish(WIRE_VERSION)!.slice();
const same = (ts: Uint8Array, name: string) => expect(ts).toEqual(new Uint8Array(cBuffer(name)));

test("fixed ops encode as the C writer does", () => {
  const w = new CommandWriter();
  w.createNode(a, surface);
  w.createNode(b, a);
  w.setRect(a, 1.5, -2, 300, 40.25);
  w.setCornerRadius(a, 8);
  w.setColor(a, Math.fround(0.1), Math.fround(0.2), Math.fround(0.3), 1);
  w.setClip(a, 1);
  w.moveNode(b, surface, NULL);
  w.moveNode(b, a, a);
  w.setState(b, 3, 1);
  w.requestLayout(b, 77);
  w.setTimer(9, 123456789012345);
  w.setTimer(9, 2 ** 64); // the cancel sentinel (UINT64_MAX), past what a number holds exactly
  w.loadFont(4, encodeUtf8("/fonts/Noto é.ttf"));
  w.destroyStyle(5);
  w.setStyles(b, [3, 1, 2]);
  w.setStyles(b, []);
  w.startSpring(a, Property.rect, 170, 18.25, [1, 2, 3, 4]);
  w.startSpring(a, Property.cornerRadius, 90, 19, [6]);
  w.destroyNode(b);
  same(finish(w), "basic");
});

test("styles, overrides and tokens encode as the C writer does", () => {
  const w = new CommandWriter();
  const px = (n: number) => ({ kind: ValueKind.px, number: n }) as const;
  const base: StyleEntry[] = [
    { property: StyleProp.width, value: px(208) },
    { property: StyleProp.height, value: { kind: ValueKind.percent, number: 50 } },
    { property: StyleProp.flexGrow, value: { kind: ValueKind.number, number: 1 } },
    { property: StyleProp.marginLeft, value: { kind: ValueKind.auto } },
    { property: StyleProp.maxWidth, value: { kind: ValueKind.none } },
    { property: StyleProp.backgroundColor, value: { kind: ValueKind.color, color: [Math.fround(0.2), Math.fround(0.45), Math.fround(0.95), 1] } },
    { property: StyleProp.flexDirection, value: { kind: ValueKind.keyword, keyword: FlexDirection.column } },
    { property: StyleProp.textColor, value: { kind: ValueKind.token, token: 3 } },
    { property: StyleProp.fontFamily, value: { kind: ValueKind.string, string: "Noto Sans, sans-serif" } },
    { property: StyleProp.language, value: { kind: ValueKind.string, string: "sr-Cyrl" } },
    { property: StyleProp.direction, value: { kind: ValueKind.inherit } },
    { property: StyleProp.opacity, value: { kind: ValueKind.initial } },
    { property: transitionOf(StyleProp.backgroundColor), value: { kind: ValueKind.spring, stiffness: 90, damping: 19 } },
    { property: transitionOf(StyleProp.layout), value: { kind: ValueKind.spring, stiffness: 170, damping: 24 } },
  ];
  w.defineStyle(1, [
    { states: 0, entries: base },
    { states: State.hover | State.pressed, entries: [{ property: StyleProp.backgroundColor, value: { kind: ValueKind.token, token: 7 } }] },
  ]);
  w.defineStyle(2, []);
  w.setProps(a, [{ property: StyleProp.borderWidth, value: px(2) }, { property: StyleProp.width, value: { kind: ValueKind.unset } }]);
  w.setToken(3, { property: 0, value: { kind: ValueKind.color, color: [1, 1, 1, 1] } });
  w.setToken(4, { property: 0, value: { kind: ValueKind.token, token: 3 } });
  w.setToken(5, { property: 0, value: { kind: ValueKind.string, string: "x" } });
  w.setToken(3, { property: 0, value: { kind: ValueKind.unset } });
  same(finish(w), "styles");
});

test("a method that throws writes nothing", () => {
  const w = new CommandWriter();
  w.createNode(a, surface);
  expect(() => w.setRect(a, 0, 0, -1, 1)).toThrow(RangeError, "width");
  expect(() => w.setColor(a, NaN, 0, 0, 1)).toThrow(RangeError);
  expect(() => w.startSpring(a, 77 as Property, 100, 10, [1])).toThrow(RangeError, "property");
  expect(() => w.startSpring(a, Property.cornerRadius, 100, 10, [-1])).toThrow(RangeError, "target");
  expect(() => w.startSpring(a, Property.rect, 100, 10, [1, 2])).toThrow(RangeError, "count");
  expect(() => w.setClip(a, 2)).toThrow(RangeError, "flags");
  expect(() => w.setStyles(a, [1, 0])).toThrow(RangeError, "ids");
  expect(() => w.loadFont(1, new Uint8Array(0))).toThrow(RangeError, "path_bytes");
  expect(() => w.loadFont(1, new Uint8Array([0xff]))).toThrow(RangeError, "path");
  expect(() => w.setState(a, -1, 0)).toThrow(RangeError, "mask");
  expect(w.opCount).toBe(1);
  const bytes = finish(w);
  expect(bytes.length).toBe(16 + 20);
  expect(new Uint32Array(bytes.buffer, 0, 4)).toEqual(new Uint32Array([MAGIC_COMMANDS, 0, 36, 1]));
});

test("the writer grows its buffer and starts over after finish", () => {
  const w = new CommandWriter();
  for (let i = 0; i < 10000; i++) w.setState(a, 1, i & 1);
  const bytes = finish(w);
  expect(bytes.length).toBe(16 + 10000 * 20);
  expect(w.finish(WIRE_VERSION)).toBe(null);
  w.setState(a, 1, 1);
  expect(finish(w).length).toBe(36);
});

test("events decode as the core encodes them", () => {
  const r = new EventReader(cEvents());
  const all = [];
  for (let e = r.next(); e; e = r.next()) all.push(e);
  expect(all.length).toBe(7); // the unknown event from a newer core is skipped
  const [clock, frame, resized, close, timer, font, layout] = all;
  expect(clock).toEqual({ type: EventType.clock, nowNs: 2 ** 60 + 12345 });
  expect(frame).toEqual({ type: EventType.frameCommitted, surface, frameId: 42, predictedPresentNs: 16666667 });
  expect(resized).toEqual({ type: EventType.surfaceResized, surface, width: 980, height: 640.5, scale120: 150 });
  expect(close).toEqual({ type: EventType.surfaceCloseRequested, surface });
  expect(timer).toEqual({ type: EventType.timerFired, id: 7, deadlineNs: 2500000000, nowNs: 2516666667 });
  expect(font).toEqual({ type: EventType.fontLoaded, id: 3, result: 9 });
  if (layout?.type !== EventType.layoutMeasured) throw new Error("expected layout_measured");
  expect(layout.requestId).toBe(9);
  expect(layout.node).toEqual(node(5, 2));
  expect(layout.presentationInSurface).toEqual({ x: 3, y: 3.5, width: 100.25, height: 20 });
});

test("times past 2^53 decode rounded; malformed buffers throw", () => {
  const ev = new EventReader(cEvents()).next();
  if (ev?.type !== EventType.clock) throw new Error("expected the clock first");
  expect(ev.nowNs).toBe(2 ** 60 + 12345); // the nearest double: rounding allowed for times
  const bad = new Uint8Array(cEvents());
  bad[0] = 0;
  expect(() => new EventReader(bad.buffer)).toThrow(WireError);
  const short = cEvents().slice(0, 40);
  new Uint32Array(short, 8, 1)[0] = 40;
  const r = new EventReader(short);
  expect(() => {
    for (;;) if (!r.next()) break;
  }).toThrow(WireError);
});

test("UTF-8 lengths and offsets", () => {
  const s = "héllo \u{1F600}!";
  expect(utf8Length(s)).toBe(encodeUtf8(s).length);
  expect(utf8Length("a\ud800b")).toBe(5); // a lone surrogate encodes as U+FFFD
  expect(utf8OffsetFromUtf16(s, 2)).toBe(3);
  expect(utf8OffsetFromUtf16(s, 7)).toBe(7); // inside the pair: its start
  expect(utf8OffsetFromUtf16(s, 8)).toBe(11);
  expect(utf16OffsetFromUtf8(s, 3)).toBe(2);
  expect(utf16OffsetFromUtf8(s, 10)).toBe(6); // inside the emoji: its start
  expect(utf16OffsetFromUtf8(s, 99)).toBe(s.length);
  const map = new Utf8OffsetMap(s);
  for (let i = 0; i <= s.length; i++) expect(map.toUtf8(i)).toBe(utf8OffsetFromUtf16(s, i));
  for (let b8 = 0; b8 <= 12; b8++) expect(map.toUtf16(b8)).toBe(utf16OffsetFromUtf8(s, b8));
  expect(map.byteLength).toBe(12);
});
