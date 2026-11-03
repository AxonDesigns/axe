// spring_rect in TypeScript: the C example (demo_logic.c) on @axe/core. One rounded rect
// springs between four anchors every period, changing color; resizing the window springs it
// to its anchor's new place.
//
//   spring_rect_ts [-- --period S --stall]
//   spring_rect_ts --headless --at S --out f.ppm
//
// --stall blocks JS for 500 ms after each switch: the spring keeps moving (frames never wait
// for JS).

import { app, Node, Property } from "@axe/core";

const SPRING = { stiffness: 170, damping: 18.25 };
const anchors = [[0.25, 0.3], [0.75, 0.3], [0.75, 0.7], [0.25, 0.7]] as const;
const colors = [
  [0.95, 0.45, 0.25, 1], // orange
  [0.25, 0.7, 0.95, 1],  // sky
  [0.55, 0.85, 0.35, 1], // green
  [0.85, 0.35, 0.8, 1],  // magenta
] as const;

const args = app.args;
const periodArg = args.indexOf("--period");
const periodMs = periodArg >= 0 ? Number(args[periodArg + 1]) * 1000 : 2000;
const stall = args.includes("--stall");

function rect(anchor: number, width: number, height: number): [number, number, number, number] {
  const [cx, cy] = anchors[anchor % anchors.length]!;
  const w = width * 0.3, h = height * 0.3;
  return [cx * width - w * 0.5, cy * height - h * 0.5, w, h];
}

app.onSurface((surface) => {
  let anchor = 0;
  const node = Node.create(surface);
  const color = colors[0];
  node.setColor(color[0], color[1], color[2], color[3]);
  node.setRect(...rect(anchor, surface.width, surface.height));
  node.setCornerRadius(Math.min(surface.width, surface.height) * 0.08);
  surface.onResize(({ width, height }) => {
    node.spring(Property.rect, rect(anchor, width, height), SPRING);
    node.setCornerRadius(Math.min(width, height) * 0.08);
  });
  const off = surface.onFrame(() => {
    off();
    setInterval(() => {
      anchor = (anchor + 1) % anchors.length;
      node.spring(Property.rect, rect(anchor, surface.width, surface.height), SPRING);
      node.spring(Property.color, colors[anchor]!, SPRING);
      if (stall) {
        console.log("stalling JS for 500 ms");
        const end = performance.now() + 500;
        while (performance.now() < end);
      }
    }, periodMs);
  });
});
