// Run twice by tests/hot/scene.cmake: "hot" goes through version 1, drives state, and updates to
// version 2; "fresh" starts at version 2 and drives the same state. The images must be equal.

import { app } from "@axe/core";
import { expect, nextTurn, step, test } from "@axe/testing";
import { capture, hotEdit } from "axe:test";
import { hotState } from "@axe/ui/hot";

const [mode, out] = app.args as [string, string];
const P = "declare const probe: any;\n";
const probe: Record<string, { set(v: unknown): void }> = {};
(globalThis as unknown as { probe: unknown }).probe = probe;

async function edit(files: Record<string, string | null>): Promise<void> {
  const before = hotState().updates;
  hotEdit(files);
  while (hotState().updates === before) await nextTurn();
  await nextTurn();
}

const styles = (color: string): string => `import { style } from "@axe/ui";
export const S = {
  page: style({ flexDirection: "column", gap: 6, padding: 8, width: 300 }),
  card: style({ padding: 6, backgroundColor: "${color}", cornerRadius: 4 }),
};
`;

const main = `import { app, mount } from "@axe/ui";
import { App } from "./app";
app.onSurface((s) => mount(App, s));
`;

const view = (marker: string, badge: string): string => `${P}import { For, Show, Text, View, signal } from "@axe/ui";
import { S } from "./styles";

function Card(props: { id: string }) {
  const n = signal(0);
  probe["card" + props.id] = n;
  return <View style={S.card}><Text>${marker} {props.id}: {n()}</Text>${badge}</View>;
}

const items = signal(["a", "b", "c"]);
probe.items = items;

export function App() {
  const open = signal(false);
  probe.open = open;
  return (
    <View style={S.page}>
      <For each={items()}>{(id: string) => <Card id={id} />}</For>
      <Show when={open()}><Text>open</Text></Show>
    </View>
  );
}
`;

async function drive(): Promise<void> {
  probe["carda"]!.set(1);
  probe["cardc"]!.set(3);
  probe["items"]!.set(["c", "a", "b"]);
  probe["open"]!.set(true);
  await nextTurn();
}

async function shot(path: string): Promise<void> {
  await step(50);
  await nextTurn();
  capture(path);
  await step(50);
  await nextTurn();
}

// The overlay: on top of the last scene, which is back exactly when the error is fixed.
if (mode === "overlay") {
  test("the overlay", async () => {
    const good = { "styles.ts": styles("#def"), "app.tsx": view("v2", "") };
    await edit({ ...good, "main.tsx": main });
    await drive();
    await shot(`${out}-before.ppm`);
    await edit({ "app.tsx": `export function App( {\n` });
    expect(hotState().overlay ?? "").toContain("app.tsx:");
    await shot(`${out}-error.ppm`);
    await edit(good);
    expect(hotState().overlay).toBe(null);
    await shot(`${out}-after.ppm`);
  });
} else test(`the scene (${mode})`, async () => {
  const v2 = { "styles.ts": styles("#def"), "app.tsx": view("v2", "<View />") };
  if (mode === "hot") {
    await edit({ "styles.ts": styles("#fed"), "app.tsx": view("v1", ""), "main.tsx": main });
    await drive();
    await edit(v2);
    expect(hotState().fullReloads).toBe(0);
  } else {
    await edit({ ...v2, "main.tsx": main });
    await drive();
  }
  expect(hotState().overlay).toBe(null);
  await step(50);
  await nextTurn();
  capture(out);
  await step(50);
  await nextTurn();
});
