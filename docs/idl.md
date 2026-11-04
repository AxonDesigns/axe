# The wire IDL

`idl/wire.idl` describes every op of the command buffer, every event, every record inside
them, the enums and bitmasks they use, and the style properties and the values each one
takes. `tools/idl/axe_idl.py` (stdlib Python, 3.10 or newer) generates from it everything
that reads or writes the wire, plus the tables of [`command-buffer.md`](command-buffer.md).
The format is line-based, one field per line, so a change to the wire shows up in review as
a few changed lines.

```sh
python3 -I tools/idl/axe_idl.py                    # regenerate every output
python3 -I tools/idl/axe_idl.py --check            # fail if an output is out of date
python3 -I tools/idl/axe_idl.py --compat-base REF  # also check the rules against REF's IDL
```

## Outputs

All are committed. CTest's `idl_up_to_date` regenerates them in memory and fails if a
committed file differs, and `idl_generator` runs the generator's own tests
(`tools/idl/test_axe_idl.py`).

| File | Contents |
| --- | --- |
| `include/ui/wire_ids.h` | The public enums (opcodes, events, properties, style properties, value kinds, keywords, states, text fields), with the IDL's doc strings as comments. `ui/wire.h` includes it. |
| `src/wire/gen/wire_gen.h` | Per op, event and record: a struct of its fixed part; `_read` (zero-fills the fields an older writer didn't send) and `_write`; size accessors (`_header_size`, `_size`, `_<tail>_stride`); a `_tail` struct with `_tail_of`, which locates the variable data; `_check` for ops whose fields have constraints; `_put` to append an op or event. |
| `src/wire/gen/wire_gen.c` | The `_check` and `_put` functions; `ui_wire_validate_commands` and the framing of every op and event; the style values' sizes, reader, writer and validators (`ui_wire_style_value_valid`, `ui_wire_style_token_value_valid`). |
| `ts/packages/wire/src/gen/*.ts` | The same for TypeScript: enums (`ids.ts`), `CommandWriter` (`commands.ts`), `decodeEvent` (`events.ts`), style values with per-property value types and validators (`styles.ts`). |
| `docs/command-buffer.md` | The command, `start_spring` property, event and op history tables, between `<!-- idl:NAME -->` markers. |

The C API's `ui_cmd_*` functions (`src/wire/wire.c`) stay hand-written: they check their
arguments and build what an op needs (the string table of `set_text`, the split into
`append_text` ops), then call the generated encoders. They contain no offsets.

## Lexical rules

- `#` starts a comment, except inside a string.
- Strings are in double quotes; `\"` and `\\` escape. A string at the end of a line is that
  line's documentation.
- Indentation is spaces only; tabs are an error. A line indented under a top-level line
  belongs to it.
- Names are `lower_snake_case`. The generator derives `camelCase` (TypeScript) and
  `UPPER_CASE` (C constants) from them.
- Every error names the file and line.

## Top-level statements

```
wire <version>
magic commands <u32> "<4 letters>"
magic events <u32> "<4 letters>"
changes
  <id>  "<description>"
enum <name> : <u8|u16|u32> [c <c_type> <C_PREFIX_>] ["doc"]
  <value> = <integer> [components <n>] [min <number>] ["doc"]
bitmask <name> : <u8|u16|u32> [c <c_type> <C_PREFIX_>] ["doc"]
  <bit> = <integer> ["doc"]
record <name> [original <bytes>] ["doc"]
  <fields>
  [tail
  <tail items>]
command <name> = <opcode> original <bytes> ["doc"]
  ...as a record
event <name> = <opcode> original <bytes> ["doc"]
  ...as a record
union <name> by <enum> c <c_type>
  <kind> <member> <type> [checks] [, <member> <type> [checks]]...
style_properties c <c_type> <C_PREFIX_> ["doc"]
  <id> <name> <values> [inherited] [transition] [layout] [shaping] ["doc"]
  section "doc"
token_values <values>
transition_values <values>
```

- **`magic`** gives the value and its letters; the generator checks that they agree in
  little-endian order.
- **`changes`** lists every change that appended a field, with an id (a commit, or
  `m1.1`) and a description. Fields name it with `since`.
- **`enum` and `bitmask`** with `c` are public: they go into `wire_ids.h` as
  `typedef enum <c_type>`, each value as `<C_PREFIX_><VALUE>`. Without `c` they are
  internal constants (`UI_WIRE_<ENUM>_<VALUE>`). `enum op` and `enum event_type` must list
  exactly the commands' and events' opcodes. `components` and `min` describe a property's
  values (`start_spring`).
- **`record`** is a layout used inside ops: nested in a fixed part, or repeated in a tail.
- **`command` and `event`** are a record with an opcode, and must state their original size
  (see [Evolution](#evolution-rules)).
- **`union`** describes a value whose layout depends on a kind field (style values). Each
  line gives the fields of one kind; a kind without a line has no fields.
- **`style_properties`** lists every style property: its id (1–32767; bit 15 marks a
  transition), its name, the values it takes, and flags. `section` puts a heading comment
  before the next property in the generated enum.

## Fields

```
<name> <type> [= <holds>] [checks] [since <change>] [default 0] ["doc"]
```

| Type | Bytes | Notes |
| --- | --- | --- |
| `u8`, `u16`, `u32`, `u64`, `f32` | 1, 2, 4, 8, 4 | little-endian; `u64 time` holds animation-clock nanoseconds (below) |
| `node`, `handle` | 8 | `u32 index`, `u32 generation`; `node` documents that it names a node |
| `<enum>`, `<bitmask>` | its storage | `<enum> : u8` stores it narrower than its declaration |
| `<record>` | its size | nested fixed record |
| `f32[4]` | 16 | fixed array of a scalar |
| `reserved` (a name) | — | a field named `reserved` of type u8, u16 or u32 is written 0 and never read |

Fields are packed in order with no implicit padding, and each must sit at a multiple of its
alignment: 2 for u16, 4 for u32, f32, u64, handles and records. A command's or event's fixed
part is a multiple of 4.

**`time`.** Times are nanoseconds of the animation clock since the app started. They fit a
u64 but pass 2^53 after about 104 days, so languages without 64-bit integers read them
rounded: TypeScript decodes a `time` to a `number` with rounding allowed, while a plain
`u64` (a counter or id that must be exact) throws past 2^53.

**What a field holds** (`=`): `count(<tail item>)` (the item's length in elements or bytes);
`header` (the fixed part's size, written by the encoder; 0 read as the original size);
`header(0)` (the same, but this version writes 0 and only the original fields while every
appended field is 0, as an older writer did, and the whole fixed part otherwise:
`_written_size` says which); `stride(<tail item>)` (the size of each record of a tail array;
0 read as the record's original size); `size` and `size/4` (a record's own size in bytes or
words).

**Checks** are the values the core accepts; anything else drops the op, the generated C
`_check` reports it, and the TypeScript encoder throws a RangeError before writing:

| Check | Applies to | Meaning |
| --- | --- | --- |
| `finite` | f32 | not NaN or infinite |
| `whole` | f32 | an integer value |
| `nonzero` | numbers | not 0 |
| `known` | enum, bitmask | one of the values; for a bitmask, no other bit set |
| `>= x`, `> x`, `<= x`, `< x` | numbers | bounds |
| `a..b`, `a<..b`, `a..<b`, `a<..`, `..b` | numbers | a range; `<` excludes that end |
| `components(<field>)` | a count | equals the component count of that enum field's value |
| `>= min(<field>)` | f32 tail arrays | every element at least that value's `min` |
| `utf8`, `no_nul` | bytes | well-formed UTF-8; no zero byte |

Constraints that depend on other state (handles, span order, `set_text` fields that come
from the node) are not checks: the scene applies them.

## Tails

After `tail`, the variable data in order:

```
<name> <scalar or record>[<count field>]     an array: count elements
<name> bytes[<count field>] [pad 4] [multiple 4] [utf8] [no_nul]
<name> <record>                              one record that carries its own size
<name> <union>(<kind field>)                 a union value (in a record that carries its size)
```

- An array of a record with `= size` (style blocks and entries) is walked record by record.
- An array of a fixed record uses the op's `stride` field when it has one.
- `pad 4` zero-pads to a multiple of 4; `multiple 4` requires the length to be one.
- **Parallel arrays** share a count: `count(<first>)` names the first, and the others give
  the same field in brackets (`tokens u32[n]`, then `token_values style_entry[n]`). The
  encoders check that their lengths agree.
- In TypeScript, the arrays an appended count counts are optional in a record (absent: none),
  so code written before them compiles unchanged.
- Without a `header` field, the variable data starts at the original size. Such an op can't
  grow its fixed part: its appended fields would have to follow the variable data.

## Style values

Each alternative of a property's values is a value kind with optional constraints:

```
px [range]  percent [range]  number [range] [whole]  color  auto  none  string [<= n]
keyword <enum>  keyword <enum>[<value> ...]  token  spring  transition-only
```

Every value property also takes `token` (any non-zero id), `inherit` and `initial`;
overrides also take `unset`. Floats must be finite. `keyword <enum>[...]` allows only the
listed values (below 32). `transition-only` marks a property that can't be declared, only
transitioned (`layout`). `token_values` lists what a theme token can hold (`keyword any`:
any keyword), `transition_values` what a transition takes.

## Evolution rules

The generator enforces [the format's evolution rules](command-buffer.md#evolving-the-format)
within one schema:

- A command or event states `original N`, its fixed part as first shipped, and the generator
  checks it against the fields: everything past `N` must be marked `since <change>`
  (`reserved` padding aside), and nothing may straddle it.
- A field with `since` either takes over reserved bytes inside the original size or is
  appended after it. Either way its default is zero bytes: `default` other than 0 is an
  error; an enum field needs a value 0; a size can't be appended, since 0 would not mean
  "what an older writer meant".
- An appended count is allowed when what it counts is new variable data: arrays after every
  item an original field sizes, in a record whose original fields include its header field.
  Then 0, "no elements", is what an older writer meant.
- `since` must name a change listed under `changes`.

Between versions, `--compat-base REF` compares the schema with the one at git `REF`
(the merge base, in review): no op, event, record, enum value or style property may be
removed or renumbered; every earlier field must keep its name, offset, type and size, except
reserved bytes that a `since` field took over; fields past an earlier size must have
`since` (reserved padding aside); original sizes and the order of variable data don't
change. A version bump (`wire`) lifts these checks.

### The older reader

The rules say what an older reader does with a newer buffer; `tools/wire/old_reader.py`
checks that it does. It builds `REF`'s reader (its generated codec and its
`tests/wire/wire_dump.c`, from `git archive`) and runs the command fixtures added or changed
since `REF` through it:

```sh
python3 -I tools/wire/old_reader.py --base REF [FIXTURE.bin ...]
```

Each fixture must be accepted (framing), every op the older reader knows must decode as this
version's dump (`<fixture>.bin.txt`) says, without the lines describing data that reader
doesn't know (`NEW_ONLY` in the script), and ops it doesn't know must be skipped as unknown.
CI's `idl` job runs it against the PR's base or the previous commit, so a wire change comes
with fixtures of what it adds (`test_wire_fixtures --write` dumps a new one).

## Using it for packages

Nothing in the grammar is specific to the framework's own ops: a package's IDL can declare
its own commands, events, records and enums, and the same generator will produce its C and
TypeScript codecs. What packages need on top (opcode ranges per package, generated bindings
for every language) comes with packaging.
