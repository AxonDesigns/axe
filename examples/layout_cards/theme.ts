// layout_cards' theme. It exports tokens only, each made by a call of its own, so they keep their
// ids and this module is a hot reload boundary: edit a color here and it changes in place.

import { Token, app, type ColorValue, rgba } from "@axe/ui";
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
const KEYS = Object.keys(THEMES[0]) as Key[];
const current = THEMES[dark.peek() ? 1 : 0], other = THEMES[dark.peek() ? 0 : 1];
export const T = {
  page: Token.create(current.page),
  card: Token.create(current.card),
  border: Token.create(current.border),
  title: Token.create(current.title),
  body: Token.create(current.body),
  muted: Token.create(current.muted),
  accent: Token.create(current.accent),
  accentText: Token.create(current.accentText),
  hover: Token.create(current.hover),
};
export const I = {
  page: Token.create(other.page),
  card: Token.create(other.card),
  border: Token.create(other.border),
  title: Token.create(other.title),
  body: Token.create(other.body),
  muted: Token.create(other.muted),
  accent: Token.create(other.accent),
  accentText: Token.create(other.accentText),
  hover: Token.create(other.hover),
};

// The tokens follow `dark`, at the end of a turn that changed it, and when this module is
// evaluated again, so an edit to a color above shows. A turn handler, not an effect: this module
// has no owner in a bundle.
let applied: boolean | null = null;
app.onTurnEnd(() => {
  if (applied === dark.peek()) return;
  applied = dark.peek();
  for (const key of KEYS) T[key].set(THEMES[applied ? 1 : 0][key]);
  for (const key of KEYS) I[key].set(THEMES[applied ? 0 : 1][key]);
});
