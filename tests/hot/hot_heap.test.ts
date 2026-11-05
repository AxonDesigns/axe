// Long sessions (docs/ui.md, "Hot reload"): 200 edits of one component. The engine can't unload
// a module, so each version stays; this measures what that costs, after everything that can be
// released was. It reports the growth per edit and bounds it.

import { expect, nextTurn, test } from "@axe/testing";
import { gc, heapDetail, heapUsed, hotEdit } from "axe:test";
import { hotState } from "@axe/ui/hot";

async function edit(files: Record<string, string | null>): Promise<void> {
  const before = hotState().updates;
  hotEdit(files);
  while (hotState().updates === before) await nextTurn();
  await nextTurn();
}

const view = (n: number): string => `import { For, Text, View, signal, style } from "@axe/ui";
const card = style({ padding: ${1 + (n % 4)} });
function Row(props: { name: string }) {
  const hits = signal(0);
  return <View style={card}><Text>{props.name} ${n} {hits()}</Text></View>;
}
const items = ["a", "b", "c", "d", "e", "f", "g", "h"];
export function App() {
  const title = "version ${n}";
  return <View><Text>{title}</Text><For each={items}>{(i: string) => <Row name={i} />}</For></View>;
}
`;

const main = `import { app, mount } from "@axe/ui";
import { App } from "./app";
app.onSurface((s) => mount(App, s));
`;

test("200 edits of one component", async () => {
  await edit({ "app.tsx": view(0), "main.tsx": main });
  const heap = (): number => {
    gc();
    return heapUsed();
  };
  const samples: number[] = [];
  let at50 = heapDetail();
  for (let i = 1; i <= 200; i++) {
    await edit({ "app.tsx": view(i) });
    if (i % 50 === 0) samples.push(heap());
    if (i === 50) at50 = heapDetail();
  }
  const end = heapDetail();
  const per = (a: number, b: number): string => ((b - a) / 150).toFixed(1);
  console.log(`per edit: ${per(at50.objects, end.objects)} objects, ${per(at50.functions, end.functions)} functions, ` +
              `${per(at50.bytecodeBytes, end.bytecodeBytes)} bytes of bytecode, ${per(at50.stringBytes, end.stringBytes)} bytes of strings`);
  expect(hotState().overlay).toBe(null);
  expect(hotState().fullReloads).toBe(0);
  const perEdit = (samples[3]! - samples[0]!) / 150;
  console.log(`heap at 50, 100, 150, 200 edits: ${samples.map((s) => (s / 1024).toFixed(0)).join(", ")} KB; ${(perEdit / 1024).toFixed(2)} KB per edit`);
  expect(perEdit < 50 * 1024).toBeTruthy();
});
