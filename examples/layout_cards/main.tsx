// layout_cards in TSX: the C example (main.c) and the TypeScript one (main.ts) with signals and
// components. A wrapping grid of cards laid out by flexbox; every 2.5 s a card is inserted, moved
// or removed and the cards slide to their new places; every 4 s the theme switches between
// light and dark; every 6 s the page turns between left-to-right and right-to-left. A panel in
// the other theme (a token scope aliasing every theme token), and a hover state that re-themes a
// card's subtree, hovered card by card every 1.5 s.
//
//   layout_cards_tsx [-- --no-shuffle --no-theme --no-direction --no-hover --dark --rtl]
//   layout_cards_tsx --headless --at S --out f.ppm [-- ...]
//
// Its headless images equal the C version's (CTest: layout_cards_equivalence).

import {
  For,
  State,
  Text,
  TextAlign,
  TextSlant,
  Token,
  View,
  app,
  loadFont,
  mount,
  signal,
  spring,
  style,
  type ColorValue,
  rgba,
} from "@axe/ui";

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
const dark = signal(flags.has("--dark"));
const rtl = signal(flags.has("--rtl"));

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
const T = tokens(THEMES[dark.peek() ? 1 : 0]);
const I = tokens(THEMES[dark.peek() ? 0 : 1]);

function setTheme(): void {
  for (const key of KEYS) T[key].set(THEMES[dark.peek() ? 1 : 0][key]);
  for (const key of KEYS) I[key].set(THEMES[dark.peek() ? 0 : 1][key]);
}

const colorSpring = spring(90, 19);
const S = {
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

// Everything but the color (from the styles) and the direction (from the page), as C sets it.
const textOptions = { families, slant: TextSlant.normal, language: null, lineHeight: 0, align: TextAlign.start } as const;

// ---- State ------------------------------------------------------------------------------------

let nextId = 0, step = 0, hoverStep = 0, seed = 7;
const cards = signal<number[]>([]); // card ids, in display order
const panelCards: number[] = [];
const hovered = signal<number | null>(null); // a card id

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

// ---- Components -------------------------------------------------------------------------------

function Card(props: { id: number }) {
  const id = props.id;
  return (
    <View style={S.card} state={hovered() === id ? State.hover : 0}>
      <Text style={S.title} {...textOptions} size={16} weight={650}>Card {id}</Text>
      <Text style={S.body} {...textOptions} size={13} weight={400}>{PARAGRAPHS[id % PARAGRAPHS.length]}</Text>
      <View style={S.footer}>
        <Text style={S.label} {...textOptions} size={12} weight={400}>#{String(id).padStart(3, "0")}</Text>
        <Text style={S.button} {...textOptions} size={12} weight={600}>Open</Text>
      </View>
    </View>
  );
}

function App() {
  return (
    <View style={[S.page, { direction: rtl() ? "rtl" : "ltr" }]}>
      {/* The panel first: a heading and its own cards, in the inverted scheme. */}
      <View style={S.panel}>
        <Text style={[S.title, { width: "100%" }]} {...textOptions} size={15} weight={650}>
          A panel in the other theme: its scope aliases every token
        </Text>
        {panelCards.map((id) => <Card id={id} />)}
      </View>
      <For each={cards()}>{(id: number) => <Card id={id} />}</For>
    </View>
  );
}

let started = false;
app.onSurface((s) => {
  if (started) return; // layout follows the surface: only the first one matters
  started = true;
  for (let i = 0; i < PANEL_CARDS; i++) panelCards.push(nextId++);
  cards.set(Array.from({ length: 10 }, () => nextId++));
  mount(App, s);
  // The first frame starts the clock's cycles; timers keep them going while idle.
  const off = s.onFrame(() => {
    off();
    if (options.shuffle) setInterval(shuffle, SHUFFLE_MS);
    if (options.theme) setInterval(() => (dark.update((d) => !d), setTheme()), THEME_MS);
    if (options.direction) setInterval(() => rtl.update((r) => !r), DIRECTION_MS);
    if (options.hover) setInterval(hoverNext, HOVER_MS);
  });
});
