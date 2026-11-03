// The app of test_stall_js.c: a rect springs across the surface, and JS spins in a busy loop
// from the second frame on, until the C test releases it; then it turns the rect red.
import { Node, Property, app } from "@axe/core";
import * as stall from "axe:stall";

app.onSurface((s) => {
  const n = Node.create(s);
  n.setRect(20, 30, 40, 40);
  n.setColor(1, 1, 1);
  n.spring(Property.rect, [220, 30, 40, 40], { stiffness: 170, damping: 18.25 });
  const off = s.onFrame((f) => {
    if (f.frameId < 2) return;
    off();
    stall.blocked();
    while (!stall.released()); // a busy JS loop: the core must not wait for it
    n.setColor(1, 0, 0);
  });
});
