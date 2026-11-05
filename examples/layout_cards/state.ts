// layout_cards' state: signals only, so this module is a hot reload boundary: an edit here keeps
// every value, and what imports it keeps working (docs/ui.md, "Hot reload").

import { app, signal } from "@axe/ui";

export const dark = signal(app.args.includes("--dark"));
export const rtl = signal(app.args.includes("--rtl"));
export const cards = signal<number[]>([]); // card ids, in display order
export const panelCards = signal<number[]>([]);
export const hovered = signal<number | null>(null); // a card id
export const nextId = signal(0);
