# The Luau binding

A second language behind the wire, the IDL and the runtime host interface. It exists to prove they
aren't JavaScript's, and CI keeps it running so they stay that way. It isn't a product: no signals,
no components, no hot reload. What it found is in [`docs/decisions.md`](../../docs/decisions.md),
milestone 3b.5.

| File | What |
| --- | --- |
| `host.c`, `axe_luau.h` | The runtime host, on the logic thread: turns, timers, errors, `require` |
| `lib/axe.luau` | The imperative API: surfaces, nodes, text, styles and tokens, springs, timers, measurement |
| `lib/wire/runtime.luau`, `reader.luau` | The hand-written part of the wire package |
| `lib/wire/gen/` | Encoders and decoders, generated from `idl/wire.idl` |

```lua
local axe = require("axe")

axe.on_surface(function(surface)
  local node = axe.node(surface)
  node:set_rect(40, 40, 200, 120)
  node:set_color(0.25, 0.7, 0.95)
  node:measure(function(layout) print(layout.layout_in_surface.width) end)
  axe.set_interval(function()
    node:spring(axe.ids.property.corner_radius, { 24 }, { stiffness = 170, damping = 18.25 })
  end, 2000)
end)
```

```sh
cmake --build --preset debug --target spring_rect_luau
build/debug/examples/spring_rect_luau                                    # a window
build/debug/examples/spring_rect_luau --headless --at 2.5 --out out.ppm
build/debug/examples/spring_rect_luau --headless --strict other.luau     # another app
```

`examples/spring_rect/main.luau` is the example. `axe_luau_app` (`CMakeLists.txt`) builds an app:
`axe-run` with the source embedded, so it takes the same options.

Where Luau differs from the TypeScript API, because the language does:

- **Measurement takes a callback:** `node:measure(function(layout, err) ... end)`. There are no
  promises.
- **Text offsets are UTF-8 bytes,** as a Luau string counts them and as the wire has them. A span is
  `{ start, stop }`, since `end` is a keyword.
- **A string can hold any bytes.** The encoders check what must be UTF-8. Text that isn't reaches the
  core, which replaces what's wrong.
- **Declarations go in property id order,** since a table has no order: a logical property wins
  over its physical side in one block.
- **`false` where JS has `null`:** `ellipsis = false`, `language = false`.
- **Nothing reports a leaked node:** Luau can't run code when a table is collected.
