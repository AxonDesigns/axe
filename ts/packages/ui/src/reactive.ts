// Fine-grained reactivity (docs/ui.md, "Signals"): signals, computeds, effects and the owners
// that dispose them.
//
// The graph is push-pull with three states. A write marks its direct observers DIRTY and
// everything downstream CHECK, and queues the effects it reaches; no user code runs then. A
// computed runs only when read: in CHECK it first brings its sources up to date, in order, and
// runs only if one of them actually changed (each link remembers the version of its source it
// last saw). A computed therefore runs at most once per change, after every source is settled,
// and never sees a mix of old and new values (diamonds included). An unchanged value stops
// the propagation there.
//
// Effects run at the end of the turn (setBeforeFlush): render bindings first, then effects, each
// in creation order; writes they make queue further rounds, up to MAX_ROUNDS.
//
// Objects are kept small: QuickJS pays for every field of every object it creates (about 0.1 us
// each), and a list of 10,000 rows makes tens of thousands of them. Dependency links are reused
// across runs: a run that reads the same sources in the same order allocates nothing; links past
// the last one read are removed after the run.

import * as host from "axe:host";
import { debug, setBeforeFlush } from "@axe/core/internal";

// Flags (Owner.flags, Computation.flags): bits 0–3, then the state (bits 4–5) and the kind
// (bits 6–7) of a computation.
const DISPOSED = 1;
const RUNNING = 2;
const QUEUED = 4;
const HAS_ERROR = 8;
const STATE = 0x30;
const CLEAN = 0x00;
const CHECK = 0x10;
const DIRTY = 0x20;
const KIND = 0xc0;
const COMPUTED = 0x00;
const RENDER = 0x40;
const EFFECT = 0x80;

const MAX_ROUNDS = 100;

// ---- Owners -----------------------------------------------------------------------------------

/** What disposes things: a component instance, a control-flow branch, a computation, or a root.
 * Disposing one disposes its children (newest first), runs its cleanups (in reverse order) and,
 * for the one disposed first, removes the nodes it created (`nodes`). */
export class Owner {
  // The rarely used fields are added when needed: QuickJS pays for each field of each object,
  // and a long list has an owner per row.
  /** Children, in creation order; disposed ones are left in place until compaction. */
  declare owned?: AnyOwner[] | null;
  /** How many of `owned` are disposed. */
  declare dead?: number;
  declare cleanups?: (() => void)[] | null;
  /** Set by an error boundary: takes errors thrown below it. */
  declare onError?: ((error: unknown) => void) | null;
  /** For error reports: a component's name. */
  declare name?: string | null;
  /** Development only: this owner's hot reload scope (hotcore.ts). */
  declare hot?: unknown;
  /** Context values: an object whose prototype chain is the owners' (context.ts). */
  context: Record<symbol, unknown> | null;
  flags = 0;
  /** Where this owner's nodes are (a region, set by the code that mounted them): removed with
   * it. A field rather than a closure, at one per row of a long list. */
  nodes: { remove(): void } | null = null;

  constructor(readonly parent: AnyOwner | null) {
    this.context = contextOf(parent);
    adopt(this, parent);
  }

  get disposed(): boolean {
    return (this.flags & DISPOSED) !== 0;
  }
}

type AnyOwner = Owner | Computation;

/** The context values in effect under `o`: its own, or the nearest owner's above. */
export function contextOf(o: AnyOwner | null): Record<symbol, unknown> | null {
  for (; o; o = o.parent) if (o.context !== undefined) return o.context;
  return null;
}

function adopt(child: AnyOwner, parent: AnyOwner | null): void {
  if (!parent) return;
  if (parent.flags & DISPOSED) {
    // Created by code running after its owner went away (a promise continuation): it can never
    // be disposed through the tree, so it starts disposed.
    child.flags |= DISPOSED;
    return;
  }
  if (parent.owned) parent.owned.push(child);
  else parent.owned = [child];
}

let currentOwner: AnyOwner | null = null;
let errorSink: ((error: unknown, owner: Owner | null) => void) | null = null;
let listener: Computation | null = null;
/** The run in progress: sources record it when tracked, so a run links each source once. */
let runId = 0;
/** The last link the running computation confirmed. */
let cursor: Link | null = null;
let runCount = 0;

/** @internal Development only: the nearest hot reload scope at or above `o`. */
export function nearestHot(o: AnyOwner | null): unknown {
  for (; o; o = o.parent) {
    const h = (o as Owner).hot;
    if (h) return h;
  }
  return null;
}

/** @internal Development only: makes `o` the current owner, for a module's top level, which
 * isn't one function call. Returns the one it replaced. */
export function swapOwner(o: Owner | null): Owner | null {
  const prev = currentOwner as Owner | null;
  currentOwner = o;
  return prev;
}

/** @internal Development only: errors no boundary takes go to `fn` instead of being reported,
 * while an update re-runs components. */
export function setErrorSink(fn: ((error: unknown, owner: Owner | null) => void) | null): void {
  errorSink = fn;
}

/** The owner code is running under (components, effects, control flow), or null. */
export function getOwner(): Owner | null {
  return currentOwner as Owner | null;
}

/** Runs `fn` under `owner`, untracked: for code that runs later (a timer, a promise) and should
 * create things owned by, or read context from, the owner it started under. */
export function runWithOwner<T>(owner: Owner | null, fn: () => T): T {
  const prevOwner = currentOwner, prevListener = listener;
  currentOwner = owner;
  listener = null;
  try {
    return fn();
  } finally {
    currentOwner = prevOwner;
    listener = prevListener;
  }
}

/** @internal runWithOwner without a closure: `fn(a, b)` under `owner`, untracked. */
export function runWith2<A, B, T>(owner: Owner | null, fn: (a: A, b: B) => T, a: A, b: B): T {
  const prevOwner = currentOwner, prevListener = listener;
  currentOwner = owner;
  listener = null;
  try {
    return fn(a, b);
  } finally {
    currentOwner = prevOwner;
    listener = prevListener;
  }
}

/** @internal Disposes what `o` owns (children, then cleanups), keeping `o`. */
export function disposeOwned(o: Owner): void {
  if (o.owned) disposeList(o, o.owned);
  runCleanups(o);
}

/** Runs `fn` with a new root owner and returns its result; `fn` gets the function that disposes
 * it. Roots aren't owned by anything: dispose them yourself (mount() makes one). */
export function root<T>(fn: (dispose: () => void) => T): T {
  const o = new Owner(null);
  return runWithOwner(o, () => fn(() => dispose(o)));
}

/** Registers `fn` to run when the current owner is disposed, or, inside a computation, before
 * its next run. Cleanups run in reverse order of registration. */
export function onCleanup(fn: () => void): void {
  const o = currentOwner;
  if (!o) {
    if (debug) console.warn("onCleanup outside a component, an effect or a root: it never runs");
    return;
  }
  if (o.cleanups) o.cleanups.push(fn);
  else o.cleanups = [fn];
}

/** Disposes `o` and everything it owns, and removes the nodes they created. */
export function dispose(o: AnyOwner): void {
  if (o.flags & DISPOSED) return;
  o.flags |= DISPOSED;
  unlinkChild(o);
  teardown(o, true);
}

// Takes a disposed `o` out of its parent's children: at once when it is the newest, otherwise
// by compaction once half of them are disposed (a long list's rows go one by one).
function unlinkChild(o: AnyOwner): void {
  const p = o.parent;
  const list = p?.owned;
  if (!list) return;
  if (list[list.length - 1] === o) {
    list.pop();
    return;
  }
  const dead = (p!.dead = (p!.dead ?? 0) + 1);
  if (dead > 16 && dead * 2 > list.length) {
    p!.owned = list.filter((c) => !(c.flags & DISPOSED));
    p!.dead = 0;
  }
}

// `removeNodes` is true for the owner disposed first: its nodes contain, or sit beside, every
// node its descendants created, so they need no ops of their own.
function teardown(o: AnyOwner, removeNodes: boolean): void {
  o.flags |= DISPOSED;
  const list = o.owned;
  if (removeNodes) removeNodesOf(o);
  if (list) disposeList(o, list);
  runCleanups(o);
  if (o instanceof Computation) unlinkSources(o, null);
  else o.nodes = null;
}

function removeNodesOf(o: AnyOwner): void {
  if (o instanceof Owner && o.nodes) {
    const nodes = o.nodes;
    o.nodes = null;
    try {
      nodes.remove();
    } catch (e) {
      report(e, o, "removing nodes");
    }
    return;
  }
  // Nothing of its own: its children's nodes are its nodes.
  const list = o.owned;
  if (list) for (const c of list) if (!(c.flags & DISPOSED)) removeNodesOf(c);
}

function disposeList(o: AnyOwner, list: AnyOwner[]): void {
  o.owned = null;
  o.dead = 0;
  for (let i = list.length - 1; i >= 0; i--) {
    const c = list[i]!;
    if (c.flags & DISPOSED) continue;
    c.flags |= DISPOSED;
    teardown(c, false);
  }
}

function runCleanups(o: AnyOwner): void {
  const list = o.cleanups;
  if (!list) return;
  o.cleanups = null;
  for (let i = list.length - 1; i >= 0; i--) {
    try {
      list[i]!();
    } catch (e) {
      handleError(e, o.parent ?? o, "a cleanup");
    }
  }
}

// ---- Errors -------------------------------------------------------------------------------------

/** Sends `error` to the nearest error boundary above `owner`; without one, reports it through
 * the host (stderr and the hook API) and the app goes on. */
export function handleError(error: unknown, owner: AnyOwner | null, what: string): void {
  for (let o = owner; o; o = o.parent) {
    // Boundaries are usually components' owners; a computation can be one too (ErrorBoundary
    // called directly inside a binding).
    const take = (o as { onError?: ((e: unknown) => void) | null }).onError;
    if (!take || o.flags & DISPOSED) continue;
    try {
      take(error);
      return;
    } catch (e) {
      error = e; // the boundary itself failed: the next one up gets that
    }
  }
  AXE_DEV: if (errorSink) {
    errorSink(error, owner as Owner | null);
    return;
  }
  report(error, owner, what);
}

function report(error: unknown, owner: AnyOwner | null, what: string): void {
  const names: string[] = [];
  for (let o = owner; o; o = o.parent) if (o instanceof Owner && o.name) names.push(o.name);
  const where = names.length ? `in ${what} of ${names.join(" < ")}` : `in ${what}`;
  host.reportError(error, where);
}

// ---- Graph ----------------------------------------------------------------------------------------

/** An edge: `target` read `source`, whose version was `version` then. Kept in two lists: the
 * target's sources (singly linked, in read order) and the source's observers (doubly linked). */
class Link {
  prevObserver: Link | null = null;
  nextObserver: Link | null = null;
  constructor(
    readonly source: Source,
    readonly target: Computation,
    public version: number,
    public nextSource: Link | null,
  ) {}
}

/** What can be read: a signal, or a computed. */
interface Source {
  version: number;
  observers: Link | null;
  /** The run that last tracked it (runId), so a run reading it twice links it once. */
  trackedIn: number;
}

let nextId = 1;

/** A binding or an effect (and, as Memo, a computed). */
class Computation {
  // Added when needed (most bindings own nothing and clean nothing up): QuickJS pays for each
  // field of each object. A computation has no context of its own: lookups go to its parent.
  declare owned?: AnyOwner[] | null;
  declare dead?: number;
  declare cleanups?: (() => void)[] | null;
  declare context?: undefined;
  flags: number;
  sources: Link | null = null;
  /** The last value, or the error (HAS_ERROR) a computed's readers get. */
  value: unknown = undefined;
  readonly id = nextId++;

  constructor(
    // Loosely typed: computations of every value type share the queue and the update functions.
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    readonly fn: (prev: any) => unknown,
    kind: number,
    readonly parent: AnyOwner | null,
  ) {
    this.flags = kind | DIRTY;
    adopt(this, parent);
  }
}

/** A computed: a computation that is also a source. */
class Memo extends Computation implements Source {
  version = 0;
  observers: Link | null = null;
  trackedIn = 0;

  constructor(
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    fn: (prev: any) => unknown,
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    readonly equals: false | ((a: any, b: any) => boolean) | undefined,
    parent: AnyOwner | null,
  ) {
    super(fn, COMPUTED, parent);
  }
}

function track(s: Source): void {
  const c = listener;
  if (!c || s.trackedIn === runId) return;
  s.trackedIn = runId;
  const prev = cursor;
  const next = prev ? prev.nextSource : c.sources;
  if (next && next.source === s) {
    next.version = s.version;
    cursor = next;
    return;
  }
  const l = new Link(s, c, s.version, next);
  if (prev) prev.nextSource = l;
  else c.sources = l;
  cursor = l;
  // At the head of the observers: the order doesn't matter (the flush orders by creation), and a
  // source then needs no tail pointer.
  l.nextObserver = s.observers;
  if (s.observers) s.observers.prevObserver = l;
  s.observers = l;
}

function detach(l: Link): void {
  const s = l.source;
  if (l.prevObserver) l.prevObserver.nextObserver = l.nextObserver;
  else s.observers = l.nextObserver;
  if (l.nextObserver) l.nextObserver.prevObserver = l.prevObserver;
  l.prevObserver = l.nextObserver = null;
}

// Removes the links after `keep` (all of them when null).
function unlinkSources(c: Computation, keep: Link | null): void {
  let l = keep ? keep.nextSource : c.sources;
  if (keep) keep.nextSource = null;
  else c.sources = null;
  while (l) {
    detach(l);
    l = l.nextSource;
  }
}

function mark(c: Computation, state: number): void {
  const f = c.flags;
  if ((f & STATE) >= state || f & DISPOSED) return;
  c.flags = (f & ~STATE) | state;
  if ((f & KIND) !== COMPUTED) {
    if (!(f & QUEUED)) {
      c.flags |= QUEUED;
      queue.push(c);
    }
    return;
  }
  if ((f & STATE) === CLEAN) for (let l = (c as Memo).observers; l; l = l.nextObserver) mark(l.target, CHECK);
}

// Brings `c` up to date: in CHECK, refreshes its sources in order until one changed.
function update(c: Computation): void {
  if ((c.flags & STATE) === CHECK) {
    for (let l = c.sources; l; l = l.nextSource) {
      const s = l.source;
      if (s instanceof Memo && s.flags & STATE) update(s);
      if (s.version !== l.version) {
        c.flags = (c.flags & ~STATE) | DIRTY;
        break;
      }
    }
    if ((c.flags & STATE) === CHECK) c.flags &= ~STATE;
  }
  if ((c.flags & STATE) === DIRTY) run(c);
}

function run(c: Computation): void {
  // What the last run owned goes first: its children, then its cleanups. Nodes it mounted are
  // the region's to remove (regions.ts): the binding clears its region when it runs.
  if (c.owned) disposeList(c, c.owned);
  if (c.cleanups) runCleanups(c);
  const prevOwner = currentOwner, prevListener = listener, prevRun = runId, prevCursor = cursor;
  currentOwner = c;
  listener = c;
  runId = ++runCount;
  cursor = null;
  c.flags = (c.flags & ~STATE) | RUNNING; // a write during the run marks it again
  try {
    const v = c.fn(c.value);
    if ((c.flags & KIND) === COMPUTED) {
      const m = c as Memo;
      const eq = m.equals;
      const same = !(m.flags & HAS_ERROR) && m.version > 0 && eq !== false && (eq ? eq(m.value, v) : Object.is(m.value, v));
      m.flags &= ~HAS_ERROR;
      if (!same) {
        m.value = v;
        m.version++;
      }
    } else {
      c.value = v;
    }
  } catch (e) {
    if ((c.flags & KIND) === COMPUTED) {
      // Readers get the error, until a source changes.
      c.value = e;
      c.flags |= HAS_ERROR;
      (c as Memo).version++;
    } else {
      handleError(e, c, (c.flags & KIND) === RENDER ? "a binding" : "an effect");
    }
  } finally {
    c.flags &= ~RUNNING;
    unlinkSources(c, cursor);
    currentOwner = prevOwner;
    listener = prevListener;
    runId = prevRun;
    cursor = prevCursor;
  }
}

function readComputed<T>(c: Memo): T {
  if (c.flags & RUNNING) throw new Error("a computed reads itself (a cycle)");
  if (!(c.flags & DISPOSED)) update(c);
  track(c);
  if (c.flags & HAS_ERROR) throw c.value;
  return c.value as T;
}

// ---- Signals ------------------------------------------------------------------------------------

export interface SignalOptions<T> {
  /** When a write counts as a change: Object.is by default; false: every write does. */
  readonly equals?: false | ((a: T, b: T) => boolean);
  /** For error messages and devtools. */
  readonly name?: string;
}

/** A value that can change. Read it by calling it (inside a computed, an effect or a binding,
 * that subscribes to it); change it with set or update. */
export interface Signal<T> {
  (): T;
  /** Sets the value. Observers update at the end of the turn, if it changed. */
  set(value: T): void;
  /** Sets the value to `fn(current)`. */
  update(fn: (value: T) => T): void;
  /** The value, without subscribing. */
  peek(): T;
}

// A signal is its getter: the function carries the source's fields itself (one object per
// signal, not two), and set, update and peek are shared functions, not three closures each.
interface SignalFn<T> extends Source {
  (): T;
  value: T;
  equals?: false | ((a: T, b: T) => boolean);
  label?: string;
}

// Shared by every signal, as properties: changing a getter's prototype instead would cost
// QuickJS a shape of its own per signal (about 200 bytes).
function signalSet(this: SignalFn<unknown>, value: unknown): void {
  write(this, value);
}
function signalUpdate(this: SignalFn<unknown>, fn: (v: unknown) => unknown): void {
  write(this, fn(this.value));
}
function signalPeek(this: SignalFn<unknown>): unknown {
  return this.value;
}

// A factory, so the getter is an anonymous function: a named one would cost a __name call per
// signal (esbuild's --keep-names).
function getter<T>(): SignalFn<T> {
  const g = (() => {
    if (listener) track(g);
    return g.value;
  }) as SignalFn<T>;
  return g;
}

/** A new signal holding `initial`. */
export function signal<T>(initial: T, options?: SignalOptions<T>): Signal<T> {
  const g = getter<T>() as SignalFn<T> & { set: unknown; update: unknown; peek: unknown };
  g.set = signalSet;
  g.update = signalUpdate;
  g.peek = signalPeek;
  g.value = initial;
  g.version = 0;
  g.observers = null;
  g.trackedIn = 0;
  if (options?.equals !== undefined) g.equals = options.equals;
  if (options?.name !== undefined) g.label = options.name;
  return g as unknown as Signal<T>;
}

function write<T>(node: SignalFn<T>, value: T): void {
  const eq = node.equals;
  if (eq !== false && (eq ? eq(node.value, value) : Object.is(node.value, value))) return;
  if (debug && listener && (listener.flags & KIND) === COMPUTED)
    console.warn(`a computed wrote signal${node.label ? ` "${node.label}"` : ""}: computeds should only read`);
  node.value = value;
  node.version++;
  for (let l = node.observers; l; l = l.nextObserver) mark(l.target, DIRTY);
}

// ---- Computations -------------------------------------------------------------------------------

export interface ComputedOptions<T> {
  /** When a new value counts as a change: Object.is by default; false: every run does. */
  readonly equals?: false | ((a: T, b: T) => boolean);
}

/** A value derived from signals and other computeds: computed when read, again only after
 * something it read changed, and owned by the current owner. A thrown error is rethrown to its
 * readers until then. */
export function computed<T>(fn: (prev: T | undefined) => T, options?: ComputedOptions<T>): () => T {
  const c = new Memo(fn, options?.equals, currentOwner);
  if (!currentOwner && debug) console.warn("computed outside a component, an effect or a root: it is never disposed");
  return () => readComputed<T>(c);
}

/** Runs `fn`, and again whenever something it read changes, at the end of the turn, after the
 * bindings (so the scene ops for the same changes are written first). Its first run is at the end
 * of the turn that created it. Owned by the current owner: onCleanup inside runs before each
 * new run and when it is disposed. */
export function effect(fn: () => void): void {
  const c = new Computation(fn, EFFECT, currentOwner);
  if (!currentOwner && debug) console.warn("effect outside a component, an effect or a root: it is never disposed");
  if (c.flags & DISPOSED) return;
  c.flags |= QUEUED;
  queue.push(c);
}

/** @internal A binding: runs `fn` now, and again at the end of any turn in which something it
 * read changed, before effects. A binding that read nothing is dropped after its first run. */
export function renderEffect<T>(fn: (prev: T | undefined) => T): void {
  start(new Computation(fn, RENDER, currentOwner));
}

/** @internal A binding carrying its data: `fn` runs with `this` the binding, so a binding needs
 * no closure (and no captured variables) of its own. Start it with startBinding. */
export class Binding<S = unknown> extends Computation {
  constructor(
    fn: (this: Binding<S>) => void,
    public slot: number,
    public read: () => unknown,
    public state: S,
  ) {
    super(fn as (prev: unknown) => unknown, RENDER, currentOwner);
  }
}

/** @internal Runs a binding now; as renderEffect, it is dropped if it read nothing. */
export function startBinding(b: Binding<never> | Binding<unknown>): void {
  start(b as Computation);
}

function start(c: Computation): void {
  if (c.flags & DISPOSED) return;
  run(c);
  // Static: nothing can make it run again. Keep it only for what it owns.
  if (!c.sources && !c.owned && !c.cleanups && !(c.flags & QUEUED)) {
    c.flags |= DISPOSED;
    unlinkChild(c);
  }
}

/** Runs `fn` without subscribing the current computation to what it reads. */
export function untrack<T>(fn: () => T): T {
  if (!listener) return fn();
  const prev = listener;
  listener = null;
  try {
    return fn();
  } finally {
    listener = prev;
  }
}

/** @internal Whether code is running inside a computation that tracks reads. */
export function tracking(): boolean {
  return listener !== null;
}

// ---- The flush ----------------------------------------------------------------------------------

let queue: Computation[] = [];
let flushing = false;

// Writes usually reach bindings in creation order already: then no sort is needed.
function ordered(batch: Computation[]): boolean {
  for (let i = 1; i < batch.length; i++) if (byKindThenAge(batch[i - 1]!, batch[i]!) > 0) return false;
  return true;
}

const byKindThenAge = (a: Computation, b: Computation) => (a.flags & KIND) - (b.flags & KIND) || a.id - b.id;

/** @internal Runs queued bindings and effects until none remain (tests use it through nextTurn;
 * the host calls it at the end of every turn). */
export function flush(): void {
  if (flushing) return;
  flushing = true;
  try {
    for (let round = 0; queue.length; round++) {
      if (round === MAX_ROUNDS) {
        const names = [...new Set(queue.map((c) => ownerName(c)))].join(", ");
        report(new Error(`effects did not settle after ${MAX_ROUNDS} rounds (they keep writing what they read): ${names}`),
               null, "the effect flush");
        break; // the rest stay queued for the next turn
      }
      const batch = queue;
      queue = [];
      if (batch.length > 1 && !ordered(batch)) batch.sort(byKindThenAge);
      for (const c of batch) {
        c.flags &= ~QUEUED;
        if (!(c.flags & DISPOSED)) update(c);
      }
    }
  } finally {
    flushing = false;
  }
}

function ownerName(c: AnyOwner): string {
  for (let o: AnyOwner | null = c; o; o = o.parent) if (o instanceof Owner && o.name) return o.name;
  return "(no component)";
}

setBeforeFlush(flush);
