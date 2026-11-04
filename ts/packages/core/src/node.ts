// Nodes: handles allocated here (no round trip to the core), a mirror of the tree for
// destroying subtrees, and the Node API over the wire ops.
//
// Ownership is explicit: destroy() frees a node and its subtree. In debug builds a
// FinalizationRegistry reports subtrees that became unreachable from JS without being
// destroyed, since the app can no longer remove them (docs/typescript.md, "Nodes").

import { EventType, NULL_HANDLE, type Handle, type Property, type Rect } from "@axe/wire";
import { Surface } from "./app.js";
import { commands, debug, on, reportLeak } from "./runtime.js";
import { Style, entries, tokenArrays, unset, type AnyToken, type Overrides, type TokenDecl, type Transitions } from "./style.js";
import { setText, type TextOptions } from "./text.js";

// ---- Handles and the tree ---------------------------------------------------------------
// Per slot, as the C allocator (ui_handle_alloc) does it: generations, a LIFO free list.

const NONE = -1;
const SURFACE = -2; // the parent is a surface
let capacity = 0;
let generation = new Uint32Array(0);
let alive = new Uint8Array(0);
let wrapped = new Uint8Array(0); // the slot's Node object hasn't been collected
let nextFree = new Int32Array(0);
let parentOf = new Int32Array(0);
let firstChild = new Int32Array(0);
let lastChild = new Int32Array(0);
let nextSibling = new Int32Array(0);
let prevSibling = new Int32Array(0);
let used = 0;
let freeHead = NONE;

function grow<T extends Uint8Array | Uint32Array | Int32Array>(a: T, n: number, make: (n: number) => T): T {
  const b = make(n);
  b.set(a);
  return b;
}

function reserve(n: number): void {
  if (n <= capacity) return;
  let cap = Math.max(capacity * 2, 64);
  while (cap < n) cap *= 2;
  generation = grow(generation, cap, (k) => new Uint32Array(k));
  alive = grow(alive, cap, (k) => new Uint8Array(k));
  wrapped = grow(wrapped, cap, (k) => new Uint8Array(k));
  nextFree = grow(nextFree, cap, (k) => new Int32Array(k));
  parentOf = grow(parentOf, cap, (k) => new Int32Array(k));
  firstChild = grow(firstChild, cap, (k) => new Int32Array(k));
  lastChild = grow(lastChild, cap, (k) => new Int32Array(k));
  nextSibling = grow(nextSibling, cap, (k) => new Int32Array(k));
  prevSibling = grow(prevSibling, cap, (k) => new Int32Array(k));
  capacity = cap;
}

const MAX_SLOTS = 1 << 20;

function allocSlot(): number {
  let slot: number;
  if (freeHead !== NONE) {
    slot = freeHead;
    freeHead = nextFree[slot]!;
  } else {
    if (used >= MAX_SLOTS) throw new RangeError("too many nodes (2^20)");
    reserve(used + 1);
    slot = used++;
  }
  generation[slot] = (generation[slot]! + 1) >>> 0 || 1; // 0 is the null handle
  alive[slot] = 1;
  return slot;
}

function freeSlot(slot: number): void {
  alive[slot] = 0;
  nextFree[slot] = freeHead;
  freeHead = slot;
}

function link(slot: number, parent: number, before: number): void {
  parentOf[slot] = parent;
  if (parent < 0) {
    nextSibling[slot] = prevSibling[slot] = NONE;
    return;
  }
  nextSibling[slot] = before;
  prevSibling[slot] = before !== NONE ? prevSibling[before]! : lastChild[parent]!;
  if (prevSibling[slot] !== NONE) nextSibling[prevSibling[slot]!] = slot;
  else firstChild[parent] = slot;
  if (before !== NONE) prevSibling[before] = slot;
  else lastChild[parent] = slot;
}

function unlink(slot: number): void {
  const p = parentOf[slot]!;
  if (p < 0) return;
  const prev = prevSibling[slot]!, next = nextSibling[slot]!;
  if (prev !== NONE) nextSibling[prev] = next;
  else firstChild[p] = next;
  if (next !== NONE) prevSibling[next] = prev;
  else lastChild[p] = prev;
}

// The subtree in pre-order (the order C frees handles in, so both reuse slots alike).
function subtree(slot: number): number[] {
  const out: number[] = [];
  const walk = (s: number) => {
    out.push(s);
    for (let c = firstChild[s]!; c !== NONE; c = nextSibling[c]!) walk(c);
  };
  walk(slot);
  return out;
}

// ---- Leaks (debug builds) -------------------------------------------------------------------

// A subtree is leaked when its root's Node object is collected while the node lives and no
// ancestor's Node object is alive: then nothing can destroy it. A child dropped while its
// parent's object lives isn't: destroying the parent takes it along.
const leaks = debug
  ? new FinalizationRegistry<{ slot: number; generation: number }>(({ slot, generation: gen }) => {
      if (!alive[slot] || generation[slot] !== gen) return;
      wrapped[slot] = 0;
      for (let p = parentOf[slot]!; p >= 0; p = parentOf[p]!) if (wrapped[p]) return;
      const nodes = subtree(slot);
      for (const s of nodes) wrapped[s] = 0;
      reportLeak(slot, gen, nodes.length);
    })
  : null;

// ---- Measurement ----------------------------------------------------------------------------

/** The node is gone: destroyed, or its handle stale. */
export class StaleNodeError extends Error {
  override name = "StaleNodeError";
}

export interface Layout {
  /** Where layout put the node, relative to its parent's border box, and to the surface. */
  readonly layoutInParent: Rect;
  readonly layoutInSurface: Rect;
  /** Where it is drawn (differs from layout while a layout animation runs). */
  readonly presentationInParent: Rect;
  readonly presentationInSurface: Rect;
}

let nextRequest = 1;
const requests = new Map<number, { resolve: (l: Layout) => void; reject: (e: Error) => void; node: Handle }>();

on(EventType.layoutMeasured, (e) => {
  if (e.type !== EventType.layoutMeasured) return;
  const r = requests.get(e.requestId);
  if (!r) return;
  requests.delete(e.requestId);
  if (e.result !== 0) {
    r.reject(new StaleNodeError(`node ${r.node.index}:${r.node.generation} no longer exists`));
    return;
  }
  r.resolve({
    layoutInParent: e.layoutInParent,
    layoutInSurface: e.layoutInSurface,
    presentationInParent: e.presentationInParent,
    presentationInSurface: e.presentationInSurface,
  });
});

// ---- Nodes ----------------------------------------------------------------------------------

export interface NodeOptions {
  /** Styles to apply, in order (later ones win). */
  readonly styles?: readonly Style[];
}

export interface SpringOptions {
  readonly stiffness: number;
  readonly damping: number;
}

/** A node of the scene. Created under a node or a surface; owned by the app until
 * destroy(). */
export class Node {
  readonly handle: Handle;
  readonly #slot: number;
  // The leak registry's token: an object of its own (QuickJS-ng never finalizes a target that
  // is also its own unregister token).
  readonly #lease: { slot: number; generation: number } | null;

  private constructor(parent: Node | Surface, before: Node | null) {
    const parentSlot = parent instanceof Surface ? SURFACE : parent.#checked("create a child of");
    const slot = allocSlot();
    this.#slot = slot;
    this.handle = Object.freeze({ index: slot, generation: generation[slot]! });
    firstChild[slot] = lastChild[slot] = NONE;
    try {
      commands.createNode(this.handle, parent.handle);
    } catch (e) {
      freeSlot(slot);
      throw e;
    }
    link(slot, parentSlot, NONE);
    wrapped[slot] = 1;
    this.#lease = leaks ? { slot, generation: this.handle.generation } : null;
    if (this.#lease) leaks!.register(this, this.#lease, this.#lease);
    if (before) this.moveTo(parent, before);
  }

  /** A new node, last among `parent`'s children (a surface's for a root-level node). */
  static create(parent: Node | Surface, options?: NodeOptions): Node {
    const n = new Node(parent, null);
    if (options?.styles) n.setStyles(...options.styles);
    return n;
  }

  /** False once the node (or an ancestor) was destroyed. */
  get alive(): boolean {
    return alive[this.#slot] === 1 && generation[this.#slot] === this.handle.generation;
  }

  #checked(what: string): number {
    if (!this.alive) throw new StaleNodeError(`can't ${what} node ${this.handle.index}:${this.handle.generation}: it was destroyed`);
    return this.#slot;
  }

  /** Destroys the node and its subtree. Their handles are freed for reuse. */
  destroy(): void {
    const slot = this.#checked("destroy");
    commands.destroyNode(this.handle);
    unlink(slot);
    for (const s of subtree(slot)) freeSlot(s);
    if (this.#lease) leaks!.unregister(this.#lease);
  }

  /** Moves the node (with its subtree) under `parent`, before `before` (a child of `parent`;
   * null: last). Reorders it when the parent stays. */
  moveTo(parent: Node | Surface, before: Node | null = null): void {
    const slot = this.#checked("move");
    let parentSlot = SURFACE;
    if (parent instanceof Node) {
      parentSlot = parent.#checked("move a node under");
      for (let p = parentSlot; p >= 0; p = parentOf[p]!)
        if (p === slot) throw new RangeError("a node can't move under itself or its descendants");
    }
    let beforeSlot = NONE;
    if (before) {
      beforeSlot = before.#checked("move a node before");
      if (beforeSlot === slot || parentOf[beforeSlot] !== parentSlot) throw new RangeError("`before` must be another child of `parent`");
    }
    commands.moveNode(this.handle, parent.handle, before ? before.handle : NULL_HANDLE);
    unlink(slot);
    link(slot, parentSlot, beforeSlot);
  }

  /** Replaces the node's style list. */
  setStyles(...styles: Style[]): void {
    this.#checked("style");
    commands.setStyles(this.handle, styles.map((s) => s.id));
  }

  /** Sets per-node overrides, which win over every style; `unset` removes one. */
  setProps(decls: Overrides, transitions?: Transitions): void {
    this.#checked("set properties of");
    commands.setProps(this.handle, entries(decls, transitions, true));
  }

  /** Sets the node's own token values (`token.with(value)`) for it and its subtree; they win over
   * its styles' (docs/styles.md, "Token scopes"). */
  setTokens(...decls: TokenDecl[]): void {
    this.#checked("set tokens of");
    const { tokens, tokenValues } = tokenArrays(decls);
    commands.setTokenOverrides(this.handle, tokens, tokenValues);
  }

  /** Removes the node's own values of these tokens. */
  unsetTokens(...tokens: AnyToken[]): void {
    this.#checked("unset tokens of");
    commands.setTokenOverrides(this.handle, tokens.map((t) => t.id), tokens.map(() => ({ property: 0, value: unset })));
  }

  /** Sets the states in `mask` to those in `value` (State bits): hover, pressed... */
  setState(mask: number, value: number): void {
    this.#checked("set the state of");
    commands.setState(this.handle, mask, value);
  }

  /** The node's text: a paragraph that wraps to its width (docs/text.md). */
  setText(text: string, options?: TextOptions): void {
    this.#checked("set the text of");
    setText(this.handle, text, options ?? {});
  }

  /** The node's absolute frame in its parent's padding box (logical px). */
  setRect(x: number, y: number, width: number, height: number): void {
    this.#checked("set the rect of");
    commands.setRect(this.handle, x, y, width, height);
  }

  /** The background color (sRGB, straight alpha): an override of background_color. */
  setColor(r: number, g: number, b: number, a = 1): void {
    this.#checked("set the color of");
    commands.setColor(this.handle, r, g, b, a);
  }

  setCornerRadius(radius: number): void {
    this.#checked("set the corner radius of");
    commands.setCornerRadius(this.handle, radius);
  }

  /** Clips the node's text and descendants to its rect. */
  setClip(clip: boolean): void {
    this.#checked("clip");
    commands.setClip(this.handle, clip ? 1 : 0);
  }

  /** Starts or retargets a spring on a property (Property.rect, .color, .cornerRadius),
   * keeping its velocity. */
  spring(property: Property, target: readonly number[], options: SpringOptions): void {
    this.#checked("animate");
    commands.startSpring(this.handle, property, options.stiffness, options.damping, target);
  }

  /** The node's layout and presentation rects, once the frame that applies this request has
   * laid it out. Rejects with StaleNodeError if the node is gone by then. */
  measure(): Promise<Layout> {
    if (!this.alive) return Promise.reject(new StaleNodeError(`node ${this.handle.index}:${this.handle.generation} was destroyed`));
    const id = nextRequest++;
    if (nextRequest > 0xffffffff) nextRequest = 1;
    commands.requestLayout(this.handle, id);
    return new Promise((resolve, reject) => requests.set(id, { resolve, reject, node: this.handle }));
  }
}
