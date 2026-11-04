// cards: the benchmark app (bench_script's startup, memory and first-frame cases). A frozen
// copy of examples/layout_cards/main.ts as of milestone 3a (cf963fc): a wrapping grid of 10
// cards, three themed text styles each, eight theme tokens, and the timers that shuffle,
// re-theme and mirror them.
//
// FROZEN. Its instruction counts are baselines (bench/baselines.json), so it changes only on
// purpose, with the baselines updated from CI's artifact in a commit of its own
// (CONTRIBUTING.md, "Bench apps"). The example evolves freely; this doesn't follow it.

import {
  app,
  keyword,
  loadFont,
  Node,
  number,
  px,
  rgba,
  spring,
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

const flags = new Set(app.args);
const options = {
  shuffle: !flags.has("--no-shuffle"),
  theme: !flags.has("--no-theme"),
  direction: !flags.has("--no-direction"),
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

// Theme tokens, light and dark, in the C version's order (so their ids match).
type Theme = Record<"page" | "card" | "border" | "title" | "body" | "muted" | "accent" | "accentText", ColorValue>;
const THEMES: [Theme, Theme] = [
  {
    page: rgba(0.93, 0.94, 0.96), card: rgba(1, 1, 1), border: rgba(0.84, 0.86, 0.9), title: rgba(0.1, 0.11, 0.14),
    body: rgba(0.3, 0.32, 0.36), muted: rgba(0.5, 0.52, 0.56), accent: rgba(0.2, 0.45, 0.95), accentText: rgba(1, 1, 1),
  },
  {
    page: rgba(0.07, 0.08, 0.1), card: rgba(0.13, 0.14, 0.17), border: rgba(0.24, 0.26, 0.31), title: rgba(0.93, 0.94, 0.96),
    body: rgba(0.72, 0.74, 0.78), muted: rgba(0.55, 0.57, 0.62), accent: rgba(0.38, 0.62, 1), accentText: rgba(0.05, 0.06, 0.08),
  },
];
const theme = THEMES[dark ? 1 : 0];
const T = {
  page: Token.create(theme.page),
  card: Token.create(theme.card),
  border: Token.create(theme.border),
  title: Token.create(theme.title),
  body: Token.create(theme.body),
  muted: Token.create(theme.muted),
  accent: Token.create(theme.accent),
  accentText: Token.create(theme.accentText),
};

function setTheme(): void {
  const t = THEMES[dark ? 1 : 0];
  for (const key of Object.keys(T) as (keyof Theme)[]) T[key].set(t[key]);
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
let nextId = 0, step = 0, seed = 7;

function nextRandom(): number {
  seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
  return seed >>> 8;
}

function setDirection(): void {
  page!.setProps({ direction: keyword(rtl ? Direction.rtl : Direction.ltr) });
}

function addCard(at: number): void {
  const id = nextId++;
  const card = Node.create(page!, { styles: [S.card] });
  text(Node.create(card, { styles: [S.title] }), `Card ${id}`, 16, 650);
  text(Node.create(card, { styles: [S.body] }), PARAGRAPHS[id % PARAGRAPHS.length]!, 13, 400);
  const footer = Node.create(card, { styles: [S.footer] });
  text(Node.create(footer, { styles: [S.label] }), `#${String(id).padStart(3, "0")}`, 12, 400);
  text(Node.create(footer, { styles: [S.button] }), "Open", 12, 600);
  if (at < cards.length) card.moveTo(page!, cards[at]!.node);
  cards.splice(at, 0, { node: card, id });
}

function removeCard(at: number): void {
  const [c] = cards.splice(at, 1);
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

app.onSurface((s) => {
  if (surface) return; // layout follows the surface: only the first one matters
  surface = s;
  page = Node.create(s, { styles: [S.page] });
  setDirection();
  for (let i = 0; i < 10; i++) addCard(cards.length);
  // The first frame starts the clock's cycles; timers keep them going while idle.
  const off = s.onFrame(() => {
    off();
    if (options.shuffle) setInterval(shuffle, SHUFFLE_MS);
    if (options.theme) setInterval(() => ((dark = !dark), setTheme()), THEME_MS);
    if (options.direction) setInterval(() => ((rtl = !rtl), setDirection()), DIRECTION_MS);
  });
});
