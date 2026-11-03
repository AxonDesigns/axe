// The binding between @axe/core and the script host (docs/script.md, "The turn"): the
// command writer over the host's storage, event dispatch, and the turn's hooks.

import * as host from "axe:host";
import { CommandWriter, EventType, WIRE_VERSION, decodeEvent, type WireEvent } from "@axe/wire";

/** The turn's command writer: every op @axe/core writes goes here, and the host sends it to
 * the core when the turn ends. */
export const commands = new CommandWriter((_current, minBytes) => host.growCommandBuffer(minBytes));
commands.adopt(host.commandBuffer());

type Listener = (event: WireEvent) => void;
const listeners = new Map<number, Listener[]>();
const turnStart: (() => void)[] = [];
const turnEnd: (() => void)[] = [];

/** Calls `fn` with every event of `type`, in order (one event per call). */
export function on(type: number, fn: Listener): void {
  let list = listeners.get(type);
  if (!list) listeners.set(type, (list = []));
  list.push(fn);
}

/** Calls `fn` at the start of every turn, before its events. */
export function onTurnStart(fn: () => void): () => void {
  turnStart.push(fn);
  return () => remove(turnStart, fn);
}

/** Calls `fn` at the end of every turn, before its commands are sent: the place to batch
 * updates (M3b's signal scheduler). */
export function onTurnEnd(fn: () => void): () => void {
  turnEnd.push(fn);
  return () => remove(turnEnd, fn);
}

function remove<T>(list: T[], item: T): void {
  const i = list.indexOf(item);
  if (i >= 0) list.splice(i, 1);
}

// Views of the turn's event buffer, made once per buffer.
let viewed: ArrayBuffer | null = null;
let u32 = new Uint32Array(0);
let f32 = new Float32Array(0);

host.setDispatcher((events, type, at) => {
  if (events !== viewed) {
    viewed = events;
    u32 = new Uint32Array(events, 0, events.byteLength >> 2);
    f32 = new Float32Array(events, 0, events.byteLength >> 2);
  }
  if (type === EventType.clock) {
    for (const fn of turnStart.slice()) fn();
    return;
  }
  const list = listeners.get(type);
  if (!list) return;
  const event = decodeEvent(u32, f32, type, at);
  if (event) for (const fn of list.slice()) fn(event);
});

host.setFlush(() => {
  for (const fn of turnEnd.slice()) fn();
  const bytes = commands.finish(WIRE_VERSION);
  if (bytes) commands.adopt(host.submitCommands(bytes.length));
});

/** The turn's animation-clock time, in ms since the app started. */
export function animationTimeMs(): number {
  return host.animationTime() / 1e6;
}

export const debug = host.debug;
export const args = host.args;
export const reportLeak = host.reportLeak;
