import { For, Text, View, signal } from "@axe/ui";
import { S } from "./styles";
import { limit } from "./util";

const items = Array.from({ length: 40 }, (_, i) => `item ${i}`);

function Row(props: { name: string; index: () => number }) {
  const hits = signal(0);
  return (
    <View style={S.row}>
      <Text style={S.label}>{props.name}</Text>
      <Text style={S.label}>{hits()}</Text>
    </View>
  );
}

export function Panel() {
  const label = "version __COMPONENT__";
  const open = signal(true);
  return (
    <View style={S.panel}>
      <Text style={S.title}>Template __TEMPLATE__</Text>
      <Text style={S.label}>{label} {open() ? "open" : "closed"}</Text>
      <For each={items.slice(0, limit)}>{(name: string, index: () => number) => <Row name={name} index={index} />}</For>
    </View>
  );
}

export function App() {
  return <View style={S.page}><Panel /></View>;
}
