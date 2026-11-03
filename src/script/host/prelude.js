// The script host's prelude (docs/script.md, "Host API"): the web globals Axe provides, on
// top of a few natives the host passes in. Evaluated once as a script, before the app's
// bundle; the natives are reachable only through these closures afterwards.
(function (natives) {
  "use strict";
  const { print, setTimer, clearTimer, enqueue, encode, encodeInto, decode, now, timeOrigin } = natives;

  // ---- console --------------------------------------------------------------------------

  function inspect(v, depth, seen) {
    switch (typeof v) {
      case "string":
        return depth ? JSON.stringify(v) : v;
      case "number":
        return Object.is(v, -0) ? "-0" : String(v);
      case "bigint":
        return `${v}n`;
      case "symbol":
        return v.toString();
      case "undefined":
        return "undefined";
      case "boolean":
        return String(v);
      case "function":
        return `[Function: ${v.name || "(anonymous)"}]`;
    }
    if (v === null) return "null";
    if (v instanceof Error) {
      const head = `${v.name}: ${v.message}`;
      return typeof v.stack === "string" && v.stack ? `${head}\n${v.stack.replace(/\n$/, "")}` : head;
    }
    if (seen.has(v)) return "[Circular]";
    if (depth > 3) return Array.isArray(v) ? "[Array]" : "[Object]";
    seen.add(v);
    let out;
    try {
      if (Array.isArray(v)) {
        out = v.length ? `[ ${Array.from(v, (x) => inspect(x, depth + 1, seen)).join(", ")} ]` : "[]";
      } else if (ArrayBuffer.isView(v)) {
        const items = Array.from(v.length > 32 ? v.subarray(0, 32) : v, String).join(", ");
        out = `${v.constructor.name}(${v.length}) [ ${items}${v.length > 32 ? ", ..." : ""} ]`;
      } else if (v instanceof ArrayBuffer) {
        out = `ArrayBuffer { byteLength: ${v.byteLength} }`;
      } else if (v instanceof Map) {
        const items = Array.from(v, ([k, x]) => `${inspect(k, depth + 1, seen)} => ${inspect(x, depth + 1, seen)}`);
        out = `Map(${v.size}) { ${items.join(", ")} }`;
      } else if (v instanceof Set) {
        out = `Set(${v.size}) { ${Array.from(v, (x) => inspect(x, depth + 1, seen)).join(", ")} }`;
      } else if (v instanceof Promise) {
        out = "Promise {}";
      } else {
        const ctor = Object.getPrototypeOf(v)?.constructor;
        const name = ctor && ctor !== Object ? `${ctor.name} ` : "";
        const keys = Object.keys(v);
        const body = keys.map((k) => `${/^[A-Za-z_$][\w$]*$/.test(k) ? k : JSON.stringify(k)}: ${inspect(v[k], depth + 1, seen)}`);
        out = keys.length ? `${name}{ ${body.join(", ")} }` : `${name}{}`;
      }
    } finally {
      seen.delete(v);
    }
    return out;
  }

  const format = (args) => args.map((a) => inspect(a, 0, new Set())).join(" ");
  const level = (n) => (...args) => print(n, format(args));
  globalThis.console = Object.freeze({ debug: level(0), info: level(1), log: level(1), warn: level(2), error: level(3) });

  // ---- Timers and microtasks (the animation clock: docs/script.md, "Timers") --------------

  const timer = (repeat) =>
    function (handler, timeout = 0, ...args) {
      if (typeof handler !== "function") throw new TypeError(`${repeat ? "setInterval" : "setTimeout"}: the handler must be a function`);
      return setTimer(handler, timeout, args, repeat);
    };
  globalThis.setTimeout = timer(false);
  globalThis.setInterval = timer(true);
  globalThis.clearTimeout = (id) => clearTimer(id);
  globalThis.clearInterval = (id) => clearTimer(id);
  globalThis.queueMicrotask = (callback) => {
    if (typeof callback !== "function") throw new TypeError("queueMicrotask: the callback must be a function");
    enqueue(callback);
  };

  globalThis.performance = Object.freeze({ now, timeOrigin });

  // ---- TextEncoder and TextDecoder (UTF-8 only) ------------------------------------------

  const bytesOf = (input) => {
    if (input === undefined) return new Uint8Array(0);
    if (input instanceof ArrayBuffer) return new Uint8Array(input);
    if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    throw new TypeError("TextDecoder.decode: expected an ArrayBuffer or an ArrayBufferView");
  };

  // How many bytes at the start of `b` end on a character boundary: an incomplete sequence at
  // the end waits for the next chunk when streaming.
  const complete = (b) => {
    for (let back = 1; back <= Math.min(3, b.length); back++) {
      const c = b[b.length - back];
      if ((c & 0xc0) === 0x80) continue; // a continuation byte
      const need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
      return need > back ? b.length - back : b.length;
    }
    return b.length;
  };

  class TextEncoder {
    get encoding() {
      return "utf-8";
    }
    encode(input = "") {
      return new Uint8Array(encode(String(input)));
    }
    encodeInto(source, destination) {
      if (!(destination instanceof Uint8Array)) throw new TypeError("TextEncoder.encodeInto: the destination must be a Uint8Array");
      const [read, written] = encodeInto(String(source), destination);
      return { read, written };
    }
  }

  class TextDecoder {
    #fatal;
    #ignoreBOM;
    #pending = null;
    #started = false;

    constructor(label = "utf-8", options = {}) {
      const l = String(label).trim().toLowerCase();
      if (l !== "utf-8" && l !== "utf8" && l !== "unicode-1-1-utf-8") {
        throw new RangeError(`TextDecoder: only UTF-8 is supported, not "${label}"`);
      }
      this.#fatal = Boolean(options.fatal);
      this.#ignoreBOM = Boolean(options.ignoreBOM);
    }
    get encoding() {
      return "utf-8";
    }
    get fatal() {
      return this.#fatal;
    }
    get ignoreBOM() {
      return this.#ignoreBOM;
    }
    decode(input, options = {}) {
      let bytes = bytesOf(input);
      if (this.#pending) {
        const joined = new Uint8Array(this.#pending.length + bytes.length);
        joined.set(this.#pending);
        joined.set(bytes, this.#pending.length);
        bytes = joined;
        this.#pending = null;
      }
      const stream = Boolean(options.stream);
      const end = stream ? complete(bytes) : bytes.length;
      if (end < bytes.length) this.#pending = bytes.slice(end);
      let s = decode(bytes, end, this.#fatal);
      if (!this.#started && !this.#ignoreBOM && s.charCodeAt(0) === 0xfeff) s = s.slice(1);
      this.#started = stream && (this.#started || end > 0);
      return s;
    }
  }

  globalThis.TextEncoder = TextEncoder;
  globalThis.TextDecoder = TextDecoder;
});
