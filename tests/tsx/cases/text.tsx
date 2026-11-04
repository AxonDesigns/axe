// Paragraphs: constant ones go in the template; one binding for a dynamic one; a one-off call
// for a local one. Spans are parts of the paragraph.
import { Span, Text, View } from "@axe/ui";

const GREETING = "Hello";

export function Texts(props: { name: () => string; n: number }) {
  const local = props.n * 2;
  return (
    <View>
      <Text size={14} weight={600}>
        Static {GREETING} text,
        across lines.
      </Text>
      <Text>
        Hi, <Span weight={700}>{props.name()}</Span>!
      </Text>
      <Text>{local} items</Text>
      <Text />
    </View>
  );
}

// A module-level spread is constant props: the Text stays a template node.
const OPTIONS = { families: ["Noto Sans"], size: 12 } as const;
export const spread = <Text {...OPTIONS} weight={700}>constant</Text>;
