import { style } from "@axe/ui";

export const S = {
  page: style({ flexGrow: 1, padding: 8, backgroundColor: "#f4f4f6" }),
  panel: style({ flexDirection: "column", gap: 2, padding: __STYLE__, backgroundColor: "#fff" }),
  row: style({ justifyContent: "space-between", padding: 2 }),
  title: style({ fontSize: 16, textColor: "#123" }),
  label: style({ fontSize: 12, textColor: "#456" }),
};
