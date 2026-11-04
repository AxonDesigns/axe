// A component: a template for its static structure, bindings for what reads signals.
import { Text, View, style, type Element } from "@axe/ui";

const card = style({ padding: 8 });
const title = style({ fontSize: 18 });

export function Card(props: { title: string; count: () => number; children?: Element }) {
  return (
    <View style={card}>
      <Text style={title}>{props.title}</Text>
      <Text>Count: {props.count()}</Text>
      {props.children}
    </View>
  );
}
