// Benchmark B3 (bench/bench_ui.c): 1,000 Text nodes bound to 1,000 signals, all updated in one
// turn. Frozen (CONTRIBUTING.md, "Bench apps").

import { Text, View, app, mount, signal, style } from "@axe/ui";

const S = {
  page: style({ flexWrap: "wrap", gap: 4, padding: 8 }),
  text: style({ width: 90, textColor: "#1b1d22", fontSize: 11 }),
};
const labels = Array.from({ length: 1000 }, (_, i) => signal(`label ${i}`));

function App() {
  return <View style={S.page}>{labels.map((l) => <Text style={S.text}>{l()}</Text>)}</View>;
}

app.onSurface((s) => mount(App, s));

/** Updates every label. */
(globalThis as Record<string, unknown>)["benchUpdate"] = (k: number) => {
  for (let i = 0; i < labels.length; i++) labels[i]!.set(`label ${i} · ${k}`);
};
