// Hot reload end to end: real saves, in each shape editors use, seen by the watcher thread and
// rebuilt into a running app.

import { expect, nextTurn, test } from "@axe/testing";
import { hotSave } from "axe:test";
import { hotState } from "@axe/ui/hot";

const probe: Record<string, unknown> = {};
(globalThis as unknown as { probe: unknown }).probe = probe;

// The watcher runs on its own thread, in wall time, whatever the paused clock does.
async function until(ok: () => boolean, what: string): Promise<void> {
  for (let i = 0; i < 20000 && !ok(); i++) await nextTurn();
  if (!ok()) throw new Error(`timed out waiting for ${what}`);
}

const view = (n: number): string => `declare const probe: any;
import { Text, View, signal } from "@axe/ui";
export function App() {
  const count = signal(0);
  probe.count = count;
  probe.version = ${n};
  return <View><Text>{${n}} {count()}</Text></View>;
}
`;

const main = `import { app, mount } from "@axe/ui";
import { App } from "./app";
app.onSurface((s) => mount(App, s));
`;

test("each way of saving updates the app, and keeps state", async () => {
  hotSave("app.tsx", view(1), "write");
  hotSave("main.tsx", main, "rename");
  await until(() => probe["version"] === 1, "the first version");
  (probe["count"] as { set(n: number): void }).set(5);
  await nextTurn();
  const shapes = ["write", "rename", "recreate"] as const;
  let n = 2;
  for (const how of shapes) {
    hotSave("app.tsx", view(n), how);
    const want = n;
    await until(() => probe["version"] === want, `a save by ${how}`);
    expect((probe["count"] as () => number)()).toBe(5);
    n++;
  }
  expect(hotState().overlay).toBe(null);
  expect(hotState().fullReloads).toBe(0);
});

test("a burst of saves is one update", async () => {
  const before = hotState().updates;
  for (let i = 0; i < 10; i++) hotSave("app.tsx", view(100 + i), "write");
  await until(() => probe["version"] === 109, "the last save");
  expect(hotState().updates - before <= 2).toBeTruthy();
});

test("a new file, a deleted one still imported, and the fix", async () => {
  hotSave("lib.ts", "export const value = 1;\n", "write");
  const uses = `declare const probe: any;
import { View } from "@axe/ui";
import { value } from "./lib";
export function App() {
  probe.value = value;
  return <View />;
}
`;
  hotSave("app.tsx", uses, "rename");
  await until(() => probe["value"] === 1, "the new file");
  hotSave("lib.ts", "", "delete");
  await until(() => (hotState().overlay ?? "").includes("still imports it"), "the overlay");
  hotSave("lib.ts", "export const value = 2;\n", "write");
  await until(() => probe["value"] === 2 && hotState().overlay === null, "the file back");
});
