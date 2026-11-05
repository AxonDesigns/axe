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
// Its headless images equal the C version's (CTest: layout_cards_equivalence). With --dev, edits
// to cards.tsx and styles.ts apply while it runs (docs/ui.md, "Hot reload").

import { app, getOwner, loadFont, mount, runWithOwner } from "@axe/ui";
import { App } from "./cards";
import { populate, startCycles } from "./state";
import { setTheme } from "./styles";

loadFont(`${AXE_SOURCE_DIR}/tests/fonts/NotoSans-VF.ttf`).catch((e) => console.error(e));

let started = false;
app.onSurface((s) => {
  if (started) return; // layout follows the surface: only the first one matters
  started = true;
  populate();
  mount(App, s);
  // The first frame starts the clock's cycles; timers keep them going while idle.
  const owner = getOwner();
  const off = s.onFrame(() => {
    off();
    runWithOwner(owner, () => startCycles(setTheme));
  });
});
