# Command and event buffers (wire v0)

The language runtime and the core talk only through two binary buffers:

- **Command buffer**: logic → core. Operations on nodes. Applied atomically, in order, at
  the next frame boundary.
- **Event buffer**: core → logic. What happened: frames committed, surfaces resized or
  asked to close.

Both share one framing. The C encoder and decoder live in `src/wire/`; the public API is
`include/ui/wire.h`. Later milestones will generate encoders and decoders from the IDL.

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

| Opcode | Op | Payload | Bytes |
| --- | --- | --- | --- |
| 1 | `create_node` | node handle, parent handle (a node, or a surface for a root-level node) | 16 |
| 2 | `destroy_node` | node handle; destroys the whole subtree | 8 |
| 3 | `set_rect` | node, `f32 x, y, width, height` (logical px, relative to the parent; width and height ≥ 0) | 24 |
| 4 | `set_corner_radius` | node, `f32 radius` (logical px, ≥ 0) | 12 |
| 5 | `set_color` | node, `f32 r, g, b, a` (sRGB, straight alpha, clamped to [0, 1]) | 24 |
| 6 | `start_spring` | node, `u16 property`, `u16 count`, `f32 stiffness`, `f32 damping`, `f32 target[count]` | 20 + 4·count |
| 7 | `set_timer` | `u32 id`, `u32` reserved (0), `u64 deadline_ns` (animation clock; `UINT64_MAX` disarms) | 16 |
| 8 | `set_text` | see [Text](#text) | 64 + variable |
| 9 | `append_text` | node, `u32 chunk_bytes`, chunk (padded to 4) | 12 + chunk |

### Properties for `start_spring`

| Id | Property | Components |
| --- | --- | --- |
| 1 | rect | 4: x, y, width, height |
| 2 | corner_radius | 1 |
| 3 | color | 4: r, g, b, a in sRGB. The spring runs in linear light. |

Springs have mass 1 and are solved in closed form, so they don't depend on the frame
rate.

- A `start_spring` on a property whose spring is running **retargets it and keeps its
  current position and velocity**.
- A `set_*` op on a property stops that property's spring.
- `count` must equal the property's component count; stiffness must be > 0 and damping ≥ 0.

### Timers

`set_timer` arms a one-shot timer, or re-arms it if one with the same `id` is armed.

- It fires as a `timer_fired` event once the animation clock reaches `deadline_ns`. That
  happens even when no surface draws, so logic can keep time while everything is idle.
- A paused clock only moves when stepped, so timers don't fire while it's paused.
- Like every command, `set_timer` takes effect at the next frame boundary.
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
| 12 | u32 | `chunk_bytes`: text bytes carried in this op |
| 16 | u16 | `span_count` (≤ 1024) |
| 18 | u16 | `family_count` (≤ 64) |
| 20 | u16 | `families_bytes`: size of the family region, a multiple of 4 |
| 22 | u16 | `ellipsis_bytes` (≤ 64) |
| 24 | u16 | `max_lines` (0 = unlimited) |
| 26 | u8 | direction: 0 auto (first strong character, per paragraph), 1 LTR, 2 RTL |
| 27 | u8 | flags: bit 0 = the ellipsis bytes replace the default U+2026 (0 bytes = none) |
| 28 | style | the default style (36 bytes, below; `start` and `end` are 0) |
| 64 | style[] | `span_count` spans |
| … | bytes | family region: `family_count` NUL-terminated UTF-8 names, zero-padded |
| … | bytes | ellipsis, padded to 4 |
| … | bytes | the first `chunk_bytes` of text, padded to 4 |

A style or span is 36 bytes:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u32 | `start`: byte offset into the text |
| 4 | u32 | `end`: exclusive; spans are sorted and do not overlap |
| 8 | u16 | family: index into the family region, or `0xFFFF` for the default family |
| 10 | u16 | weight, 1–1000 (400 regular, 700 bold) |
| 12 | u8 | slant: 0 normal, 1 italic, 2 oblique |
| 13 | u8[3] | reserved (0) |
| 16 | f32 | size, logical px, in (0, 4096] |
| 20 | f32[4] | color: sRGB, straight alpha, clamped to [0, 1] |

- Each family entry is a CSS-style list (`"Noto Sans, sans-serif"`), tried in order
  against registered fonts and then the system's. Bytes no span covers use the default
  style.
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
- **Framing errors** reject the whole buffer: a payload smaller than the fixed header, a
  `families_bytes` that is not a multiple of 4, or sizes that don't add up to the op size.
- **Drops** (counted, and the node is unchanged): a stale node, out-of-range values,
  unsorted or overlapping spans, spans past the end, a family index past the region, names
  or a custom ellipsis that are not valid UTF-8, or a missing continuation.

Example: `set_text(node {1,1}, "Hi")` with the default style (16 px, weight 400, white),
the family list "Noto Sans", and no spans:

```
55 49 43 42  00 00 00 00  64 00 00 00  01 00 00 00   header: UICB v0, 100 bytes, 1 op
08 00 54 00  01 00 00 00  01 00 00 00                set_text, 84 bytes, node {1,1}
02 00 00 00  02 00 00 00                             text_bytes 2, chunk_bytes 2
00 00 01 00  0C 00 00 00  00 00 00 00                0 spans, 1 family, 12-byte region,
                                                     no ellipsis, max_lines 0, auto, flags 0
00 00 00 00  00 00 00 00  00 00 90 01  00 00 00 00   style: start 0, end 0, family 0,
                                                     weight 400, normal, reserved
00 00 80 41  00 00 80 3F  00 00 80 3F  00 00 80 3F   size 16, r 1, g 1, b 1
00 00 80 3F                                          a 1
4E 6F 74 6F  20 53 61 6E  73 00 00 00                "Noto Sans" NUL, padding
48 69 00 00                                          "Hi", padding
```

## Events (`UIEB`)

| Opcode | Event | Payload | Bytes |
| --- | --- | --- | --- |
| 1 | `frame_committed` | surface handle, `u64 frame_id`, `u64 predicted_present_ns` | 24 |
| 2 | `surface_resized` | surface handle, `f32 width, height` (logical px), `u32 scale_120` | 20 |
| 3 | `surface_close_requested` | surface handle | 8 |
| 4 | `timer_fired` | `u32 id`, `u32` reserved, `u64 deadline_ns`, `u64 now_ns` (both animation clock) | 24 |

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

## Validation and error handling

The core validates a whole command buffer before applying any op. The following make the
**entire buffer** rejected, with nothing applied:

- wrong magic or version;
- `total_bytes` that differs from the received size;
- an op size that is not a multiple of 4, is below 4, or runs past the end;
- an op count that does not match;
- a known opcode whose payload size is wrong.

If the buffer is well-framed, every op is applied in order. Individual ops are
**dropped** (counted in `ui_app_stats.commands_dropped` and logged in debug builds,
never undefined behavior) when:

- they address a stale, destroyed or wrong-kind handle;
- they carry a non-finite or out-of-range value;
- the opcode is unknown (it came from a newer writer).

Readers of event buffers skip unknown event types by their size. Bump the version only
for changes old readers cannot skip.

## Flow control and ordering

- Each flush is one buffer. All buffers received before a frame starts are applied in
  arrival order during that frame's apply phase.
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
