// Benchmark B2 (bench/bench_ui.c): 10,000 rows mounted through For, then a stream of updates:
// each turn writes 100 items' signals (a label and a flag). A reorder moves 50 rows. With
// `-- --stream` (bench/bench_ui_live.c) it writes 100 items on every frame by itself. Frozen
// (CONTRIBUTING.md, "Bench apps").

import { For, Text, View, app, mount, signal, style, token, type Signal } from "@axe/ui";

const accent = token("#3366ff");
const S = {
  page: style({ flexDirection: "column", backgroundColor: "#f4f5f7" }),
  row: style({ height: 24, alignItems: "center", justifyContent: "space-between", padding: [0, 8] }),
  label: style({ textColor: "#1b1d22", fontSize: 13 }),
  hot: style({ width: 8, height: 8, cornerRadius: 4, backgroundColor: accent }),
  cold: style({ width: 8, height: 8, cornerRadius: 4, backgroundColor: "#c8ccd4" }),
};

// 10,000 rows; `-- --rows=N` for another count (bench_ui_live, to tell the core's cost of a
// large scene from the stream's).
const rowsArg = app.args.find((a) => a.startsWith("--rows="));
const N = rowsArg ? Number(rowsArg.slice(7)) : 10000;
interface Item {
  readonly id: number;
  readonly label: Signal<string>;
  readonly hot: Signal<boolean>;
}
const items: Item[] = Array.from({ length: N }, (_, id) => ({ id, label: signal(`Item ${id}`), hot: signal(false) }));
const list = signal(items);

function Row(props: { item: Item }) {
  const it = props.item;
  return (
    <View style={S.row}>
      <Text style={S.label}>{it.label()}</Text>
      <View style={it.hot() ? S.hot : S.cold} />
    </View>
  );
}

function App() {
  return (
    <View style={S.page}>
      <For each={list()}>{(it: Item) => <Row item={it} />}</For>
    </View>
  );
}

app.onSurface((s) => mount(App, s));

const g = globalThis as Record<string, unknown>;
/** One turn's updates: 100 items' labels and flags. */
const update = (k: number) => {
  for (let j = 0; j < 100; j++) {
    const it = items[(k * 100 + j * 97) % N]!;
    it.label.set(`Item ${it.id} · ${k}`);
    it.hot.update((h) => !h);
  }
};
g["benchUpdate"] = update;

// The live run: 100 items on every frame.
if (app.args.includes("--stream")) {
  let k = 0;
  app.onSurface((s) => s.onFrame(() => update(k++)));
}

/** Moves 50 rows to other places. */
g["benchReorder"] = (k: number) => {
  const next = [...list.peek()];
  for (let j = 0; j < 50; j++) {
    const from = (k * 131 + j * 197) % N;
    const [row] = next.splice(from, 1);
    next.splice((from * 7 + 13) % N, 0, row!);
  }
  list.set(next);
};
