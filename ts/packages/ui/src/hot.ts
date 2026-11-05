// Hot reload's runtime (docs/ui.md, "Hot reload"): @axe/ui/hot, loaded only when an app runs from
// modules (axe-run --dev). The host delivers what changed; this decides what to import again,
// applies the new versions as a transaction, re-runs what changed and shows errors.
//
// An update is all or nothing up to the re-runs: importing new versions only stages what they
// define, so a failure leaves the running app untouched.

import * as host from "axe:host";
import { Node, Style, Token, app, type Surface } from "@axe/core";
import { mountInto } from "./elements.js";
import { FAMILY, Scope, collect, keepSignal, nextEpoch, restart, type Family } from "./hotcore.js";
import { Owner, contextOf, dispose, disposeOwned, getOwner, nearestHot, onCleanup, runWithOwner, setErrorSink, swapOwner, type Signal } from "./reactive.js";
import { style as defineStyle, styleDesc, type StyleInput } from "./style.js";
import { dropUnusedVariants, redefineTemplate, template as makeTemplate, type Template, type TemplateNode } from "./template.js";
import type { Theme, ThemeDesc } from "./theme.js";
import type { Context } from "./context.js";

// ---- Modules --------------------------------------------------------------------------------

class ModuleRec {
  readonly owner = new Owner(null);
  readonly scope: Scope;
  readonly seen = new Set<string>();
  readonly families = new Set<string>();
  readonly templates = new Set<string>();
  readonly styles = new Set<string>();
  boundary = false;

  constructor(readonly file: string) {
    this.owner.name = file;
    this.scope = new Scope(null, this.owner, null, undefined, null, undefined, false);
    this.owner.hot = this.scope;
    moduleOfScope.set(this.scope, this);
  }
}

const moduleOfScope = new WeakMap<Scope, ModuleRec>();

interface Keyed {
  hash: string;
  value: unknown;
  /** A style's description, to go back to. */
  desc?: unknown;
}

const families = new Map<string, Family>();
const keyed = new Map<string, Keyed>();
const templates = new Map<string, Template>();
const modules = new Map<string, ModuleRec>();
const hotValues = new WeakSet<object>(); // what a boundary may export

interface Txn {
  commits: (() => void)[];
  reverts: (() => void)[];
  created: ModuleRec[];
  changed: Set<Family>;
  surfaces: (() => void)[];
}

let txn: Txn | null = null;
let evaluating: ModuleRec | null = null;
let ownerBefore: Owner | null = null; // ES modules evaluate one after the other, never nested

function stage(commit: () => void, revert?: () => void): void {
  if (txn) {
    txn.commits.push(commit);
    if (revert) txn.reverts.push(revert);
  } else {
    commit();
  }
}

// ---- What the transform calls ---------------------------------------------------------------

function component(m: ModuleRec, name: string, hash: string, impl: (props: unknown) => unknown): unknown {
  const key = `${m.file}:${name}`;
  m.families.add(key);
  let family = families.get(key);
  if (!family) {
    const f: Family = {
      key,
      impl,
      hash,
      instances: new Set(),
      runs: 0,
      previous: null,
      proxy: (props) => f.impl(props),
    };
    Object.defineProperty(f.proxy, "name", { value: name });
    (f.proxy as unknown as { [FAMILY]: Family })[FAMILY] = f;
    families.set(key, f);
    hotValues.add(f.proxy);
    return f.proxy;
  }
  const f = family;
  stage(() => {
    f.previous = { impl: f.impl, hash: f.hash };
    f.impl = impl;
    if (f.hash !== hash) txn?.changed.add(f);
    f.hash = hash;
  }, () => {
    if (!f.previous) return;
    f.impl = f.previous.impl;
    f.hash = f.previous.hash;
    f.previous = null;
  });
  return f.proxy;
}

function template(m: ModuleRec, key: string, nodes: readonly TemplateNode[]): Template {
  const full = `${m.file}:${key}`;
  m.templates.add(full);
  const old = templates.get(full);
  if (!old) {
    const t = makeTemplate(nodes);
    templates.set(full, t);
    hotValues.add(t);
    return t;
  }
  const before = old.nodes;
  stage(() => redefineTemplate(old, nodes), () => redefineTemplate(old, before));
  return old;
}

function keyedStyle(m: ModuleRec, full: string, hash: string, input: StyleInput): Style {
  m.styles.add(full);
  const desc = styleDesc(input); // here, so a bad value fails the update
  const old = keyed.get(full);
  if (!old) {
    const s = Style.define(desc);
    keyed.set(full, { hash, value: s, desc });
    hotValues.add(s);
    return s;
  }
  const s = old.value as Style;
  if (old.hash === hash) return s;
  const before = { hash: old.hash, desc: old.desc };
  stage(() => {
    s.redefine(desc);
    old.hash = hash;
    old.desc = desc;
  }, () => {
    s.redefine(before.desc as never);
    old.hash = before.hash;
    old.desc = before.desc;
  });
  return s;
}

function keyedSignal(full: string, hash: string, fresh: Signal<unknown>): Signal<unknown> {
  const old = keyed.get(full);
  if (!old) {
    keyed.set(full, { hash, value: fresh });
    return fresh;
  }
  if (old.hash === hash) return old.value as Signal<unknown>;
  const s = old.value as Signal<unknown>;
  stage(() => {
    old.hash = hash;
    s.set(fresh.peek());
  });
  return s;
}

function keyedToken(full: string, hash: string, fn: () => Token<never>, args: unknown[]): unknown {
  const old = keyed.get(full);
  if (!old) {
    const t = fn();
    keyed.set(full, { hash, value: t });
    hotValues.add(t);
    return t;
  }
  const t = old.value as Token<never> & { set(v: unknown): void };
  if (old.hash !== hash) {
    stage(() => {
      old.hash = hash;
      if (args.length) t.set(args[0]);
    });
  }
  return t;
}

function keyedTheme(full: string, hash: string, fn: () => Theme, desc: ThemeDesc): Theme {
  const old = keyed.get(full);
  if (!old) {
    const t = fn();
    keyed.set(full, { hash, value: t });
    hotValues.add(t);
    return t;
  }
  const t = old.value as { style: Style; components: unknown };
  if (old.hash !== hash) {
    const input = { ...(desc.style ?? {}), tokens: [...(desc.style?.tokens ?? []), ...(desc.tokens ?? [])] };
    const next = styleDesc(input as StyleInput);
    stage(() => {
      old.hash = hash;
      t.style.redefine(next);
      t.components = desc.components ?? {};
    });
  }
  return t as Theme;
}

function keyedContext(full: string, fn: () => Context<unknown>, defaultValue: unknown): Context<unknown> {
  const old = keyed.get(full);
  if (!old) {
    const c = fn();
    keyed.set(full, { hash: "", value: c });
    hotValues.add(c);
    return c;
  }
  const c = old.value as { defaultValue: unknown };
  stage(() => {
    c.defaultValue = defaultValue;
  });
  return c as Context<unknown>;
}

function keyedCall(m: ModuleRec, kind: string, key: string, hash: string, fn: (...args: never[]) => unknown, ...args: never[]): unknown {
  const scope = nearestHot(getOwner()) as Scope | null;
  if (scope?.family || scope?.isRow) {
    const made = fn(...args);
    return kind === "signal" ? keepSignal(scope, key, hash, made as { value: unknown }) : made;
  }
  // At a module's top level, once: a helper's calls have nothing to tell them apart.
  if (evaluating !== m || m.seen.has(key)) return fn(...args);
  m.seen.add(key);
  const full = `${m.file}:${key}`;
  switch (kind) {
    case "signal":
      return keyedSignal(full, hash, fn(...args) as Signal<unknown>);
    case "style":
      return keyedStyle(m, full, hash, args[0] as unknown as StyleInput);
    case "token":
      return keyedToken(full, hash, () => fn(...args) as Token<never>, args);
    case "theme":
      return keyedTheme(full, hash, () => fn(...args) as Theme, args[0] as unknown as ThemeDesc);
    case "context":
      return keyedContext(full, () => fn(...args) as Context<unknown>, args[0]);
    default:
      return fn(...args);
  }
}

function enter(file: string): ModuleRec {
  const m = new ModuleRec(file);
  if (txn) txn.created.push(m);
  else modules.set(file, m); // the app's own import()
  ownerBefore = swapOwner(m.owner);
  evaluating = m;
  return m;
}

function leave(_m: ModuleRec): void {
  swapOwner(ownerBefore);
  ownerBefore = null;
  evaluating = null;
}

/** What axe-tsx --dev writes calls to. */
export const hot = { enter, leave, component, template, keyed: keyedCall };

// ---- Module-level subscriptions ----------------------------------------------------------------

// A module's timers and app handlers end with its owner, so a new version doesn't add to them.
function moduleOf(): ModuleRec | null {
  const scope = nearestHot(getOwner()) as Scope | null;
  return scope ? (moduleOfScope.get(scope) ?? null) : null;
}

const realOnSurface = app.onSurface;
const realOnTurnStart = app.onTurnStart;
const realOnTurnEnd = app.onTurnEnd;

function ownedHandler(subscribe: (fn: () => void) => () => void) {
  return (fn: () => void): (() => void) => {
    const m = moduleOf();
    if (!m) return subscribe(fn);
    const stop = subscribe(() => runWithOwner(m.owner, fn));
    runWithOwner(m.owner, () => onCleanup(stop));
    return stop;
  };
}

app.onTurnStart = ownedHandler(realOnTurnStart);
app.onTurnEnd = ownedHandler(realOnTurnEnd);
app.onSurface = (fn) => {
  const m = moduleOf();
  if (!m) return realOnSurface(fn);
  let stop: (() => void) | null = null;
  const begin = (): void => {
    stop = realOnSurface((s) => runWithOwner(m.owner, () => fn(s)));
  };
  // During an update: from the commit, since it calls `fn` with the surfaces that exist.
  if (txn) txn.surfaces.push(begin);
  else begin();
  runWithOwner(m.owner, () => onCleanup(() => stop?.()));
  return () => stop?.();
};

const G = globalThis as unknown as {
  setTimeout: (fn: (...a: unknown[]) => void, ms?: number, ...args: unknown[]) => number;
  setInterval: (fn: (...a: unknown[]) => void, ms?: number, ...args: unknown[]) => number;
  clearTimeout: (id?: number) => void;
};
const realTimeout = G.setTimeout, realInterval = G.setInterval, realClear = G.clearTimeout;

function ownedTimer(start: typeof realTimeout, once: boolean): typeof realTimeout {
  return (fn, ms, ...args) => {
    const m = moduleOf();
    if (!m) return start(fn, ms, ...args);
    const timers = moduleTimers(m);
    const id = start((...a: unknown[]) => {
      if (once) timers.delete(id);
      runWithOwner(m.owner, () => fn(...a));
    }, ms, ...args);
    timers.add(id);
    return id;
  };
}

const timersOf = new WeakMap<ModuleRec, Set<number>>();

function moduleTimers(m: ModuleRec): Set<number> {
  let timers = timersOf.get(m);
  if (!timers) {
    const set = (timers = new Set<number>());
    timersOf.set(m, set);
    runWithOwner(m.owner, () => onCleanup(() => {
      for (const id of set) realClear(id);
      set.clear();
    }));
  }
  return timers;
}

G.setTimeout = ownedTimer(realTimeout, true);
G.setInterval = ownedTimer(realInterval, false);

// ---- Errors -----------------------------------------------------------------------------------

const buildErrors = new Map<string, string>();
const danglingErrors = new Map<string, string>();
let runtimeError: string | null = null;

// A base-64 VLQ source map's position for a generated line and column.
function originalPosition(map: string, line: number, column: number): { source: string; line: number } | null {
  const parsed = JSON.parse(map) as { sources: string[]; mappings: string };
  const lines = parsed.mappings.split(";");
  const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  let source = 0, srcLine = 0, srcCol = 0, found: { source: number; line: number } | null = null;
  for (let l = 0; l < lines.length && l <= line - 1; l++) {
    let genCol = 0;
    for (const segment of lines[l]!.split(",")) {
      if (!segment) continue;
      const fields: number[] = [];
      let value = 0, shift = 0;
      for (const c of segment) {
        const d = B64.indexOf(c);
        value |= (d & 31) << shift;
        if (d & 32) {
          shift += 5;
        } else {
          fields.push(value & 1 ? -(value >> 1) : value >> 1);
          value = 0;
          shift = 0;
        }
      }
      genCol += fields[0]!;
      if (fields.length < 4) continue;
      source += fields[1]!;
      srcLine += fields[2]!;
      srcCol += fields[3]!;
      if (l < line - 1 || genCol <= column) found = { source, line: srcLine + 1 };
    }
  }
  return found ? { source: parsed.sources[found.source] ?? "", line: found.line } : null;
}

// The first app frame of a stack, as file:line in the original source.
function locate(stack: string): string | null {
  for (const m of stack.matchAll(/(app:\/[^\s:()]+\?v=\d+):(\d+)(?::(\d+))?/g)) {
    const name = m[1]!;
    const file = name.slice(5, name.indexOf("?v="));
    const map = host.hotSourceMap(name);
    if (!map) continue;
    const at = originalPosition(map, Number(m[2]), Number(m[3] ?? 0));
    if (at) return `${file}:${at.line}`;
  }
  return null;
}

function describe(e: unknown): string {
  if (!(e instanceof Error)) return String(e);
  const where = e.stack ? locate(e.stack) : null;
  const text = `${e.name}: ${e.message}`;
  return where ? `${where}: ${text}` : text;
}

// ---- The overlay --------------------------------------------------------------------------------

const overlayNodes = new Map<Surface, Node>();
let overlayStyle: Style | null = null;
let overlayText: Style | null = null;

function overlayMessage(): string | null {
  const parts = [...buildErrors.values(), ...danglingErrors.values()];
  if (runtimeError) parts.push(runtimeError);
  return parts.length ? parts.join("\n\n") : null;
}

function drawOverlay(): void {
  for (const n of overlayNodes.values()) n.destroy();
  overlayNodes.clear();
  const message = overlayMessage();
  if (message === null) return;
  overlayStyle ??= defineStyle({
    position: "absolute", top: 0, left: 0, right: 0, bottom: 0, padding: 16,
    backgroundColor: "#7f1d1dd9",
  });
  overlayText ??= defineStyle({ textColor: "#fff", fontSize: 14, fontFamily: "monospace" });
  for (const s of app.surfaces) {
    const box = Node.create(s, { styles: [overlayStyle] });
    const text = Node.create(box, { styles: [overlayText] });
    text.setText(`Hot reload: the last working version is still running\n\n${message}`);
    overlayNodes.set(s, box);
  }
}

// ---- Updates ------------------------------------------------------------------------------------

let started = false;
let running = false;
let queued: { file: string; deleted: boolean; error?: string }[] = [];
const pending = new Set<string>();
const counters = { updates: 0, fullReloads: 0, rejected: 0 };

function isBoundary(file: string): boolean {
  return modules.get(file)?.boundary ?? true;
}

function boundaryExports(ns: Record<string, unknown>): boolean {
  const names = Object.keys(ns);
  if (!names.length) return false;
  return names.every((k) => {
    const v = ns[k];
    return (typeof v === "object" || typeof v === "function") && v !== null && hotValues.has(v);
  });
}

// What must be evaluated again for `changed`, and whether that reaches the entry, through which
// importers.
function spread(changed: Iterable<string>, past: boolean): { dirty: Set<string>; reached: string | null } {
  const dirty = new Set<string>();
  const from = new Map<string, string>();
  const queue = [...changed];
  let reached: string | null = null;
  while (queue.length) {
    const f = queue.pop()!;
    if (dirty.has(f)) continue;
    dirty.add(f);
    if (f === host.hotEntry) {
      const chain = [f];
      for (let at = from.get(f); at; at = from.get(at)) chain.push(at);
      reached = chain.reverse().join(" -> ");
      continue;
    }
    if (!past && isBoundary(f)) continue;
    for (const importer of host.hotImporters(f)) {
      if (!dirty.has(importer) && !from.has(importer)) from.set(importer, f);
      queue.push(importer);
    }
  }
  return { dirty, reached };
}

// A deleted file that is still imported holds the update back, unless every importer is changing
// too: that may be the edit removing the import, and if it isn't, the import fails and says so.
function checkDangling(deleted: string[]): void {
  for (const f of deleted) danglingErrors.set(f, "");
  for (const f of [...danglingErrors.keys()]) {
    const importers = host.hotImporters(f);
    if (importers.some((i) => !pending.has(i))) danglingErrors.set(f, `${f} was deleted, but ${importers.join(", ")} still imports it`);
    else danglingErrors.delete(f);
  }
}

// Forgets an update: `committed` when its definitions were already applied.
function abort(t: Txn, committed: boolean): void {
  host.hotRevert();
  if (committed) for (const revert of t.reverts.reverse()) revert();
  for (const m of t.created) dispose(m.owner);
  swapOwner(ownerBefore);
  ownerBefore = null;
  evaluating = null;
  txn = null;
}

function rerun(s: Scope, stash = collect(s)): unknown {
  const o = s.owner;
  const f = s.family!;
  s.region!.clear();
  disposeOwned(o);
  o.context = contextOf(o.parent); // a Provider set its own
  restart(s, stash);
  let failure: unknown = null;
  runWithOwner(o, () => {
    try {
      mountInto(s.region as never, f.impl(s.props));
    } catch (e) {
      failure = { error: e, family: f };
    }
  });
  s.stash = null;
  return failure;
}

// Runs every instance of `changed` in place, outermost first. An error rolls the family that
// threw back to its previous version; instances run again under it, with the state they had.
function rerunChanged(changed: Set<Family>): void {
  const errors: { error: unknown; owner: Owner | null }[] = [];
  setErrorSink((error, owner) => errors.push({ error, owner }));
  try {
    const targets = [...changed].flatMap((f) => [...f.instances]).sort((a, b) => a.depth - b.depth);
    const epoch = nextEpoch();
    for (const s of targets) {
      if (s.dead || s.epoch === epoch) continue;
      s.epoch = epoch;
      const stash = collect(s);
      for (let attempt = 0; attempt < 16; attempt++) {
        errors.length = 0;
        const thrown = rerun(s, stash) as { error: unknown; family: Family } | null;
        let blamed = thrown?.family ?? null;
        const first = thrown?.error ?? errors[0]?.error;
        if (first === undefined) break;
        if (!blamed) {
          const scope = nearestHot(errors[0]!.owner) as Scope | null;
          for (let at = scope; at && !blamed; at = at.parent) if (at.family?.previous) blamed = at.family;
        }
        runtimeError = describe(first);
        console.error(`hot reload: ${runtimeError}`);
        if (!blamed?.previous) {
          s.region!.clear();
          disposeOwned(s.owner);
          break;
        }
        blamed.impl = blamed.previous.impl;
        blamed.hash = blamed.previous.hash;
        blamed.previous = null;
      }
    }
  } finally {
    setErrorSink(null);
  }
}

async function apply(changes: { file: string; deleted: boolean; error?: string }[]): Promise<void> {
  counters.updates++;
  const deleted: string[] = [];
  for (const c of changes) {
    if (c.error !== undefined) {
      buildErrors.set(c.file, c.error);
      continue;
    }
    buildErrors.delete(c.file);
    if (c.deleted) {
      deleted.push(c.file);
      pending.delete(c.file);
    } else {
      pending.add(c.file);
    }
  }
  checkDangling(deleted);
  if (buildErrors.size || danglingErrors.size) {
    drawOverlay();
    return;
  }
  if (!started) pending.add(host.hotEntry);
  if (!pending.size) {
    runtimeError = null;
    drawOverlay();
    return;
  }
  // A file nothing imports has nothing to update: its code waits in the table for an import.
  const live = [...pending].filter((f) => f === host.hotEntry || modules.has(f));
  for (const f of pending) if (!live.includes(f)) pending.delete(f);
  if (!live.length) {
    runtimeError = null;
    drawOverlay();
    return;
  }
  let { dirty, reached } = spread(live, false);
  let full = reached !== null || !started;
  for (let pass = 0; pass < 2; pass++) {
    const t: Txn = { commits: [], reverts: [], created: [], changed: new Set(), surfaces: [] };
    txn = t;
    const namespaces: { file: string; ns: Record<string, unknown> }[] = [];
    try {
      const files = [...dirty];
      const names = host.hotBegin(files);
      for (let i = 0; i < files.length; i++) {
        const ns = (await import(names[i]!)) as Record<string, unknown>;
        namespaces.push({ file: files[i]!, ns });
      }
      // Modules imported on the way, statically: what they export decides what they are too.
      for (const m of t.created) {
        if (namespaces.some((n) => n.file === m.file)) continue;
        const [name] = host.hotBegin([m.file]);
        namespaces.push({ file: m.file, ns: (await import(name!)) as Record<string, unknown> });
      }
    } catch (e) {
      abort(t, false);
      runtimeError = describe(e);
      console.error(`hot reload: ${runtimeError}`);
      counters.rejected++;
      drawOverlay();
      return;
    }
    // A module that stopped being a boundary needs its importers too.
    if (pass === 0 && !full && namespaces.some(({ file, ns }) => isBoundary(file) && modules.has(file) && !boundaryExports(ns))) {
      abort(t, false);
      ({ dirty, reached } = spread(live, true));
      full = true;
      continue;
    }
    commit(t, namespaces, dirty, new Set(live), full, reached);
    return;
  }
}

function commit(t: Txn, namespaces: { file: string; ns: Record<string, unknown> }[], dirty: Set<string>,
                changed: Set<string>, full: boolean, reached: string | null): void {
  for (const fn of t.commits) fn();
  const created = new Map(t.created.map((m) => [m.file, m]));
  for (const { file, ns } of namespaces) {
    const m = created.get(file);
    if (m) m.boundary = boundaryExports(ns);
  }
  const old = t.created.map((m) => modules.get(m.file)).filter((m) => m !== undefined);
  txn = null;
  evaluating = null;

  if (full) {
    if (started) console.log(`hot reload: full reload (${reached ?? "the entry changed"}): component state is lost`);
    // The new app mounts first: if it fails, the running one is still there.
    const errors: unknown[] = [];
    setErrorSink((error) => errors.push(error));
    for (const begin of t.surfaces) begin();
    setErrorSink(null);
    if (errors.length) {
      abort(t, true);
      runtimeError = describe(errors[0]);
      console.error(`hot reload: ${runtimeError}`);
      counters.rejected++;
      drawOverlay();
      return;
    }
    host.hotCommit();
    for (const m of t.created) modules.set(m.file, m);
    for (const m of old) dispose(m.owner);
    runtimeError = null;
    if (started) counters.fullReloads++;
  } else {
    host.hotCommit();
    for (const m of t.created) modules.set(m.file, m);
    for (const m of old) dispose(m.owner); // effects and timers of the old version stop first
    for (const begin of t.surfaces) begin();
    runtimeError = null;
    // A module evaluated again because of what it imports: its components hold old bindings.
    const rerunning = new Set(t.changed);
    for (const file of dirty) {
      if (changed.has(file)) continue;
      for (const key of modules.get(file)?.families ?? []) {
        const f = families.get(key);
        if (f) rerunning.add(f);
      }
    }
    rerunChanged(rerunning);
  }
  for (const file of changed) pending.delete(file);
  for (const m of t.created) cleanUp(m, old.find((o) => o.file === m.file) ?? null);
  for (const f of families.values()) f.previous = null;
  started = true;
  drawOverlay();
}

// What the previous version made that this one doesn't.
function cleanUp(m: ModuleRec, prev: ModuleRec | null): void {
  for (const key of prev?.styles ?? []) {
    if (m.styles.has(key)) continue;
    (keyed.get(key)?.value as Style | undefined)?.destroy();
    keyed.delete(key);
  }
  for (const key of prev?.templates ?? []) {
    if (m.templates.has(key)) continue;
    const t = templates.get(key);
    if (t) dropUnusedVariants(t);
    templates.delete(key);
  }
  for (const key of m.templates) {
    const t = templates.get(key);
    if (t) dropUnusedVariants(t);
  }
  for (const key of prev?.families ?? []) {
    if (m.families.has(key)) continue;
    if (!families.get(key)?.instances.size) families.delete(key);
  }
}

async function deliver(changes: { file: string; deleted: boolean; error?: string }[]): Promise<void> {
  queued.push(...changes);
  if (running) return;
  running = true;
  try {
    while (queued.length) {
      const batch = queued;
      queued = [];
      await apply(batch);
    }
  } finally {
    running = false;
  }
}

host.hotSetHandler((changes) => void deliver(changes));

// An overlay shown before a surface existed appears with the first one.
realOnSurface(() => {
  if (overlayMessage() !== null) drawOverlay();
});

/** For tests: what hot reload did. */
export function hotState(): { overlay: string | null; updates: number; fullReloads: number; rejected: number; runs(file: string, name: string): number } {
  return {
    overlay: overlayMessage(),
    ...counters,
    runs: (file, name) => families.get(`${file}:${name}`)?.runs ?? 0,
  };
}
