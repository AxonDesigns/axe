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
- Strings, pointers and native sizes never appear. Buffers are position-independent and
  portable to WASM.

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
