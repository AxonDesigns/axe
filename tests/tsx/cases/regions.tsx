// Dynamic children keep their place: each region goes before the next static node or region.
import { Text, View } from "@axe/ui";

export function Mixed(props: { a: () => boolean; b: () => string }) {
  return (
    <View>
      {props.a() && <Text>a</Text>}
      {props.b()}
      <Text>middle</Text>
      <>
        <View />
        {props.a() ? <View /> : null}
      </>
    </View>
  );
}
