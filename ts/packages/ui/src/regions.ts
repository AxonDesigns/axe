// Where content goes (docs/ui.md, "How it works"). A region is a run of children of one parent
// (a node or a surface): the nodes it holds and nested regions, in order. Dynamic content
// (a binding's result, a branch of Show, a row of For, a component's output) lives in a region of
// its own, so it can be replaced or moved without touching its neighbors.
//
// There are no placeholder nodes: to insert into a region, the node to insert before is found by
// walking forward to the first node of a later entry, then out to the enclosing region. The scene
// is therefore exactly what the same app written with Node calls builds.

import { NO_SLOT, slotCreate, slotDestroy, slotMove, type SlotParent } from "@axe/core/internal";

export { NO_SLOT };

/** @internal The method an element mounts with (elements.ts). Here, in a module that imports
 * nothing of @axe/ui, so classes in any module of the package can be keyed by it. */
export const MOUNT = Symbol("mount");

/** A node in a region. */
class NodeEntry {
  prev: Entry | null = null;
  next: Entry | null = null;
  constructor(
    readonly slot: number,
    readonly up: Region,
  ) {}
}

type Entry = NodeEntry | Region;

export class Region {
  prev: Entry | null = null;
  next: Entry | null = null;
  first: Entry | null = null;
  last: Entry | null = null;
  /** The node after this region, when whoever placed it knows (For, while it mounts a new row
   * among empty ones): saves walking past them. */
  anchor: number | undefined = undefined;

  /** `up`: the enclosing region, or null for a region directly in a parent's children (then
   * `after` is what follows it there: a node, another such region, or NO_SLOT for the end). */
  constructor(
    readonly parent: SlotParent,
    public up: Region | null,
    public after: number | Region = NO_SLOT,
  ) {}

  /** A new region at the end of this one. */
  child(): Region {
    const r = new Region(this.parent, this);
    this.link(r);
    return r;
  }

  /** A new node at the end of this region. */
  createNode(): number {
    const slot = slotCreate(this.parent, nodeAfter(this));
    this.link(new NodeEntry(slot, this));
    return slot;
  }

  /** Records `slot`, a node just created at the end of this region (by a template instance),
   * as its last node. */
  adopt(slot: number): void {
    this.link(new NodeEntry(slot, this));
  }

  /** @internal Appends `e` (a fresh entry of this region). */
  link(e: Entry): void {
    e.prev = this.last;
    e.next = null;
    if (this.last) this.last.next = e;
    else this.first = e;
    this.last = e;
  }

  /** Puts `e`, an entry of this region (linked or not), before `ref` (null: at the end). */
  place(e: Entry, ref: Entry | null): void {
    if (e.next === ref && (ref ? ref.prev === e : this.last === e)) return;
    this.unlinkEntry(e);
    if (!ref) {
      this.link(e);
      return;
    }
    e.prev = ref.prev;
    e.next = ref;
    if (ref.prev) ref.prev.next = e;
    else this.first = e;
    ref.prev = e;
  }

  /** @internal */
  unlinkEntry(e: Entry): void {
    if (!e.prev && !e.next && this.first !== e) return; // not linked
    if (e.prev) e.prev.next = e.next;
    else this.first = e.next;
    if (e.next) e.next.prev = e.prev;
    else this.last = e.prev;
    e.prev = e.next = null;
  }

  /** Destroys every node in the region (nested regions' too) and empties it. */
  clear(): void {
    for (let e = this.first; e; e = e.next) {
      if (e instanceof NodeEntry) slotDestroy(e.slot);
      else e.clear();
    }
    this.first = this.last = null;
  }

  /** Clears the region and takes it out of the enclosing one. */
  remove(): void {
    this.clear();
    this.up?.unlinkEntry(this);
  }

  /** The region's top-level nodes, in order (nested regions' included). */
  nodes(out: number[] = []): number[] {
    for (let e = this.first; e; e = e.next) {
      if (e instanceof NodeEntry) out.push(e.slot);
      else e.nodes(out);
    }
    return out;
  }

  /** Moves the region's nodes, in order, before `before` (NO_SLOT: the end of the parent). */
  moveNodes(before: number): void {
    for (const slot of this.nodes()) slotMove(slot, this.parent, before);
  }
}

/** The first node in `e`, or NO_SLOT if it holds none. */
export function firstNode(e: Entry): number {
  if (e instanceof NodeEntry) return e.slot;
  for (let c = e.first; c; c = c.next) {
    const s = firstNode(c);
    if (s !== NO_SLOT) return s;
  }
  return NO_SLOT;
}

/** The first node after `e` among its parent's children, or NO_SLOT if none follows. */
export function nodeAfter(e: Entry): number {
  for (;;) {
    if (e instanceof Region && e.anchor !== undefined) return e.anchor;
    for (let n = e.next; n; n = n.next) {
      const s = firstNode(n);
      if (s !== NO_SLOT) return s;
    }
    if (e.up) {
      e = e.up;
      continue;
    }
    const after = (e as Region).after;
    if (typeof after === "number") return after;
    const s = firstNode(after);
    if (s !== NO_SLOT) return s;
    e = after;
  }
}
