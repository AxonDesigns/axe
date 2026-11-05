// Control flow (docs/ui.md, "Control flow"): Show, Switch and Match, Dynamic, ErrorBoundary,
// For and Index. Each branch or row has an owner of its own, so replacing it disposes exactly
// what it created.

import { ComponentElement, createComponent, mountInto, type Component, type Element, type Mountable } from "./elements.js";
import { Owner, computed, dispose, disposeOwned, getOwner, handleError, renderEffect, runWith2, runWithOwner, signal, untrack, type Signal } from "./reactive.js";
import { MOUNT, NO_SLOT, Region, firstNode, nodeAfter } from "./regions.js";
import { splitProps } from "./props.js";
import { instanceBegin } from "./hotcore.js";

type Falsy = false | null | undefined | 0 | "";

/** A branch: elements, or a function of an accessor of the value that selected it. */
export type Branch<T> = Element | ((value: () => T) => Element);

function branch<T>(children: Branch<T>, value: () => T): Element {
  return typeof children === "function" && children.length > 0
    ? untrack(() => (children as (value: () => T) => Element)(value))
    : (children as Element);
}

// ---- Show -----------------------------------------------------------------------------------------

export interface ShowProps<T> {
  readonly when: T | Falsy;
  readonly fallback?: Element;
  /** With keyed, a new truthy value mounts the branch again; without, only a change between
   * truthy and falsy does, and the branch reads the value through its accessor. */
  readonly keyed?: boolean;
  readonly children: Branch<NonNullable<T>>;
}

/** Mounts its children while `when` is truthy, `fallback` otherwise. */
export function Show<T>(props: ShowProps<T>): Element {
  const keyed = !!props.keyed;
  const cond = computed(() => props.when, keyed ? undefined : { equals: (a, b) => !a === !b });
  return () => {
    const v = cond();
    if (!v) return props.fallback;
    return branch(props.children, keyed ? () => v as NonNullable<T> : () => props.when as NonNullable<T>);
  };
}

// ---- Switch and Match ---------------------------------------------------------------------------

export interface MatchProps<T> {
  readonly when: T | Falsy;
  readonly children: Branch<NonNullable<T>>;
}

/** @internal */
export class MatchElement<T> {
  constructor(readonly props: MatchProps<T>) {}
}

/** A case of a Switch. */
export function Match<T>(props: MatchProps<T>): Element {
  return new MatchElement(props) as unknown as Element;
}

export interface SwitchProps {
  readonly fallback?: Element;
  readonly children: Element;
}

function matches(children: unknown, out: MatchElement<unknown>[]): MatchElement<unknown>[] {
  if (children instanceof MatchElement) out.push(children);
  else if (children instanceof ComponentElement && children.fn === Match) out.push(new MatchElement(children.props as MatchProps<unknown>));
  else if (Array.isArray(children)) for (const c of children) matches(c, out);
  else if (typeof children === "function") matches((children as () => unknown)(), out);
  else if (children !== null && children !== undefined && typeof children !== "boolean")
    throw new TypeError("a Switch holds only Match elements");
  return out;
}

/** Mounts the first Match whose `when` is truthy, or `fallback`. A branch mounts again only when
 * another Match is selected. */
export function Switch(props: SwitchProps): Element {
  const list = computed(() => matches(props.children, []));
  const selected = computed(() => {
    const ms = list();
    for (let i = 0; i < ms.length; i++) if (ms[i]!.props.when) return ms[i]!;
    return null;
  });
  return () => {
    const m = selected();
    if (!m) return props.fallback;
    return branch(m.props.children, () => m.props.when as NonNullable<unknown>);
  };
}

// ---- Dynamic --------------------------------------------------------------------------------------

export type DynamicProps<P> = P & { readonly component: Component<P> | Falsy };

/** Mounts `component` with the other props, again whenever `component` changes. */
export function Dynamic<P extends object>(props: DynamicProps<P>): Element {
  const comp = computed(() => props.component);
  const [, rest] = splitProps(props, ["component"]);
  return () => {
    const c = comp();
    return c ? createComponent(c as Component<P>, rest as unknown as P) : null;
  };
}

// ---- ErrorBoundary ------------------------------------------------------------------------------

export interface ErrorBoundaryProps {
  /** Mounted instead of the children after one of them threw: elements, or a function of the
   * error and of `reset`, which mounts the children again. */
  readonly fallback: Element | ((error: unknown, reset: () => void) => Element);
  readonly children: Element;
}

const NO_ERROR = Symbol("no error");

/** Takes errors thrown below it (components, bindings, effects, cleanups) and shows `fallback`
 * instead of its children. An error in the fallback goes to the next boundary up. */
export function ErrorBoundary(props: ErrorBoundaryProps): Element {
  const error = signal<unknown>(NO_ERROR, { equals: false });
  let showingFallback = false;
  const owner = getOwner();
  if (owner) {
    owner.onError = (e) => {
      if (showingFallback) throw e;
      if (error.peek() === NO_ERROR) error.set(e); // the first one wins until reset
    };
  }
  const reset = () => error.set(NO_ERROR);
  return () => {
    const e = error();
    if (e === NO_ERROR) {
      showingFallback = false;
      return untrack(() => props.children);
    }
    showingFallback = true;
    const f = props.fallback;
    return typeof f === "function" ? untrack(() => f(e, reset)) : f;
  };
}

// ---- For ------------------------------------------------------------------------------------------

export interface ForProps<T> {
  readonly each: readonly T[] | Falsy;
  /** A row's identity: the item itself by default. */
  readonly key?: (item: T) => unknown;
  readonly fallback?: Element;
  /** Mounts a row: called once per item, with the row's index as an accessor. */
  readonly children: (item: T, index: () => number) => Element;
}

// A row of For is its own owner, carrying what the row needs: one object per row, not three.
class Row<T> extends Owner {
  old = -1; // its position in the previous list
  constructor(
    parent: Owner | null,
    readonly region: Region,
    readonly key: unknown,
    public item: T,
    public index: Signal<number> | null,
    readonly children: ForProps<T>["children"],
  ) {
    super(parent);
    this.nodes = region;
  }
}

/** Indices (into `seq`) of a longest strictly increasing subsequence of `seq`, ignoring -1. */
export function longestIncreasing(seq: readonly number[]): Set<number> {
  const tails: number[] = []; // index into seq of the smallest tail of each length
  const prev = new Int32Array(seq.length).fill(-1);
  for (let i = 0; i < seq.length; i++) {
    const v = seq[i]!;
    if (v < 0) continue;
    let lo = 0, hi = tails.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (seq[tails[mid]!]! < v) lo = mid + 1;
      else hi = mid;
    }
    if (lo > 0) prev[i] = tails[lo - 1]!;
    tails[lo] = i;
  }
  const out = new Set<number>();
  for (let i = tails.length ? tails[tails.length - 1]! : -1; i >= 0; i = prev[i]!) out.add(i);
  return out;
}

// A row's content, under its owner. A row that is one component runs it right there, in the
// row's owner and region: a component of its own would add an owner and a region per row.
function mountRowContent<T>(row: Row<T>, i: number): void {
  try {
    const el = row.children(row.item, row.index ?? (() => i));
    if (el instanceof ComponentElement) {
      row.name = el.fn.name || "Anonymous";
      AXE_DEV: instanceBegin(row, el.fn, el.props, row.region, row);
      mountInto(row.region, el.fn(el.props));
    } else {
      AXE_DEV: instanceBegin(row, null, undefined, row.region, row);
      mountInto(row.region, el);
    }
  } catch (e) {
    row.region.clear();
    disposeOwned(row);
    handleError(e, row, "a row of For");
  }
}

class ForElement<T> implements Mountable {
  constructor(readonly props: ForProps<T>) {}

  [MOUNT](region: Region): void {
    const props = this.props;
    const owner = getOwner();
    const rowsRegion = region.child();
    const fallbackRegion = region.child();
    let fallback: Owner | null = null;
    let rows: Row<T>[] = [];
    const wantsIndex = props.children.length > 1;

    // A row's region goes in place first (empty); its content is mounted later, front to back.
    const newRow = (item: T, key: unknown, i: number, before: Region | null): Row<T> => {
      const r = new Region(rowsRegion.parent, rowsRegion);
      rowsRegion.place(r, before);
      return new Row(owner, r, key, item, wantsIndex ? signal(i) : null, props.children);
    };
    const mountRow = (row: Row<T>, i: number): void => {
      runWith2(row, mountRowContent, row, i);
    };
    renderEffect(() => {
      const items = props.each || [];
      const keyOf = props.key;
      untrack(() => {
        // Old rows by key, in order (keys may repeat: they pair up in order).
        const byKey = new Map<unknown, Row<T>[]>();
        rows.forEach((r, i) => {
          r.old = i;
          const list = byKey.get(r.key);
          if (list) list.push(r);
          else byKey.set(r.key, [r]);
        });
        const next: (Row<T> | null)[] = new Array(items.length);
        const keys: unknown[] = new Array(items.length);
        let kept = 0;
        for (let i = 0; i < items.length; i++) {
          const k = keyOf ? keyOf(items[i]!) : items[i];
          keys[i] = k;
          const list = byKey.size ? byKey.get(k) : undefined;
          const row = list?.length ? list.shift()! : null;
          next[i] = row;
          if (row) kept++;
        }
        // Rows left over are gone.
        for (const list of byKey.values()) for (const r of list) dispose(r);
        if (items.length && fallback) {
          dispose(fallback);
          fallback = null;
        }
        // Rows kept in place: a longest run whose old positions increase.
        const keep = kept ? longestIncreasing(next.map((r) => (r ? r.old : -1))) : null;
        // From the end: each kept row goes right before the rows after it, which are in place
        // (`following` is their first node); a new row's empty region takes its place, and
        // remembers the node it will go before.
        const out: Row<T>[] = new Array(items.length);
        const anchors: number[] | null = kept < items.length ? new Array<number>(items.length) : null;
        let following = nodeAfter(rowsRegion);
        let after: Region | null = null;
        for (let i = items.length - 1; i >= 0; i--) {
          let row = next[i];
          if (!row) {
            row = newRow(items[i]!, keys[i], i, after);
            anchors![i] = following;
          } else {
            rowsRegion.place(row.region, after);
            if (!keep!.has(i)) row.region.moveNodes(following);
            row.item = items[i]!;
            row.index?.set(i);
            const first = firstNode(row.region);
            if (first !== NO_SLOT) following = first;
          }
          out[i] = row;
          after = row.region;
        }
        // New rows' content, front to back, each before its anchor: rows added at the end, the
        // first mount included, need no moves.
        if (anchors) {
          for (let i = 0; i < items.length; i++) {
            if (next[i]) continue;
            const r = out[i]!.region;
            r.anchor = anchors[i]!;
            mountRow(out[i]!, i);
            r.anchor = undefined;
          }
        }
        rows = out;
        // The fallback while there are no rows.
        if (items.length === 0 && props.fallback !== undefined && !fallback) {
          fallback = new Owner(owner);
          const fb = fallback;
          const r = fallbackRegion.child();
          fb.nodes = r;
          runWithOwner(fb, () => mountInto(r, props.fallback));
        }
      });
    });
  }
}

/** Mounts a row per item, keyed by item (or `key`): when the list changes, rows of items that
 * stay are kept, with their state, and moved with the fewest moves (a longest increasing
 * subsequence of their old positions stays put), so layout animations play; rows of new items
 * are mounted, those of items that went are disposed. */
export function For<T>(props: ForProps<T>): Element {
  return new ForElement(props);
}

// ---- Index ----------------------------------------------------------------------------------------

export interface IndexProps<T> {
  readonly each: readonly T[] | Falsy;
  readonly fallback?: Element;
  /** Mounts the row at an index: called once per index, with the item there as an accessor. */
  readonly children: (item: () => T, index: number) => Element;
}

class IndexElement<T> implements Mountable {
  constructor(readonly props: IndexProps<T>) {}

  [MOUNT](region: Region): void {
    const props = this.props;
    const owner = getOwner();
    const rowsRegion = region.child();
    const fallbackRegion = region.child();
    let fallback: Owner | null = null;
    const rows: { owner: Owner; item: Signal<T> }[] = [];

    renderEffect(() => {
      const items = props.each || [];
      untrack(() => {
        for (let i = 0; i < Math.min(rows.length, items.length); i++) rows[i]!.item.set(items[i]!);
        while (rows.length > items.length) dispose(rows.pop()!.owner);
        for (let i = rows.length; i < items.length; i++) {
          const o = new Owner(owner);
          const r = rowsRegion.child();
          o.nodes = r;
          const item = signal(items[i]!);
          rows.push({ owner: o, item });
          runWithOwner(o, () => {
            try {
              mountInto(r, props.children(item, i));
            } catch (e) {
              handleError(e, o, "a row of Index");
            }
          });
        }
        if (items.length === 0 && props.fallback !== undefined && !fallback) {
          fallback = new Owner(owner);
          const fb = fallback;
          const r = fallbackRegion.child();
          fb.nodes = r;
          runWithOwner(fb, () => mountInto(r, props.fallback));
        } else if (items.length > 0 && fallback) {
          dispose(fallback);
          fallback = null;
        }
      });
    });
  }
}

/** Mounts a row per index: when the list changes, each row's item accessor changes, rows are
 * added or removed at the end, and none moves. */
export function Index<T>(props: IndexProps<T>): Element {
  return new IndexElement(props);
}

