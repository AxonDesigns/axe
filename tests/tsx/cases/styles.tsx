// Styles: module-level ones and constant inline ones go in the template; a local one is applied
// once; a dynamic one is a binding. Tokens and states alike.
import { State, View, style, token, type Signal } from "@axe/ui";

const base = style({ padding: 4 });
const accent = token("#36f");
const S = { selected: style({ borderWidth: 2 }) };

export function Styled(props: { on: Signal<boolean>; width: number }) {
  const mine = style({ gap: 2 });
  return (
    <View style={[base, S.selected, { margin: 2 }]} tokens={[accent.with("#f63")]} state={State.hover}>
      <View style={mine} />
      <View style={props.on() ? base : { width: props.width }} state={props.on() ? State.selected : 0} />
    </View>
  );
}
