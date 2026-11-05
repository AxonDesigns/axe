// layout_cards' theme: its tokens, and what switches them. Colors here hold the app's state, so an
// edit here reaches main.tsx and is a full reload.

import { Token, type ColorValue, rgba } from "@axe/ui";
import { dark } from "./state";

// Theme tokens, light and dark, in the C version's order (so their ids match); then the
// inverse ones, holding the other theme's values, which the panel's scope aliases.
type Key = "page" | "card" | "border" | "title" | "body" | "muted" | "accent" | "accentText" | "hover";
type Theme = Record<Key, ColorValue>;
const THEMES: [Theme, Theme] = [
  {
    page: rgba(0.93, 0.94, 0.96), card: rgba(1, 1, 1), border: rgba(0.84, 0.86, 0.9), title: rgba(0.1, 0.11, 0.14),
    body: rgba(0.3, 0.32, 0.36), muted: rgba(0.5, 0.52, 0.56), accent: rgba(0.2, 0.45, 0.95), accentText: rgba(1, 1, 1),
    hover: rgba(0.87, 0.92, 1),
  },
  {
    page: rgba(0.07, 0.08, 0.1), card: rgba(0.13, 0.14, 0.17), border: rgba(0.24, 0.26, 0.31), title: rgba(0.93, 0.94, 0.96),
    body: rgba(0.72, 0.74, 0.78), muted: rgba(0.55, 0.57, 0.62), accent: rgba(0.38, 0.62, 1), accentText: rgba(0.05, 0.06, 0.08),
    hover: rgba(0.12, 0.18, 0.31),
  },
];
export const KEYS = Object.keys(THEMES[0]) as Key[];
const tokens = (t: Theme) => Object.fromEntries(KEYS.map((k) => [k, Token.create(t[k])])) as Record<Key, Token<ColorValue>>;
export const T = tokens(THEMES[dark.peek() ? 1 : 0]);
export const I = tokens(THEMES[dark.peek() ? 0 : 1]);

export function setTheme(): void {
  for (const key of KEYS) T[key].set(THEMES[dark.peek() ? 1 : 0][key]);
  for (const key of KEYS) I[key].set(THEMES[dark.peek() ? 0 : 1][key]);
}
