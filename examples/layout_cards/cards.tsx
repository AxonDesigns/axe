// layout_cards' components.

import { For, State, Text, TextAlign, TextSlant, View } from "@axe/ui";
import { cards, hovered, panelCards, rtl } from "./state";
import { S } from "./styles";

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

// Everything but the color (from the styles) and the direction (from the page), as C sets it.
const textOptions = { families, slant: TextSlant.normal, language: null, lineHeight: 0, align: TextAlign.start } as const;

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

export function App() {
  return (
    <View style={[S.page, { direction: rtl() ? "rtl" : "ltr" }]}>
      {/* The panel first: a heading and its own cards, in the inverted scheme. */}
      <View style={S.panel}>
        <Text style={[S.title, { width: "100%" }]} {...textOptions} size={15} weight={650}>
          A panel in the other theme: its scope aliases every token
        </Text>
        {panelCards().map((id) => <Card id={id} />)}
      </View>
      <For each={cards()}>{(id: number) => <Card id={id} />}</For>
    </View>
  );
}

