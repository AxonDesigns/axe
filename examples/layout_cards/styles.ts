// layout_cards' styles. A style-only edit here re-runs no component.

import { spring, style } from "@axe/ui";
import { I, T } from "./theme";

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
    tokens: (Object.keys(T) as (keyof typeof T)[]).map((k) => T[k].with(I[k])),
  }),
};

