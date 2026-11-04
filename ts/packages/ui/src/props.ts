// Props helpers that keep getters intact (docs/ui.md, "Props"): reading a prop is what
// subscribes, so spreading or destructuring props would read them all once, too early.

type Merge<A, B> = Omit<A, keyof B> & B;

/** Props with defaults: later sources win where they define a prop (not undefined). Getters
 * stay getters: each read of the result reads the sources then. */
export function mergeProps<A extends object, B extends object>(a: A, b: B): Merge<A, B>;
export function mergeProps<A extends object, B extends object, C extends object>(a: A, b: B, c: C): Merge<Merge<A, B>, C>;
export function mergeProps(...sources: object[]): object {
  const out = {};
  const keys = new Set<PropertyKey>();
  for (const s of sources) for (const k of Reflect.ownKeys(s)) keys.add(k);
  for (const k of keys) {
    Object.defineProperty(out, k, {
      enumerable: true,
      get() {
        for (let i = sources.length - 1; i >= 0; i--) {
          const v = (sources[i] as Record<PropertyKey, unknown>)[k];
          if (v !== undefined) return v;
        }
        return undefined;
      },
    });
  }
  return out;
}

/** Splits props into those named in `keys` and the rest, both still reading the original
 * (getters stay getters). */
export function splitProps<P extends object, K extends keyof P>(props: P, keys: readonly K[]): [Pick<P, K>, Omit<P, K>] {
  const picked = {}, rest = {};
  const set = new Set<PropertyKey>(keys);
  for (const k of Reflect.ownKeys(props)) {
    Object.defineProperty(set.has(k) ? picked : rest, k, {
      enumerable: true,
      get: () => (props as Record<PropertyKey, unknown>)[k],
    });
  }
  return [picked as Pick<P, K>, rest as Omit<P, K>];
}
