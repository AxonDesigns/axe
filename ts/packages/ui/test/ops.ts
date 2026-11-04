// Test helpers: the ops a piece of code writes, decoded enough to check what @axe/ui does.

import { Op } from "@axe/wire";
import { app, type Surface } from "@axe/core";
import { commands } from "@axe/core/internal";
import { flush } from "@axe/ui/src/reactive.js";

export interface DecodedOp {
  readonly op: string;
  readonly node?: number;
  readonly parent?: number;
  readonly before?: number;
  readonly text?: string;
  readonly spans?: number;
  readonly ids?: number[];
  readonly entries?: number;
  readonly mask?: number;
  readonly value?: number;
}

const names = Object.fromEntries(Object.entries(Op).map(([k, v]) => [v, k])) as Record<number, string>;
const decoder = new TextDecoder();

/** The ops `fn` writes into the turn's buffer, decoded (they stay in the buffer). */
export function capture(fn: () => void): DecodedOp[] {
  const start = commands.size;
  fn();
  return decode(start, commands.size);
}

/** The bytes of the ops `fn` writes. */
export function captureBytes(fn: () => void): number[] {
  const start = commands.size;
  fn();
  return Array.from(commands.u8.subarray(start, commands.size));
}

/** The ops `fn` writes, then those the end-of-turn flush writes. */
export function captureFlush(fn: () => void): DecodedOp[] {
  return capture(() => {
    fn();
    flush();
  });
}

function handle(u32: Uint32Array, word: number): number {
  const index = u32[word]!, generation = u32[word + 1]!;
  return generation === 0 ? -1 : index & 0x0fffffff;
}

function decode(start: number, end: number): DecodedOp[] {
  const u8 = commands.u8, u32 = commands.u32;
  const u16 = (at: number) => u8[at]! | (u8[at + 1]! << 8);
  const out: DecodedOp[] = [];
  for (let at = start; at < end; ) {
    const head = u32[at >> 2]!;
    const opcode = head & 0xffff, size = head >>> 16;
    const p = at + 4, w = p >> 2;
    const name = names[opcode] ?? `op${opcode}`;
    switch (opcode) {
      case Op.createNode:
        out.push({ op: name, node: handle(u32, w), parent: handle(u32, w + 2) });
        break;
      case Op.destroyNode:
        out.push({ op: name, node: handle(u32, w) });
        break;
      case Op.moveNode:
        out.push({ op: name, node: handle(u32, w), parent: handle(u32, w + 2), before: handle(u32, w + 4) });
        break;
      case Op.setStyles: {
        const count = u16(p + 8);
        const header = u16(p + 10) || 12;
        const ids: number[] = [];
        for (let i = 0; i < count; i++) ids.push(u32[(p + header) / 4 + i]!);
        out.push({ op: name, node: handle(u32, w), ids });
        break;
      }
      case Op.setProps:
        out.push({ op: name, node: handle(u32, w), entries: u16(p + 10) });
        break;
      case Op.setState:
        out.push({ op: name, node: handle(u32, w), mask: u32[w + 2]!, value: u32[w + 3]! });
        break;
      case Op.setText: {
        const chunk = u16(p + 12), header = u16(p + 14) || 64, spans = u16(p + 16);
        const strings = u16(p + 20), ellipsis = u16(p + 22), spanBytes = u16(p + 28) || 36;
        const textAt = p + header + spans * spanBytes + strings + ((ellipsis + 3) & ~3);
        out.push({ op: name, node: handle(u32, w), text: decoder.decode(u8.slice(textAt, textAt + chunk)), spans });
        break;
      }
      case Op.setTokenOverrides:
        out.push({ op: name, node: handle(u32, w), entries: u16(p + 10) });
        break;
      default:
        out.push({ op: name });
    }
    at += size;
  }
  return out;
}

/** Only the ops named. */
export const only = (ops: DecodedOp[], ...names: string[]) => ops.filter((o) => names.includes(o.op));

export function surface(): Surface {
  const s = app.surfaces[0];
  if (!s) throw new Error("no surface yet");
  return s;
}
