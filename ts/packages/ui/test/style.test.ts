// @axe/ui's styles: sugar as tests/ui/style_sugar_cases.json says, style() defining what the
// equivalent Style.define does, inline styles as overrides (diffed, unset when they go), the
// tokens and state props, token() sugar, and themes' defaults coming before the caller's style.

import {
  State,
  Style,
  Text,
  ThemeProvider,
  Token,
  View,
  createComponent,
  keyword,
  mount,
  number,
  px,
  rgba,
  signal,
  spring,
  style,
  theme,
  token,
  FlexDirection,
  type Element,
  type InlineStyle,
} from "@axe/ui";
import { expect, test } from "@axe/testing";
import { sugarProp } from "@axe/core/internal";
import cases from "@axe/tests/ui/style_sugar_cases.json";
import { capture, captureBytes, captureFlush, only, surface } from "./ops.js";

type Case = { property: string; input: unknown; value?: Record<string, unknown>; error?: string };

test("sugar parses as the shared cases say", () => {
  for (const c of cases.cases as Case[]) {
    if (c.error) {
      expect(() => sugarProp(c.property, c.input)).toThrow(RangeError, c.error);
      continue;
    }
    const v = sugarProp(c.property, c.input) as unknown as Record<string, unknown>;
    for (const [k, want] of Object.entries(c.value!)) {
      const got = v[k];
      if (Array.isArray(want)) want.forEach((w, i) => expect((got as number[])[i]!).toBeCloseTo(w as number, 1e-6));
      else expect(got).toEqual(want);
    }
  }
});

// The define_style op a style writes, its id zeroed (op header, then the id).
function defined(make: () => void): number[] {
  const bytes = captureBytes(make);
  bytes.fill(0, 4, 8);
  return bytes;
}

test("style() defines what the equivalent Style.define does", () => {
  const accent = token("#3366ff");
  const a = defined(() =>
    void style({
      width: 208,
      padding: [14, 12],
      gap: 8,
      flexDirection: "column",
      backgroundColor: "#fff",
      borderStartColor: accent,
      opacity: 0.9,
      transitions: { backgroundColor: spring(90, 19) },
      hover: { backgroundColor: "rgb(51, 102, 255)", tokens: [accent.with("#f63")] },
      states: [{ when: [State.hover, State.pressed], opacity: 1 }],
    }),
  );
  const b = defined(() =>
    void Style.define({
      decls: {
        width: px(208), paddingTop: px(14), paddingRight: px(12), paddingBottom: px(14), paddingLeft: px(12),
        rowGap: px(8), columnGap: px(8), flexDirection: keyword(FlexDirection.column), backgroundColor: rgba(1, 1, 1),
        borderStartColor: accent, opacity: number(0.9),
      },
      transitions: { backgroundColor: spring(90, 19) },
      states: [
        { when: State.hover, decls: { backgroundColor: rgba(0.2, 0.4, 1) }, tokens: [accent.with(rgba(1, 0.4, 0.2))] },
        { when: State.hover | State.pressed, decls: { opacity: number(1) } },
      ],
    }),
  );
  expect(a).toEqual(b);
});

test("style() names the property of an invalid value", () => {
  expect(() => style({ width: "12px" as never })).toThrow(RangeError, "width");
  expect(() => style({ opacity: 2 })).toThrow(RangeError, "opacity");
  expect(() => style({ hover: { backgroundColor: "rgb(300, 0, 0)" } })).toThrow(RangeError, "backgroundColor");
  expect(() => style({ padding: [1, 2, 3] as never })).toThrow(RangeError, "padding");
});

test("token() takes sugar; with() and set() too", () => {
  const size = token(4);
  const half = token("50%");
  const color = token("#0f0");
  expect(size.value).toEqual(px(4));
  expect(half.value).toEqual({ kind: 3, number: 50 });
  expect((color.value as unknown as { color: number[] }).color).toEqual([0, 1, 0, 1]);
  color.set("rgba(255, 0, 0, 0.5)");
  expect((color.value as unknown as { color: number[] }).color).toEqual([1, 0, 0, 0.5]);
  expect(color.with("#000").value).toEqual(rgba(0, 0, 0));
  expect(() => token("#12345" as never)).toThrow(RangeError, "is not a color");
  void Token;
});

function mountView(el: () => Element): { unmount(): void; ops: ReturnType<typeof capture> } {
  let m: { unmount(): void } | null = null;
  const ops = capture(() => (m = mount(el, surface())));
  return { unmount: () => m!.unmount(), ops };
}

test("inline styles are overrides: only changed ones are written, removed ones unset", () => {
  const wide = signal(true);
  const accent = style({ backgroundColor: "#36f" });
  const m = mountView(() =>
    View({
      get style() {
        const inline: InlineStyle = wide() ? { width: 100, height: 20 } : { height: 20 };
        return [accent, inline];
      },
    }),
  );
  expect(only(m.ops, "setStyles")[0]!.ids).toEqual([accent.id]);
  expect(only(m.ops, "setProps")[0]!.entries).toBe(2);
  const ops = captureFlush(() => wide.set(false));
  expect(ops.map((o) => o.op)).toEqual(["setProps"]); // the style list didn't change
  expect(ops[0]!.entries).toBe(1); // width unset; height unchanged
  m.unmount();
});

test("the tokens and state props", () => {
  const accent = token("#36f");
  const on = signal(true);
  const m = mountView(() =>
    View({
      get tokens() {
        return on() ? [accent.with("#f00")] : [];
      },
      get state() {
        return on() ? State.selected : 0;
      },
    }),
  );
  expect(only(m.ops, "setTokenOverrides")[0]!.entries).toBe(1);
  expect(only(m.ops, "setState")[0]).toEqual({ op: "setState", node: only(m.ops, "setState")[0]!.node!, mask: State.selected, value: State.selected });
  const ops = captureFlush(() => on.set(false));
  expect(only(ops, "setTokenOverrides").length).toBe(1); // the unset
  expect(only(ops, "setState")[0]!.mask).toBe(State.selected);
  expect(only(ops, "setState")[0]!.value).toBe(0);
  m.unmount();
});

test("a theme's defaults come first; the caller's style wins", () => {
  const ink = token("#111");
  const dark = theme({
    tokens: [ink.with("#eee")],
    components: { Text: style({ textColor: ink, fontSize: 15 }), View: style({ flexDirection: "column" }) },
  });
  const own = style({ fontSize: 20 });
  const defaults = dark.components;
  const m = mountView(() =>
    createComponent(ThemeProvider, {
      theme: dark,
      get children() {
        return View({ children: Text({ style: own, children: "x" }) });
      },
    }),
  );
  const set = only(m.ops, "setStyles").map((o) => o.ids);
  // The provider's View: the View default, then the theme's own style; the inner View: the
  // default; the Text: its default, then the caller's.
  expect(set).toEqual([[defaults.View!.id, dark.style.id], [defaults.View!.id], [defaults.Text!.id, own.id]]);
  m.unmount();
});

test("outside a theme, nodes get no default style", () => {
  const m = mountView(() => Text({ children: "plain" }));
  expect(only(m.ops, "setStyles").length).toBe(0);
  m.unmount();
});
