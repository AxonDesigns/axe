// Hot reload's bookkeeping (docs/ui.md, "Hot reload"), only in development: which instance of
// which component is where, so a new version can re-run it in place and its state can be
// found again. Everything calling this is a statement labeled `AXE_DEV:`, which bundles drop
// (esbuild's --drop-labels), so they hold none of it.
//
// A scope is a component instance, a row of For or a module. Scopes form a tree of their own
// beside the owners: the path to a scope (family and ordinal under its parent, a row's key) is
// what identifies it across versions.

import { type Owner, nearestHot } from "./reactive.js";

export const FAMILY = Symbol("axe.family");

export interface Family {
  readonly key: string;
  readonly proxy: (props: unknown) => unknown;
  impl: (props: unknown) => unknown;
  hash: string;
  instances: Set<Scope>;
  /** How many times instances of it ran: what tests count. */
  runs: number;
  /** The version an update replaced, until the update is over. */
  previous: { impl: (props: unknown) => unknown; hash: string } | null;
}

/** State saved by path while a subtree is recreated. */
export interface Stash {
  signals: Map<string, { hash: string; value: unknown }>;
  children: Map<string, Stash>;
  rows: Map<unknown, Stash>;
}

export class Scope {
  children = new Set<Scope>();
  counts = new Map<string, number>();
  signals = new Map<string, { hash: string; signal: { value: unknown } }>();
  /** What this scope's signals and children find their state in, during a re-run. */
  stash: Stash | null = null;
  /** The update that last ran it. */
  epoch = 0;
  dead = false;
  readonly depth: number;

  constructor(
    readonly parent: Scope | null,
    readonly owner: Owner,
    readonly family: Family | null,
    readonly props: unknown,
    readonly region: { clear(): void } | null,
    /** A row's key; undefined for anything else. */
    readonly rowKey: unknown,
    readonly isRow: boolean,
  ) {
    this.depth = parent ? parent.depth + 1 : 0;
  }
}

let epoch = 0;

/** @internal */
export function nextEpoch(): number {
  return ++epoch;
}

function ordinal(s: Scope, key: string): number {
  const n = s.counts.get(key) ?? 0;
  s.counts.set(key, n + 1);
  return n;
}

function attach(s: Scope): void {
  s.family?.instances.add(s);
  s.parent?.children.add(s);
  s.epoch = epoch;
  const o = s.owner;
  (o.cleanups ??= []).push(() => {
    s.dead = true;
    s.family?.instances.delete(s);
    s.parent?.children.delete(s);
  });
}

/** A component starts, under owner `o`. Components that aren't registered have no scope: what
 * they make belongs to the nearest one. */
export function instanceBegin(o: Owner, fn: unknown, props: unknown, region: { clear(): void }, row?: { key: unknown }): void {
  const family = (fn as { [FAMILY]?: Family } | null)?.[FAMILY] ?? null;
  if (!family && !row) return;
  const parent = nearestHot(o.parent) as Scope | null;
  const s = new Scope(parent, o, family, props, region, row?.key, row !== undefined);
  if (parent?.stash) {
    const stash = row ? parent.stash.rows.get(row.key) : parent.stash.children.get(`${family!.key}#${ordinal(parent, family!.key)}`);
    s.stash = stash ?? null;
  } else if (parent && !row) {
    ordinal(parent, family!.key);
  }
  o.hot = s;
  attach(s);
  if (family) family.runs++;
}

/** The state of `s` and everything below it. */
export function collect(s: Scope): Stash {
  const out: Stash = { signals: new Map(), children: new Map(), rows: new Map() };
  for (const [key, e] of s.signals) out.signals.set(key, { hash: e.hash, value: e.signal.value });
  const counts = new Map<string, number>();
  for (const c of s.children) {
    if (c.isRow) {
      out.rows.set(c.rowKey, collect(c));
    } else {
      const k = c.family!.key;
      const n = counts.get(k) ?? 0;
      counts.set(k, n + 1);
      out.children.set(`${k}#${n}`, collect(c));
    }
  }
  return out;
}

/** Forgets what ran, for a run under the same owner: disposing the owner detached it. */
export function restart(s: Scope, stash: Stash | null): void {
  s.children.clear();
  s.counts.clear();
  s.signals.clear();
  s.stash = stash;
  s.dead = false;
  attach(s);
  if (s.family) s.family.runs++;
}

/** A component's `signal()`, keyed: its value comes back from the stash when its initializer
 * didn't change. A key used twice in a run (a helper called twice) gets an occurrence each. */
export function keepSignal<T extends { value: unknown }>(s: Scope, key: string, hash: string, signal: T): T {
  let unique = key;
  for (let n = 1; s.signals.has(unique); n++) unique = `${key}~${n}`;
  s.signals.set(unique, { hash, signal });
  const saved = s.stash?.signals.get(unique);
  if (saved && saved.hash === hash) signal.value = saved.value;
  return signal;
}
