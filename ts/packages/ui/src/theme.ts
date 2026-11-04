// Themes (docs/ui.md, "Themes"): token values for a subtree, and default styles for the built-in
// elements. ThemeProvider renders a View whose style declares the theme's tokens, and gives the
// View and Text elements below it their default styles (the caller's style comes after, so it
// wins).

import { Style, type TokenDecl } from "@axe/core";
import { context, use } from "./context.js";
import { View, type Element } from "./elements.js";
import { style, type StyleInput, type StyleProp } from "./style.js";

export interface ThemeDefaults {
  readonly View?: Style;
  readonly Text?: Style;
}

export interface Theme {
  /** The style ThemeProvider's View gets: the theme's token declarations, and its own style. */
  readonly style: Style;
  readonly components: ThemeDefaults;
}

export interface ThemeDesc {
  /** Token values for everything under the provider (token.with(value)). */
  readonly tokens?: readonly TokenDecl[];
  /** The provider's own View: a style, or style() input. */
  readonly style?: StyleInput;
  /** Default styles of the built-in elements. */
  readonly components?: ThemeDefaults;
}

/** A theme: defined once (its style is interned), provided with ThemeProvider. */
export function theme(desc: ThemeDesc): Theme {
  const input = { ...(desc.style ?? {}), tokens: [...(desc.style?.tokens ?? []), ...(desc.tokens ?? [])] };
  return { style: style(input as never), components: desc.components ?? {} };
}

const ThemeContext = context<Theme | null>(null, "Theme");

/** @internal The built-in elements' default styles here. */
export function themeDefaults(): ThemeDefaults | undefined {
  return use(ThemeContext)?.components;
}

/** The theme in effect here, or null. */
export function useTheme(): Theme | null {
  return use(ThemeContext);
}

export interface ThemeProviderProps {
  readonly theme: Theme;
  /** More style for the provider's View, after the theme's. */
  readonly style?: StyleProp;
  readonly children?: Element;
}

/** Applies `theme` to its children: a View with the theme's token declarations, and the
 * theme's default styles for the View and Text elements inside it. */
export function ThemeProvider(props: ThemeProviderProps): Element {
  const t = props.theme;
  return ThemeContext.Provider({
    value: t,
    get children() {
      return View({
        get style() {
          return [t.style, props.style];
        },
        get children() {
          return props.children;
        },
      });
    },
  });
}
