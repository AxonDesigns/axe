// layout_cards in TypeScript: the C example (main.c) on @axe/core, op for op. A wrapping grid
// of cards laid out by flexbox; every 2.5 s a card is inserted, moved or removed and the cards
// slide to their new places; every 4 s the theme switches between light and dark; every 6 s
// the page turns between left-to-right and right-to-left. Resizing the window reflows it.
// A panel in the other theme (a token scope aliasing every theme token) and a hover state
// that re-themes a card's subtree, hovered card by card every 1.5 s.
//
//   layout_cards_ts [-- --no-shuffle --no-theme --no-direction --no-hover --dark --rtl]
//   layout_cards_ts --headless --at S --out f.ppm [-- ...]
//
// Its headless images equal the C version's (CTest: layout_cards_equivalence).

import {
  app,
  keyword,
  loadFont,
  Node,
  number,
  percent,
  px,
  rgba,
  spring,
  State,
  Style,
  Token,
  Direction,
  FlexAlign,
  FlexDirection,
  FlexWrap,
  TextAlign,
  TextSlant,
  type ColorValue,
  type Surface,
} from "@axe/core";

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
let dark = flags.has("--dark");
let rtl = flags.has("--rtl");

loadFont(`${AXE_SOURCE_DIR}/tests/fonts/NotoSans-VF.ttf`).catch((e) => console.error(e));
const families = ["Noto Sans", "sans-serif"];

const PARAGRAPHS = [
  "Cards wrap onto as many rows as the window allows.",
  "Each card is a flex column: a title, this paragraph, and a footer row whose label and " +
    "button sit at either end.",
  "When a card is inserted or removed, the others slide to their new places.",
  "The theme is a handful of tokens. Switching it changes one value per token, and every " +
    "color that refers to one springs to its new value.",
  "Text is shaped on worker threads before its frame.",
  "Resize the window: the grid reflows, and the cards animate there too, keeping their " +
    "velocity when the target moves again.",
];

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
const T = tokens(THEMES[dark ? 1 : 0]);
const I = tokens(THEMES[dark ? 0 : 1]);

function setTheme(): void {
  for (const key of KEYS) T[key].set(THEMES[dark ? 1 : 0][key]);
  for (const key of KEYS) I[key].set(THEMES[dark ? 0 : 1][key]);
}

const colorSpring = spring(90, 19);
const S = {
  page: Style.define({
    decls: {
      flexGrow: number(1), flexWrap: keyword(FlexWrap.wrap), alignItems: keyword(FlexAlign.start),
      alignContent: keyword(FlexAlign.start), paddingTop: px(20), paddingRight: px(20), paddingBottom: px(20),
      paddingLeft: px(20), rowGap: px(14), columnGap: px(14), backgroundColor: T.page,
    },
    transitions: { backgroundColor: colorSpring },
  }),
  card: Style.define({
    decls: {
      width: px(208), flexDirection: keyword(FlexDirection.column), paddingTop: px(14), paddingRight: px(14),
      paddingBottom: px(12), paddingLeft: px(14), rowGap: px(8), borderWidth: px(1), cornerRadius: px(10),
      backgroundColor: T.card, borderColor: T.border, borderStartWidth: px(4), borderStartColor: T.accent,
    },
    transitions: { backgroundColor: colorSpring, borderColor: colorSpring, layout: spring(170, 24) },
    // Hovered, it re-themes its subtree: the hover color, and the accent for its border and
    // title, resolved where they are used (in the panel, through the panel's scope).
    states: [{ when: State.hover, tokens: [T.card.with(T.hover), T.border.with(T.accent), T.title.with(T.accent)] }],
  }),
  title: Style.define({ decls: { textColor: T.title }, transitions: { textColor: colorSpring } }),
  body: Style.define({ decls: { textColor: T.body, lineHeight: number(1.4) }, transitions: { textColor: colorSpring } }),
  footer: Style.define({
    decls: { justifyContent: keyword(FlexAlign.spaceBetween), alignItems: keyword(FlexAlign.center), marginTop: px(4) },
  }),
  label: Style.define({ decls: { textColor: T.muted }, transitions: { textColor: colorSpring } }),
  button: Style.define({
    decls: {
      paddingTop: px(4), paddingRight: px(12), paddingBottom: px(4), paddingLeft: px(12), cornerRadius: px(3),
      cornerRadiusTopStart: px(11), cornerRadiusBottomStart: px(11), backgroundColor: T.accent, textColor: T.accentText,
    },
    transitions: { backgroundColor: colorSpring, textColor: colorSpring },
  }),
  // A full row of the page in the other theme: its scope makes every theme token the inverse
  // one, for it and its subtree.
  panel: Style.define({
    decls: {
      width: percent(100), flexWrap: keyword(FlexWrap.wrap), alignItems: keyword(FlexAlign.start), paddingTop: px(16),
      paddingRight: px(16), paddingBottom: px(16), paddingLeft: px(16), rowGap: px(12), columnGap: px(14),
      cornerRadius: px(14), borderWidth: px(1), backgroundColor: T.page, borderColor: T.border,
    },
    transitions: { backgroundColor: colorSpring, borderColor: colorSpring },
    tokens: KEYS.map((k) => T[k].with(I[k])),
  }),
};

// Everything but the color (from the styles) and the direction (from the page), as C sets it.
function text(node: Node, s: string, size: number, weight: number): void {
  node.setText(s, { families, size, weight, slant: TextSlant.normal, language: null, lineHeight: 0, align: TextAlign.start });
}

interface Card {
  readonly node: Node;
  readonly id: number;
}

let surface: Surface | null = null;
let page: Node | null = null;
const cards: Card[] = []; // in display order
const panelCards: Card[] = [];
let hovered: Node | null = null;
let nextId = 0, step = 0, hoverStep = 0, seed = 7;

function nextRandom(): number {
  seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
  return seed >>> 8;
}

function setDirection(): void {
  page!.setProps({ direction: keyword(rtl ? Direction.rtl : Direction.ltr) });
}

// A card's nodes, appended to `parent`.
function makeCard(parent: Node): Card {
  const id = nextId++;
  const card = Node.create(parent, { styles: [S.card] });
  text(Node.create(card, { styles: [S.title] }), `Card ${id}`, 16, 650);
  text(Node.create(card, { styles: [S.body] }), PARAGRAPHS[id % PARAGRAPHS.length]!, 13, 400);
  const footer = Node.create(card, { styles: [S.footer] });
  text(Node.create(footer, { styles: [S.label] }), `#${String(id).padStart(3, "0")}`, 12, 400);
  text(Node.create(footer, { styles: [S.button] }), "Open", 12, 600);
  return { node: card, id };
}

function addCard(at: number): void {
  const c = makeCard(page!);
  if (at < cards.length) c.node.moveTo(page!, cards[at]!.node);
  cards.splice(at, 0, c);
}

function removeCard(at: number): void {
  const [c] = cards.splice(at, 1);
  if (c!.node === hovered) hovered = null;
  c!.node.destroy();
}

function moveCard(from: number, to: number): void {
  const [c] = cards.splice(from, 1);
  const before = to < cards.length ? cards[to]!.node : null;
  cards.splice(to, 0, c!);
  c!.node.moveTo(page!, before);
}

// Insert, move, remove, move, in turn (an insert or a remove that would leave the grid too
// full or too empty moves a card instead).
function shuffle(): void {
  const kind = step++ % 4;
  if (kind === 0 && cards.length < MAX_CARDS) return addCard(nextRandom() % (cards.length + 1));
  if (kind === 2 && cards.length > 4) return removeCard(nextRandom() % cards.length);
  const from = nextRandom() % cards.length;
  let to = nextRandom() % cards.length;
  if (to === from) to = (from + 1) % cards.length;
  moveCard(from, to);
}

// Hovers the next card in turn (the panel's first), after un-hovering the last.
function hoverNext(): void {
  hovered?.setState(State.hover, 0);
  const i = hoverStep++ % (PANEL_CARDS + cards.length);
  hovered = (i < PANEL_CARDS ? panelCards[i] : cards[i - PANEL_CARDS])!.node;
  hovered.setState(State.hover, State.hover);
}

app.onSurface((s) => {
  if (surface) return; // layout follows the surface: only the first one matters
  surface = s;
  page = Node.create(s, { styles: [S.page] });
  setDirection();
  // The panel first: a heading and its own cards, in the inverted scheme.
  const panel = Node.create(page, { styles: [S.panel] });
  const heading = Node.create(panel, { styles: [S.title] });
  heading.setProps({ width: percent(100) });
  text(heading, "A panel in the other theme: its scope aliases every token", 15, 650);
  for (let i = 0; i < PANEL_CARDS; i++) panelCards.push(makeCard(panel));
  for (let i = 0; i < 10; i++) addCard(cards.length);
  // The first frame starts the clock's cycles; timers keep them going while idle.
  const off = s.onFrame(() => {
    off();
    if (options.shuffle) setInterval(shuffle, SHUFFLE_MS);
    if (options.theme) setInterval(() => ((dark = !dark), setTheme()), THEME_MS);
    if (options.direction) setInterval(() => ((rtl = !rtl), setDirection()), DIRECTION_MS);
    if (options.hover) setInterval(hoverNext, HOVER_MS);
  });
});
