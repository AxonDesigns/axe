// The leaf-text rule and other mistakes the transform reports.
import { Span, Text, View } from "@axe/ui";

export const a = <View>loose text</View>;
export const b = <View>{"a string"}</View>;
export const c = <View><Span>x</Span></View>;
export const d = <Text><View /></Text>;
export const e = <Text><Span>a <Span>nested</Span></Span></Text>;
export const f = <div />;
export const g = <View colour="red" />;
export const h = <Span>alone</Span>;
