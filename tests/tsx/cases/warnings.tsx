// A component destructuring its props reads them once: a warning pointing to the rule.
import { Text } from "@axe/ui";

export function Greeting({ name }: { name: string }) {
  return <Text>Hello {name}</Text>;
}

export const Arrow = ({ label }: { label: string }) => <Text>{label}</Text>;

// Not components: no warning.
export function helper({ x }: { x: number }) {
  return x * 2;
}
