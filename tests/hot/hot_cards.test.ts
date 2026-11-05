// layout_cards' own sources, hot: state.ts and theme.ts are boundaries, so an edit keeps the app
// where it is, and cycles.ts, plain functions, reaches the entry. (The cycles are off: the test
// sets the state.)

import { expect, nextTurn, test } from "@axe/testing";
import { hotEdit, readText } from "axe:test";
import { hotState } from "@axe/ui/hot";

const FILES = ["main.tsx", "cards.tsx", "cycles.ts", "state.ts", "styles.ts", "theme.ts"];
const source = (f: string): string => readText(`examples/layout_cards/${f}`);

async function edit(files: Record<string, string>): Promise<void> {
  const before = hotState().updates;
  hotEdit(files);
  while (hotState().updates === before) await nextTurn();
  await nextTurn();
}

interface Sig<T> {
  (): T;
  set(v: T): void;
}

test("edits keep layout_cards' state", async () => {
  await edit(Object.fromEntries(FILES.map((f) => [f, source(f)])));
  expect(hotState().overlay).toBe(null);
  const state = hotState().exports("state.ts")!;
  const cards = state["cards"] as Sig<number[]>, dark = state["dark"] as Sig<boolean>, hovered = state["hovered"] as Sig<number | null>;
  await nextTurn();
  expect(cards().length).toBe(10);
  const order = [...cards()].reverse();
  cards.set(order);
  dark.set(true);
  hovered.set(4);
  await nextTurn();
  const runs = hotState().runs("cards.tsx", "Card"), appRuns = hotState().runs("cards.tsx", "App");
  const check = (): void => {
    expect(cards()).toEqual(order);
    expect(dark()).toBe(true);
    expect(hovered()).toBe(4);
    expect(hotState().fullReloads).toBe(0);
    expect(hotState().overlay).toBe(null);
    expect(hotState().runs("cards.tsx", "Card")).toBe(runs);
    expect(hotState().runs("cards.tsx", "App")).toBe(appRuns);
  };

  await edit({ "state.ts": `${source("state.ts")}\n// edited\nconst unused = 1;\nvoid unused;\n` });
  check();
  expect(hotState().exports("state.ts")!["cards"]).toBe(cards);

  const theme = source("theme.ts");
  expect(theme.includes("0.93, 0.94, 0.96")).toBe(true);
  await edit({ "theme.ts": theme.replace("0.93, 0.94, 0.96", "0.9, 0.5, 0.5") });
  check();

  await edit({ "styles.ts": source("styles.ts").replace("padding: 20", "padding: 24") });
  check();

  // Plain functions: this propagates, to the entry, and the signals still hold.
  await edit({ "cycles.ts": `${source("cycles.ts")}\n// edited\n` });
  expect(hotState().fullReloads).toBe(1);
  expect(cards()).toEqual(order);
  expect(dark()).toBe(true);
  expect(hovered()).toBe(4);
  expect(hotState().overlay).toBe(null);
});
