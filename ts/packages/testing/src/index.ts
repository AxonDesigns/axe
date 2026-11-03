// @axe/testing: tests for TypeScript run inside the script host by axe-test-js, headless and
// on a paused clock (docs/typescript.md, "Tests"). Tests run in order, after the module
// has registered them; each may be async and drive the clock.

import * as runner from "axe:test";
import { app } from "@axe/core";

type Body = () => void | Promise<void>;
const tests: { name: string; body: Body }[] = [];

/** Registers a test. */
export function test(name: string, body: Body): void {
  tests.push({ name, body });
}

export class AssertionError extends Error {
  override name = "AssertionError";
}

const show = (v: unknown): string => {
  try {
    return typeof v === "string" ? JSON.stringify(v) : typeof v === "bigint" ? `${v}n` : JSON.stringify(v) ?? String(v);
  } catch {
    return String(v);
  }
};

function equal(a: unknown, b: unknown): boolean {
  if (Object.is(a, b)) return true;
  if (typeof a !== "object" || typeof b !== "object" || !a || !b) return false;
  if (ArrayBuffer.isView(a) && ArrayBuffer.isView(b)) {
    const x = new Uint8Array(a.buffer, a.byteOffset, a.byteLength), y = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
    return x.length === y.length && x.every((v, i) => v === y[i]);
  }
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  const ka = Object.keys(a), kb = Object.keys(b);
  return ka.length === kb.length && ka.every((k) => equal((a as Record<string, unknown>)[k], (b as Record<string, unknown>)[k]));
}

export function expect<T>(actual: T) {
  const fail = (what: string): never => {
    throw new AssertionError(what);
  };
  return {
    toBe(expected: T): void {
      if (!Object.is(actual, expected)) fail(`expected ${show(actual)} to be ${show(expected)}`);
    },
    toEqual(expected: unknown): void {
      if (!equal(actual, expected)) fail(`expected ${show(actual)} to equal ${show(expected)}`);
    },
    toBeCloseTo(expected: number, tolerance = 1e-6): void {
      if (!(Math.abs((actual as number) - expected) <= tolerance)) fail(`expected ${show(actual)} to be within ${tolerance} of ${expected}`);
    },
    toBeTruthy(): void {
      if (!actual) fail(`expected ${show(actual)} to be truthy`);
    },
    toContain(item: unknown): void {
      const ok = typeof actual === "string" ? actual.includes(item as string) : (actual as unknown[]).includes(item);
      if (!ok) fail(`expected ${show(actual)} to contain ${show(item)}`);
    },
    toThrow(type?: new (...a: never[]) => Error, message?: string): void {
      try {
        (actual as () => unknown)();
      } catch (e) {
        if (type && !(e instanceof type)) fail(`expected a ${type.name}, got ${show(String(e))}`);
        if (message && !String((e as Error).message).includes(message)) fail(`expected the message to contain ${show(message)}, got ${show((e as Error).message)}`);
        return;
      }
      fail("expected it to throw");
    },
    async toReject(type?: new (...a: never[]) => Error): Promise<void> {
      try {
        await (actual as Promise<unknown>);
      } catch (e) {
        if (type && !(e instanceof type)) fail(`expected a rejection with ${type.name}, got ${show(String(e))}`);
        return;
      }
      fail("expected it to reject");
    },
  };
}

// ---- Turns and the clock ----------------------------------------------------------------------

// A timer at the target time guarantees a turn there: a stepped clock with nothing to draw
// sends no events otherwise.

/** Steps the paused clock by `ms` and resolves in the turn at that time. */
export function step(ms: number): Promise<void> {
  runner.requestStep(ms);
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/** Resolves in a later turn, without moving the clock. */
export function nextTurn(): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

/** The script hooks the runner has seen: uncaught exceptions, rejections, long tasks, leaks. */
export const Hook = { exception: 0, rejection: 1, longTask: 2, leak: 3 } as const;
export const hooks = runner.hooks;
/** Collects garbage now; FinalizationRegistry callbacks run in a following microtask. */
export const gc = runner.gc;
export const cBuffer = runner.cBuffer;
export const cEvents = runner.cEvents;

// ---- Running ------------------------------------------------------------------------------

async function run(): Promise<void> {
  // The app's surface comes with the first turn's events, after the module's evaluation.
  await new Promise<void>((resolve) => {
    const off = app.onSurface(() => {
      queueMicrotask(() => off());
      resolve();
    });
  });
  let failed = 0;
  for (const t of tests) {
    try {
      await t.body();
      console.log(`pass ${t.name}`);
    } catch (e) {
      failed++;
      console.error(`FAIL ${t.name}: ${e instanceof Error ? `${e.name}: ${e.message}\n${e.stack ?? ""}` : String(e)}`);
    }
  }
  runner.done(failed, tests.length);
}

// After the test module has registered everything (its top level runs first).
queueMicrotask(() => void run());
