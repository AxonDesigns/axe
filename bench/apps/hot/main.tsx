// The app bench_hot edits (frozen: results compare across versions). __N__ marks what each kind
// of edit changes.
import { app, mount } from "@axe/ui";
import { App } from "./panel";

app.onSurface((s) => mount(App, s));
