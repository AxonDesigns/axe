// The app and its surfaces. Surfaces are created by the core (one, for now: ui_app_desc's) and
// reach JS through their first surface_resized event.

import { EventType, type FrameCommittedEvent, type Handle, type SurfaceResizedEvent } from "@axe/wire";
import { animationTimeMs, args, on, onTurnEnd, onTurnStart } from "./runtime.js";

export interface FrameInfo {
  /** The frame's number on its surface, from 1. */
  readonly frameId: number;
  /** When the frame is expected on screen: animation-clock ms since the app started. */
  readonly presentTime: number;
}

export interface SurfaceSize {
  /** Logical pixels. */
  readonly width: number;
  readonly height: number;
  /** The scale factor (1.25 for a 125% display). */
  readonly scale: number;
}

type Handler<T> = (value: T) => void;

/** A place the app draws into: a window, or a headless surface. Its nodes are created with
 * Node.create(surface). */
export class Surface implements SurfaceSize {
  #width = 0;
  #height = 0;
  #scale = 1;
  #closed = false;
  readonly #resize: Handler<SurfaceSize>[] = [];
  readonly #frame: Handler<FrameInfo>[] = [];
  readonly #close: Handler<Surface>[] = [];

  /** @internal Surfaces come from the core: see app.onSurface. */
  constructor(readonly handle: Handle) {}

  get width(): number {
    return this.#width;
  }
  get height(): number {
    return this.#height;
  }
  get scale(): number {
    return this.#scale;
  }
  /** True once the platform asked to close it (a window's close button). */
  get closeRequested(): boolean {
    return this.#closed;
  }

  /** Calls `fn` whenever the surface's logical size or scale changes. Returns a function that
   * removes it. */
  onResize(fn: Handler<SurfaceSize>): () => void {
    return add(this.#resize, fn);
  }
  /** Calls `fn` after every frame drawn on the surface. Frames aren't a clock: an idle surface
   * draws none (use timers or app.time). */
  onFrame(fn: Handler<FrameInfo>): () => void {
    return add(this.#frame, fn);
  }
  /** Calls `fn` when the platform asks to close the surface. */
  onCloseRequested(fn: Handler<Surface>): () => void {
    return add(this.#close, fn);
  }
  /** The next frame drawn on this surface. */
  nextFrame(): Promise<FrameInfo> {
    return new Promise((resolve) => {
      const off = this.onFrame((f) => {
        off();
        resolve(f);
      });
    });
  }

  /** @internal */
  resized(e: SurfaceResizedEvent): void {
    this.#width = e.width;
    this.#height = e.height;
    this.#scale = e.scale120 / 120;
    for (const fn of this.#resize.slice()) fn(this);
  }
  /** @internal */
  framed(e: FrameCommittedEvent): void {
    const info = { frameId: e.frameId, presentTime: e.predictedPresentNs / 1e6 };
    for (const fn of this.#frame.slice()) fn(info);
  }
  /** @internal */
  closing(): void {
    this.#closed = true;
    for (const fn of this.#close.slice()) fn(this);
  }
}

function add<T>(list: Handler<T>[], fn: Handler<T>): () => void {
  list.push(fn);
  return () => {
    const i = list.indexOf(fn);
    if (i >= 0) list.splice(i, 1);
  };
}

const surfaces = new Map<number, Surface>(); // by handle index
const surfaceListeners: Handler<Surface>[] = [];

function surfaceOf(h: Handle): Surface | undefined {
  const s = surfaces.get(h.index);
  return s && s.handle.generation === h.generation ? s : undefined;
}

on(EventType.surfaceResized, (e) => {
  if (e.type !== EventType.surfaceResized) return;
  let s = surfaceOf(e.surface);
  const first = !s;
  if (!s) {
    s = new Surface(e.surface);
    surfaces.set(e.surface.index, s);
  }
  s.resized(e);
  if (first) for (const fn of surfaceListeners.slice()) fn(s);
});
on(EventType.frameCommitted, (e) => {
  if (e.type === EventType.frameCommitted) surfaceOf(e.surface)?.framed(e);
});
on(EventType.surfaceCloseRequested, (e) => {
  if (e.type === EventType.surfaceCloseRequested) surfaceOf(e.surface)?.closing();
});

/** The app: its surfaces, its time and its arguments. */
export const app = {
  /** Calls `fn` with every surface, those that exist now first (in the order they appeared),
   * then each new one as it appears, with its size already known. Returns a function that
   * stops it. */
  onSurface(fn: Handler<Surface>): () => void {
    for (const s of surfaces.values()) fn(s);
    return add(surfaceListeners, fn);
  },
  /** The surfaces that exist. */
  get surfaces(): readonly Surface[] {
    return [...surfaces.values()];
  },
  /** The animation clock at this turn, in ms since the app started. It moves between turns,
   * not during one; a paused clock (tests) moves only when stepped. */
  get time(): number {
    return animationTimeMs();
  },
  /** The app's arguments (axe-run's arguments after `--`). */
  get args(): readonly string[] {
    return args;
  },
  /** Calls `fn` at the start of every turn, before its events. */
  onTurnStart,
  /** Calls `fn` at the end of every turn, before its commands are sent. */
  onTurnEnd,
};
