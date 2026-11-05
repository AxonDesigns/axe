// Elements (docs/ui.md, "Components"): what components return, and how it mounts. An element is
// a description; it creates nodes only where it is inserted, so a component's children mount
// under (and see the context of) the component that places them.
//
// - A Mountable (View, Text, a component) mounts at the end of a region.
// - A function is dynamic content: a binding mounts its result, and mounts it again (in place)
//   whenever something the function read changes.
// - Arrays mount in order; null, undefined and booleans mount nothing.
// - Text goes only in Text (the leaf-text rule): a string in a View is an error pointing there.

import { Node, parseColor, type TextOptions, type TextSpanOptions } from "@axe/core";
import {
  slotSetState,
  slotSetText,
  slotSetTokens,
  slotUnsetTokens,
} from "@axe/core/internal";
import type { Style, TokenDecl, TextAlign, TextDirection, TextSlant, ColorString } from "@axe/core";
import { Owner, dispose, getOwner, handleError, renderEffect, runWithOwner, untrack } from "./reactive.js";
import { MOUNT, Region } from "./regions.js";
import { instanceBegin } from "./hotcore.js";
import { styleBinding, type StyleProp } from "./style.js";
import { themeDefaults } from "./theme.js";

// MOUNT lives in regions.ts, which imports nothing of @axe/ui: modules in an import cycle with
// this one (theme, template) define classes keyed by it while this one may not be evaluated yet.
export { MOUNT };

/** Something that creates nodes where it is inserted. */
export interface Mountable {
  [MOUNT](region: Region): void;
}

declare const JSX_ELEMENT: unique symbol;
/** The type of every JSX expression (TypeScript gives them all one type): a View, a Text, a
 * Span or a component alike. The transform checks what types can't (a node inside a Text, a
 * Span outside one). */
export interface JsxElement {
  readonly [JSX_ELEMENT]: true;
}

/** What a component returns, and what a View holds: nodes and components, lists of them,
 * functions giving them (dynamic content), and nothing (null, undefined, booleans). No text:
 * text goes in Text. */
export type Element = Mountable | JsxElement | null | undefined | boolean | readonly Element[] | (() => Element);

/** What a Text holds: strings, numbers, Spans, lists of them, and functions giving them. */
export type TextContent =
  | string
  | number
  | SpanElement
  | JsxElement
  | null
  | undefined
  | boolean
  | readonly TextContent[]
  | (() => TextContent);

/** A function component: it runs once; props whose values can change are getters (read them
 * where they are used, not when the component starts). */
export type Component<P = object> = (props: P) => Element;

function isMountable(v: unknown): v is Mountable {
  return typeof v === "object" && v !== null && MOUNT in v;
}

const preview = (v: unknown) => {
  const s = String(v);
  return s.length > 24 ? `${s.slice(0, 24)}…` : s;
};

/** Mounts `el` at the end of `region`, under the current owner. */
export function mountInto(region: Region, el: unknown): void {
  if (el === null || el === undefined || typeof el === "boolean") return;
  if (isMountable(el)) {
    el[MOUNT](region);
    return;
  }
  if (typeof el === "function") {
    insert(region.child(), el as () => unknown);
    return;
  }
  if (Array.isArray(el)) {
    for (const x of el) mountInto(region, x);
    return;
  }
  if (typeof el === "string" || typeof el === "number")
    throw new TypeError(`text in a View must be inside <Text>: <Text>${preview(el)}</Text> (docs/ui.md, "Text")`);
  if (el instanceof SpanElement) throw new TypeError("a Span must be inside <Text> (docs/ui.md, \"Text\")");
  throw new TypeError(`can't mount ${typeof el} (${preview(el)}): a component returns elements, lists of them or functions giving them`);
}

/** @internal Dynamic content: a binding that empties `region` and mounts `read()`'s result,
 * now and whenever something `read` read changes. */
export function insert(region: Region, read: () => unknown): void {
  let first = true;
  renderEffect(() => {
    if (!first) region.clear();
    first = false;
    const v = read();
    untrack(() => mountInto(region, v));
  });
}

// ---- Components ---------------------------------------------------------------------------------

/** @internal */
export class ComponentElement<P> implements Mountable {
  constructor(
    readonly fn: Component<P>,
    readonly props: P,
  ) {}

  [MOUNT](region: Region): void {
    const r = region.child();
    const o = new Owner(getOwner());
    o.name = this.fn.name || "Anonymous";
    o.nodes = r;
    AXE_DEV: instanceBegin(o, this.fn, this.props, r);
    runWithOwner(o, () => {
      try {
        mountInto(r, this.fn(this.props));
      } catch (e) {
        dispose(o); // what it mounted before throwing goes, with its cleanups
        handleError(e, o, "a component");
      }
    });
  }
}

/** An element that runs `fn` with `props` where it is mounted, under an owner of its own: what
 * <Card title="x" /> compiles to. */
export function createComponent<P>(fn: Component<P>, props: P): Element {
  return new ComponentElement(fn, props);
}

// ---- Nodes ----------------------------------------------------------------------------------------

/** Props every node takes. Props that can change are getters (the transform writes them). */
export interface NodeProps {
  /** Styles and inline styles, in order: later ones win; inline styles are overrides. */
  readonly style?: StyleProp | (() => StyleProp);
  /** The node's own token declarations for its subtree (token.with(value)). */
  readonly tokens?: readonly TokenDecl[];
  /** State bits (State.hover | State.selected...) the app sets; input sets them from milestone 4. */
  readonly state?: number;
  /** Called with the node once it exists: for measure() and the like. The node belongs to the
   * UI: don't destroy or move it. */
  readonly ref?: (node: Node) => void;
}

export interface ViewProps extends NodeProps {
  readonly children?: Element;
}

// A prop is dynamic when it is a getter (the transform writes props that read signals as
// getters) or holds a function, a list holding one included; anything else applies once, with no
// binding.
function hasFunction(v: unknown): boolean {
  if (typeof v === "function") return true;
  if (Array.isArray(v)) for (const x of v) if (hasFunction(x)) return true;
  return false;
}

/** @internal */
export function dynamicProp(props: object, key: string): boolean {
  const d = Object.getOwnPropertyDescriptor(props, key);
  return d !== undefined && (d.get !== undefined || hasFunction(d.value));
}

function bind(props: object, key: string, fn: () => void): void {
  if (dynamicProp(props, key)) renderEffect(fn);
  else fn();
}

function nodeBindings(slot: number, props: NodeProps, defaults: Style | undefined): void {
  if ("style" in props && dynamicProp(props, "style")) renderEffect(styleBinding(slot, () => props.style, defaults));
  else if (defaults || props.style) styleBinding(slot, () => props.style, defaults)();
  if ("tokens" in props) {
    let declared: readonly TokenDecl[] = [];
    bind(props, "tokens", () => {
      const next = props.tokens ?? [];
      const gone = declared.filter((d) => !next.some((n) => n.token === d.token)).map((d) => d.token);
      if (gone.length) slotUnsetTokens(slot, gone);
      if (next.length) slotSetTokens(slot, next);
      declared = next;
    });
  }
  if ("state" in props) {
    let state = 0;
    bind(props, "state", () => {
      const next = props.state ?? 0;
      if (next !== state) slotSetState(slot, state | next, next);
      state = next;
    });
  }
  if (props.ref) props.ref(Node.adopt(slot));
}

class ViewElement implements Mountable {
  constructor(readonly props: ViewProps) {}

  [MOUNT](region: Region): void {
    const slot = region.createNode();
    nodeBindings(slot, this.props, themeDefaults()?.View);
    const children = this.props.children;
    if (children !== undefined && children !== null) mountInto(new Region(slot, null), children);
  }
}

/** A box: a flex container holding other nodes (docs/layout.md). Text goes in Text. */
export function View(props: ViewProps): Element {
  return new ViewElement(props);
}

// ---- Text -----------------------------------------------------------------------------------------

/** A span's style: anything left out comes from the paragraph. */
export interface SpanProps {
  readonly families?: readonly string[];
  readonly size?: number;
  readonly weight?: number;
  readonly slant?: TextSlant;
  readonly language?: string | null;
  readonly color?: ColorString | readonly [number, number, number, number];
  readonly children?: TextContent;
}

/** @internal */
export class SpanElement {
  constructor(readonly props: SpanProps) {}
}

/** A run of a Text with its own style (bold, a color, another font). Spans don't nest. */
export function Span(props: SpanProps): SpanElement {
  return new SpanElement(props);
}

export interface TextProps extends NodeProps {
  readonly children?: TextContent;
  /** Paragraph options; anything left out comes from the node's styles (fontFamily, fontSize,
   * textColor...) or what it inherits. */
  readonly families?: readonly string[];
  readonly size?: number;
  readonly weight?: number;
  readonly slant?: TextSlant;
  readonly language?: string | null;
  readonly color?: ColorString | readonly [number, number, number, number];
  readonly lineHeight?: number;
  readonly align?: TextAlign;
  readonly direction?: TextDirection;
  readonly maxLines?: number;
  readonly ellipsis?: string | null;
}

const STYLE_KEYS = ["families", "size", "weight", "slant", "language"] as const;
const PARAGRAPH_KEYS = ["lineHeight", "align", "direction", "maxLines", "ellipsis"] as const;

function color(c: SpanProps["color"]): readonly [number, number, number, number] | undefined {
  if (c === undefined) return undefined;
  return typeof c === "string" ? parseColor(c, "color").color : c;
}

export interface Paragraph {
  text: string;
  spans: TextSpanOptions[];
}

/** @internal A Text's content as text and spans. */
export function gatherText(c: unknown, p: Paragraph): void {
  gather(c, p, false);
}

function gather(c: unknown, p: Paragraph, inSpan: boolean): void {
  if (c === null || c === undefined || typeof c === "boolean") return;
  if (typeof c === "string") p.text += c;
  else if (typeof c === "number") p.text += String(c);
  else if (typeof c === "function") gather((c as () => unknown)(), p, inSpan);
  else if (Array.isArray(c)) for (const x of c) gather(x, p, inSpan);
  else if (c instanceof SpanElement) {
    if (inSpan) throw new TypeError("Spans don't nest: close one before opening the next");
    const start = p.text.length;
    gather(c.props.children, p, true);
    if (p.text.length > start) {
      const o: Record<string, unknown> = { start, end: p.text.length };
      for (const k of STYLE_KEYS) if (c.props[k] !== undefined) o[k] = c.props[k];
      if (c.props.color !== undefined) o["color"] = color(c.props.color);
      p.spans.push(o as unknown as TextSpanOptions);
    }
  } else if (isMountable(c)) throw new TypeError("a Text holds only text and Spans: put other nodes beside it, in a View");
  else throw new TypeError(`a Text can't hold ${typeof c}`);
}

/** @internal A Text's options from its props (read inside its binding, so getters track). */
export function textOptions(props: TextProps, spans: TextSpanOptions[]): TextOptions {
  const o: Record<string, unknown> = {};
  for (const k of STYLE_KEYS) if (props[k] !== undefined) o[k] = props[k];
  for (const k of PARAGRAPH_KEYS) if (props[k] !== undefined) o[k] = props[k];
  if (props.color !== undefined) o["color"] = color(props.color);
  if (spans.length) o["spans"] = spans;
  return o as TextOptions;
}

function setParagraph(slot: number, props: TextProps): void {
  const p: Paragraph = { text: "", spans: [] };
  gather(props.children, p, false);
  slotSetText(slot, p.text, textOptions(props, p.spans));
}

// A Text whose props are plain values and whose children hold no function: one set_text.
function staticText(props: TextProps): boolean {
  for (const k in props) {
    if (k === "style" || k === "tokens" || k === "state" || k === "ref") continue;
    if (k === "children" ? dynamicChildren(props, "children") : dynamicProp(props, k)) return false;
  }
  return true;
}

function dynamicChildren(owner: object, key: string): boolean {
  const d = Object.getOwnPropertyDescriptor(owner, key);
  if (!d) return false;
  if (d.get) return true;
  return dynamicContent(d.value);
}

function dynamicContent(c: unknown): boolean {
  if (typeof c === "function") return true;
  if (Array.isArray(c)) return c.some(dynamicContent);
  if (c instanceof SpanElement) {
    for (const k in c.props) if (k === "children" ? dynamicChildren(c.props, k) : dynamicProp(c.props, k)) return true;
  }
  return false;
}

class TextElement implements Mountable {
  constructor(readonly props: TextProps) {}

  [MOUNT](region: Region): void {
    const slot = region.createNode();
    const props = this.props;
    nodeBindings(slot, props, themeDefaults()?.Text);
    if (staticText(props)) setParagraph(slot, props);
    else renderEffect(() => setParagraph(slot, props));
  }
}

/** A paragraph: its children are its text (strings, numbers, Spans, and functions giving them),
 * one set_text however many parts it has. */
export function Text(props: TextProps): Element {
  return new TextElement(props);
}
