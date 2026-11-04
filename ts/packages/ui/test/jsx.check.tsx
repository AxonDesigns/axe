// Type tests for TSX (checked by ts_typecheck, never run): props are typed, text goes only in
// Text. What types can't see (a View inside a Text, a Span outside one) the transform reports
// (tools/axe-tsx/tests).

import { For, Show, Span, Text, View, signal, style, type Element } from "@axe/ui";

const card = style({ padding: 8 });
const count = signal(0);

function Card(props: { title: string; children?: Element }) {
  return (
    <View style={card}>
      <Text weight={700}>{props.title}</Text>
      {props.children}
    </View>
  );
}

export const ok = (
  <View style={[card, { gap: 4 }]}>
    <Card title="Inbox">
      <Text>
        You have <Span weight={700}>{count()}</Span> messages
      </Text>
    </Card>
    <Show when={count() > 0} fallback={<Text>None</Text>}>
      <Text>Some</Text>
    </Show>
    <For each={[1, 2, 3]}>{(n: number) => <Text>{n}</Text>}</For>
  </View>
);

// @ts-expect-error text in a View must be inside Text
export const loose = <View>loose text</View>;
// @ts-expect-error a number too
export const number = <View>{42}</View>;
// @ts-expect-error a prop of the wrong type
export const wrongProp = <Card title={1} />;
// @ts-expect-error a missing prop
export const missing = <Card />;
// @ts-expect-error an inline style with a unit string
export const badStyle = <View style={{ width: "12px" }} />;
