-- The Luau wire package against the C codec: the encoders write the bytes ui_cmd_* writes for the
-- same ops (the runner's reference cases, the ones TypeScript is held to), every op among them,
-- and the decoders read what the core's encoder writes.

local EventReader = require("axe/wire/reader")
local Writer = require("axe/wire/commands")
local ids = require("axe/wire/ids")
local rt = require("axe/wire/runtime")
local testing = require("axe/testing")
local test, expect_eq, expect_error, c_buffer, c_events =
  testing.test, testing.expect_eq, testing.expect_error, testing.c_buffer, testing.c_events

local function node(index, generation) return { index = index, generation = generation } end
local a, b = node(1, 1), node(2, 3)
local surface = node(bit32.lshift(1, 28), 1)
local NULL = rt.NULL_HANDLE
local prop, kind = ids.style_prop, ids.value_kind

local function new() return Writer.new(ids.MAGIC_COMMANDS) end
local function bytes_of(w) -- a string of the finished buffer
  local buf, size = w:finish(ids.WIRE_VERSION)
  return buffer.readstring(buf, 0, size)
end

local opcodes_seen = {}
local function same(w, name)
  local expected = c_buffer(name)
  expect_eq(bytes_of(w) == buffer.tostring(expected), true, name)
  local at = rt.HEADER_SIZE
  while at < buffer.len(expected) do
    opcodes_seen[buffer.readu16(expected, at)] = true
    at += buffer.readu16(expected, at + 2)
  end
end

local function px(n) return { kind = kind.px, number = n } end
local function decl(value) return { property = 0, value = value } end

test("fixed ops encode as the C writer does", function()
  local w = new()
  w:create_node(a, surface)
  w:create_node(b, a)
  w:set_rect(a, 1.5, -2, 300, 40.25)
  w:set_corner_radius(a, 8)
  w:set_color(a, 0.1, 0.2, 0.3, 1)
  w:set_clip(a, 1)
  w:move_node(b, surface, NULL)
  w:move_node(b, a, a)
  w:set_state(b, 3, 1)
  w:request_layout(b, 77)
  w:set_timer(9, 123456789012345)
  w:set_timer(9, 2 ^ 64) -- the cancel sentinel (UINT64_MAX), past what a number holds exactly
  w:load_font(4, "/fonts/Noto é.ttf")
  w:destroy_style(5)
  w:set_styles(b, { 3, 1, 2 })
  w:set_styles(b, {})
  w:start_spring(a, ids.property.rect, 170, 18.25, { 1, 2, 3, 4 })
  w:start_spring(a, ids.property.corner_radius, 90, 19, { 6 })
  w:destroy_node(b)
  same(w, "basic")
end)

test("styles, overrides and tokens encode as the C writer does", function()
  local w = new()
  local base = {
    { property = prop.width, value = px(208) },
    { property = prop.height, value = { kind = kind.percent, number = 50 } },
    { property = prop.flex_grow, value = { kind = kind.number, number = 1 } },
    { property = prop.margin_left, value = { kind = kind.auto } },
    { property = prop.max_width, value = { kind = kind.none } },
    { property = prop.background_color, value = { kind = kind.color, color = { 0.2, 0.45, 0.95, 1 } } },
    { property = prop.flex_direction, value = { kind = kind.keyword, keyword = ids.flex_direction.column } },
    { property = prop.text_color, value = { kind = kind.token, token = 3 } },
    { property = prop.font_family, value = { kind = kind.string, string = "Noto Sans, sans-serif" } },
    { property = prop.language, value = { kind = kind.string, string = "sr-Cyrl" } },
    { property = prop.direction, value = { kind = kind.inherit } },
    { property = prop.opacity, value = { kind = kind.initial } },
    { property = ids.transition_of(prop.background_color), value = { kind = kind.spring, stiffness = 90, damping = 19 } },
    { property = ids.transition_of(prop.layout), value = { kind = kind.spring, stiffness = 170, damping = 24 } },
  }
  local hover = { { property = prop.background_color, value = { kind = kind.token, token = 7 } } }
  w:define_style(1, { { states = 0, entries = base }, { states = bit32.bor(ids.state.hover, ids.state.pressed), entries = hover } })
  w:define_style(2, {})
  w:set_props(a, { { property = prop.border_width, value = px(2) }, { property = prop.width, value = { kind = kind.unset } } })
  w:set_token(3, decl({ kind = kind.color, color = { 1, 1, 1, 1 } }))
  w:set_token(4, decl({ kind = kind.token, token = 3 }))
  w:set_token(5, decl({ kind = kind.string, string = "x" }))
  w:set_token(3, decl({ kind = kind.unset }))
  same(w, "styles")
end)

test("token declarations encode as the C writer does", function()
  local w = new()
  local base = { { property = prop.background_color, value = { kind = kind.token, token = 3 } } }
  w:define_style(1, {
    {
      states = 0,
      entries = base,
      tokens = { 3, 4, 6 },
      token_values = {
        decl({ kind = kind.color, color = { 0.25, 0.5, 1, 1 } }),
        decl({ kind = kind.token, token = 5 }),
        decl({ kind = kind.string, string = "Noto Serif" }),
      },
    },
    { states = ids.state.hover, entries = {}, tokens = { 3 }, token_values = { decl(px(2)) } },
    { states = ids.state.pressed, entries = base }, -- no declarations: the 8-byte header
  })
  w:set_token_overrides(a, { 3, 9, 10 }, {
    decl({ kind = kind.color, color = { 1, 0, 0, 1 } }),
    decl({ kind = kind.unset }),
    decl({ kind = kind.keyword, keyword = 2 }),
  })
  w:set_token_overrides(a, {}, {})
  same(w, "scopes")
end)

local field = ids.text_field
local TEXT = "h\u{e9}llo \u{1f600} world"

local function default_style(from_node)
  return { family = 0xffff, weight = 400, slant = 0, from_node = from_node, language = 0, size = 16, color = { 0, 0, 0, 1 } }
end

test("text encodes as the C writer does", function()
  local w = new()
  local bold = default_style(bit32.band(bit32.bnot(field.weight), field.span_all))
  bold.weight = 700
  local serif = default_style(bit32.band(bit32.bnot(bit32.bor(field.family, field.language, field.size)), field.span_all))
  serif.family, serif.language, serif.size = 1, 3, 20
  local style = default_style(bit32.band(bit32.bnot(bit32.bor(field.family, field.size, field.weight)), field.span_all))
  style.family = 0
  local strings = "Noto Sans, sans-serif\0Noto Serif\0ro\0"
  w:set_text({
    node = a,
    text_bytes = #TEXT,
    string_count = 3,
    max_lines = 2,
    direction = 0,
    flags = 1,
    style = style,
    line_height = 1.4,
    align = ids.text_align.center,
    from_node = bit32.band(bit32.bnot(bit32.bor(field.family, field.size, field.weight, field.align, field.line_height)), field.all),
  }, { { start = 1, ["end"] = 3, style = bold }, { start = 7, ["end"] = 11, style = serif } },
  strings .. string.rep("\0", rt.pad4(#strings) - #strings), "...", TEXT)
  w:set_text({
    node = b, text_bytes = 4, string_count = 0, max_lines = 0, direction = 0, flags = 0,
    style = default_style(field.span_all), line_height = 0, align = 0, from_node = field.all,
  }, {}, "", "", "Open")
  same(w, "text")
end)

test("templates encode as the C writer does", function()
  local ops = new()
  local function local_node(i) return node(i, 1) end
  ops:create_node(local_node(0), NULL)
  ops:create_node(local_node(1), local_node(0))
  ops:set_state(local_node(1), ids.state.hover, ids.state.hover)
  local buf, size = ops:finish(ids.WIRE_VERSION)
  local w = new()
  w:define_template(7, 2, buffer.readstring(buf, rt.HEADER_SIZE, size - rt.HEADER_SIZE))
  w:instantiate_template(7, surface, NULL, { { node = node(3, 1) }, { node = node(4, 1) } })
  w:instantiate_template(7, a, b, { { node = node(5, 2) }, { node = node(6, 1) } })
  w:destroy_template(7)
  same(w, "templates")
end)

test("long text ops: append_text encodes as the C writer does", function()
  local big = string.rep("abcdefghijklmnopqrstuvwxyz", 7693):sub(1, 200000)
  local w = new()
  local axe_max = rt.MAX_PAYLOAD
  local header = 72
  local room = (axe_max - header) // 4 * 4
  w:set_text({
    node = a, text_bytes = #big, string_count = 0, max_lines = 0, direction = 0, flags = 1,
    style = default_style(field.span_all), line_height = 0, align = 0, from_node = field.all,
  }, {}, "", "", big:sub(1, room))
  local max = (axe_max - 12) // 4 * 4
  local sent = room
  while sent < #big do
    w:append_text(a, big:sub(sent + 1, sent + max))
    sent += max
  end
  same(w, "long_text")
end)

test("every op is among the reference cases", function()
  for name, opcode in pairs(ids.op) do expect_eq(opcodes_seen[opcode], true, name) end
end)

test("a method that errors writes nothing", function()
  local w = new()
  w:create_node(a, surface)
  expect_error(function() w:set_rect(a, 0, 0, -1, 1) end, "width")
  expect_error(function() w:set_color(a, 0 / 0, 0, 0, 1) end, "set_color")
  expect_error(function() w:start_spring(a, 77, 100, 10, { 1 }) end, "property")
  expect_error(function() w:start_spring(a, ids.property.corner_radius, 100, 10, { -1 }) end, "target")
  expect_error(function() w:start_spring(a, ids.property.rect, 100, 10, { 1, 2 }) end, "count")
  expect_error(function() w:set_clip(a, 2) end, "flags")
  expect_error(function() w:set_styles(a, { 1, 0 }) end, "ids")
  expect_error(function() w:load_font(1, "") end, "path_bytes")
  expect_error(function() w:load_font(1, "\255") end, "path")
  expect_error(function() w:load_font(1, "a\0b") end, "path")
  expect_error(function() w:set_state(a, -1, 0) end, "mask")
  local none = { kind = kind.none }
  expect_error(function() w:set_token_overrides(a, { 1, 0 }, { decl(none), decl(none) }) end, "tokens")
  expect_error(function() w:set_token_overrides(a, { 1 }, {}) end, "values")
  expect_error(function() w:define_style(1, { { states = 0, entries = {}, tokens = { 1 }, token_values = {} } }) end, "token_values")
  -- What no check catches fails halfway through the op, and the op was never committed: a handle
  -- that isn't one, and a string that isn't one.
  expect_error(function() w:set_rect({}, 0, 0, 1, 1) end, "")
  expect_error(function() w:set_token(1, decl({ kind = kind.string, string = 5 })) end, "")
  expect_eq(w.op_count, 1)
  local bytes = bytes_of(w)
  expect_eq(#bytes, 16 + 20)
  expect_eq(string.unpack("<I4", bytes, 1), ids.MAGIC_COMMANDS)
  expect_eq(string.unpack("<I4", bytes, 9), 36)
  expect_eq(string.unpack("<I4", bytes, 13), 1)
end)

test("the writer grows its buffer and starts over after finish", function()
  local w = new()
  for i = 1, 10000 do w:set_state(a, 1, i % 2) end
  expect_eq(#bytes_of(w), 16 + 10000 * 20)
  expect_eq((w:finish(ids.WIRE_VERSION)), nil)
  w:set_state(a, 1, 1)
  expect_eq(#bytes_of(w), 36)
end)

test("a mark rolls back what was written after it", function()
  local w = new()
  w:create_node(a, surface)
  local m = w:mark()
  w:create_node(b, a)
  w:rollback(m)
  expect_eq(w.op_count, 1)
  expect_eq(#bytes_of(w), 16 + 20)
end)

test("a Luau string can hold what a JS string can't: the checks catch it", function()
  expect_eq(rt.is_utf8("h\u{e9}llo \u{1f600}"), true)
  expect_eq(rt.is_utf8("a\0b"), true)          -- NUL is valid UTF-8: no_nul is its own check
  expect_eq(rt.is_utf8("\xed\xa0\x80"), false) -- a surrogate
  expect_eq(rt.is_utf8("\xc0\x80"), false)     -- overlong
  expect_eq(rt.is_utf8("\x80"), false)         -- a lone continuation byte
  expect_eq(rt.is_utf8("\xf4\x90\x80\x80"), false) -- past U+10FFFF
  expect_eq(rt.is_utf8("\xe2\x82"), false)     -- cut short
  local styles = require("axe/wire/styles")
  expect_eq(styles.value_valid(prop.font_family, { kind = kind.string, string = "Noto" }, false), true)
  expect_eq(styles.value_valid(prop.font_family, { kind = kind.string, string = "\xed\xa0\x80" }, false), false)
  expect_eq(styles.value_valid(prop.font_family, { kind = kind.string, string = "a\0b" }, false), false)
  expect_eq(styles.token_value_valid({ kind = kind.string, string = "\255" }), false)
end)

test("the IDL's names that are Luau keywords are still there", function()
  expect_eq(ids.flex_align["end"], 2)
  expect_eq(ids.text_align["end"], 1)
end)

test("events decode as the core encodes them", function()
  local r = EventReader.new(c_events())
  local all = {}
  while true do
    local e = r:next()
    if not e then break end
    table.insert(all, e)
  end
  expect_eq(#all, 7) -- the unknown event from a newer core is skipped
  local t = ids.event_type
  expect_eq(all[1], { type = t.clock, now_ns = 2 ^ 60 + 12345 })
  expect_eq(all[2], { type = t.frame_committed, surface = surface, frame_id = 42, predicted_present_ns = 16666667 })
  expect_eq(all[3], { type = t.surface_resized, surface = surface, width = 980, height = 640.5, scale_120 = 150 })
  expect_eq(all[4], { type = t.surface_close_requested, surface = surface })
  expect_eq(all[5], { type = t.timer_fired, id = 7, deadline_ns = 2500000000, now_ns = 2516666667 })
  expect_eq(all[6], { type = t.font_loaded, id = 3, result = 9 })
  local layout = all[7]
  expect_eq(layout.type, t.layout_measured)
  expect_eq(layout.request_id, 9)
  expect_eq(layout.result, 0)
  expect_eq(layout.node, node(5, 2))
  expect_eq(layout.layout_in_parent, { x = 0, y = 0.5, width = 100.25, height = 20 })
  expect_eq(layout.presentation_in_surface, { x = 3, y = 3.5, width = 100.25, height = 20 })
end)

test("every event is among the reference events", function()
  local seen = {}
  local r = EventReader.new(c_events())
  while true do
    local e = r:next()
    if not e then break end
    seen[e.type] = true
  end
  for name, opcode in pairs(ids.event_type) do expect_eq(seen[opcode], true, name) end
end)

test("malformed buffers are errors; a counter past 2^53 is too", function()
  local b2 = c_events()
  buffer.writeu8(b2, 0, 0)
  expect_error(function() EventReader.new(b2) end, "bad header")
  local b3 = buffer.create(40)
  buffer.copy(b3, 0, c_events(), 0, 40)
  buffer.writeu32(b3, 8, 40)
  local r = EventReader.new(b3)
  expect_error(function() while r:next() do end end, "malformed")
  local counter = buffer.create(8)
  buffer.writeu32(counter, 4, 0x200000)
  expect_error(function() rt.read_u64(counter, 0) end, "2^53")
  expect_error(function() rt.write_u64(counter, 0, 2 ^ 53) end, "safe")
  expect_eq(rt.read_time(counter, 0), 2 ^ 53)
end)

testing.run()
