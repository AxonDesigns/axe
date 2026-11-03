// The hand-written part of @axe/wire: buffer management, u64 and UTF-8 helpers. The op
// encoders and event decoders built on it are generated (./gen, from idl/wire.idl).

/** A generational handle: `index` is kind << 28 | slot (docs/command-buffer.md, "Handles"). */
export interface Handle {
  readonly index: number;
  readonly generation: number;
}

export const NULL_HANDLE: Handle = Object.freeze({ index: 0, generation: 0 });

export const HEADER_SIZE = 16;
export const OP_HEADER_SIZE = 4;
export const MAX_OP_SIZE = 0xfffc;
export const MAX_PAYLOAD = MAX_OP_SIZE - OP_HEADER_SIZE;
export const MAX_BUFFER_SIZE = 64 << 20;

if (new Uint8Array(new Uint32Array([1]).buffer)[0] !== 1) {
  throw new Error("@axe/wire needs a little-endian platform");
}

/** A malformed buffer: wrong magic, version or framing. */
export class WireError extends Error {
  override name = "WireError";
}

export function bad(op: string, field: string): RangeError {
  return new RangeError(`${op}: invalid ${field}`);
}

/** True for an integer in [0, max]. */
export function isUint(v: number, max: number): boolean {
  return v >= 0 && v <= max && Math.floor(v) === v;
}

const TWO32 = 4294967296;

/** Writes an exact u64 (a safe integer) at u32 index `w`. */
export function splitU64(u32: Uint32Array, w: number, v: number): void {
  if (!Number.isSafeInteger(v) || v < 0) throw new RangeError(`u64 ${v} is not a safe non-negative integer`);
  u32[w] = v % TWO32;
  u32[w + 1] = Math.floor(v / TWO32);
}

/** Writes a time in ns (rounded; values past 2^64 - 1 clamp to it) at u32 index `w`. */
export function splitTime(u32: Uint32Array, w: number, v: number): void {
  if (!(v >= 0)) throw new RangeError(`time ${v} must be >= 0`);
  const r = Math.round(v);
  if (r >= TWO32 * TWO32) {
    u32[w] = 0xffffffff;
    u32[w + 1] = 0xffffffff;
    return;
  }
  const hi = Math.floor(r / TWO32);
  u32[w] = r - hi * TWO32;
  u32[w + 1] = hi;
}

/** Reads an exact u64; a value past 2^53 throws (counters never get there). */
export function joinU64(u32: Uint32Array, w: number): number {
  const hi = u32[w + 1]!;
  if (hi >= 0x200000) throw new RangeError("u64 counter past 2^53");
  return hi * TWO32 + u32[w]!;
}

/** Reads a time in ns: exact below 2^53, rounded above. */
export function joinTime(u32: Uint32Array, w: number): number {
  return u32[w + 1]! * TWO32 + u32[w]!;
}

// ---- UTF-8 ------------------------------------------------------------------------------
// Text offsets on the wire are UTF-8 byte offsets; JS strings index UTF-16 code units. The
// conversions here treat a lone surrogate as U+FFFD (3 bytes), as TextEncoder encodes it.

/** Bytes of `s` encoded as UTF-8. */
export function utf8Length(s: string): number {
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 0x80) n += 1;
    else if (c < 0x800) n += 2;
    else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length && (s.charCodeAt(i + 1) & 0xfc00) === 0xdc00) {
      n += 4;
      i++;
    } else n += 3;
  }
  return n;
}

let encoder: TextEncoder | undefined;

/** Encodes `s` into `u8` at `at` (which must have room for utf8Length(s) bytes); returns the
 * number of bytes written. */
export function encodeUtf8Into(s: string, u8: Uint8Array, at: number): number {
  encoder ??= new TextEncoder();
  return encoder.encodeInto(s, u8.subarray(at)).written;
}

/** UTF-8 bytes of a string. */
export function encodeUtf8(s: string): Uint8Array {
  // Short ASCII strings (labels, names) are encoded here: cheaper than a native call.
  if (s.length <= 64) {
    const out = new Uint8Array(s.length);
    let i = 0;
    for (; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c >= 0x80) break;
      out[i] = c;
    }
    if (i === s.length) return out;
  }
  encoder ??= new TextEncoder();
  return encoder.encode(s);
}

/** True if the bytes are well-formed UTF-8 (Unicode §3.9). */
export function isUtf8(b: Uint8Array): boolean {
  for (let i = 0; i < b.length; ) {
    const c = b[i]!;
    if (c < 0x80) {
      i++;
      continue;
    }
    let n: number, lo = 0x80, hi = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) n = 1;
    else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      if (c === 0xe0) lo = 0xa0;
      if (c === 0xed) hi = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      if (c === 0xf0) lo = 0x90;
      if (c === 0xf4) hi = 0x8f;
    } else return false;
    if (i + n >= b.length + 0 && i + n > b.length - 1 + 1) return false;
    const second = b[i + 1];
    if (second === undefined || second < lo || second > hi) return false;
    for (let k = 2; k <= n; k++) {
      const x = b[i + k];
      if (x === undefined || (x & 0xc0) !== 0x80) return false;
    }
    i += n + 1;
  }
  return true;
}

/** The UTF-8 byte offset of UTF-16 index `i` in `s` (an index inside a surrogate pair maps to
 * the pair's start). */
export function utf8OffsetFromUtf16(s: string, i: number): number {
  if (i <= 0) return 0;
  let n = 0;
  const end = Math.min(i, s.length);
  for (let k = 0; k < end; k++) {
    const c = s.charCodeAt(k);
    if (c < 0x80) n += 1;
    else if (c < 0x800) n += 2;
    else if (c >= 0xd800 && c <= 0xdbff && k + 1 < s.length && (s.charCodeAt(k + 1) & 0xfc00) === 0xdc00) {
      if (k + 1 >= end) break; // `i` splits the pair
      n += 4;
      k++;
    } else n += 3;
  }
  return n;
}

/** The UTF-16 index of UTF-8 byte offset `b` in `s` (an offset inside a character maps to its
 * start). */
export function utf16OffsetFromUtf8(s: string, b: number): number {
  let n = 0;
  for (let k = 0; k < s.length; k++) {
    const c = s.charCodeAt(k);
    const pair = c >= 0xd800 && c <= 0xdbff && k + 1 < s.length && (s.charCodeAt(k + 1) & 0xfc00) === 0xdc00;
    const len = c < 0x80 ? 1 : c < 0x800 ? 2 : pair ? 4 : 3;
    if (n + len > b) return k;
    n += len;
    if (pair) k++;
  }
  return s.length;
}

/** Offsets of one string both ways, for repeated lookups (hit testing, selections): O(1)
 * from UTF-16 to UTF-8, O(log n) back. */
export class Utf8OffsetMap {
  /** utf8[i]: the UTF-8 offset of UTF-16 index i, for i in [0, length]. */
  private readonly utf8: Uint32Array;

  constructor(readonly text: string) {
    const map = new Uint32Array(text.length + 1);
    let n = 0;
    for (let k = 0; k < text.length; k++) {
      map[k] = n;
      const c = text.charCodeAt(k);
      if (c >= 0xd800 && c <= 0xdbff && k + 1 < text.length && (text.charCodeAt(k + 1) & 0xfc00) === 0xdc00) {
        map[k + 1] = n; // inside the pair: its start
        n += 4;
        k++;
      } else n += c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
    }
    map[text.length] = n;
    this.utf8 = map;
  }

  get byteLength(): number {
    return this.utf8[this.text.length]!;
  }

  toUtf8(i: number): number {
    return this.utf8[Math.max(0, Math.min(i, this.text.length))]!;
  }

  toUtf16(b: number): number {
    const map = this.utf8;
    let lo = 0, hi = this.text.length;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (map[mid]! <= b) lo = mid;
      else hi = mid - 1;
    }
    // A low surrogate shares its pair's offset: step back to the pair's start.
    while (lo > 0 && map[lo - 1] === map[lo]) lo--;
    return lo;
  }
}

// ---- Buffers ------------------------------------------------------------------------------

/** Grows a writer's buffer: returns a buffer of at least `minBytes` holding `current`'s bytes. */
export type Grow = (current: ArrayBuffer, minBytes: number) => ArrayBuffer;

export const defaultGrow: Grow = (current, minBytes) => {
  let size = Math.max(current.byteLength * 2, 4096);
  while (size < minBytes) size *= 2;
  const next = new ArrayBuffer(Math.min(size, MAX_BUFFER_SIZE));
  new Uint8Array(next).set(new Uint8Array(current));
  return next;
};

/** A position to roll back to (ops written after it are discarded). */
export interface WriterMark {
  readonly size: number;
  readonly ops: number;
}

/** Appends ops to one buffer and frames it (docs/command-buffer.md, "Header", "Op framing"). */
export class WireWriter {
  buffer: ArrayBuffer;
  u8!: Uint8Array;
  u32!: Uint32Array;
  f32!: Float32Array;
  /** Bytes used, the header included. */
  size = HEADER_SIZE;
  /** Ops written since the last finish. */
  opCount = 0;

  constructor(
    readonly magic: number,
    private readonly grow: Grow = defaultGrow,
    initial: ArrayBuffer = new ArrayBuffer(4096),
  ) {
    this.buffer = initial;
    this.views();
  }

  private views(): void {
    this.u8 = new Uint8Array(this.buffer);
    this.u32 = new Uint32Array(this.buffer, 0, this.buffer.byteLength >> 2);
    this.f32 = new Float32Array(this.buffer, 0, this.buffer.byteLength >> 2);
  }

  /** Replaces the storage (the host hands over a new buffer after a flush). */
  adopt(buffer: ArrayBuffer): void {
    this.buffer = buffer;
    this.views();
  }

  /** Reserves an op of `payload` bytes (a multiple of 4) and returns its payload's offset. */
  protected op(opcode: number, payload: number): number {
    const opSize = OP_HEADER_SIZE + payload;
    if (opSize > MAX_OP_SIZE) throw new RangeError(`op ${opcode}: ${payload} bytes don't fit one op`);
    const start = this.size, end = start + opSize;
    if (end > MAX_BUFFER_SIZE) throw new RangeError("command buffer past 64 MiB");
    if (end > this.buffer.byteLength) {
      this.buffer = this.grow(this.buffer, end);
      this.views();
    }
    this.u32[start >> 2] = (opcode | (opSize << 16)) >>> 0;
    this.size = end;
    this.opCount++;
    return start + OP_HEADER_SIZE;
  }

  mark(): WriterMark {
    return { size: this.size, ops: this.opCount };
  }

  rollback(m: WriterMark): void {
    this.size = m.size;
    this.opCount = m.ops;
  }

  /** Writes the header and returns the buffer's bytes (a view valid until the next write),
   * or null if no op was written. The writer starts a new buffer. */
  finish(version: number): Uint8Array | null {
    if (!this.opCount) return null;
    const u32 = this.u32;
    u32[0] = this.magic;
    u32[1] = version & 0xffff; // version, then flags 0
    u32[2] = this.size;
    u32[3] = this.opCount;
    const bytes = this.u8.subarray(0, this.size);
    this.size = HEADER_SIZE;
    this.opCount = 0;
    return bytes;
  }
}
