// Components: dynamic props are getters, children a getter; spreads merge; control flow is
// components too, and JSX inside their callbacks compiles as well.
import { For, Show, Text, View } from "@axe/ui";
import { Card } from "./basic";

export function List(props: { items: () => string[]; open: () => boolean }) {
  const extra = { title: "x" };
  return (
    <View>
      <Card title="static" count={() => 1} />
      <Card title={props.items()[0]!} count={() => props.items().length}>
        <Text>inside</Text>
      </Card>
      <Card {...extra} count={() => 2} />
      <Show when={props.open()} fallback={<Text>closed</Text>}>
        <For each={props.items()}>{(item: string) => <Text>{item}</Text>}</For>
      </Show>
    </View>
  );
}
