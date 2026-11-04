// @axe/ui: apps with fine-grained signals (docs/ui.md). Components run once; only what reads a
// signal runs again when it changes, at the end of the turn, so a turn writes one atomic command
// buffer.
export {
  computed,
  effect,
  getOwner,
  onCleanup,
  root,
  runWithOwner,
  signal,
  untrack,
  type ComputedOptions,
  type Owner,
  type Signal,
  type SignalOptions,
} from "./reactive.js";
export {
  Span,
  Text,
  View,
  createComponent,
  type Component,
  type Element,
  type NodeProps,
  type SpanProps,
  type TextContent,
  type TextProps,
  type ViewProps,
} from "./elements.js";
export {
  Dynamic,
  ErrorBoundary,
  For,
  Index,
  Match,
  Show,
  Switch,
  type Branch,
  type DynamicProps,
  type ErrorBoundaryProps,
  type ForProps,
  type IndexProps,
  type MatchProps,
  type ShowProps,
  type SwitchProps,
} from "./flow.js";
export { context, use, type Context, type ProviderProps } from "./context.js";
export { mergeProps, splitProps } from "./props.js";
export { mount, unmount, type Mounted } from "./mount.js";
export {
  restyle,
  style,
  type BlockInput,
  type Box,
  type DeclsInput,
  type InlineStyle,
  type PropInput,
  type StyleInput,
  type StyleProp,
  type TransitionsInput,
} from "./style.js";
export { ThemeProvider, theme, useTheme, type Theme, type ThemeDefaults, type ThemeDesc, type ThemeProviderProps } from "./theme.js";
export { token } from "./token.js";
// Everything of @axe/core too, so an app imports from one place.
export * from "@axe/core";
