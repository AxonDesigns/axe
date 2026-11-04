// Context (docs/ui.md, "Context"): a value a component provides to everything mounted below it.
// use() reads it once, where it is called; a value that changes goes in a signal inside it.

import { contextOf, getOwner } from "./reactive.js";
import type { Element } from "./elements.js";

export interface ProviderProps<T> {
  readonly value: T;
  readonly children?: Element;
}

export interface Context<T> {
  readonly id: symbol;
  readonly defaultValue: T;
  /** Provides `value` to its children. */
  readonly Provider: (props: ProviderProps<T>) => Element;
}

/** A new context; use() returns `defaultValue` where no Provider gives it a value. */
export function context<T>(defaultValue: T, name = "context"): Context<T> {
  const id = Symbol(name);
  function Provider(props: ProviderProps<T>): Element {
    const o = getOwner();
    if (!o) throw new Error(`${name}.Provider rendered outside a component`);
    o.context = Object.create(contextOf(o)) as Record<symbol, unknown>;
    o.context[id] = props.value;
    return props.children;
  }
  Object.defineProperty(Provider, "name", { value: `${name}.Provider` });
  return { id, defaultValue, Provider };
}

/** The value of `ctx` here: the nearest Provider's above, or its default. */
export function use<T>(ctx: Context<T>): T {
  const c = contextOf(getOwner());
  return c && ctx.id in c ? (c[ctx.id] as T) : ctx.defaultValue;
}
