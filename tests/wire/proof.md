# The wire migration proof (milestone 3a)

Before the hand-written encoders and decoders were removed, the code generated from
`idl/wire.idl` had to produce the same bytes for every buffer the tests, the examples and the
fuzzers make, and read every buffer the same way. This file records how that was shown.

## Encoding: every call, both encoders

A build with `-DUI_WIRE_PROOF=ON` compiled the pre-IDL encoders (`legacy_wire.c`, copied from
`src/wire/wire.c` before the change) next to the generated ones. Every `ui_cmd_*` call ran
both, and aborted unless the results and the bytes written so far were identical; so did
every `ui_cmd_writer_take`. A difference planted in one generated encoder (`set_state`'s
value + 1) aborted `test_style` at once, so the check was live.

- **The test suite:** 27 of 27 tests passed in the proof build (debug).
- **The examples,** headless, under both schedulers: `spring_rect` for 5 s, `layout_cards`
  at 0, 3, 7 and 13 s and at 5 s in right-to-left dark mode, and `text_paragraph`.
- **The event writers** are pure functions of their arguments; they were compared through
  the recorded event buffers below (every event the core sent during the suite and the
  examples, decoded by both readers).

## Decoding: canonical dumps

`proof_dump.c` dumps a buffer as text: the framing verdict, then every op with every field
(floats as bits), the variable data located (offset, length and hash), every style entry
decoded, and whether the core would drop the op. One dump uses the reads the scene, the
`set_text` decoder and the style decoder did before (copied verbatim); the other uses only
the generated codec. `wire_proof` compares them.

`UI_WIRE_RECORD=<dir>` made the proof build keep a copy of every command buffer written,
every buffer the scene validated (which includes the older- and newer-writer buffers
`test_wire_compat` builds by hand) and every event buffer the core sent.

| Input | Buffers | Different |
| --- | --- | --- |
| Recorded during the suite and the examples: 5,428 command buffers, 5,456 validated by the scene, 719 event buffers | 11,603 | 0 |
| `fuzz/corpus/commands`, read as `fuzz_commands` does (whole, split at the op its first byte names, and each part as an event buffer too) | 144 | 0 |
| `fuzz/corpus/set_text`, wrapped as `fuzz_set_text` does | 12 | 0 |
| `fuzz_wire_proof` (libFuzzer, ASan + UBSan) for 301 s, seeded with the corpora and 5,987 recorded buffers | 5,432,666 runs | 0 |

A difference planted in the generated `set_text` reader (`align` read one byte later) made
4 recorded buffers decode differently, so the comparison was live.

The fuzzer's first run found one input decoded differently. It was not a bug: it carried an
event of type 7, which the new reader knows as `clock` (new in this milestone) and rejects
as too short, while the old reader skipped it as unknown. The dumps now renumber type 7 to
an unknown type before comparing, so both readers skip it; the clock event has tests of its
own.

## What stays

The proof build, the copied pre-IDL code and the fuzzer were removed with the hand-written
codec. What stays is `fixtures/`: 575 buffers and, next to each, the dump the pre-IDL code
made of it (`.bin.txt`):

- 225 recorded buffers chosen to cover every op and event and the sizes they came in;
- 175 inputs from the fuzzer's corpus, each as a command buffer and as an event buffer.

`tests/test_wire_fixtures.c` dumps each with the generated codec and requires the same text,
so the generated code keeps decoding what the hand-written code decoded.
