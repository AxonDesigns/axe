// @axe/ui/jsx-runtime: what TSX compiles to (docs/ui.md, "How TSX compiles"), and the JSX types
// TypeScript checks TSX with (tsconfig: "jsx": "preserve", "jsxImportSource": "@axe/ui").
// Apps don't call these: axe-tsx writes them.

import type { JsxElement, Element, SpanElement } from "./elements.js";
import { insert as insertDynamic, mountInto, textOptions, gatherText, type Paragraph, type TextProps } from "./elements.js";
import { renderEffect } from "./reactive.js";
import { Region } from "./regions.js";
import { styleBinding } from "./style.js";
import { themeDefaults } from "./theme.js";
import { Node } from "@axe/core";
import { slotSetState, slotSetText, slotSetTokens, slotUnsetTokens } from "@axe/core/internal";
import type { TokenDecl, TextOptions } from "@axe/core";

export { template, instance, type Template, type TemplateNode } from "./template.js";
export { Span, createComponent } from "./elements.js";
export { mergeProps } from "./props.js";

/** A node's style, bound: set_styles and set_props whenever what `read` reads changes. `text`:
 * the node is a Text (its theme default differs). */
export function bindStyle(slot: number, read: () => unknown, text?: 1): void {
  const d = themeDefaults();
  renderEffect(styleBinding(slot, read, text ? d?.Text : d?.View));
}

/** A node's style that isn't constant at module level but doesn't change (a local style). */
export function applyStyle(slot: number, value: unknown, text?: 1): void {
  const d = themeDefaults();
  styleBinding(slot, () => value, text ? d?.Text : d?.View)();
}

/** A node's token declarations, bound. */
export function bindTokens(slot: number, read: () => readonly TokenDecl[] | undefined): void {
  let declared: readonly TokenDecl[] = [];
  renderEffect(() => {
    const next = read() ?? [];
    const gone = declared.filter((d) => !next.some((n) => n.token === d.token)).map((d) => d.token);
    if (gone.length) slotUnsetTokens(slot, gone);
    if (next.length) slotSetTokens(slot, next);
    declared = next;
  });
}

/** A node's state bits, bound. */
export function bindState(slot: number, read: () => number | undefined): void {
  let state = 0;
  renderEffect(() => {
    const next = read() ?? 0;
    if (next !== state) slotSetState(slot, state | next, next);
    state = next;
  });
}

/** A Text's paragraph, bound: one set_text whenever any part or option changes. */
export function bindText(slot: number, read: () => TextProps): void {
  renderEffect(() => {
    const props = read();
    const p: Paragraph = { text: "", spans: [] };
    gatherText(props.children, p);
    slotSetText(slot, p.text, textOptions(props, p.spans) as TextOptions);
  });
}

/** A Text's paragraph that doesn't change but isn't the same for every instance. */
export function applyText(slot: number, props: TextProps): void {
  const p: Paragraph = { text: "", spans: [] };
  gatherText(props.children, p);
  slotSetText(slot, p.text, textOptions(props, p.spans) as TextOptions);
}

/** Calls a ref with the node. */
export function ref(slot: number, fn: (node: Node) => void): void {
  fn(Node.adopt(slot));
}

/** A region for dynamic children of a template's node: before `after` (the next static child's
 * slot, the next region, or null for the end). */
export function region(parent: number, after: number | Region | null): Region {
  return new Region(parent, null, after ?? -1);
}

/** Mounts `value` in `region`: a function is dynamic content (mounted again when what it reads
 * changes); anything else mounts once. */
export function insert(region: Region, value: unknown): void {
  if (typeof value === "function") insertDynamic(region, value as () => unknown);
  else mountInto(region, value);
}

// ---- JSX types --------------------------------------------------------------------------------

export declare namespace JSX {
  /** Every JSX expression's type. */
  type Element = JsxElement;
  /** What a tag may name: a component returning elements (View, Text, Card...), or a Span. */
  type ElementType = (props: never) => import("./elements.js").Element | SpanElement;
  interface ElementChildrenAttribute {
    children: object;
  }
  interface ElementAttributesProperty {
    props: object;
  }
  // Tags are components (View, Text, Span...): there are no lowercase intrinsic elements.
  // eslint-disable-next-line @typescript-eslint/no-empty-interface
  interface IntrinsicElements {}
}

export type { Element as UiElement };
