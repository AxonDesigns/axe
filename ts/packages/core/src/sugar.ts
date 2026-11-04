// Value sugar (docs/ui.md, "Styles"): plain numbers and strings for style values. A number is
// logical px (or a plain number, for properties that take numbers: opacity, flexGrow...); a
// string is a color (#rgb, #rrggbb, #rrggbbaa, rgb(), rgba()), a percentage ("50%"), "auto",
// "none", "inherit", "initial", a keyword's CSS-style name ("row-reverse"), or, for string
// properties, the string itself. @axe/ui's transform parses constant values at compile time with
// the same rules (tests/style_sugar_cases.json keeps the two in step).

import {
  ValueKind,
  styleKeywords,
  stylePropKinds,
  type AutoValue,
  type ColorValue,
  type InheritValue,
  type InitialValue,
  type KeywordPropName,
  type KeywordValue,
  type NoneValue,
  type NumberValue,
  type PercentValue,
  type PxValue,
  type StylePropValues,
  type StyleValue,
  type StringValue,
} from "@axe/wire";

// ---- Types ------------------------------------------------------------------------------------

type HexDigit =
  | "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9"
  | "a" | "b" | "c" | "d" | "e" | "f" | "A" | "B" | "C" | "D" | "E" | "F";
type HexLength<S extends string, N extends 0[] = []> = S extends ""
  ? N["length"]
  : N["length"] extends 8
    ? -1
    : S extends `${HexDigit}${infer R}`
      ? HexLength<R, [...N, 0]>
      : -1;

/** A color string: #rgb, #rrggbb, #rrggbbaa, rgb(r, g, b) or rgba(r, g, b, a), components 0–255
 * and alpha 0–1. Hex digits are checked by the type checker (ValidColor); rgb() and rgba() by
 * the transform for constants, at run time otherwise. */
export type ColorString = `#${string}` | `rgb(${string})` | `rgba(${string})`;
/** A percentage string: "50%". */
export type PercentString = `${number}%`;

/** `V` if it is a valid color string, else an error message as a type. */
export type ValidColor<V> = V extends `#${infer H}`
  ? HexLength<H> extends 3 | 6 | 8
    ? V
    : `invalid color "${V}": use #rgb, #rrggbb or #rrggbbaa`
  : V;

/** What a plain value of one kind can be written as. */
export type SugarOf<V> = V extends PxValue
  ? number | PxValue
  : V extends PercentValue
    ? PercentString | PercentValue
    : V extends NumberValue
      ? number | NumberValue
      : V extends ColorValue
        ? ColorString | ColorValue
        : V extends AutoValue
          ? "auto" | AutoValue
          : V extends NoneValue
            ? "none" | NoneValue
            : V extends StringValue
              ? string | StringValue
              : V extends KeywordValue
                ? KeywordValue
                : never;

/** A property's sugar: its values' sugar, its keyword names, inherit and initial. */
export type PropSugar<K extends keyof StylePropValues> =
  | (K extends KeywordPropName ? keyof (typeof styleKeywords)[K] | KeywordValue : SugarOf<StylePropValues[K]>)
  | "inherit"
  | "initial"
  | InheritValue
  | InitialValue;

// ---- Parsing ------------------------------------------------------------------------------------

const HEX = /^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/;
const FUNC = /^(rgba?)\(\s*([^)]*?)\s*\)$/;
const NUM = /^[+-]?(\d+\.?\d*|\.\d+)(e[+-]?\d+)?$/i;
const PERCENT = /^([+-]?(\d+\.?\d*|\.\d+)(e[+-]?\d+)?)%$/i;

/** Parses a color string; throws a RangeError naming `what` if it isn't one. */
export function parseColor(s: string, what = "color"): ColorValue {
  const hex = HEX.exec(s);
  if (hex) {
    const h = hex[1]!;
    if (h.length === 3)
      return { kind: ValueKind.color, color: [parseInt(h[0]!, 16) / 15, parseInt(h[1]!, 16) / 15, parseInt(h[2]!, 16) / 15, 1] };
    return { kind: ValueKind.color, color: [hexPair(h, 0), hexPair(h, 2), hexPair(h, 4), h.length === 8 ? hexPair(h, 6) : 1] };
  }
  const fn = FUNC.exec(s);
  if (!fn) return notColor(s, what);
  const parts = fn[2]!.split(",").map((p) => p.trim());
  if (parts.length !== (fn[1] === "rgba" ? 4 : 3) || !parts.every((p) => NUM.test(p))) return notColor(s, what);
  const n = parts.map(Number);
  for (let i = 0; i < 3; i++) if (!(n[i]! >= 0 && n[i]! <= 255)) return notColor(s, what);
  const a = n.length === 4 ? n[3]! : 1;
  if (!(a >= 0 && a <= 1)) return notColor(s, what);
  return { kind: ValueKind.color, color: [n[0]! / 255, n[1]! / 255, n[2]! / 255, a] };
}

function hexPair(h: string, i: number): number {
  return parseInt(h.slice(i, i + 2), 16) / 255;
}

function notColor(s: string, what: string): never {
  throw new RangeError(`${what}: "${s}" is not a color (#rgb, #rrggbb, #rrggbbaa, rgb(r, g, b) or rgba(r, g, b, a))`);
}

const props = stylePropKinds as Record<string, readonly number[] | undefined>;
const keywords = styleKeywords as Record<string, Record<string, number> | undefined>;

const INHERIT: InheritValue = Object.freeze({ kind: ValueKind.inherit });
const INITIAL: InitialValue = Object.freeze({ kind: ValueKind.initial });
const AUTO: AutoValue = Object.freeze({ kind: ValueKind.auto });
const NONE: NoneValue = Object.freeze({ kind: ValueKind.none });

/** A style value from sugar for one of `kinds` (value kinds), or a value object as it is.
 * `what` names the property in errors. Tokens aren't handled here. */
export function sugarValue(kinds: readonly number[], input: unknown, what: string, keywordNames?: Record<string, number>): StyleValue {
  if (typeof input === "object" && input !== null && "kind" in input) return input as StyleValue;
  if (typeof input === "number") {
    if (!Number.isFinite(input)) throw new RangeError(`${what}: ${input} is not a finite number`);
    if (kinds.includes(ValueKind.px)) return { kind: ValueKind.px, number: input };
    if (kinds.includes(ValueKind.number)) return { kind: ValueKind.number, number: input };
    throw new RangeError(`${what}: takes no number`);
  }
  if (typeof input !== "string") throw new RangeError(`${what}: can't take ${input === null ? "null" : typeof input}`);
  if (input === "inherit") return INHERIT;
  if (input === "initial") return INITIAL;
  if (keywordNames) {
    const k = keywordNames[input];
    if (k !== undefined) return { kind: ValueKind.keyword, keyword: k };
  }
  if (input === "auto" && kinds.includes(ValueKind.auto)) return AUTO;
  if (input === "none" && kinds.includes(ValueKind.none)) return NONE;
  if (kinds.includes(ValueKind.string)) return { kind: ValueKind.string, string: input };
  if (kinds.includes(ValueKind.color) && (input.startsWith("#") || input.startsWith("rgb"))) return parseColor(input, what);
  const pct = PERCENT.exec(input);
  if (pct && kinds.includes(ValueKind.percent)) return { kind: ValueKind.percent, number: Number(pct[1]) };
  if (keywordNames)
    throw new RangeError(`${what}: "${input}" is not one of ${Object.keys(keywordNames).map((k) => `"${k}"`).join(", ")}`);
  throw new RangeError(`${what}: "${input}" is not a value it takes`);
}

/** A property's value from sugar (by its camelCase name), as a style or override declares it. */
export function sugarProp(name: string, input: unknown): StyleValue {
  const kinds = props[name];
  if (!kinds) throw new RangeError(`unknown style property "${name}"`);
  return sugarValue(kinds, input, name, keywords[name]);
}

const TOKEN_KINDS = [ValueKind.px, ValueKind.percent, ValueKind.number, ValueKind.color, ValueKind.auto, ValueKind.none];

/** A token's value from sugar: a number is px, a string a color, a percentage, auto or none.
 * (A string token takes string(s), a keyword token keyword(k), a number token number(n).) */
export function sugarToken(input: unknown, what: string): StyleValue {
  if (typeof input === "string" && !(input.startsWith("#") || input.startsWith("rgb") || PERCENT.test(input) || input === "auto" || input === "none"))
    throw new RangeError(`${what}: "${input}" is not a color, a percentage, auto or none (a string token takes string("..."))`);
  const v = sugarValue(TOKEN_KINDS, input, what);
  if (v.kind === ValueKind.inherit || v.kind === ValueKind.initial) throw new RangeError(`${what}: a token can't hold ${input}`);
  return v;
}
