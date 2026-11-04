# Script apps

An app can be a JavaScript bundle instead of C logic. The bundle runs on the logic thread,
in a JS engine behind Axe's own engine interface, and talks to the core only through the
command and event buffers ([`command-buffer.md`](command-buffer.md)), exactly as C logic
does. TypeScript is compiled to such bundles ([`typescript.md`](typescript.md)).

```c
ui_script_desc script = {.bundle_path = "app.axbc"};
ui_app_desc desc = {.surface = {...}, .script = &script};
```

| Layer | Where | Per language? |
| --- | --- | --- |
| Engine interface | `src/script/engine.h`; QuickJS-ng in `src/script/quickjs/` | One adapter per engine |
| Runtime host | `src/script/host/`: turns, timers, errors, host API, bundles | One per runtime (shared by JS engines) |
| Wire package | `@axe/wire`, generated from `idl/wire.idl` | Generated per language |
| Language framework | `@axe/core` (later: signals, TSX) | Yes |

## Engine interface

`src/script/engine.h` is the contract between the host and an engine. Nothing outside
`src/script/<engine>/` names an engine type; QuickJS-ng is the only implementation. Another
engine (JavaScriptCore, V8), or another language with a comparable runtime (Luau, Dart),
implements the same header.

**Model.**

- **One engine, one thread.** An engine is one heap and one realm. It is used by one thread
  at a time: the one that created it, or the last to call `ui_js_attach_thread`, whose stack
  the overflow check uses.
- **Values are scoped handles.** A `ui_js_val` belongs to the innermost scope open when it was
  made, and stays valid until that scope closes; `ui_js_scope_escape` moves one outward. A
  native function's own values are released when it returns. `ui_js_ref` keeps a value across
  scopes: strong, or weak (an object only, empty once collected). This is the N-API model,
  which maps onto V8 handle scopes, JSC's protected values and QuickJS's reference counts.
- **Errors are pending exceptions.** A function returning a value returns the empty value
  (`id` 0) with an exception pending; a function returning bool returns false. There is never
  more than one, and nothing else throws. `ui_js_take_exception` clears it and describes it:
  name, message and stack, in UTF-8, and whether it is uncatchable (an interrupt, or out of
  memory).
- **Strings cross as UTF-8.** Invalid UTF-8 going in becomes U+FFFD.

**What an engine provides.**

| Group | Functions |
| --- | --- |
| Lifecycle | create (memory limit, stack limit, module hooks, rejection tracker), destroy, attach thread, engine name and version |
| Values | primitives, strings, objects, arrays, errors, the global object, typeof and tests, ToNumber, ToString, truthiness, strict equality |
| Objects | get, set, define (enumerable or not), indexes, length, call, construct |
| Natives | functions with a data pointer; objects carrying C data with a finalizer |
| Exceptions | throw a value or an error of a kind, take and describe the pending one |
| References | strong and weak |
| Modules | host modules (named exports from an object: `axe:host`), modules from source or bytecode with a resolve and load hook, top-level await (evaluation returns a promise), classic scripts |
| Bytecode | compile a module without evaluating it (imports are recorded, not loaded), evaluate bytecode |
| Microtasks | drain the queue (a job that throws is reported and the rest run), test, enqueue; promises from C and their state; a rejection tracker reporting unhandled rejections and later handlers |
| Binary data | ArrayBuffers over host memory (no copy), copies, the bytes of buffers and typed arrays, detaching |
| Diagnostics | an interrupt hook polled while JS runs (true throws an uncatchable error), memory limit, memory statistics, a forced collection |

`tests/test_js_engine.c` is the conformance test: an implementation passes it.

**Bytecode** is engine-specific, and an engine may not validate it: QuickJS doesn't. See
[Bundles](#bundles).

## The runtime host

`ui_app_desc.script` makes the logic actor host a script app instead of calling
`ui_logic_desc`. The host is created on the logic thread when the actor starts and lives
there: one engine per app, under both schedulers. The JS engine is QuickJS-ng v0.18.0, built
without `quickjs-libc`, so apps have no `std` or `os` module and no file system, process or
network access.

### The turn

Logic runs in turns, one per event buffer the core sends ([`command-buffer.md`](command-buffer.md#events-uieb)):

1. **Time.** The buffer's first event, `clock`, sets the turn's animation-clock time.
2. **The app, on the first turn.** The bundle is evaluated (its top level runs), followed
   by a microtask checkpoint. The first event buffer is sent at startup, so this happens
   immediately, and always before the app's first event.
3. **Each event, in order.** The host's own timer event runs the due JS timers; every
   other event, `clock` included (it marks the turn's start), is passed to the dispatcher
   `@axe/core` registered. A
   **microtask checkpoint follows each event and each timer callback**, as on the web, where
   each is a task: a Promise continuation runs before the next event's handler.
4. **Flush.** `@axe/core` runs the `app.onTurnEnd` handlers, then `@axe/ui`'s bindings and
   effects ([`ui.md`](ui.md), "Signals"), and finishes the turn's command buffer. The host
   appends its wire timer's re-arm, and the logic actor sends it: **one command buffer per
   turn**, possibly none. It applies atomically at a frame boundary, as every command buffer
   does.

Guarantees:

- Events reach JS in the order the core sent them, never interleaved with another turn.
- Everything a turn writes applies at once, no earlier than the next frame, and turns'
  buffers apply in order.
- While a turn runs, the core keeps drawing and animating: frames and springs never wait for
  JS (`tests/test_stall*`).

### Timers

`setTimeout` and `setInterval` run on the **animation clock**, not on wall time:

- A deadline is the **turn's time** plus the delay, in ms of the animation clock: every
  call within a turn measures from the same instant.
- All JS timers share one wire timer (`set_timer` id `0xFFFFFFFF`), armed for the earliest
  deadline; its `timer_fired` event runs every timer due by the turn's time, earliest deadline
  first and ties in creation order.
- A timer never fires in the turn that set it, even with a delay of 0: it runs in a later
  turn (at the earliest when its `set_timer` has applied and the core has stepped).
- An interval's next deadline is its previous deadline plus the interval, so it doesn't
  drift; if the clock jumped past more than one period, it fires once and goes on from the
  current time. Intervals are at least 1 ms.
- `clearTimeout` and `clearInterval` remove the timer, from its own callback too.

How this differs from the web:

- **A paused clock stops them.** Under a paused clock (tests, devtools) no timer fires until
  the clock is stepped, and stepping fires them exactly at their deadlines' turns, so the
  same steps give the same callbacks in the same order, under either scheduler. A scaled
  clock scales them.
- **No 4 ms clamping** of nested timeouts, and no throttling of background work: the core
  decides when the clock moves.
- **The handler must be a function**: strings aren't evaluated.
- **Timers keep running while nothing draws**: the core fires the wire timer even when every
  surface is idle.

### Time

| Clock | In JS | Use |
| --- | --- | --- |
| Wall time (monotonic) | `performance.now()`, ms since the app started, advancing during a turn; `performance.timeOrigin` | Measuring how long code takes |
| Animation clock | `app.time` (ms since the app started, the turn's time), the times on events | Anything visible: animation, scheduling, timers |

The animation clock is the one frames are drawn at: it can be paused, stepped and scaled.
Under the test runner `performance.now()` reads the animation clock too, so tests that
measure time stay deterministic.

Times on the wire are u64 nanoseconds. Past 2^53 ns (about 104 days of uptime) a JS number
can't hold every value, so `@axe/wire` reads times rounded, while counters that must be exact
(`frame_id`) throw if they ever pass 2^53 (docs/idl.md, `time`).

### Errors

A JS error never stops the core or the app:

- **Uncaught exceptions** (from the app's evaluation, an event handler, a timer, a
  microtask) are printed to stderr with their stack trace and reported through the hook API
  (`UI_TRACE_SCRIPT_EXCEPTION`, with the text in `ui_trace_event.text`). The turn goes on
  with the next event.
- **Unhandled rejections** still unhandled at the end of a microtask checkpoint are reported
  the same way (`UI_TRACE_SCRIPT_REJECTION`). A rejection handled before the checkpoint ends
  is not reported. A rejected app evaluation (top-level await) is reported as an exception.
- **The ops a failed task wrote are kept**: they reach the core with the rest of the turn.
  Encoders validate before writing, so a buffer never holds a partial op, and the scene
  always matches what JS believes it built (handles in particular). Until the core receives
  something else, it keeps showing what it has.
- **Long tasks:** a task (one event with its checkpoint, a timer callback, the app's
  evaluation, the flush) that takes longer than `ui_script_desc.long_task_ns` (50 ms by
  default) of wall time is logged and reported (`UI_TRACE_SCRIPT_LONG_TASK`, with
  `duration_ns`). Nothing interrupts it; the frames go on regardless.
- **Leaks** (debug builds): see [`typescript.md`](typescript.md#nodes).

### Host API

The globals, and nothing else of the web or of Node:

| Global | Notes |
| --- | --- |
| `console.log`, `info`, `debug`, `warn`, `error` | to the app's logger (stderr); objects are formatted, errors with their stack |
| `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval` | on the animation clock ([Timers](#timers)) |
| `queueMicrotask` | |
| `TextEncoder`, `TextDecoder` | UTF-8 only; `encodeInto`, `fatal`, `ignoreBOM`, streaming; a lone surrogate encodes as U+FFFD, invalid input decodes to U+FFFD per maximal subpart |
| `performance.now()`, `performance.timeOrigin` | wall time ([Time](#time)) |
| ECMAScript's own | `Promise`, `WeakRef`, `FinalizationRegistry`, typed arrays, `JSON`, and the rest of the language (no `Intl`) |

The globals are defined by a short prelude (`src/script/host/prelude.js`) over a few natives.
`@axe/core` reaches the host through the module `axe:host`, which isn't for apps: the
dispatcher and flush hooks, the command storage, the turn's animation time, the arguments,
leak reports, and `reportError`. `reportError` reports an error that `@axe/ui` caught with
no error boundary to take it, exactly as an uncaught exception is reported (stderr and
`UI_TRACE_SCRIPT_EXCEPTION`), without throwing.

**Commands and events cross without copies.** The event buffer is an ArrayBuffer over the
core's bytes for the length of the turn. The command buffer is written in place into host
memory exposed as an ArrayBuffer; at the flush the host takes that memory and hands it to the
core, and JS gets fresh storage. A buffer handed out is detached before its memory moves.

### Bundles

A bundle is one ES module: everything the app imports bundled in, except `axe:host`.

- **Source** (`.js`, from esbuild) is compiled when the app starts.
- **Bytecode** (`.axbc`, from `axe-jsc`) is compiled at build time, for a faster start.
  It is the engine's bytecode behind a 16-byte header:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u32 | magic `AXBC` |
| 4 | u16 | container version (1) |
| 6 | u16 | flags (0) |
| 8 | u32 | engine id: FNV-1a 32 of the engine's name, a space and its version (`quickjs-ng 0.18.0`) |
| 12 | u32 | bytecode size |
| 16 | | the bytecode |

The host tells the two apart by the magic. Bytecode for another engine, or another version
of it, is refused with a message to rebuild the bundle.

**Bytecode is trusted input.** QuickJS reads bytecode without validating it, so a crafted
file can corrupt memory and take over the process. Only load bytecode that this build's
`axe-jsc` produced from code you trust, shipped with the app; never load it from the network,
a user's documents, or anything else an attacker could write. The header's checks catch a
stale bundle, not a malicious one. Source bundles carry no such risk beyond running the
app's own code.
