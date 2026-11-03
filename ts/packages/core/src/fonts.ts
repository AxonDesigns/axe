// Fonts loaded at run time (docs/command-buffer.md, "Fonts"): load_font, answered by
// font_loaded. Text everywhere is shaped again with the new font, in the frame its buffer
// applies.

import { EventType, encodeUtf8 } from "@axe/wire";
import { commands, on } from "./runtime.js";

const IO_ERROR = 9; // UI_ERR_IO (include/ui/result.h)

/** A font file that couldn't be loaded: unreadable, or not a font. */
export class FontLoadError extends Error {
  override name = "FontLoadError";
  constructor(readonly path: string, readonly result: number) {
    super(`can't load the font ${path} (${result === IO_ERROR ? "unreadable" : "not a font"})`);
  }
}

let nextId = 1;
const pending = new Map<number, { path: string; resolve: () => void; reject: (e: Error) => void }>();

on(EventType.fontLoaded, (e) => {
  if (e.type !== EventType.fontLoaded) return;
  const p = pending.get(e.id);
  if (!p) return;
  pending.delete(e.id);
  if (e.result === 0) p.resolve();
  else p.reject(new FontLoadError(p.path, e.result));
});

/** Registers face 0 of the font file at `path` (UTF-8, at most 4096 bytes). Resolves once it
 * is registered, in the frame its commands apply; text using it is shaped with it there. */
export function loadFont(path: string): Promise<void> {
  const id = nextId++;
  try {
    commands.loadFont(id, encodeUtf8(path));
  } catch (e) {
    return Promise.reject(e);
  }
  return new Promise((resolve, reject) => pending.set(id, { path, resolve, reject }));
}
