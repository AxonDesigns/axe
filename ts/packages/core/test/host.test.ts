// The script host as @axe/core sees it (docs/script.md): timers on the stepped animation
// clock, the order of events and Promise continuations, uncaught errors reported while the
// app goes on, and the leak safety net.

import * as host from "axe:host";
import { Node, app } from "@axe/core";
import { Hook, expect, gc, hooks, nextTurn, step, test } from "@axe/testing";

const surface = () => app.surfaces[0]!;

test("app.time and performance.now follow the stepped clock", async () => {
  const t0 = app.time;
  expect(performance.now()).toBe(t0); // the runner maps performance.now to the animation clock
  await step(250);
  expect(app.time).toBe(t0 + 250);
  expect(performance.now()).toBe(t0 + 250);
});

test("timers fire at their deadlines on the animation clock, in order", async () => {
  const log: string[] = [];
  const t0 = app.time;
  const at = (s: string) => log.push(`${s}@${app.time - t0}`);
  setTimeout(() => at("b"), 20);
  setTimeout(() => at("a"), 10);
  setTimeout(() => at("a2"), 10);
  let n = 0;
  const iv = setInterval(() => {
    at(`i${n}`);
    if (++n === 3) clearInterval(iv);
  }, 15);
  const cancelled = setTimeout(() => at("never"), 5);
  clearTimeout(cancelled);
  for (let i = 0; i < 8; i++) await step(10);
  // Each fires in the first turn at or after its deadline: steps of 10 ms.
  expect(log).toEqual(["a@10", "a2@10", "i0@20", "b@20", "i1@30", "i2@50"]);
});

test("a timer set for 0 never runs in the turn that set it", async () => {
  let ran = false;
  setTimeout(() => (ran = true), 0);
  await Promise.resolve();
  expect(ran).toBe(false);
  await step(10);
  await step(10);
  expect(ran).toBe(true);
});

test("a Promise continuation runs before the next event's handler", async () => {
  const log: string[] = [];
  const off = surface().onFrame(() => {
    log.push("frame");
    void Promise.resolve().then(() => log.push("then"));
  });
  const n = Node.create(surface());
  n.setColor(1, 0, 0);
  void n.measure().then(() => log.push("measured"));
  await step(16);
  await step(16);
  off();
  n.destroy();
  // frame_committed, its continuation, then layout_measured (sent after the frame).
  expect(log.slice(0, 3)).toEqual(["frame", "then", "measured"]);
});

test("uncaught errors are reported and the app goes on", async () => {
  const seen = hooks().length;
  setTimeout(function failing() {
    throw new TypeError("from a timer");
  }, 0);
  void Promise.reject(new RangeError("nobody catches this"));
  queueMicrotask(() => {
    throw new Error("from a microtask");
  });
  await step(10);
  await step(10);
  const texts = hooks().slice(seen).map((h) => `${h.kind}:${h.text}`);
  expect(texts.some((t) => t.startsWith(`${Hook.exception}:Uncaught TypeError: from a timer`) && t.includes("failing"))).toBe(true);
  expect(texts.some((t) => t.startsWith(`${Hook.rejection}:Uncaught (in promise) RangeError: nobody catches this`))).toBe(true);
  expect(texts.some((t) => t.startsWith(`${Hook.exception}:Uncaught Error: from a microtask`))).toBe(true);
});

test("leaks: a subtree unreachable from JS without destroy() is reported (debug builds)", async () => {
  if (!host.debug) return;
  const s = surface();
  const leaks = () => hooks().filter((h) => h.kind === Hook.leak).length;
  const before = leaks();
  // A child dropped while its parent's object lives is not a leak.
  const parent = Node.create(s);
  (() => void Node.create(parent))();
  gc();
  await nextTurn();
  await step(10);
  expect(leaks()).toBe(before);
  // A node destroyed and dropped is not a leak either.
  (() => Node.create(s).destroy())();
  gc();
  await step(10);
  expect(leaks()).toBe(before);
  // Dropping the parent's object too leaves a subtree nothing can destroy: one report.
  (() => void Node.create(s))();
  const dropParent = () => (parentRef = null);
  let parentRef: Node | null = parent;
  dropParent();
  void parentRef;
  gc();
  await step(10);
  await step(10);
  expect(leaks()).toBe(before + 1); // the root-level node created above, alone
  const text = hooks().filter((h) => h.kind === Hook.leak).at(-1)!.text;
  expect(text).toContain("became unreachable from JS without destroy()");
});
