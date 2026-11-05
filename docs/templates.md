# Templates

A template is a static subtree defined once and instantiated with one op. TSX compiles the
static part of each subtree into one (its nodes, constant styles, inline styles, token
declarations, states and constant text), so mounting a component costs one op for its
structure, plus a handful for what is dynamic ([`ui.md`](ui.md), "How TSX compiles").

## Ops

| Opcode | Op | Payload |
| --- | --- | --- |
| 21 | `define_template` | `u32 id` (not 0), `u16 header_bytes` (12), `u16 node_count` (1–1024), `u16 ops_bytes`, `u16` reserved, then the template's ops |
| 22 | `instantiate_template` | `u32 template`, `u16 header_bytes` (24), `u16 count`, parent handle, `before` handle, then `count` node handles |
| 23 | `destroy_template` | `u32 id` |

**A template's ops** are ordinary framed ops:
- **Which:** `create_node`, `set_styles`, `set_props`, `set_token_overrides`, `set_text` (one
  op's worth of text) and `set_state`.
- **Local handles:** they name the template's nodes by local handles: slot = the node's index,
  generation 1 (`ui_template_node(i)` in C).
- **Creation:** each node is created once, before any op names it. Its parent is a node created
  before it, or the null handle for a root of the instance; there may be several roots.

**An instance is exactly its template's ops** with the instance's handles:
- local handle `i` becomes `nodes[i]`;
- a null parent becomes `parent`;
- each root, right after its `create_node`, gets `move_node(root, parent, before)` when `before`
  isn't null.

So the scene an instance makes is the scene those ops make: the same node checks (a handle must
name a free slot with a newer generation), the same drops, the same resolution.

## How the core does it

- **Expanded on receipt.** The core expands every instance into those ops when it receives a
  command buffer, before it prepares or applies anything (`src/core/template.c`). The scene,
  text preparation on the workers, and the checks downstream all see plain ops, so
  equivalence holds by construction; `test_template` checks it byte for byte anyway, on fixed
  and random templates, and under the full-recomputation check.
- **Receipt order.** Definitions follow receipt order, which is the order buffers apply in:
  - an instance uses the definition its id has at that point;
  - redefining an id replaces the definition for later instances, and existing instances, being
    plain nodes, don't change (hot reload relies on this);
  - `destroy_template` forgets one.
- **Dropped:**
  - a definition that breaks the rules above, keeping any earlier definition of its id;
  - an instance of an unknown template, or whose handle count isn't its template's node count;
  - an instance that would grow the expanded buffer past 64 MiB (an error is logged).

  Debug builds report each drop.
- **The scene ignores** `define_template` and `destroy_template`. An `instantiate_template`
  that reaches it unexpanded is dropped.
- **Older readers** skip all three as unknown ops (`cmd-template-*` fixtures). Since an instance
  carries nodes, a buffer meant for such a reader must not use templates.

## C API

```c
ui_cmd_writer *ops;
ui_cmd_writer_create(&ops);
ui_cmd_create_node(ops, ui_template_node(0), (ui_handle){0, 0}); // a root
ui_cmd_set_styles(ops, ui_template_node(0), (uint32_t[]){card}, 1);
ui_cmd_create_node(ops, ui_template_node(1), ui_template_node(0));
ui_cmd_set_text(ops, ui_template_node(1), &title);
ui_cmd_define_template(w, 1, 2, ops); // moves ops' ops into the definition

ui_cmd_instantiate_template(w, 1, parent, (ui_handle){0, 0}, (ui_handle[]){a, b}, 2);
```

## In @axe/ui

- **Defined lazily.** `@axe/ui` defines a template when it is first instantiated, once per theme
  in effect, since a theme's default styles are part of a template's style lists.
- **Released with the theme.** A theme's variant is destroyed once no instance and no
  `ThemeProvider` of that theme uses it; the variant without a theme stays.
- **Too large for one op:** a template whose ops don't fit one op (about 64 KiB) is written as
  plain ops with the instance's handles.
- **Hot reload:** a new version redefines the same ids, for every live variant. Instances that
  exist don't change; their component runs again. Variants no instance uses after an update are
  destroyed, and come back when one is instantiated ([`ui.md`](ui.md), "Hot reload").
