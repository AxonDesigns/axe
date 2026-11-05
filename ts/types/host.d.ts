// What Axe's script host provides (docs/script.md, "Host API"): a small subset of the web's
// globals, and nothing else: no file system, network or process access. Apps are type-checked
// against this file instead of the DOM or Node's types.

interface Console {
  log(...data: unknown[]): void;
  info(...data: unknown[]): void;
  warn(...data: unknown[]): void;
  error(...data: unknown[]): void;
  debug(...data: unknown[]): void;
}
declare var console: Console;

/** Runs `handler` once after `timeout` ms of the animation clock (not wall time: a paused
 * clock fires nothing until it is stepped). Returns an id for clearTimeout. */
declare function setTimeout<A extends unknown[]>(handler: (...args: A) => void, timeout?: number, ...args: A): number;
declare function clearTimeout(id?: number): void;
/** Runs `handler` every `timeout` ms of the animation clock (at least 1 ms), each deadline
 * following the previous one, so it doesn't drift. */
declare function setInterval<A extends unknown[]>(handler: (...args: A) => void, timeout?: number, ...args: A): number;
declare function clearInterval(id?: number): void;
declare function queueMicrotask(callback: () => void): void;

interface TextEncoderEncodeIntoResult {
  read: number;
  written: number;
}
/** UTF-8 only, as on the web. */
declare class TextEncoder {
  readonly encoding: "utf-8";
  encode(input?: string): Uint8Array<ArrayBuffer>;
  encodeInto(source: string, destination: Uint8Array): TextEncoderEncodeIntoResult;
}
/** UTF-8 only: any other label throws a RangeError. */
declare class TextDecoder {
  constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });
  readonly encoding: "utf-8";
  readonly fatal: boolean;
  readonly ignoreBOM: boolean;
  decode(input?: ArrayBuffer | ArrayBufferView, options?: { stream?: boolean }): string;
}

interface Performance {
  /** Milliseconds of monotonic wall time since the app started, as on the web. Under the test
   * runner it reads the animation clock instead, so tests stay deterministic. */
  now(): number;
  /** When the app started, in ms since the Unix epoch. */
  readonly timeOrigin: number;
}
declare var performance: Performance;

/** Bindings between the host and @axe/core. Not for apps: their shape may change in any
 * release. */
declare module "axe:host" {
  /** Called by the host for each event of a turn, in order, with the event buffer, the event's
   * type, and its payload's byte offset and size. A microtask checkpoint follows each call. */
  export function setDispatcher(dispatch: (events: ArrayBuffer, type: number, at: number, size: number) => void): void;
  /** Called by the host at the end of every turn, after its last microtask checkpoint, to
   * finish the turn's command buffer. */
  export function setFlush(flush: () => void): void;
  /** The storage the turn's commands are written into. */
  export function commandBuffer(): ArrayBuffer;
  /** A larger storage holding the current one's bytes (the old one is detached). */
  export function growCommandBuffer(minBytes: number): ArrayBuffer;
  /** Hands the first `size` bytes (a finished buffer, header written) to the core and
   * returns fresh storage. */
  export function submitCommands(size: number): ArrayBuffer;
  /** The current turn's animation-clock time, in ns since the app started. */
  export function animationTime(): number;
  /** Reports, in debug builds, a subtree that became unreachable from JS without being
   * destroyed. */
  export function reportLeak(index: number, generation: number, nodes: number): void;
  /** Reports `error` as uncaught (stderr and the hook API's script exception), without
   * throwing: for errors @axe/ui catches that no error boundary takes. `context` says where
   * ("in an effect of Card"). */
  export function reportError(error: unknown, context: string): void;
  /** Hot reload (src/script/host/dev.h); these exist only when the app runs from modules. */
  export function hotSetHandler(fn: (changes: { file: string; deleted: boolean; error?: string }[]) => void): void;
  /** Stages a new version of each file and returns the names to import. */
  export function hotBegin(files: string[]): string[];
  export function hotCommit(): void;
  export function hotRevert(): void;
  export function hotImporters(file: string): string[];
  export function hotSourceMap(name: string): string | undefined;
  export function hotFiles(): string[];
  export const hotEntry: string;
  /** True in debug builds of the host. */
  export const debug: boolean;
  /** The app's arguments (axe-run's arguments after `--`). */
  export const args: readonly string[];
}

/** The test runner's bindings (tests/ts/runner.c); @axe/testing wraps them. */
declare module "axe:test" {
  export function requestStep(ms: number): void;
  export function done(failed: number, total: number): void;
  /** kind: 0 exception, 1 rejection, 2 long task, 3 leak. */
  export function hooks(): { kind: number; text: string }[];
  export function gc(): void;
  /** Writes the surface as a PPM after the next frame. */
  export function capture(path: string): void;
  /** Hot reload's tests: writes or (null) deletes files of the app, and posts their rebuild. */
  export function hotEdit(files: Record<string, string | null>): void;
  export function cBuffer(name: string): ArrayBuffer;
  export function cEvents(): ArrayBuffer;
}

/** tests/test_stall_js.c's bindings. */
declare module "axe:stall" {
  export function blocked(): void;
  export function released(): boolean;
}
