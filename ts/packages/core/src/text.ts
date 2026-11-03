// A node's text (docs/text.md, docs/command-buffer.md "Text"): what ui_cmd_set_text does,
// for JS strings. Offsets in the options are UTF-16 indices, as JS strings count; the wire's
// are UTF-8 byte offsets, converted here.

import {
  MAX_PAYLOAD,
  TextField,
  Utf8OffsetMap,
  encodeUtf8,
  type Handle,
  type TextAlign,
  type TextDirection,
  type TextSlant,
  type TextStyle,
} from "@axe/wire";
import { commands } from "./runtime.js";

/** A style for text. A field left out comes from the node (its styles, or what it inherits);
 * for a span, from the paragraph. */
export interface TextStyleOptions {
  /** Family names in order of preference, e.g. ["Noto Sans", "sans-serif"]. */
  readonly families?: readonly string[];
  /** Logical px, in (0, 4096]. */
  readonly size?: number;
  /** 1 to 1000 (400 regular, 700 bold). */
  readonly weight?: number;
  readonly slant?: TextSlant;
  /** A BCP 47 tag ("ro", "sr-Cyrl"), or null for none. */
  readonly language?: string | null;
  /** sRGB, straight alpha. */
  readonly color?: readonly [number, number, number, number];
}

export interface TextSpanOptions extends TextStyleOptions {
  /** UTF-16 indices into the text: sorted, not overlapping. */
  readonly start: number;
  readonly end: number;
}

export interface TextOptions extends TextStyleOptions {
  /** A multiple of the size, or 0 for the fonts' own line heights. */
  readonly lineHeight?: number;
  readonly align?: TextAlign;
  readonly direction?: TextDirection;
  /** 0: unlimited. */
  readonly maxLines?: number;
  /** Shown where maxLines cuts the text: undefined for U+2026, null or "" for none. */
  readonly ellipsis?: string | null;
  readonly spans?: readonly TextSpanOptions[];
}

const MAX_TEXT = 16 << 20;
const MAX_SPANS = 1024;
const MAX_STRINGS = 64;
const MAX_ELLIPSIS = 64;
const FAMILY_DEFAULT = 0xffff;
const SPAN_FIELDS = TextField.spanAll;
const LANGUAGE = /^[A-Za-z0-9-]{1,35}$/;

const pad4 = (n: number) => (n + 3) & ~3;

// String regions by their families and languages: apps reuse a handful.
const regions = new Map<string, Uint8Array>();

// The string region: family lists, then language tags, each once.
class Strings {
  readonly families: string[] = [];
  readonly languages: string[] = [];

  family(names: readonly string[]): number {
    if (!names.length) throw new RangeError("setText: families needs at least one name");
    const list = names.join(", ");
    let i = this.families.indexOf(list);
    if (i < 0) i = this.families.push(list) - 1;
    return i;
  }

  language(tag: string): number {
    if (!LANGUAGE.test(tag)) throw new RangeError(`setText: "${tag}" is not a language tag`);
    let i = this.languages.indexOf(tag);
    if (i < 0) i = this.languages.push(tag) - 1;
    return i;
  }

  region(): Uint8Array {
    const key = `${this.families.join("\n")}\u0001${this.languages.join("\n")}`;
    let out = regions.get(key);
    if (!out) {
      out = this.build();
      if (regions.size >= 64) regions.clear(); // a few combinations in practice
      regions.set(key, out);
    }
    return out;
  }

  private build(): Uint8Array {
    const all = [...this.families, ...this.languages];
    if (all.length > MAX_STRINGS) throw new RangeError(`setText: more than ${MAX_STRINGS} families and languages`);
    const parts = all.map((s) => {
      if (s.includes("\0")) throw new RangeError("setText: a family or language contains NUL");
      return encodeUtf8(s);
    });
    const size = parts.reduce((n, p) => n + p.length + 1, 0);
    const out = new Uint8Array(pad4(size));
    let at = 0;
    for (const p of parts) {
      out.set(p, at);
      at += p.length + 1; // NUL-terminated; the padding is zeros
    }
    return out;
  }
}

function check(ok: boolean, what: string): void {
  if (!ok) throw new RangeError(`setText: invalid ${what}`);
}

// A style as the wire has it: fields that come from the node are placeholders, flagged in
// fromNode, which older readers show (as ui_cmd_set_text writes them).
function wireStyle(o: TextStyleOptions, strings: Strings): { style: TextStyle; set: number } {
  let set = 0;
  let family = FAMILY_DEFAULT, weight = 400, slant = 0, language = 0, size = 16;
  let color: readonly [number, number, number, number] = [0, 0, 0, 1];
  if (o.families !== undefined) {
    set |= TextField.family;
    family = strings.family(o.families);
  }
  if (o.size !== undefined) {
    check(Number.isFinite(o.size) && o.size > 0 && o.size <= 4096, "size");
    set |= TextField.size;
    size = o.size;
  }
  if (o.weight !== undefined) {
    check(Number.isInteger(o.weight) && o.weight >= 1 && o.weight <= 1000, "weight");
    set |= TextField.weight;
    weight = o.weight;
  }
  if (o.slant !== undefined) {
    check(Number.isInteger(o.slant) && o.slant >= 0 && o.slant <= 2, "slant");
    set |= TextField.slant;
    slant = o.slant;
  }
  if (o.language !== undefined) {
    set |= TextField.language;
    language = o.language === null ? 0 : strings.language(o.language) + 1; // + families, below
  }
  if (o.color !== undefined) {
    check(o.color.length === 4 && o.color.every(Number.isFinite), "color");
    set |= TextField.color;
    color = o.color;
  }
  return { style: { family, weight, slant, fromNode: ~set & SPAN_FIELDS, language, size, color }, set };
}

export function setText(node: Handle, text: string, o: TextOptions): void {
  const bytes = encodeUtf8(text);
  check(bytes.length <= MAX_TEXT, "length (at most 16 MiB)");
  const strings = new Strings();
  const base = wireStyle(o, strings);
  let set = base.set;
  let lineHeight = 0, align = 0, direction = 0;
  if (o.lineHeight !== undefined) {
    check(Number.isFinite(o.lineHeight) && o.lineHeight >= 0 && o.lineHeight <= 100, "lineHeight");
    set |= TextField.lineHeight;
    lineHeight = o.lineHeight;
  }
  if (o.align !== undefined) {
    check(Number.isInteger(o.align) && o.align >= 0 && o.align <= 5, "align");
    set |= TextField.align;
    align = o.align;
  }
  if (o.direction !== undefined) {
    check(Number.isInteger(o.direction) && o.direction >= 0 && o.direction <= 2, "direction");
    set |= TextField.direction;
    direction = o.direction;
  }
  const maxLines = o.maxLines ?? 0;
  check(Number.isInteger(maxLines) && maxLines >= 0 && maxLines <= 0xffff, "maxLines");

  const spans = o.spans ?? [];
  check(spans.length <= MAX_SPANS, "spans (at most 1024)");
  const map = spans.length ? new Utf8OffsetMap(text) : null;
  let prevEnd = 0;
  const wireSpans = spans.map((sp) => {
    check(Number.isInteger(sp.start) && Number.isInteger(sp.end) && sp.start < sp.end && sp.start >= prevEnd && sp.end <= text.length,
          "spans (sorted, not overlapping, within the text)");
    prevEnd = sp.end;
    const w = wireStyle(sp, strings);
    return { start: map!.toUtf8(sp.start), end: map!.toUtf8(sp.end), style: w.style };
  });
  // Languages are numbered after the families in the string region.
  const families = strings.families.length;
  const relabel = (s: TextStyle): TextStyle => (s.language ? { ...s, language: s.language + families } : s);
  const style = relabel(base.style);
  const spanRecords = wireSpans.map((s) => ({ ...s, style: relabel(s.style) }));
  const region = strings.region();
  const ellipsis = o.ellipsis === undefined || o.ellipsis === null ? new Uint8Array(0) : encodeUtf8(o.ellipsis);
  check(ellipsis.length <= MAX_ELLIPSIS, "ellipsis (at most 64 bytes)");
  const header = 72 + spanRecords.length * 36 + region.length + pad4(ellipsis.length);
  check(header <= MAX_PAYLOAD && region.length <= 0xffff, "text options (too large for one op)");
  const room = (MAX_PAYLOAD - header) & ~3;
  const chunk = Math.min(bytes.length, room);

  // Text longer than one op continues in append_text ops; all of it applies, or none.
  const mark = commands.mark();
  try {
    commands.setText(
      {
        node,
        textBytes: bytes.length,
        stringCount: strings.families.length + strings.languages.length,
        maxLines,
        direction: set & TextField.direction ? direction : 0,
        flags: o.ellipsis !== undefined ? 1 : 0, // custom ellipsis (null: none)
        style,
        lineHeight: set & TextField.lineHeight ? lineHeight : 0,
        align: set & TextField.align ? align : 0,
        fromNode: ~set & TextField.all,
      },
      spanRecords,
      region,
      ellipsis,
      bytes.subarray(0, chunk),
    );
    const max = (MAX_PAYLOAD - 12) & ~3;
    for (let sent = chunk; sent < bytes.length; sent += max)
      commands.appendText(node, bytes.subarray(sent, Math.min(bytes.length, sent + max)));
  } catch (e) {
    commands.rollback(mark);
    throw e;
  }
}
