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
| `fuzz_set_text` | the `set_text`/`append_text` decoding in `src/core/text_op.c` and its application in `src/core/scene.c`, and all of `src/text/` (fonts, shaping, paragraph layout, rasterizer, atlas, glyph cache, hit testing) and `src/unicode/` |
| `fuzz_commands` | `src/wire/`, `src/style/`, `src/layout/` and `src/core/scene.c`, `preview.c`, `display_list.c`: framing and validation of every op, applying them, style resolution, layout, springs and transitions, clipping, layers and display-list building; every node's previewed text values must match what applying gives |

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

## Benchmarks

`bench/bench_layout` and `bench/bench_text` print the median and the best of several runs.
Run them from a release build:

```sh
cmake --build --preset release && build/release/bench/bench_layout && build/release/bench/bench_text
```

Times vary by 10–20% from run to run on a busy desktop. Compare best runs, and quote ranges
across a few runs.

### The benchmark guard

CI catches regressions with instruction counts. On the same machine they don't change from
run to run. Every CPU case is a `noinline` `bench_case_*` function. The `bench-callgrind`
job in `ci.yml` does the following:

1. It runs both benchmarks with `--callgrind`, which runs each CPU case once.
2. It counts each case's instructions with Callgrind.
3. It compares the counts with `bench/baselines.json`.

The job's log lists every case with its baseline, its measured count and the change, then
the verdict, so a failure can be understood from the log alone.

- **Regressions fail.** A case more than **2%** above its baseline fails the job, as does a
  baseline case that no longer exists. Counts are exact, so 2% is a real change in work,
  not noise.
- **Improvements pass, and are reported.** A case more than 2% below its baseline passes,
  with a note to update the baseline so the gain is locked in. Until then, a later change
  could give the gain back unnoticed.
- **The artifact holds the counts.** Every run uploads its counts as the `bench-callgrind`
  artifact (`callgrind-counts.json`), in `bench/baselines.json`'s format. It is stamped with
  the run: its ID, the commit and the ref.
- **Running only the guard:** start `ci` by hand (Actions, or
  `gh workflow run ci.yml --ref <branch> -f jobs=bench`).

### When a change costs or saves instructions

If a change legitimately costs instructions (a fix, a feature), or saves them, update the
baselines from CI:

1. Push the change. Its CI run fails (a cost) or reports an improvement (a saving).
2. Download that run's artifact:

   ```sh
   gh run download <run-id> -n bench-callgrind
   ```

3. Make it the baselines. `--update` refuses counts that weren't measured in CI:

   ```sh
   python3 -I tools/bench/callgrind.py --update callgrind-counts.json --reason "why the counts moved"
   ```

4. Commit `bench/baselines.json` alone. The commit message should give the reason, the
   cases that moved and by how much, and the run ID. For a cost, the reason must say why
   the work is worth it.

For a cost, the change and its baselines can sit in one pull request, as two commits.

### Local counts are for investigation only

```sh
python3 -I tools/bench/callgrind.py --build build/release   # needs valgrind
```

A local run prints the same table. The counts depend on the compiler and the libraries,
though: the development machine's differ from CI's by up to 6% on identical code, enough to
"fail" against CI's baselines. Use local runs to find where instructions go (compare two
local builds, or profile with `callgrind_annotate`), never as baselines. The
`axe-ci-ubuntu:24.04` Docker image reproduced CI's counts within 0.1% in M2.1. It is the
closest local stand-in, and still only for investigation.

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
