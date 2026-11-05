// Templates (docs/templates.md): the static part of a TSX subtree (its nodes, constant styles,
// inline styles, token declarations, states and static text) defined once in the core and
// instantiated with one op. The transform writes `template([...])` at module level and an
// `instance(t, setup)` where the subtree is used; `setup` gets the instance's node slots and
// attaches the dynamic parts (bindings, regions) to them.
//
// A template's style lists include the theme's default styles, so a template is defined once per
// theme in effect where it is instantiated (a variant). A theme's variant is destroyed once no
// instance and no ThemeProvider of that theme uses it; the variant without a theme stays.

import { CommandWriter, NULL_HANDLE, WIRE_VERSION, type Handle } from "@axe/wire";
import { NO_SLOT, commands, setText, slotAllocTree, slotHandle, slotInstantiate, slotParentHandle, tokenArrays } from "@axe/core/internal";
import type { TextOptions, TokenDecl } from "@axe/core";
import { gatherText, textOptions, type Mountable, type Paragraph, type TextProps } from "./elements.js";
import { getOwner, onCleanup } from "./reactive.js";
import { MOUNT, nodeAfter, type Region } from "./regions.js";
import { collect } from "./style.js";
import { useTheme, type Theme, type ThemeDefaults } from "./theme.js";

/** One node of a template. */
export interface TemplateNode {
  /** Its parent's index in the template; -1 for a root. */
  readonly p: number;
  /** 1 for a Text node. */
  readonly t?: 1;
  /** 1 when its style is bound at run time: the template leaves its style list out. */
  readonly sb?: 1;
  /** Its static props (read once, when a variant is defined): style, tokens, state, and for a
   * Text its paragraph options and, if static, its children. */
  readonly props?: () => TemplateProps;
}

export type TemplateProps = Omit<TextProps, "ref">;

interface Variant {
  id: number; // 0: plain ops (the template doesn't fit one op)
  refs: number;
  theme: Theme | null;
}

export class Template {
  parents: Int16Array;
  readonly variants = new Map<Theme | null, Variant>();

  constructor(public nodes: readonly TemplateNode[]) {
    this.parents = Int16Array.from(nodes, (n) => n.p);
  }
}

/** A template (module level; defined in the core when first instantiated). */
export function template(nodes: readonly TemplateNode[]): Template {
  if (nodes.length < 1 || nodes.length > 1024) throw new RangeError("a template has 1 to 1024 nodes");
  return new Template(nodes);
}

let nextId = 1;
const LOCAL_GENERATION = 1;
const MAX_OPS = 65532 - 4 - 12;

const local = (i: number): Handle => ({ index: i, generation: LOCAL_GENERATION });

// The template's ops into `w`, naming node i `h(i)`; roots under `root` (and, for plain ops,
// moved before `before`).
function writeOps(w: CommandWriter, t: Template, defaults: ThemeDefaults | undefined, h: (i: number) => Handle,
                  root: Handle, before: Handle | null): void {
  for (let i = 0; i < t.nodes.length; i++) {
    const n = t.nodes[i]!;
    const node = h(i);
    w.createNode(node, n.p < 0 ? root : h(n.p));
    if (n.p < 0 && before) w.moveNode(node, root, before);
    const props: TemplateProps = n.props ? n.props() : {};
    if (!n.sb) {
      const fallback = n.t ? defaults?.Text : defaults?.View;
      const ids: number[] = fallback ? [fallback.id] : [];
      const inline: { property: number; value: never }[] = [];
      collect(props.style, ids, inline as never);
      if (ids.length) w.setStyles(node, ids);
      if (inline.length) w.setProps(node, inline);
    }
    if (props.tokens?.length) {
      const { tokens, tokenValues } = tokenArrays(props.tokens as TokenDecl[]);
      w.setTokenOverrides(node, tokens, tokenValues);
    }
    if (props.state) w.setState(node, props.state, props.state);
    if (n.t && "children" in props) {
      const p: Paragraph = { text: "", spans: [] };
      gatherText(props.children, p);
      setText(node, p.text, textOptions(props, p.spans) as TextOptions, w);
    }
  }
}

function variantOps(t: Template, theme: Theme | null): Uint8Array {
  const w = new CommandWriter();
  writeOps(w, t, theme?.components, local, NULL_HANDLE, null);
  const bytes = w.finish(WIRE_VERSION);
  return bytes ? bytes.subarray(16) : new Uint8Array(0);
}

function variantOf(t: Template, theme: Theme | null): Variant {
  let v = t.variants.get(theme);
  if (v) return v;
  const ops = variantOps(t, theme);
  v = { id: 0, refs: 0, theme };
  if (ops.length <= MAX_OPS) {
    v.id = nextId++;
    commands.defineTemplate(v.id, t.nodes.length, ops);
  }
  t.variants.set(theme, v);
  if (theme) (themeVariants.get(theme) ?? themeVariants.set(theme, new Set()).get(theme)!).add({ t, v });
  return v;
}

/** @internal Development only: `t`'s new nodes, under the ids of its variants. */
export function redefineTemplate(t: Template, nodes: readonly TemplateNode[]): void {
  t.nodes = nodes;
  t.parents = Int16Array.from(nodes, (n) => n.p);
  for (const v of t.variants.values()) {
    const ops = variantOps(t, v.theme);
    if (ops.length > MAX_OPS) {
      if (v.id) commands.destroyTemplate(v.id);
      v.id = 0;
    } else {
      if (!v.id) v.id = nextId++;
      commands.defineTemplate(v.id, nodes.length, ops);
    }
  }
}

/** @internal Development only: destroys the variants of `t` no instance uses. They come back
 * when one is instantiated. */
export function dropUnusedVariants(t: Template): void {
  for (const v of [...t.variants.values()]) {
    if (v.refs > 0) continue;
    if (v.id) commands.destroyTemplate(v.id);
    t.variants.delete(v.theme);
    const set = v.theme ? themeVariants.get(v.theme) : undefined;
    if (set) for (const e of set) if (e.v === v) set.delete(e);
  }
}

// Variants of each theme, and how many ThemeProviders of it are mounted.
const themeVariants = new Map<Theme, Set<{ t: Template; v: Variant }>>();
const providers = new Map<Theme, number>();

function release(t: Template, v: Variant): void {
  if (!v.theme || v.refs > 0 || (providers.get(v.theme) ?? 0) > 0) return;
  if (v.id) commands.destroyTemplate(v.id);
  t.variants.delete(v.theme);
  const set = themeVariants.get(v.theme);
  if (set) for (const e of set) if (e.v === v) set.delete(e);
}

/** @internal ThemeProvider's: its theme's variants live while a provider of it is mounted. */
export function provideTheme(theme: Theme): void {
  providers.set(theme, (providers.get(theme) ?? 0) + 1);
  onCleanup(() => {
    const n = (providers.get(theme) ?? 1) - 1;
    if (n > 0) {
      providers.set(theme, n);
      return;
    }
    providers.delete(theme);
    for (const e of [...(themeVariants.get(theme) ?? [])]) release(e.t, e.v);
  });
}

class Instance implements Mountable {
  constructor(
    readonly t: Template,
    readonly setup: ((nodes: number[]) => void) | undefined,
  ) {}

  [MOUNT](region: Region): void {
    const t = this.t;
    const theme = useTheme();
    const v = variantOf(t, theme);
    const before = nodeAfter(region);
    let slots: number[];
    if (v.id) {
      slots = slotInstantiate(v.id, t.parents, region.parent, before);
    } else {
      // Too large for one op: the same ops, with the instance's handles.
      slots = slotAllocTree(t.parents, region.parent, before);
      writeOps(commands, t, theme?.components, (i) => slotHandle(slots[i]!), slotParentHandle(region.parent),
               before === NO_SLOT ? null : slotHandle(before));
    }
    for (let i = 0; i < slots.length; i++) if (t.parents[i]! < 0) region.adopt(slots[i]!);
    // Only a theme's variant is ever released: the others need no count (nor a cleanup per
    // instance), except for hot reload, which destroys what no instance uses.
    if ((AXE_DEV || v.theme) && getOwner()) {
      v.refs++;
      onCleanup(() => {
        v.refs--;
        release(t, v);
      });
    }
    this.setup?.(slots);
  }
}

/** An instance of `t`, mounted where it is inserted; `setup` gets its nodes' slots. */
export function instance(t: Template, setup?: (nodes: number[]) => void): Mountable {
  return new Instance(t, setup);
}

