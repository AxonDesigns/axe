// @axe/ui's signals: glitch-free diamonds, unchanged values stopping propagation, dynamic
// dependencies, effects at the end of the turn (render bindings first, creation order), owners
// and the order of disposal and cleanups, errors with and without a boundary.

import { app, computed, effect, onCleanup, root, signal, untrack } from "@axe/ui";
import { Hook, expect, hooks, nextTurn, test } from "@axe/testing";
import { dispose, getOwner, renderEffect } from "../src/reactive.js";
import { flush } from "../src/reactive.js";

const exceptions = () => hooks().filter((h) => h.kind === Hook.exception);

test("a signal reads, sets and updates; peek doesn't subscribe", () => {
  const s = signal(1);
  expect(s()).toBe(1);
  s.set(2);
  s.update((v) => v * 10);
  expect(s.peek()).toBe(20);
});

test("a computed runs when read, once per change, and caches", () => {
  root((done) => {
    const s = signal(1);
    let runs = 0;
    const double = computed(() => (runs++, s() * 2));
    expect(runs).toBe(0); // lazy
    expect(double()).toBe(2);
    expect(double()).toBe(2);
    expect(runs).toBe(1);
    s.set(5);
    s.set(6);
    expect(runs).toBe(1); // writes run nothing
    expect(double()).toBe(12);
    expect(runs).toBe(2);
    done();
  });
});

test("diamonds are glitch-free: each computed runs once and never sees a mix", () => {
  root((done) => {
    const a = signal(1);
    const b = computed(() => a() + 1);
    const c = computed(() => a() * 2);
    const seen: [number, number][] = [];
    const d = computed(() => {
      seen.push([b(), c()]);
      return b() + c();
    });
    let effects = 0;
    let last = 0;
    effect(() => {
      effects++;
      last = d();
    });
    flush();
    a.set(2);
    a.set(3);
    flush();
    // Every pair d saw is consistent: b = a + 1, c = 2a for the same a.
    for (const [x, y] of seen) expect(y).toBe((x - 1) * 2);
    expect(seen.length).toBe(2);
    expect(effects).toBe(2);
    expect(last).toBe(4 + 6);
    done();
  });
});

test("an unchanged computed stops the propagation", () => {
  root((done) => {
    const n = signal(2);
    const parity = computed(() => n() % 2);
    let downstream = 0;
    effect(() => {
      parity();
      downstream++;
    });
    flush();
    n.set(4); // parity still 0
    flush();
    expect(downstream).toBe(1);
    n.set(5);
    flush();
    expect(downstream).toBe(2);
    done();
  });
});

test("dependencies are dynamic: a branch not taken isn't tracked", () => {
  root((done) => {
    const useA = signal(true), a = signal("a"), b = signal("b");
    let runs = 0;
    effect(() => {
      runs++;
      void (useA() ? a() : b());
    });
    flush();
    b.set("b2"); // not read
    flush();
    expect(runs).toBe(1);
    useA.set(false);
    flush();
    expect(runs).toBe(2);
    a.set("a2"); // not read any more
    flush();
    expect(runs).toBe(2);
    b.set("b3");
    flush();
    expect(runs).toBe(3);
    done();
  });
});

test("effects run at the end of the turn, once however many writes", async () => {
  const s = signal(0);
  let runs = 0;
  let seen = -1;
  const dispose = root((done) => {
    effect(() => {
      runs++;
      seen = s();
    });
    return done;
  });
  expect(runs).toBe(0); // the first run too waits for the turn's end
  await nextTurn();
  expect(runs).toBe(1);
  s.set(1);
  s.set(2);
  s.set(3);
  expect(runs).toBe(1);
  await nextTurn();
  expect(runs).toBe(2);
  expect(seen).toBe(3);
  dispose();
});

test("bindings run before effects, each in creation order", () => {
  root((done) => {
    const s = signal(0);
    const order: string[] = [];
    effect(() => (s(), order.push("effect 1")));
    renderEffect(() => (s(), order.push("binding 1")));
    effect(() => (s(), order.push("effect 2")));
    renderEffect(() => (s(), order.push("binding 2")));
    flush();
    order.length = 0;
    s.set(1);
    flush();
    expect(order).toEqual(["binding 1", "binding 2", "effect 1", "effect 2"]);
    done();
  });
});

test("an effect's writes queue another round; effects that never settle are reported", () => {
  root((done) => {
    const a = signal(0), b = signal(0);
    effect(() => b.set(a() + 1));
    let seen = -1;
    effect(() => (seen = b()));
    flush();
    expect(seen).toBe(1);
    a.set(5);
    flush();
    expect(seen).toBe(6);

    const before = exceptions().length;
    const loop = signal(0);
    effect(() => loop.set(loop() + 1));
    flush();
    const reported = exceptions().slice(before);
    expect(reported.length).toBe(1);
    expect(reported[0]!.text).toContain("did not settle");
    done();
  });
  flush(); // the looping effect is disposed with its root: nothing left queued
});

test("untrack reads without subscribing", () => {
  root((done) => {
    const a = signal(1), b = signal(1);
    let runs = 0;
    effect(() => {
      runs++;
      a();
      untrack(() => b());
    });
    flush();
    b.set(2);
    flush();
    expect(runs).toBe(1);
    done();
  });
});

test("disposal: children newest first, cleanups in reverse order, effects stop", () => {
  const order: string[] = [];
  const s = signal(0);
  let runs = 0;
  root((done) => {
    onCleanup(() => order.push("root 1"));
    onCleanup(() => order.push("root 2"));
    effect(() => {
      s();
      runs++;
      onCleanup(() => order.push(`effect run ${runs}`));
    });
    renderEffect(() => {
      s();
      onCleanup(() => order.push("binding"));
    });
    flush();
    s.set(1);
    flush(); // the effect's cleanup runs before its next run
    expect(order).toEqual(["binding", "effect run 1"]);
    order.length = 0;
    done();
  });
  // The binding (created last) first, then the effect, then the root's own, last first.
  expect(order).toEqual(["binding", "effect run 2", "root 2", "root 1"]);
  s.set(2);
  flush();
  expect(runs).toBe(2);
});

test("a computed's error reaches its readers until a source changes", () => {
  root((done) => {
    const s = signal(0);
    const c = computed(() => {
      if (s() === 0) throw new Error("zero");
      return 10 / s();
    });
    expect(() => c()).toThrow(Error, "zero");
    expect(() => c()).toThrow(Error, "zero");
    s.set(2);
    expect(c()).toBe(5);
    done();
  });
});

test("an error in an effect with no boundary is reported, and the rest runs", () => {
  root((done) => {
    const s = signal(0);
    let other = 0;
    effect(() => {
      if (s() === 1) throw new Error("effect failed");
    });
    effect(() => {
      s();
      other++;
    });
    flush();
    const before = exceptions().length;
    s.set(1);
    flush();
    const reported = exceptions().slice(before);
    expect(reported.length).toBe(1);
    expect(reported[0]!.text).toContain("effect failed");
    expect(reported[0]!.text).toContain("in an effect");
    expect(other).toBe(2);
    // It stays subscribed: a later change runs it again.
    s.set(2);
    flush();
    expect(exceptions().length).toBe(before + 1);
    done();
  });
});

test("an owner disposed while a computation runs stops it from running again", () => {
  root((done) => {
    const s = signal(0);
    let runs = 0;
    const owner = getOwner()!;
    effect(() => {
      runs++;
      if (s() === 1) dispose(owner);
    });
    flush();
    s.set(1);
    flush();
    s.set(2);
    flush();
    expect(runs).toBe(2);
    done();
  });
});

test("writes in onTurnEnd handlers reach the same turn's effects", async () => {
  const s = signal(0);
  const seen: number[] = [];
  const disposeRoot = root((done) => {
    effect(() => void seen.push(s()));
    return done;
  });
  const off = app.onTurnEnd(() => {
    if (s.peek() === 1) s.set(2);
  });
  await nextTurn();
  s.set(1);
  await nextTurn();
  // The handler ran before the flush: the effect saw 2 in that turn, never 1.
  expect(seen).toEqual([0, 2]);
  off();
  disposeRoot();
});

test("a computed with equals: false propagates every run; a custom equals compares", () => {
  root((done) => {
    const s = signal({ x: 1 }, { equals: (a, b) => a.x === b.x });
    let runs = 0;
    effect(() => {
      s();
      runs++;
    });
    flush();
    s.set({ x: 1 }); // equal by x
    flush();
    expect(runs).toBe(1);
    s.set({ x: 2 });
    flush();
    expect(runs).toBe(2);
    const t = signal(0);
    const always = computed(() => (t(), {}), { equals: false });
    let downstream = 0;
    effect(() => {
      always();
      downstream++;
    });
    flush();
    t.set(1);
    flush();
    expect(downstream).toBe(2);
    done();
  });
});

test("a throwing cleanup is reported and the others still run", () => {
  const order: string[] = [];
  const before = exceptions().length;
  root((done) => {
    onCleanup(() => order.push("first"));
    onCleanup(() => {
      throw new Error("cleanup failed");
    });
    onCleanup(() => order.push("last"));
    done();
  });
  expect(order).toEqual(["last", "first"]);
  expect(exceptions().length).toBe(before + 1);
});
