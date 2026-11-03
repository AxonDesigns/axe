// The script benchmarks' app (bench/bench_script.c): common ops encoded from JS, and a frame
// handler counting the events it is given.
import { FlexDirection, Node, Style, app, keyword, px } from "@axe/core";
import { commands } from "../../ts/packages/core/src/runtime.js";

const card = Style.define({ decls: { width: px(208), flexDirection: keyword(FlexDirection.column) } });
const body = Style.define({ decls: { paddingTop: px(4) } });
let parent: Node | null = null;
const nodes: Node[] = [];
let frames = 0;

app.onSurface((s) => {
  parent = Node.create(s);
  for (let i = 0; i < 16; i++) nodes.push(Node.create(parent));
  s.onFrame(() => frames++);
});

declare global {
  var benchEncode: (groups: number) => number;
  var benchFrames: () => number;
}

// Encodes `groups` groups of the ops an app sends most (7 ops each), into the turn's writer,
// then discards them. Returns the bytes they took.
globalThis.benchEncode = (groups: number): number => {
  const mark = commands.mark();
  const start = commands.size;
  for (let i = 0; i < groups; i++) {
    const n = nodes[i & 15]!;
    n.setRect(i & 255, 2, 100, 20);
    n.setColor(0.5, 0.25, 0.75, 1);
    n.setProps({ width: px(100 + (i & 7)), height: px(20), marginLeft: px(4) });
    n.setStyles(card, body);
    n.setText(`Card ${i}`, { size: 13 });
    Node.create(parent!).destroy();
  }
  const bytes = commands.size - start;
  commands.rollback(mark);
  return bytes;
};
globalThis.benchFrames = () => frames;

declare global {
  var benchKind: (kind: number, n: number) => number;
}

// One op at a time through @axe/core (and kind 6 through the bare @axe/wire writer): its
// cost in us, for the per-op breakdown.
globalThis.benchKind = (kind: number, n: number): number => {
  const mark = commands.mark();
  const t0 = performance.now();
  for (let i = 0; i < n; i++) {
    const node = nodes[i & 15]!;
    switch (kind) {
      case 0: node.setRect(i & 255, 2, 100, 20); break;
      case 1: node.setColor(0.5, 0.25, 0.75, 1); break;
      case 2: node.setProps({ width: px(100 + (i & 7)), height: px(20), marginLeft: px(4) }); break;
      case 3: node.setStyles(card, body); break;
      case 4: node.setText(`Card ${i}`, { size: 13 }); break;
      case 5: Node.create(parent!).destroy(); break;
      default: commands.setRect(node.handle, i & 255, 2, 100, 20); break;
    }
  }
  const us = ((performance.now() - t0) * 1000) / n;
  commands.rollback(mark);
  return us;
};
