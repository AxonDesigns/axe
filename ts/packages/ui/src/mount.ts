// Mounting an app on a surface (docs/ui.md, "Mounting").

import type { Surface } from "@axe/core";
import { createComponent, mountInto, type Component } from "./elements.js";
import { Owner, dispose, handleError, runWithOwner } from "./reactive.js";
import { Region } from "./regions.js";

export interface Mounted {
  /** Disposes the app: its effects stop, its cleanups run, its nodes are destroyed. */
  unmount(): void;
}

/** Mounts `App` on `surface`, after the surface's other root-level nodes. */
export function mount(App: Component<Record<string, never>>, surface: Surface): Mounted {
  const owner = new Owner(null);
  owner.name = "mount";
  const region = new Region(surface, null);
  owner.onDisposeNodes = () => region.clear();
  runWithOwner(owner, () => {
    try {
      mountInto(region, createComponent(App, {}));
    } catch (e) {
      handleError(e, owner, "mounting the app");
    }
  });
  return { unmount: () => dispose(owner) };
}

/** Same as `mounted.unmount()`. */
export function unmount(mounted: Mounted): void {
  mounted.unmount();
}
