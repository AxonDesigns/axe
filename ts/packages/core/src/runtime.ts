// The binding between @axe/core and the script host (docs/script.md, "The turn"): the
// command writer over the host's storage, event dispatch, and the turn's hooks.

import * as host from "axe:host";
import { CommandWriter, EventType, TextField, WIRE_VERSION, decodeEvent, encodeUtf8Into, type WireEvent } from "@axe/wire";

/** The writer with what @axe/ui writes most, written straight into the buffer: no options
 * objects, handle objects or intermediate arrays. Each writes exactly what the general encoders
 * would (tests compare them byte for byte). */
export class CoreWriter extends CommandWriter {
  /** set_text of `text` with every field from the node (setText(text, {})): no spans, strings
   * or ellipsis. False, writing nothing, when the text needs more than one op. */
  setTextPlain(index: number, generation: number, text: string): boolean {
    // Room for the worst case (3 bytes per UTF-16 unit), encoded natively, then the op shrunk to
    // what was written: no JS pass over the text to measure it.
    const units = text.length;
    if (units * 3 > MAX_PLAIN_TEXT) return false; // long text: the general encoder
    const at = this.op(8, 72 + ((units * 3 + 3) & ~3));
    const n = units ? encodeUtf8Into(text, this.u8, at + 72) : 0;
    const padded = (n + 3) & ~3;
    const opSize = 4 + 72 + padded;
    this.size = at - 4 + opSize;
    const u32 = this.u32, f32 = this.f32;
    u32[(at - 4) >> 2] = (8 | (opSize << 16)) >>> 0;
    const word = at >> 2;
    u32[word + 0] = index;
    u32[word + 1] = generation;
    u32[word + 2] = n; // text_bytes
    u32[word + 3] = (n | (72 << 16)) >>> 0; // chunk_bytes, header_bytes
    u32[word + 4] = 0; // span_count, string_count
    u32[word + 5] = 0; // strings_bytes, ellipsis_bytes
    u32[word + 6] = 0; // max_lines, direction, flags
    u32[word + 7] = 36; // span_bytes
    u32[word + 8] = 0;
    u32[word + 9] = (0xffff | (400 << 16)) >>> 0; // family: the default; weight 400
    u32[word + 10] = (TextField.spanAll << 8) >>> 0; // slant 0; from_node: every span field; language 0
    f32[word + 11] = 16;
    f32[word + 12] = 0;
    f32[word + 13] = 0;
    f32[word + 14] = 0;
    f32[word + 15] = 1;
    f32[word + 16] = 0; // line height
    u32[word + 17] = (TextField.all << 16) >>> 0; // align 0; from_node: every field
    for (let i = at + 72 + n; i < at + 72 + padded; i++) this.u8[i] = 0;
    return true;
  }

  /** instantiate_template naming slots by (index, generation) pairs. */
  instantiateRaw(template: number, parentIndex: number, parentGeneration: number, beforeIndex: number,
                 beforeGeneration: number, slots: readonly number[], generations: Uint32Array): void {
    const n = slots.length;
    if (!(template > 0 && n >= 1 && n <= 1024)) throw new RangeError("instantiate_template: bad template or count");
    const at = this.op(22, 24 + n * 8);
    const u32 = this.u32;
    let word = at >> 2;
    u32[word] = template;
    u32[word + 1] = (24 | (n << 16)) >>> 0;
    u32[word + 2] = parentIndex;
    u32[word + 3] = parentGeneration;
    u32[word + 4] = beforeIndex;
    u32[word + 5] = beforeGeneration;
    word += 6;
    for (let i = 0; i < n; i++, word += 2) {
      const slot = slots[i]!;
      u32[word] = slot;
      u32[word + 1] = generations[slot]!;
    }
  }

  /** set_styles of a node to these style ids. */
  setStylesRaw(index: number, generation: number, ids: readonly number[]): void {
    const n = ids.length;
    if (n > 255) throw new RangeError("set_styles: at most 255 styles");
    const at = this.op(14, 12 + n * 4);
    const u32 = this.u32;
    const word = at >> 2;
    u32[word] = index;
    u32[word + 1] = generation;
    u32[word + 2] = (n | (12 << 16)) >>> 0;
    for (let i = 0; i < n; i++) {
      const id = ids[i]!;
      if (!(id > 0 && id === id >>> 0)) throw new RangeError("set_styles: ids must not be 0");
      u32[word + 3 + i] = id;
    }
  }
}

// Text that fits one op with no spans or strings.
const MAX_PLAIN_TEXT = 65532 - 4 - 72;

/** The turn's command writer: every op @axe/core writes goes here, and the host sends it to
 * the core when the turn ends. */
export const commands = new CoreWriter((_current, minBytes) => host.growCommandBuffer(minBytes));
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

let beforeFlush: (() => void) | null = null;

/** @internal Sets the one hook that runs after every onTurnEnd handler, just before the turn's
 * commands are finished: @axe/ui's effect scheduler, so effects see the writes the turn-end
 * handlers made, and what they write goes into the same buffer. */
export function setBeforeFlush(fn: (() => void) | null): void {
  beforeFlush = fn;
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
  beforeFlush?.();
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
