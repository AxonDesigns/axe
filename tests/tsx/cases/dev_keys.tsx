// --dev output: components registered, signals, styles, tokens, themes and contexts keyed, and
// templates with keys of their own. Snapshots with names starting dev_ run with --dev.
import { Text, Token, View, context, signal, style, theme, token } from "@axe/ui";

const S = { card: style({ padding: 4 }), row: style({ gap: 2 }) };
const accent = token("#f00");
const local = Token.local<never>();
const made = Token.create(4);
const dark = theme({ tokens: [] });
const Locale = context("en");
export const count = signal<number>(0);

function helper() {
  return signal(1); // a helper's call: keyed too; the runtime tells repeated ones apart
}

export function Card(props: { title: string }) {
  const n = signal(1);
  const m = signal(1);
  return (
    <View style={S.card}>
      <Text>{props.title} {n()} {m()}</Text>
    </View>
  );
}

const Arrow = () => <View style={S.row} />;
const Plain = () => 1;

export default function App() {
  return <View><Card title="x" /><Arrow /></View>;
}

export { helper, accent, local, made, dark, Locale, Plain };
