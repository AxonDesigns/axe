// Reading an event buffer: the framing (docs/command-buffer.md, "Header", "Op framing") over
// the generated decoders. Unknown events (from a newer core) are skipped; a known event
// shorter than its original size is malformed.

import { HEADER_SIZE, OP_HEADER_SIZE, WireError } from "./runtime.js";
import { MAGIC_EVENTS, WIRE_VERSION } from "./gen/ids.js";
import { decodeEvent, eventOriginalSize, type WireEvent } from "./gen/events.js";

export class EventReader {
  readonly #u32: Uint32Array;
  readonly #f32: Float32Array;
  readonly #end: number;
  #at: number;
  #remaining: number;

  constructor(buffer: ArrayBuffer, offset = 0, length = buffer.byteLength - offset) {
    if (offset % 4 || length % 4 || length < HEADER_SIZE) throw new WireError("not an event buffer: bad size");
    this.#u32 = new Uint32Array(buffer, 0, buffer.byteLength >> 2);
    this.#f32 = new Float32Array(buffer, 0, buffer.byteLength >> 2);
    const w = offset >> 2;
    const u32 = this.#u32;
    if (u32[w] !== MAGIC_EVENTS || (u32[w + 1]! & 0xffff) !== WIRE_VERSION || u32[w + 2] !== length)
      throw new WireError("not an event buffer of this version: bad header");
    this.#at = offset + HEADER_SIZE;
    this.#end = offset + length;
    this.#remaining = u32[w + 3]!;
  }

  /** The next event, or null after the last. */
  next(): WireEvent | null {
    for (;;) {
      if (this.#at === this.#end) {
        if (this.#remaining) throw new WireError("malformed event buffer: fewer events than its header says");
        return null;
      }
      if (this.#end - this.#at < OP_HEADER_SIZE || !this.#remaining) throw new WireError("malformed event buffer: framing");
      const word = this.#u32[this.#at >> 2]!;
      const type = word & 0xffff, size = word >>> 16;
      if (size < OP_HEADER_SIZE || size % 4 || size > this.#end - this.#at) throw new WireError("malformed event buffer: op size");
      const payload = this.#at + OP_HEADER_SIZE;
      this.#at += size;
      this.#remaining--;
      const original = eventOriginalSize(type);
      if (!original) continue; // unknown: from a newer core
      if (size - OP_HEADER_SIZE < original) throw new WireError(`malformed event buffer: event ${type} too short`);
      return decodeEvent(this.#u32, this.#f32, type, payload);
    }
  }
}
