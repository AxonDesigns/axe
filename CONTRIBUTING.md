# Contributing

## Build and test

Clang 19 or newer (22 in CI), CMake, Ninja, Rust 1.93, and the system packages listed in
`.github/actions/setup/action.yml`. Clone with `--recursive`, then vendor the Rust crates
once with `tools/vendor-rust.sh`.

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

The presets are `debug`, `release`, `asan` (ASan + UBSan) and `fuzz` (libFuzzer + ASan +
UBSan). Tests run headless on lavapipe. Before committing, the debug, release and asan
suites must all pass.

## Fuzzing

Every target in `fuzz/` runs briefly in CI (the `fuzz` preset's tests) and for 10 minutes
each night (`.github/workflows/fuzz.yml`, which also takes a manual run with a time per
target).

**Before committing a change to code a fuzz target covers, run that target locally for a
few minutes** (3 to 5), and commit only once it finishes without a finding. The CI smoke
test's 10,000 runs are not enough to catch most bugs a change introduces.

| Target | Covers |
| --- | --- |
| `fuzz_utf8` | `src/unicode/utf8.c` |
| `fuzz_linebreak` | `src/unicode/linebreak.c`, `grapheme.c`, the UCD tables |
| `fuzz_bidi` | `src/unicode/bidi.c`, the UCD tables |
| `fuzz_set_text` | the `set_text`/`append_text` decoding in `src/core/scene.c`, and all of `src/text/` (fonts, shaping, paragraph layout, rasterizer, atlas, hit testing) and `src/unicode/` |
| `fuzz_commands` | `src/wire/` and `src/core/scene.c`: framing and validation of every op, applying them, springs, clipping and display-list building |

```sh
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p /tmp/corpus-set_text
build/fuzz/fuzz/fuzz_set_text -max_total_time=300 -max_len=8192 \
    /tmp/corpus-set_text fuzz/corpus/set_text
```

The first directory collects new inputs. Keep it out of the repository: the committed
corpus holds only seeds and regression inputs.

### When a fuzzer finds something

- **Locally**, libFuzzer writes the input to `crash-<hash>` (or `leak-`, `timeout-`,
  `oom-`) in the working directory. Rerun it alone with
  `build/fuzz/fuzz/fuzz_set_text crash-<hash>`.
- **Nightly**, the inputs are uploaded as the `fuzz-crashes-<target>` artifact of the
  failed run:

  ```sh
  gh run download <run-id> -n fuzz-crashes-set_text
  ```

Fix the bug, then commit the input as `fuzz/corpus/<target>/regress-<what>` so every CI
run replays it. Where a unit test can show the bug more directly, add one too.

## Wire format

Changes to the command or event buffers follow the rules in
[docs/command-buffer.md](docs/command-buffer.md#evolving-the-format): fields are only
appended, and their defaults are encoded as zero. Readers accept payloads at least as
large as an op's original size. Variable-length ops carry their header size. Record every
appended field in the op history there, and extend `tests/test_wire_compat.c` so buffers
from older and newer writers still apply. Bump `UI_WIRE_VERSION` only for a change the
rules can't express.

## Decisions

Choices the architecture leaves open go in [docs/decisions.md](docs/decisions.md), under
the milestone that made them.
