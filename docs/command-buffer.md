# Command and event buffers (wire v0)

The language runtime and the core talk only through two binary buffers:

- **Command buffer**: logic → core. Operations on nodes. Applied atomically, in order, at
  the first frame boundary after its preparation (shaping text, reading fonts) is done.
- **Event buffer**: core → logic. What happened: frames committed, surfaces resized or
  asked to close.

Both share one framing. Every op, event and record is described once, in `idl/wire.idl`
([`docs/idl.md`](idl.md)), and every encoder and decoder is generated from it: the C codec
(`src/wire/gen/`) used by the C API's writer (`include/ui/wire.h`) and by the core, and the
TypeScript package `@axe/wire`. The tables below are generated too. Writers and readers of
different ages interoperate: see [Evolving the format](#evolving-the-format).

## Encoding rules

- Little-endian, IEEE 754 `f32`, no padding beyond what is listed.
- Every op starts on a 4-byte boundary, and every op size is a multiple of 4.
- A `u64` may sit at an offset that is a multiple of 4 but not of 8. Readers must load it
  unaligned (`memcpy`).
- Pointers and native sizes never appear, so buffers are position-independent and portable
  to WASM. Text appears only in the text ops, as length-counted UTF-8 byte arrays (family
  names are NUL-terminated inside a sized region).

## Header (16 bytes)

| Offset | Type | Field | Value |
| --- | --- | --- | --- |
| 0 | u32 | magic | `0x42434955` ("UICB") for commands, `0x42454955` ("UIEB") for events |
| 4 | u16 | version | `0` |
| 6 | u16 | flags | `0`; reserved |
| 8 | u32 | total_bytes | size of the whole buffer, header included |
| 12 | u32 | op_count | number of ops that follow |

## Op framing

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u16 | opcode |
| 2 | u16 | size: bytes of this op including these 4, a multiple of 4, at least 4 |
| 4 | … | payload (`size - 4` bytes) |

The maximum op size is 65 532 bytes and the maximum buffer size is 64 MiB.

## Handles

A handle is 8 bytes: `u32 index`, `u32 generation`.

- `index` = `kind << 28 | slot`. Kind 0 is a node; kind 1 is a surface.
- Generation 0 is the null handle and is never valid.
- **Node handles are allocated by the logic side** (`ui_handle_alloc`), with no round trip
  to the core. The core mirrors the allocator:
  - `create_node` must name a free slot with a generation newer than any earlier use of
    that slot.
  - Every other op must match a live node's slot and generation exactly.
- **Surface handles are allocated by the core** and reach logic through events.
- Slots at or above 2²⁰ are rejected.

## Commands (`UICB`)

Bytes are what this version writes. They are also each op's original size, except where
the [op history](#op-history) lists appended fields.

<!-- idl:commands -->
| Opcode | Op | Payload | Bytes |
| --- | --- | --- | --- |
| 1 | `create_node` | node handle, parent handle (a node, or a surface for a root-level node) | 16 |
| 2 | `destroy_node` | node handle; destroys the whole subtree | 8 |
| 3 | `set_rect` | node, `f32 x, y, width, height` (logical px; width and height ≥ 0): the node's absolute frame in its parent's padding box ([`docs/layout.md`](layout.md), "set_rect") | 24 |
| 4 | `set_corner_radius` | node, `f32 radius` (logical px, ≥ 0) | 12 |
| 5 | `set_color` | node, `f32 r, g, b, a` (sRGB, straight alpha, clamped to [0, 1]) | 24 |
| 6 | `start_spring` | node, `u16 property`, `u16 count`, `f32 stiffness`, `f32 damping`, `f32 target[count]` | 20 + 4·count |
| 7 | `set_timer` | `u32 id`, `u32` reserved (0), `u64 deadline_ns` (animation clock; `UINT64_MAX` disarms) | 16 |
| 8 | `set_text` | see [Text](#text) | 72 + variable |
| 9 | `append_text` | node, `u16 chunk_bytes`, `u16 header_bytes` (12), chunk (padded to 4) | 12 + chunk |
| 10 | `set_clip` | node, `u32 flags` (bit 0: clip; other bits must be 0) | 12 |
| 11 | `load_font` | `u32 id`, `u16 path_bytes` (1–4096), `u16 header_bytes` (8), the path in UTF-8 (padded to 4, no NUL) | 8 + path |
| 12 | `define_style` | `u32 id`, `u16 header_bytes` (8), `u16 block_count`, blocks (see [`docs/styles.md`](styles.md)) | 8 + blocks |
| 13 | `destroy_style` | `u32 id` | 4 |
| 14 | `set_styles` | node, `u16 count`, `u16 header_bytes` (12), `u32 ids[count]` | 12 + 4·count |
| 15 | `set_props` | node, `u16 header_bytes` (12), `u16 entry_count`, entries | 12 + entries |
| 16 | `set_token` | `u32 id`, one entry (property 0) | 4 + entry |
| 17 | `set_state` | node, `u32 mask`, `u32 value` | 16 |
| 18 | `move_node` | node, parent handle (a node, or a surface for a root-level node), `before` handle (a sibling under that parent; null appends) | 24 |
| 19 | `request_layout` | node, `u32 request_id`: answered by `layout_measured` ([`docs/layout.md`](layout.md), "Measurement") | 12 |
| 20 | `set_token_overrides` | node, `u16 header_bytes` (12), `u16 count`, `u32 tokens[count]`, then `count` entries (property 0): the node's own token declarations (see [`docs/styles.md`](styles.md), "Token scopes"); `unset` removes one | 12 + variable |
| 21 | `define_template` | `u32 id` (not 0), `u16 header_bytes` (12), `u16 node_count` (1–1024), `u16 ops_bytes`, `u16` reserved, then the template's ops: a static subtree to instantiate (see [`docs/templates.md`](templates.md)) | 12 + ops |
| 22 | `instantiate_template` | `u32 template`, `u16 header_bytes` (24), `u16 count`, parent handle, `before` handle (null appends), `handle nodes[count]`: the template's ops with these handles for its nodes (see [`docs/templates.md`](templates.md)) | 24 + nodes |
| 23 | `destroy_template` | `u32 id`: later instances of it are dropped | 4 |
<!-- /idl:commands -->

`set_color`, `set_corner_radius` and `set_clip` set the node's overrides of the matching
style properties ([`docs/styles.md`](styles.md), "Older ops"). Styles, tokens, states and
transitions are described there.

### Properties for `start_spring`

<!-- idl:spring-properties -->
| Id | Property | Components |
| --- | --- | --- |
| 1 | rect | 4: x, y, width, height in logical pixels, relative to the parent |
| 2 | corner_radius | 1: logical pixels |
| 3 | color | 4: sRGB, straight alpha; springs interpolate in linear light |
<!-- /idl:spring-properties -->

Springs have mass 1 and are solved in closed form, so they don't depend on the frame
rate.

- A `start_spring` on a property whose spring is running **retargets it and keeps its
  current position and velocity**.
- A `set_*` op on a property stops that property's spring, unless the node's styles declare
  a transition for it, which then animates the change.
- `count` must equal the property's component count; stiffness must be > 0 and damping ≥ 0.
  A property this reader doesn't know (from a newer writer) drops the op, as does a negative
  corner radius target (as for `set_corner_radius`).

### Clipping

`set_clip` with bit 0 set clips the node's text and all of its descendants to the node's
rect; a node inside another clipped node is clipped to the intersection. The clip is
rectangular, in whole physical pixels: rounded corners are not clipped, and the node's own
background is not affected. Text and rects entirely outside the clip are culled before
they reach the display list, so clipped-away glyphs are never rasterized.

### Fonts

`load_font` registers face 0 of a font file and answers with `font_loaded` carrying the
same id, when its buffer applies. The file is read on a worker thread. On success all
text is shaped again (also on workers, before the buffer applies), since the new font can
change family matches and fallbacks, and the surfaces redraw with the new shaping in the
frame that applies the buffer. A path that is
empty, too long, contains NUL or is not valid UTF-8 drops the op (no event).

### Timers

`set_timer` arms a one-shot timer, or re-arms it if one with the same `id` is armed.

- It fires as a `timer_fired` event once the animation clock reaches `deadline_ns`. That
  happens even when no surface draws, so logic can keep time while everything is idle.
- A paused clock only moves when stepped, so timers don't fire while it's paused.
- Like every command, `set_timer` takes effect at the frame boundary where its buffer
  applies.
- `deadline_ns = UINT64_MAX` disarms the timer (`ui_cmd_cancel_timer`).
- An app holds at most 4096 armed timers; further ones are dropped like an invalid op.

### Text

`set_text` gives a node a paragraph of styled text. The text wraps to the node's width, is
drawn over its background with its origin at the node's top-left, and is laid out again
whenever the width or the surface scale changes.

| Offset | Type | Field |
| --- | --- | --- |
| 0 | handle | node |
| 8 | u32 | `text_bytes`: total UTF-8 length (≤ 16 MiB) |
| 12 | u16 | `chunk_bytes`: text bytes carried in this op |
| 14 | u16 | `header_bytes`: where the spans start, 72 in this version; 0 = 64 |
| 16 | u16 | `span_count` (≤ 1024) |
| 18 | u16 | `string_count`: family lists and language tags (≤ 64) |
| 20 | u16 | `strings_bytes`: size of the string region, a multiple of 4 |
| 22 | u16 | `ellipsis_bytes` (≤ 64) |
| 24 | u16 | `max_lines` (0 = unlimited) |
| 26 | u8 | direction: 0 auto (first strong character, per paragraph), 1 LTR, 2 RTL |
| 27 | u8 | flags: bit 0 = the ellipsis bytes replace the default U+2026 (0 bytes = none) |
| 28 | style | the default style (36 bytes, below). Its `start` and `end` are unused: the first 2 bytes are `u16 span_bytes`, the size of each span record (36 in this version; 0 = 36), and the other 6 are 0 |
| 64 | f32 | `line_height`: line box height as a multiple of the default size; 0 = from font metrics; ≤ 100 |
| 68 | u8 | align: 0 start, 1 end, 2 left, 3 right, 4 center, 5 justify |
| 69 | u8 | reserved (0) |
| 70 | u16 | `from_node`: the fields that come from the node rather than this op (bits below); 0 = all set here |
| `header_bytes` | style[] | `span_count` spans, each `span_bytes` long |
| … | bytes | string region: `string_count` NUL-terminated UTF-8 strings, zero-padded |
| … | bytes | ellipsis, padded to 4 |
| … | bytes | the first `chunk_bytes` of text, padded to 4 |

A style or span is 36 bytes:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u32 | `start`: byte offset into the text |
| 4 | u32 | `end`: exclusive; spans are sorted and do not overlap |
| 8 | u16 | family: index of a family list in the string region, or `0xFFFF` for the default |
| 10 | u16 | weight, 1–1000 (400 regular, 700 bold) |
| 12 | u8 | slant: 0 normal, 1 italic, 2 oblique |
| 13 | u8 | `from_node` of a span: its fields that come from the paragraph's (bits below); 0 = all set here. Unused in the default style (offset 70 has its bits) |
| 14 | u16 | language: index of a BCP 47 tag in the string region **plus one**; 0 = default |
| 16 | f32 | size, logical px, in (0, 4096] |
| 20 | f32[4] | color: sRGB, straight alpha, clamped to [0, 1] |

- **`from_node`** bits: 1 family, 2 size, 4 weight, 8 slant, 16 language, 32 color (a
  span's are these), 64 line height, 128 alignment, 256 direction. A field with its bit set
  takes the node's computed value ([`docs/styles.md`](styles.md), "Inheritance") for the
  paragraph, and the paragraph's for a span; its bytes are a placeholder (an older reader
  shows them) and aren't checked. Direction from the node is the node's `direction`; set
  here, it is the paragraph's own (auto included). `ui_cmd_set_text` sends the fields not in
  `ui_text_desc.set` (and `ui_text_span_desc.set`) as `from_node`.
- **Alignment** is relative to the paragraph's direction for start and end. Justify
  stretches the spaces of every line except the last line of a paragraph (and lines
  without spaces, which stay start-aligned).
- **Line height** 0 lets each line be as tall as its fonts need. Otherwise every line is
  `line_height × size × scale` px, rounded, with the baseline placed by the default style's
  font, so fallback fonts with tall metrics don't change the spacing.
- **Languages** select language-specific glyph forms (OpenType `locl`, e.g. Romanian ș,
  Serbian Cyrillic). `ui_cmd_set_text` writes `families` and then `languages` into the
  string region. A language change splits shaping runs.
- Each family entry is a CSS-style list (`"Noto Sans, sans-serif"`), tried in order
  against registered fonts and then the system's. Bytes no span covers use the default
  style.
- **Fields from other versions:** a reader takes the fields it knows from the first
  `header_bytes` (and from each span's first `span_bytes`) and ignores the rest. A header
  from before `line_height` and `align` existed (64 bytes) gets line height 0 and start
  alignment.
- **Long text** continues in `append_text` ops for the same node. They must follow the
  `set_text` directly, in the same buffer, until `text_bytes` have arrived. Otherwise the
  `set_text` is dropped, together with the continuations it consumed, and the node keeps its
  old text. A buffer is applied atomically, so long text changes atomically too. A stray
  `append_text` is dropped. `ui_cmd_set_text` splits text automatically.
- **Invalid UTF-8** is replaced with U+FFFD, one per maximal subpart of an ill-formed
  sequence (Unicode §3.9). Each replacement is counted in
  `ui_app_stats.text_invalid_sequences`. Span offsets are moved with their characters, and
  offsets inside a sequence snap to its start. Offsets reported back for this text refer to
  the stored, sanitized text.
- **Framing errors** reject the whole buffer: a payload smaller than the original 64-byte
  header, a `header_bytes` below 64 or `span_bytes` below 36, either of them or
  `strings_bytes` not a multiple of 4, or regions that add up to more than the op holds.
- **Drops** (counted, and the node is unchanged): a stale node, out-of-range values,
  unsorted or overlapping spans, spans past the end, a family or language index past the
  string region, strings or a custom ellipsis that are not valid UTF-8, or a missing
  continuation.

Example: `set_text(node {1,1}, "Hi")` with the default style (16 px, weight 400, white),
the family list "Noto Sans", and no spans:

```
55 49 43 42  00 00 00 00  6C 00 00 00  01 00 00 00   header: UICB v0, 108 bytes, 1 op
08 00 5C 00  01 00 00 00  01 00 00 00                set_text, 92 bytes, node {1,1}
02 00 00 00  02 00 48 00                             text_bytes 2, chunk_bytes 2,
                                                     header_bytes 72
00 00 01 00  0C 00 00 00  00 00 00 00                0 spans, 1 string, 12-byte region,
                                                     no ellipsis, max_lines 0, auto, flags 0
24 00 00 00  00 00 00 00  00 00 90 01  00 00 00 00   default style: span_bytes 36, unused,
                                                     family 0, weight 400, normal,
                                                     no language
00 00 80 41  00 00 80 3F  00 00 80 3F  00 00 80 3F   size 16, r 1, g 1, b 1
00 00 80 3F                                          a 1
00 00 00 00  00 00 00 00                             line_height 0, align start, reserved
4E 6F 74 6F  20 53 61 6E  73 00 00 00                "Noto Sans" NUL, padding
48 69 00 00                                          "Hi", padding
```

## Events (`UIEB`)

<!-- idl:events -->
| Opcode | Event | Payload | Bytes |
| --- | --- | --- | --- |
| 1 | `frame_committed` | surface handle, `u64 frame_id`, `u64 predicted_present_ns` | 24 |
| 2 | `surface_resized` | surface handle, `f32 width, height` (logical px), `u32 scale_120` | 20 |
| 3 | `surface_close_requested` | surface handle | 8 |
| 4 | `timer_fired` | `u32 id`, `u32` reserved, `u64 deadline_ns`, `u64 now_ns` (both animation clock) | 24 |
| 5 | `font_loaded` | `u32 id` (from `load_font`), `u32 result` (`ui_result`: 0 OK, `UI_ERR_IO`, `UI_ERR_INVALID_ARGUMENT`) | 8 |
| 6 | `layout_measured` | `u32 request_id`, `u32 result` (0 OK, `UI_ERR_STALE_HANDLE`), node handle, surface handle (null when stale), then four rects of `f32 x, y, width, height` (logical px): `layout_in_parent`, `layout_in_surface`, `presentation_in_parent`, `presentation_in_surface` | 88 |
| 7 | `clock` | `u64 now_ns`: the animation clock when the core sent the buffer; first in every event buffer | 8 |
<!-- /idl:events -->

- `predicted_present_ns` is animation-clock time: when the frame is expected on screen.
  Pausing or stepping the clock moves it, so logic that keeps time from these events is
  deterministic under a stepped clock.
- `scale_120` is the scale in 120ths: 120 = 1.0, 150 = 1.25, 180 = 1.5.
- `surface_resized` is sent once when a surface appears and again whenever its logical
  size or scale changes. It always precedes that frame's `frame_committed`.
- `frame_committed` is sent only for frames that were drawn. A surface with nothing to draw
  (no applied commands, no running spring, no size or configure change) goes idle and
  sends nothing, so logic must not use these events as a clock. Use timers for that.
- When several timers are due at once they fire earliest deadline first, ties by `id`.
  Re-arming from `deadline_ns` rather than `now_ns` keeps a periodic timer from drifting.
- `layout_measured` answers come in the order of their requests, after the
  `frame_committed` of the frame that laid the node out.
- `clock` comes first in every event buffer, and one buffer (perhaps with nothing else) is
  sent at startup. Its `now_ns` is the animation clock when the core sends the buffer (a
  buffer that waited while logic was busy carries the time it finally left), and never goes
  back: it is at least every time the buffer reports and every earlier clock. Logic keeps
  time from it even while nothing draws, and language runtimes base their timers on it
  ([`docs/script.md`](script.md), "Timers").

## Validation and error handling

The core validates a whole command buffer before applying any op. The following make the
**entire buffer** rejected, with nothing applied:

- wrong magic or version;
- `total_bytes` that differs from the received size;
- an op size that is not a multiple of 4, is below 4, or runs past the end;
- an op count that does not match;
- a known opcode whose payload is smaller than its original size, or smaller than the
  variable data its own counts describe (a longer one is fine: see below).

If the buffer is well-framed, every op is applied in order. Individual ops are
**dropped** (counted in `ui_app_stats.commands_dropped` and logged in debug builds,
never undefined behavior) when:

- they address a stale, destroyed or wrong-kind handle;
- they carry a non-finite or out-of-range value;
- the opcode is unknown (it came from a newer writer).

Readers of event buffers skip unknown event types by their size, and read known events
under the same rules as commands.

## Evolving the format

Ops and events change without a version bump as long as each change follows these rules.

- **Fields are only appended**, never inserted, removed or reordered, and an existing field
  never changes meaning. A fixed-size op appends to its payload. A variable-length op
  appends to its fixed header, or to a record inside it such as a `set_text` span.
- **Every op has an original size**, that of its payload, header or record as first
  shipped. A reader accepts a known opcode whose payload is at least that large:
  - bytes past the fields it knows come from a newer writer and are ignored;
  - fields the writer didn't send, because it predates them, take their default. Every
    appended field's default is encoded as zero bytes, so a reader fills missing fields
    with zeros;
  - only a payload smaller than the original size is a framing error, which rejects the
    buffer.
- **Variable-length ops carry their header size** (`set_text`, `append_text`,
  `load_font`, `define_style` and its blocks, `set_styles`, `set_props`,
  `set_token_overrides`), and `set_text` also
  carries its span size. Style entries carry their size in words. Readers find the variable data
  from these sizes, never from their own idea of the layout. A size of 0 stands for the
  original size: writers from before the size fields existed left those bytes 0. Data
  appended after the variable data is ignored like any trailing field. `start_spring`'s
  targets are counted, so its appended fields follow `target[count]`.
- **Reserved bytes** are written as 0 and ignored. A new field may take them over only if
  0 is its default; that is how span `language` arrived.
- **Behavior an old reader must not silently skip** needs a new opcode, which old readers
  drop and count, or a flag bit that readers check. `set_clip`, for example, drops the op
  when a flag it doesn't know is set.
- **New variable data** goes after the old, counted by an appended field (0: none, what
  older writers sent), in a record whose header says where its data starts. A style block's
  token declarations (milestone 3a.1) follow its entries this way; a block without them keeps
  the original 8-byte header, so buffers that don't use them are byte for byte as before.
- **Checked against an older reader.** CI builds the base commit's reader and runs the
  fixtures a change adds through it (`docs/idl.md`, "The older reader"): ops it knows must
  decode as before, and ops it doesn't must be skipped.
- **Bump the version** only for changes these rules can't express: a field that changes
  meaning or encoding, a removed field, or a framing change. Readers reject buffers with a
  version they don't know.

### Op history

Every appended field, with the change that added it (`since` in the IDL). Ops and events not
listed are at their original size.

<!-- idl:history -->
| Op | Original size | Appended |
| --- | --- | --- |
| `set_text` (8) | header 64, span 36 | `line_height` (f32 at 64) and `align` (u8 at 68): 62d0e10, "Text alignment and line height". span `language` (u16 at 14, formerly reserved): 461c82d, "Per-span language". `header_bytes` (u16 at 14, formerly reserved) and `span_bytes` (u16 at 28, formerly reserved): m1.1, "Milestone 1.1, wire evolution rules". `from_node` (u16 at 70) and span `from_node` (u8 at 13, formerly reserved): ae568d4, "Shaping properties in styles, prepared on workers" |
| `append_text` (9) | header 12 | `header_bytes` (u16 at 10, formerly reserved): m1.1, "Milestone 1.1, wire evolution rules" |
| `load_font` (11) | header 8 | `header_bytes` (u16 at 6, formerly reserved): m1.1, "Milestone 1.1, wire evolution rules" |
| `define_style` (12) | header 8, block 8 | block `token_count` (u16 at 8): m3a.1, "Milestone 3a.1, token scopes" |
<!-- /idl:history -->

## Flow control and ordering

- Each flush is one buffer. Buffers are applied in arrival order, each one whole.
- **Preparation.** When the core receives a buffer, worker threads start preparing it:
  every `set_text` is decoded and shaped and its glyphs rasterized, and every `load_font`
  file is read. A buffer applies at the first frame boundary where it and every buffer
  before it are prepared; until then, it and every later buffer wait for a later frame,
  while frames and animations continue. Most buffers have nothing to prepare, or are
  prepared long before the next frame.
- Writes become visible no earlier than the next frame commit after the flush.
- **Logic → core is lossless.** The channel is unbounded: a flush is never refused,
  delayed or dropped, however far ahead logic runs, and buffers apply in the order they
  were flushed. The only failure is running out of memory. Even then nothing is lost: the
  logic actor holds the message and retries, and the core leaves messages queued until it
  can take them.
- **Core → logic is bounded.** The core keeps at most two event buffers queued for logic.
  Later events are appended to one pending buffer, so a stalled logic actor gets one
  batch instead of a backlog. Event order is always preserved.

## Example

`create_node(n={1,1}, parent=surface {0x10000000,1})` followed by
`set_corner_radius(n, 8.0)`:

```
55 49 43 42  00 00 00 00  34 00 00 00  02 00 00 00   header: UICB v0, 52 bytes, 2 ops
01 00 14 00  01 00 00 00  01 00 00 00                create_node, 20 bytes, node {1,1}
             00 00 00 10  01 00 00 00                parent {0x10000000,1}
04 00 10 00  01 00 00 00  01 00 00 00  00 00 00 41   set_corner_radius, 16 bytes, 8.0
```
