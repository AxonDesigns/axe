// token(value): Token.create, with value sugar (a number is px, "#36f" a color, "50%" a
// percentage, "auto", "none").

import { Token } from "@axe/core";

/** A theme token holding `value` (sugar or a value object), or referring to another token of its
 * kind. Token.local() makes one with no global value. */
export const token: typeof Token.create = Token.create.bind(Token) as typeof Token.create;
