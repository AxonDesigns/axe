// layout_cards' tokens and styles. A style-only edit here re-runs no component.

import { Token, spring, style, type ColorValue, rgba } from "@axe/ui";
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
const tokens = (t: Theme) => Object.fromEntries(KEYS.map((k) => [k, Token.create(t[k])])) as Record<Key, Token<ColorValue>>;
export const T = tokens(THEMES[dark.peek() ? 1 : 0]);
export const I = tokens(THEMES[dark.peek() ? 0 : 1]);

export function setTheme(): void {
  for (const key of KEYS) T[key].set(THEMES[dark.peek() ? 1 : 0][key]);
  for (const key of KEYS) I[key].set(THEMES[dark.peek() ? 0 : 1][key]);
}

const colorSpring = spring(90, 19);
export const S = {
  page: style({
    flexGrow: 1, flexWrap: "wrap", alignItems: "start", alignContent: "start", padding: 20, gap: 14,
    backgroundColor: T.page,
    transitions: { backgroundColor: colorSpring },
  }),
  card: style({
    width: 208, flexDirection: "column", padding: [14, 14, 12, 14], rowGap: 8, borderWidth: 1, cornerRadius: 10,
    backgroundColor: T.card, borderColor: T.border, borderStartWidth: 4, borderStartColor: T.accent,
    transitions: { backgroundColor: colorSpring, borderColor: colorSpring, layout: spring(170, 24) },
    // Hovered, it re-themes its subtree: the hover color, and the accent for its border and
    // title, resolved where they are used (in the panel, through the panel's scope).
    hover: { tokens: [T.card.with(T.hover), T.border.with(T.accent), T.title.with(T.accent)] },
  }),
  title: style({ textColor: T.title, transitions: { textColor: colorSpring } }),
  body: style({ textColor: T.body, lineHeight: 1.4, transitions: { textColor: colorSpring } }),
  footer: style({ justifyContent: "space-between", alignItems: "center", marginTop: 4 }),
  label: style({ textColor: T.muted, transitions: { textColor: colorSpring } }),
  button: style({
    padding: [4, 12], cornerRadius: 3, cornerRadiusTopStart: 11, cornerRadiusBottomStart: 11,
    backgroundColor: T.accent, textColor: T.accentText,
    transitions: { backgroundColor: colorSpring, textColor: colorSpring },
  }),
  // A full row of the page in the other theme: its scope makes every theme token the inverse
  // one, for it and its subtree.
  panel: style({
    width: "100%", flexWrap: "wrap", alignItems: "start", padding: 16, rowGap: 12, columnGap: 14,
    cornerRadius: 14, borderWidth: 1, backgroundColor: T.page, borderColor: T.border,
    transitions: { backgroundColor: colorSpring, borderColor: colorSpring },
    tokens: KEYS.map((k) => T[k].with(I[k])),
  }),
};

