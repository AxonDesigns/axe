// layout_cards' state: what the cards are, what is hovered, the theme and the direction, and the
// cycles that change them. Hot reload keeps its signals across edits (docs/ui.md, "Hot reload").

import { app, signal } from "@axe/ui";

const MAX_CARDS = 24;
const SHUFFLE_MS = 2500;
const THEME_MS = 4000;
const DIRECTION_MS = 6000;
const HOVER_MS = 1500;
const PANEL_CARDS = 2;

const flags = new Set(app.args);
const options = {
  shuffle: !flags.has("--no-shuffle"),
  theme: !flags.has("--no-theme"),
  direction: !flags.has("--no-direction"),
  hover: !flags.has("--no-hover"),
};
export const dark = signal(flags.has("--dark"));
export const rtl = signal(flags.has("--rtl"));

let nextId = 0, step = 0, hoverStep = 0, seed = 7;
export const cards = signal<number[]>([]); // card ids, in display order
export const panelCards: number[] = [];
export const hovered = signal<number | null>(null); // a card id

function nextRandom(): number {
  seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
  return seed >>> 8;
}

// Insert, move, remove, move, in turn (an insert or a remove that would leave the grid too
// full or too empty moves a card instead).
function shuffle(): void {
  const list = [...cards.peek()];
  const kind = step++ % 4;
  if (kind === 0 && list.length < MAX_CARDS) {
    list.splice(nextRandom() % (list.length + 1), 0, nextId++);
  } else if (kind === 2 && list.length > 4) {
    const [gone] = list.splice(nextRandom() % list.length, 1);
    if (hovered.peek() === gone) hovered.set(null);
  } else {
    const from = nextRandom() % list.length;
    let to = nextRandom() % list.length;
    if (to === from) to = (from + 1) % list.length;
    const [c] = list.splice(from, 1);
    list.splice(to, 0, c!);
  }
  cards.set(list);
}

// Hovers the next card in turn (the panel's first), after un-hovering the last.
function hoverNext(): void {
  const list = cards.peek();
  const i = hoverStep++ % (PANEL_CARDS + list.length);
  hovered.set(i < PANEL_CARDS ? panelCards[i]! : list[i - PANEL_CARDS]!);
}

/** The first cards. */
export function populate(): void {
  if (cards.peek().length) return;
  for (let i = 0; i < PANEL_CARDS; i++) panelCards.push(nextId++);
  cards.set(Array.from({ length: 10 }, () => nextId++));
}

/** The cycles that change the state; `onTheme` runs after each theme switch. */
export function startCycles(onTheme: () => void): void {
  if (options.shuffle) setInterval(shuffle, SHUFFLE_MS);
  if (options.theme) setInterval(() => (dark.update((d) => !d), onTheme()), THEME_MS);
  if (options.direction) setInterval(() => rtl.update((r) => !r), DIRECTION_MS);
  if (options.hover) setInterval(hoverNext, HOVER_MS);
}
