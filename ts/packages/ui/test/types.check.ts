// Type tests for @axe/ui, checked by ts_typecheck (never run): each @ts-expect-error must stay
// an error, or tsc fails ("Unused '@ts-expect-error' directive").

import {
  Show,
  Span,
  Text,
  Token,
  View,
  createComponent,
  px,
  auto,
  signal,
  spring,
  style,
  token,
  type ColorValue,
  type PxValue,
  type Signal,
} from "@axe/ui";
import type { AutoValue } from "@axe/wire";

// ---- Styles -------------------------------------------------------------------------------------

style({ width: 208, height: "50%", maxWidth: "none", flexDirection: "column", backgroundColor: "#36f" });
style({ backgroundColor: "#3366ff", borderColor: "#3366ff80", textColor: "rgb(51, 102, 255)", opacity: 0.5 });
style({ padding: [4, 8], margin: [1, 2, 3, 4], gap: 8, justifyContent: "space-between" });
style({ hover: { backgroundColor: "#fff" }, transitions: { backgroundColor: spring(90, 19), layout: "none" } });
style({ width: px(4), minWidth: auto, fontFamily: "Noto Sans, sans-serif", fontWeight: 650, lineHeight: "auto" });

// @ts-expect-error a hex color of the wrong length
style({ backgroundColor: "#12345" });
// @ts-expect-error not a hex digit
style({ backgroundColor: "#ggg" });
// @ts-expect-error inside a state block too
style({ hover: { borderColor: "#zzzzzz" } });
// @ts-expect-error a length takes no unit string
style({ width: "12px" });
// @ts-expect-error a color isn't a length
style({ width: "#fff" });
// @ts-expect-error a number isn't a color
style({ backgroundColor: 3 });
// @ts-expect-error an unknown keyword
style({ flexDirection: "columns" });
// @ts-expect-error a keyword justify-content doesn't take
style({ justifyContent: "baseline" });
// @ts-expect-error opacity takes no percentage
style({ opacity: "50%" });
// @ts-expect-error a token of another kind
style({ width: token("#fff") });
// @ts-expect-error an unknown property
style({ colour: "#fff" });
// @ts-expect-error a transition of a property without one
style({ transitions: { width: spring(1, 1) } });

// ---- Tokens -------------------------------------------------------------------------------------

const size: Token<PxValue> = token(4);
const ink: Token<ColorValue> = token("#111");
ink.with("#eee");
ink.with(ink);
size.set(8);
// @ts-expect-error a token has one kind: a union is a compile error
type Both = Token<PxValue | AutoValue>;
// @ts-expect-error a declaration of another kind
size.with("#fff");
// @ts-expect-error an invalid hex color in a declaration
ink.with("#12");

// ---- Elements -----------------------------------------------------------------------------------

View({ children: [View({}), Text({ children: "text goes in Text" })] });
Text({ children: ["a", 1, Span({ weight: 700, children: "b" }), () => "c"] });
// @ts-expect-error text in a View must be inside Text
View({ children: "loose text" });
// @ts-expect-error a number in a View too
View({ children: [View({}), 42] });
// @ts-expect-error a Span must be inside Text
View({ children: Span({ children: "x" }) });
// @ts-expect-error a View can't go inside a Text
Text({ children: View({}) });

// Components: typed props, typed children.
function Card(props: { title: string; count: Signal<number> }) {
  return Text({ get children() { return [props.title, " ", props.count()]; } });
}
createComponent(Card, { title: "x", count: signal(1) });
// @ts-expect-error a prop of the wrong type
createComponent(Card, { title: 1, count: signal(1) });
createComponent(Show<number>, { when: 1, children: (n: () => number) => Text({ get children() { return n(); } }) });

export type { Both };
