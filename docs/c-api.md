# The C API

`include/ui/` is the public C API: the app, surfaces and scripts (`app.h`, `script.h`), the
command writer and event reader (`wire.h`), handles, hooks and results. This page is how it
changes without breaking its callers' source, and what callers must do for that to hold.
(The ABI, a package boundary, gets its versioning policy with packaging:
`docs/architecture.md`, "Performance budgets and API stability".)

## How the API changes

- **Structs only gain fields, at their end.** A field is never removed, reordered, renamed
  or retyped, and none is inserted before an existing one.
- **A new field's zero means what callers meant before it existed.** A zeroed field keeps
  the earlier behavior: `ui_style_block_desc.token_count` 0 is a block without token
  declarations, `ui_script_desc.long_task_ns` 0 the default threshold. Callers that never
  heard of a field therefore keep working when they rebuild.
- **Functions keep their signatures.** A new parameter comes as a new function or a new
  struct field, never by changing a declaration.
- **Enums only gain values.** Existing values keep their numbers (the wire's enums are
  generated, and `--compat-base` enforces it: `docs/idl.md`).

## What callers do

- **Initialize structs with designated initializers** and let the rest be zero:

  ```c
  ui_style_block_desc block = {.decls = decls, .count = 2};
  ui_app_desc desc = {.surface = {.kind = UI_SURFACE_HEADLESS, .width = 640, .height = 480}};
  ```

  A struct that gains a field then means the same thing, and compiles warning-free.
- **Positional initializers are unsupported** for these structs. One written before a field
  was added still compiles, and zero-fills the new field, but it relies on the order of
  fields the API doesn't promise to stop extending, and `-Wmissing-field-initializers`
  (in `-Wextra`) flags it, so a `-Werror` build breaks. Token scopes (milestone 3a.1) added
  two fields to `ui_style_block_desc`, and every positional `{states, decls, count}` in this
  repository had to become designated.
- **Zero a struct you fill field by field** (`= {0}` or `memset`), for the same reason.

## Fixed pairs

A few small structs mirror a fixed record and will never gain a field: a change would be a
new type. Positional initializers are supported for them, and the headers mark them `Fixed
pair`:

| Struct | Fields |
| --- | --- |
| `ui_handle` | `index`, `generation` |
| `ui_rect` | `x`, `y`, `width`, `height` |
| `ui_style_decl` | `property`, `value` |
| `ui_token_decl` | `token`, `value` |

`ui_value` is a tagged union: build it with the constructors (`ui_px`, `ui_rgba`,
`ui_token_ref`...), or designated initializers.

## Checked

`tests/c_api/designated.c` is a caller of every descriptor and config struct, written as
above. CTest compiles it with `-Wall -Wextra -Wpedantic -Werror -Wmissing-field-initializers`
as C11, C17 and C23 (`c_api_designated_*`); add each new public struct to it.
`tests/c_api/positional.c` holds a positional initializer of a grown struct, and
`c_api_positional_rejected` expects that compile to fail on the missing field.
