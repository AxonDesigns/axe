// Styles with sugar (docs/ui.md, "Styles"): style() defines an interned style from a flat object;
// a node's `style` prop lists styles and inline objects. Inline objects, constant or not, are
// the node's overrides: they win over every style, state blocks included, as inline styles beat
// state rules in CSS.

import {
  State,
  Style,
  Token,
  none,
  unset,
  type PropSugar,
  type StyleBlock,
  type StyleDesc,
  type TokenDecl,
  type ValidColor,
} from "@axe/core";
import { entries, slotSetEntries, slotSetStyles, sugarProp, type StyleEntry } from "@axe/core/internal";
import type {
  AutoValue,
  ColorValue,
  InheritValue,
  InitialValue,
  KeywordValue,
  NoneValue,
  NumberValue,
  PercentValue,
  PxValue,
  SpringValue,
  StringValue,
  StylePropValues,
  StyleValue,
  TransitionPropName,
} from "@axe/wire";

// ---- Types ----------------------------------------------------------------------------------

type PropName = keyof StylePropValues;

type TokenOf<V> = V extends PxValue ? Token<PxValue>
  : V extends PercentValue ? Token<PercentValue>
  : V extends NumberValue ? Token<NumberValue>
  : V extends ColorValue ? Token<ColorValue>
  : V extends KeywordValue ? Token<KeywordValue>
  : V extends AutoValue ? Token<AutoValue>
  : V extends NoneValue ? Token<NoneValue>
  : V extends StringValue ? Token<StringValue>
  : never;

/** What a property takes: its values' sugar (numbers as px, "50%", "#36f", "auto", keyword
 * names such as "row-reverse"), a value object, a token of its kind, inherit or initial. */
export type PropInput<K extends PropName> = PropSugar<K> | TokenOf<StylePropValues[K]>;

/** One value for all four sides, [vertical, horizontal], or [top, right, bottom, left]. */
export type Box<T> = T | readonly [T, T] | readonly [T, T, T, T];

export interface Shorthands {
  readonly padding?: Box<PropInput<"paddingTop">>;
  readonly margin?: Box<PropInput<"marginTop">>;
  readonly inset?: Box<PropInput<"top">>;
  /** Both gaps. */
  readonly gap?: PropInput<"rowGap">;
}

/** Declarations: every property (camelCase), and the shorthands. */
export type DeclsInput = { readonly [K in PropName]?: PropInput<K> } & Shorthands;

/** Transitions: a spring, or "none" to turn one off. `layout` animates the layout rect. */
export type TransitionsInput = { readonly [K in TransitionPropName]?: SpringValue | NoneValue | "none" | InheritValue | InitialValue };

export type BlockInput = DeclsInput & {
  readonly transitions?: TransitionsInput;
  /** The block's token declarations for the node's subtree (token.with(value)). */
  readonly tokens?: readonly TokenDecl[];
};

type StateName = "hover" | "pressed" | "focused" | "disabled" | "selected";

/** style()'s input: a block, blocks for single states (hover: {...}), and blocks for
 * combinations (states: [{ when: [State.hover, State.pressed], ... }]). */
export type StyleInput = BlockInput & { readonly [S in StateName]?: BlockInput } & {
  readonly states?: readonly (BlockInput & { readonly when: number | readonly number[] })[];
};

/** An inline style: declarations and transitions, applied as the node's overrides. */
export type InlineStyle = DeclsInput & { readonly transitions?: TransitionsInput };

/** A node's `style` prop: styles and inline objects in order (later ones win), falsy entries
 * skipped, nested lists flattened. */
export type StyleProp = Style | InlineStyle | false | null | undefined | readonly StyleProp[];

type ColorPropName = { [K in PropName]: ColorValue extends StylePropValues[K] ? K : never }[PropName];
type CheckBlock<S> = {
  [K in keyof S]: K extends ColorPropName ? ValidColor<S[K]>
    : K extends StateName ? CheckBlock<S[K]>
    : K extends "states" ? { [I in keyof S[K]]: CheckBlock<S[K][I]> }
    : S[K];
};

// ---- Parsing --------------------------------------------------------------------------------

const STATES: Record<StateName, number> = {
  hover: State.hover,
  pressed: State.pressed,
  focused: State.focused,
  disabled: State.disabled,
  selected: State.selected,
};

const SIDES = {
  padding: ["paddingTop", "paddingRight", "paddingBottom", "paddingLeft"],
  margin: ["marginTop", "marginRight", "marginBottom", "marginLeft"],
  inset: ["top", "right", "bottom", "left"],
} as const;

type Decl = StyleValue | Token<never>;
type DeclMap = Record<string, Decl>;

function value(name: string, v: unknown): Decl {
  return v instanceof Token ? (v as Token<never>) : sugarProp(name, v);
}

function addDecl(out: DeclMap, name: string, v: unknown): void {
  if (v === undefined) return;
  if (name in SIDES) {
    const sides = SIDES[name as keyof typeof SIDES];
    const list = Array.isArray(v) ? (v as unknown[]) : [v];
    const pick = list.length === 1 ? [0, 0, 0, 0] : list.length === 2 ? [0, 1, 0, 1] : list.length === 4 ? [0, 1, 2, 3] : null;
    if (!pick) throw new RangeError(`${name}: one value, [vertical, horizontal] or [top, right, bottom, left]`);
    for (let i = 0; i < 4; i++) out[sides[i]!] = value(sides[i]!, list[pick[i]!]);
    return;
  }
  if (name === "gap") {
    out["rowGap"] = value("rowGap", v);
    out["columnGap"] = value("columnGap", v);
    return;
  }
  out[name] = value(name, v);
}

function transitions(t: TransitionsInput | undefined): Record<string, StyleValue> | undefined {
  if (!t) return undefined;
  const out: Record<string, StyleValue> = {};
  for (const k in t) {
    const v = (t as Record<string, unknown>)[k];
    if (v !== undefined) out[k] = v === "none" ? none : (v as StyleValue);
  }
  return out;
}

const BLOCK_KEYS = new Set(["transitions", "tokens", "states", "when", ...Object.keys(STATES)]);

function block(b: BlockInput, where: string): StyleBlock {
  const decls: DeclMap = {};
  for (const k in b) {
    if (BLOCK_KEYS.has(k)) continue;
    try {
      addDecl(decls, k, (b as Record<string, unknown>)[k]);
    } catch (e) {
      if (e instanceof RangeError) throw new RangeError(`${where}: ${e.message}`);
      throw e;
    }
  }
  const out: { decls: object; transitions?: object; tokens?: readonly TokenDecl[] } = { decls };
  const t = transitions(b.transitions);
  if (t) out.transitions = t;
  if (b.tokens?.length) out.tokens = b.tokens;
  return out as StyleBlock;
}

/** @internal style()'s parse: a StyleDesc for Style.define. */
export function styleDesc(input: StyleInput): StyleDesc {
  const states: (StyleBlock & { when: number })[] = [];
  for (const name of Object.keys(STATES) as StateName[]) {
    const b = input[name];
    if (b) states.push({ ...block(b, name), when: STATES[name] });
  }
  for (const s of input.states ?? []) {
    const when = typeof s.when === "number" ? s.when : s.when.reduce((m, x) => m | x, 0);
    states.push({ ...block(s, "states"), when });
  }
  const base = block(input, "style");
  return states.length ? { ...base, states } : base;
}

/** Defines an interned style: applying it costs one op. Numbers are logical px; colors are
 * "#36f", "#3366ff", "#3366ff80", "rgb(51, 102, 255)" or "rgba(51, 102, 255, 0.5)"; percentages
 * "50%"; keywords by name ("column", "space-between"); tokens directly. An invalid value is a
 * compile error where the type checker can tell, a RangeError naming the property otherwise. */
export function style<const S extends StyleInput>(input: S extends CheckBlock<S> ? S : CheckBlock<S>): Style {
  return Style.define(styleDesc(input as StyleInput));
}

/** Redefines `target` from sugar: every node using it is resolved again. */
export function restyle<const S extends StyleInput>(target: Style, input: S extends CheckBlock<S> ? S : CheckBlock<S>): void {
  target.redefine(styleDesc(input as StyleInput));
}

/** @internal An inline style's wire entries (overrides). */
export function inlineEntries(input: InlineStyle): StyleEntry[] {
  const decls: DeclMap = {};
  for (const k in input) {
    if (k === "transitions") continue;
    addDecl(decls, k, (input as Record<string, unknown>)[k]);
  }
  return entries(decls as never, transitions(input.transitions) as never, true);
}

// ---- Applying a style prop ----------------------------------------------------------------------

function collect(v: unknown, ids: number[], inline: StyleEntry[]): void {
  if (v === null || v === undefined || v === false) return;
  if (v instanceof Style) {
    ids.push(v.id);
    return;
  }
  if (Array.isArray(v)) {
    for (const x of v) collect(x, ids, inline);
    return;
  }
  if (typeof v === "function") {
    collect((v as () => unknown)(), ids, inline);
    return;
  }
  if (typeof v === "object") {
    for (const e of inlineEntries(v as InlineStyle)) inline.push(e);
    return;
  }
  throw new TypeError(`style: ${typeof v} is not a style, an inline style or a list of them`);
}

function sameValue(a: StyleValue | undefined, b: StyleValue): boolean {
  if (!a || a.kind !== b.kind) return false;
  const x = a as unknown as Record<string, unknown>, y = b as unknown as Record<string, unknown>;
  for (const k in y) {
    const p = x[k], q = y[k];
    if (Array.isArray(q)) {
      if (!Array.isArray(p) || p.length !== q.length || q.some((c, i) => c !== p[i])) return false;
    } else if (p !== q) return false;
  }
  return true;
}

/** @internal A binding's body for a node's style prop: writes set_styles when the list of styles
 * changes, and set_props with the overrides that changed (unset for those that went). */
export function styleBinding(slot: number, read: () => unknown, defaults: Style | undefined): () => void {
  let ids: number[] = [];
  let overrides = new Map<number, StyleValue>();
  return () => {
    const nextIds: number[] = defaults ? [defaults.id] : [];
    const inline: StyleEntry[] = [];
    collect(read(), nextIds, inline);
    if (nextIds.length !== ids.length || nextIds.some((id, i) => id !== ids[i])) slotSetStyles(slot, nextIds);
    ids = nextIds;
    const next = new Map<number, StyleValue>();
    for (const e of inline) next.set(e.property, e.value);
    const changed: StyleEntry[] = [];
    for (const [p, v] of next) if (!sameValue(overrides.get(p), v)) changed.push({ property: p, value: v });
    for (const p of overrides.keys()) if (!next.has(p)) changed.push({ property: p, value: unset });
    if (changed.length) slotSetEntries(slot, changed);
    overrides = next;
  };
}
