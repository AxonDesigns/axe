// @axe/core/internal: what @axe/ui builds on. Not for apps: it may change in any release.
export {
  NO_SLOT,
  slotAlive,
  slotChildren,
  slotCreate,
  slotDestroy,
  slotHandle,
  slotMove,
  slotParent,
  slotSetEntries,
  slotSetProps,
  slotSetState,
  slotSetStyles,
  slotSetText,
  slotSetTokens,
  slotUnsetTokens,
  type SlotParent,
} from "./node.js";
export { entries, tokenArrays, type StyleEntry } from "./style.js";
export { animationTimeMs, commands, debug, onTurnEnd, onTurnStart, setBeforeFlush } from "./runtime.js";
export { sugarProp, sugarToken, sugarValue } from "./sugar.js";
export { Node } from "./node.js";
