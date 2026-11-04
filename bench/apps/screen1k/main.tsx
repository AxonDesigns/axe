// Benchmark B1 (bench/bench_ui.c): a screen of about 1,000 nodes. 60 cards of 16 nodes each,
// 5 paragraphs a card (300 in all), three bindings a card. Frozen (CONTRIBUTING.md, "Bench apps").

import { For, Text, View, app, mount, signal, style, token } from "@axe/ui";

const T = { page: token("#f4f5f7"), card: token("#ffffff"), ink: token("#1b1d22"), muted: token("#6b7080"), accent: token("#3366ff") };
const families = ["Noto Sans", "sans-serif"];
const S = {
  page: style({ flexGrow: 1, flexWrap: "wrap", alignContent: "start", padding: 16, gap: 12, backgroundColor: T.page }),
  card: style({ width: 220, flexDirection: "column", padding: 12, rowGap: 6, cornerRadius: 8, backgroundColor: T.card }),
  header: style({ justifyContent: "space-between", alignItems: "center" }),
  title: style({ textColor: T.ink, fontFamily: "Noto Sans, sans-serif", fontSize: 15, fontWeight: 650 }),
  on: style({ width: 10, height: 10, cornerRadius: 5, backgroundColor: T.accent }),
  off: style({ width: 10, height: 10, cornerRadius: 5, backgroundColor: T.muted }),
  body: style({ textColor: T.muted, fontSize: 12, lineHeight: 1.4 }),
  stats: style({ gap: 6 }),
  cell: style({ flexGrow: 1, height: 18, cornerRadius: 3, backgroundColor: T.page }),
  bar: style({ width: "60%", height: 4, backgroundColor: T.accent }),
  value: style({ textColor: T.ink, fontSize: 11 }),
  footer: style({ justifyContent: "space-between", marginTop: 4 }),
  label: style({ textColor: T.muted, fontSize: 11 }),
  button: style({ padding: [2, 8], cornerRadius: 4, backgroundColor: T.accent }),
  buttonText: style({ textColor: "#fff", fontSize: 11 }),
};

const counts = Array.from({ length: 60 }, (_, i) => signal(i));
const flags = Array.from({ length: 60 }, (_, i) => signal(i % 3 === 0));
const ids = Array.from({ length: 60 }, (_, i) => i);

function Card(props: { i: number }) {
  const i = props.i;
  return (
    <View style={S.card}>
      <View style={S.header}>
        <Text style={S.title} families={families}>Item {i} · {counts[i]!()}</Text>
        <View style={flags[i]!() ? S.on : S.off} />
      </View>
      <Text style={S.body}>A card with a header, a paragraph, three stat cells and a footer row.</Text>
      <View style={S.stats}>
        <View style={S.cell}>
          <Text style={S.value}>{counts[i]!() * 3}</Text>
        </View>
        <View style={S.cell}>
          <View style={S.bar} />
        </View>
        <View style={S.cell}>
          <View style={S.bar} />
        </View>
      </View>
      <View style={S.footer}>
        <Text style={S.label}>#{i}</Text>
        <View style={S.button}>
          <Text style={S.buttonText}>Open</Text>
        </View>
      </View>
    </View>
  );
}

function App() {
  return (
    <View style={S.page}>
      <For each={ids}>{(i: number) => <Card i={i} />}</For>
    </View>
  );
}

app.onSurface((s) => mount(App, s));

/** One turn's worth of changes: 6 counts and 6 flags. */
(globalThis as Record<string, unknown>)["benchUpdate"] = (k: number) => {
  for (let j = 0; j < 6; j++) {
    const i = (k * 7 + j * 11) % 60;
    counts[i]!.update((c) => c + 1);
    flags[i]!.update((f) => !f);
  }
};
