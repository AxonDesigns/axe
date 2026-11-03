// Styles, tokens and style values (docs/styles.md). Values are plain objects built by the
// constructors below; styles and tokens are defined once and referenced by id, so applying
// one costs one op. The value types per property are generated from idl/wire.idl.

import {
  StyleProp,
  ValueKind,
  styleTokenValueValid,
  styleValueValid,
  transitionOf,
  type AutoValue,
  type ColorValue,
  type InheritValue,
  type InitialValue,
  type KeywordValue,
  type NoneValue,
  type NumberValue,
  type PercentValue,
  type PxValue,
  type SpringValue,
  type StringValue,
  type StylePropName,
  type StylePropValues,
  type StyleValue,
  type TokenValue,
  type TransitionPropName,
  type UnsetValue,
} from "@axe/wire";
import { commands } from "./runtime.js";

// ---- Values ---------------------------------------------------------------------------------

/** Logical pixels. */
export const px = (n: number): PxValue => ({ kind: ValueKind.px, number: n });
export const percent = (n: number): PercentValue => ({ kind: ValueKind.percent, number: n });
export const number = (n: number): NumberValue => ({ kind: ValueKind.number, number: n });
/** sRGB, straight alpha, each component in [0, 1]. */
export const rgba = (r: number, g: number, b: number, a = 1): ColorValue => ({ kind: ValueKind.color, color: [r, g, b, a] });
/** A keyword: one of the generated enums' values (FlexDirection.column, TextAlign.center). */
export const keyword = (k: number): KeywordValue => ({ kind: ValueKind.keyword, keyword: k });
export const string = (s: string): StringValue => ({ kind: ValueKind.string, string: s });
/** A transition: a spring of this stiffness and damping (mass 1). */
export const spring = (stiffness: number, damping: number): SpringValue => ({ kind: ValueKind.spring, stiffness, damping });
export const auto: AutoValue = Object.freeze({ kind: ValueKind.auto });
export const none: NoneValue = Object.freeze({ kind: ValueKind.none });
/** The parent's computed value. */
export const inherit: InheritValue = Object.freeze({ kind: ValueKind.inherit });
/** The property's initial value. */
export const initial: InitialValue = Object.freeze({ kind: ValueKind.initial });
/** In a node's overrides: removes the override. */
export const unset: UnsetValue = Object.freeze({ kind: ValueKind.unset });

/** What a token may hold. */
export type TokenType = PxValue | PercentValue | NumberValue | ColorValue | KeywordValue | AutoValue | NoneValue | StringValue;

let nextTokenId = 1;

/** A theme token: a value styles refer to by id. Setting it updates every node using it, with
 * their transitions. */
export class Token<V extends TokenType = TokenType> {
  readonly id: number;
  #value: V | Token<V> | null;

  private constructor(value: V | Token<V>) {
    this.id = nextTokenId++;
    this.#value = value;
    write(this.id, value);
  }

  /** A new token holding `value` (or referring to another token). */
  static create<V extends TokenType>(value: V | Token<V>): Token<V> {
    return new Token<V>(value);
  }

  get value(): V | Token<V> | null {
    return this.#value;
  }

  set(value: V | Token<V>): void {
    write(this.id, value);
    this.#value = value;
  }

  /** Removes the token: declarations using it count as absent until it is set again. */
  unset(): void {
    commands.setToken(this.id, { property: 0, value: unset });
    this.#value = null;
  }
}

function write(id: number, value: TokenType | Token): void {
  const v = valueOf(value);
  if (!styleTokenValueValid(v)) throw new RangeError(`Token ${id}: a token can't hold that value`);
  commands.setToken(id, { property: 0, value: v });
}

function valueOf(v: StyleValue | Token): StyleValue {
  return v instanceof Token ? { kind: ValueKind.token, token: v.id } : v;
}

// ---- Declarations -----------------------------------------------------------------------------

type CssWide = InheritValue | InitialValue;
type Tokenized<V> = V | (V extends TokenType ? Token<V> : never) | CssWide;

/** Declarations: each property with the values it takes, a token of such a value, inherit or
 * initial. Order matters only between a logical property and its physical side (the later
 * one wins), as within a CSS block. */
export type Decls = { readonly [K in keyof StylePropValues]?: Tokenized<StylePropValues[K]> };
/** Per-node overrides: as Decls, plus unset to remove one. */
export type Overrides = { readonly [K in keyof StylePropValues]?: Tokenized<StylePropValues[K]> | UnsetValue };
/** Transitions: a spring, or none to turn one off. `layout` animates the node's layout rect. */
export type Transitions = { readonly [K in TransitionPropName]?: SpringValue | NoneValue | CssWide | UnsetValue };

export interface StyleEntry {
  readonly property: number;
  readonly value: StyleValue;
}

/** Wire entries of declarations and transitions, checked as the core checks them. */
export function entries(decls: Overrides | undefined, transitions: Transitions | undefined, override: boolean): StyleEntry[] {
  const out: StyleEntry[] = [];
  const add = (property: number, name: string, v: StyleValue | Token) => {
    const value = valueOf(v);
    if (!styleValueValid(property, value, override)) throw new RangeError(`${name}: invalid value ${JSON.stringify(value)}`);
    out.push({ property, value });
  };
  for (const [name, v] of Object.entries(decls ?? {})) {
    const id = StyleProp[name as StylePropName];
    if (id === undefined) throw new RangeError(`unknown style property "${name}"`);
    if (v !== undefined) add(id, name, v as StyleValue | Token);
  }
  for (const [name, v] of Object.entries(transitions ?? {})) {
    const id = StyleProp[name as StylePropName];
    if (id === undefined) throw new RangeError(`unknown style property "${name}"`);
    if (v !== undefined) add(transitionOf(id), `transition of ${name}`, v as StyleValue);
  }
  return out;
}

// ---- Styles ---------------------------------------------------------------------------------

export interface StyleBlock {
  readonly decls?: Decls;
  readonly transitions?: Transitions;
}

export interface StyleDesc extends StyleBlock {
  /** Blocks that apply while the node has every state in `when` (State bits). The most
   * specific (most states) win, then the later style, then the later block. */
  readonly states?: readonly (StyleBlock & { readonly when: number })[];
}

let nextStyleId = 1;

/** An interned style: defined once, applied to nodes by id (node.setStyles). */
export class Style {
  readonly id: number;
  #destroyed = false;

  private constructor(desc: StyleDesc) {
    this.id = nextStyleId++;
    define(this.id, desc);
  }

  static define(desc: StyleDesc): Style {
    return new Style(desc);
  }

  /** Changes the style: every node using it is resolved again. */
  redefine(desc: StyleDesc): void {
    if (this.#destroyed) throw new Error(`Style ${this.id} was destroyed`);
    define(this.id, desc);
  }

  /** Removes the style: nodes listing it resolve as if it weren't there. */
  destroy(): void {
    if (this.#destroyed) return;
    commands.destroyStyle(this.id);
    this.#destroyed = true;
  }
}

function define(id: number, desc: StyleDesc): void {
  const blocks = [{ states: 0, entries: entries(desc.decls, desc.transitions, false) }];
  for (const b of desc.states ?? []) {
    if (!(b.when > 0 && b.when <= 0xff && Number.isInteger(b.when))) throw new RangeError("a state block needs state bits (State)");
    blocks.push({ states: b.when, entries: entries(b.decls, b.transitions, false) });
  }
  commands.defineStyle(id, blocks);
}
