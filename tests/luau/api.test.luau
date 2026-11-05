-- The imperative API against the real core: what it writes reaches the scene, and measurement
-- brings it back.

local axe = require("axe")
local rt = require("axe/wire/runtime")
local testing = require("axe/testing")
local test, step, expect_eq, expect_error, c_buffer, hooks =
  testing.test, testing.step, testing.expect_eq, testing.expect_error, testing.c_buffer, testing.hooks

local ids = axe.ids
local function surface() return axe.surfaces()[1] end

-- Waits for a node's layout: the frame that applies the request answers it.
local function wait(node_measure)
  local answered, result, message = false, nil, nil
  node_measure(function(layout, err) answered, result, message = true, layout, err end)
  while not answered do step(16) end
  return result, message
end

local function measure(node)
  return wait(function(fn) node:measure(fn) end)
end

local function round(r) return { x = r.x, y = r.y, width = r.width, height = r.height } end

test("a surface has its size, and its nodes lay out", function()
  local s = surface()
  expect_eq({ s.width, s.height, s.scale }, { 320, 240, 1 })
  local n = axe.node(s)
  n:set_rect(10, 20, 100, 50)
  local layout = measure(n)
  expect_eq(round(layout.layout_in_surface), { x = 10, y = 20, width = 100, height = 50 })
  n:destroy()
end)

test("moving a node moves it under its new parent", function()
  local a, b = axe.node(surface()), axe.node(surface())
  a:set_rect(10, 10, 200, 200)
  b:set_rect(50, 50, 100, 100)
  local child = axe.node(a)
  child:set_rect(5, 5, 20, 20)
  expect_eq(round(measure(child).layout_in_surface), { x = 15, y = 15, width = 20, height = 20 })
  child:move_to(b)
  expect_eq(round(measure(child).layout_in_surface), { x = 55, y = 55, width = 20, height = 20 })
  a:destroy()
  b:destroy()
end)

test("move_to orders siblings and refuses what can't be", function()
  local p = axe.node(surface())
  local a, b, c = axe.node(p), axe.node(p), axe.node(p)
  c:move_to(p, a)
  expect_eq(p.children[1] == c and p.children[2] == a and p.children[3] == b, true)
  expect_error(function() p:move_to(a) end, "descendants")
  expect_error(function() a:move_to(p, a) end, "another child")
  expect_error(function() a:move_to(surface(), b) end, "another child")
  p:destroy()
end)

test("destroying a node frees its subtree's handles, and generations tell them apart", function()
  local p = axe.node(surface())
  local child = axe.node(p)
  local slot = child.handle.index
  local generation = child.handle.generation
  p:destroy()
  expect_eq(p:alive(), false)
  expect_eq(child:alive(), false)
  expect_error(function() child:set_color(1, 0, 0) end, "destroyed")
  local n, m = axe.node(surface()), axe.node(surface()) -- LIFO: the subtree's slots again
  local reused = n.handle.index == slot and n or m
  expect_eq(reused.handle.index, slot)
  expect_eq(reused.handle.generation, generation + 1)
  n:destroy()
  m:destroy()
end)

test("a node destroyed before its layout arrives reports it", function()
  local n = axe.node(surface())
  n:set_rect(0, 0, 10, 10)
  step(16)
  local layout, message = wait(function(fn)
    n:measure(fn)
    n:destroy()
  end)
  expect_eq(layout, nil)
  expect_eq(string.find(message, "no longer exists", 1, true) ~= nil, true)
  expect_error(function() n:measure(function() end) end, "destroyed")
end)

test("styles lay out through the core, by id", function()
  local row = axe.style({
    decls = {
      flex_direction = axe.keyword(ids.flex_direction.row),
      justify_content = axe.keyword(ids.flex_align["end"]),
      column_gap = axe.px(10),
    },
  })
  local item = axe.style({ decls = { width = axe.px(40), height = axe.px(30) } })
  local p = axe.node(surface(), { styles = { row } })
  p:set_rect(0, 0, 200, 100)
  local a, b = axe.node(p, { styles = { item } }), axe.node(p, { styles = { item } })
  expect_eq(round(measure(b).layout_in_surface), { x = 160, y = 0, width = 40, height = 30 })
  expect_eq(round(measure(a).layout_in_surface), { x = 110, y = 0, width = 40, height = 30 })
  item:redefine({ decls = { width = axe.px(50), height = axe.px(30) } })
  expect_eq(round(measure(a).layout_in_surface), { x = 90, y = 0, width = 50, height = 30 })
  p:destroy()
  row:destroy()
  item:destroy()
end)

test("overrides win over styles, and unset removes one", function()
  local item = axe.style({ decls = { width = axe.px(40), height = axe.px(30) } })
  local p = axe.node(surface())
  p:set_rect(0, 0, 200, 100)
  local n = axe.node(p, { styles = { item } })
  n:set_props({ width = axe.px(70) })
  expect_eq(measure(n).layout_in_surface.width, 70)
  n:set_props({ width = axe.unset })
  expect_eq(measure(n).layout_in_surface.width, 40)
  expect_error(function() n:set_props({ width = axe.px(-1) }) end, "width")
  expect_error(function() n:set_props({ not_a_property = axe.px(1) }) end, "not_a_property")
  expect_error(function() axe.style({ decls = { width = axe.unset } }) end, "width")
  p:destroy()
  item:destroy()
end)

test("tokens: global values, scopes, and a change reaching every node that reads one", function()
  local size = axe.token(axe.px(40))
  local item = axe.style({ decls = { width = size, height = axe.px(30) } })
  local p = axe.node(surface())
  p:set_rect(0, 0, 200, 100)
  local n = axe.node(p, { styles = { item } })
  expect_eq(measure(n).layout_in_surface.width, 40)
  size:set(axe.px(60))
  expect_eq(measure(n).layout_in_surface.width, 60)
  p:set_tokens(size:with(axe.px(25)))
  expect_eq(measure(n).layout_in_surface.width, 25)
  p:unset_tokens(size)
  expect_eq(measure(n).layout_in_surface.width, 60)
  local scoped = axe.style({ decls = {}, tokens = { size:with(axe.px(33)) } })
  p:set_styles(scoped)
  expect_eq(measure(n).layout_in_surface.width, 33)
  expect_error(function() axe.token(axe.spring(1, 1)) end, "can't hold")
  p:destroy()
  item:destroy()
  scoped:destroy()
end)

test("springs animate on the core's clock, and land", function()
  local n = axe.node(surface())
  n:set_rect(0, 0, 10, 10)
  step(16)
  n:spring(ids.property.rect, { 100, 0, 10, 10 }, { stiffness = 170, damping = 26 })
  local moving = measure(n)
  step(50)
  local later = measure(n)
  expect_eq(later.presentation_in_surface.x > moving.presentation_in_surface.x, true, "still moving")
  expect_eq(later.presentation_in_surface.x < 100, true, "not there yet")
  for _ = 1, 40 do step(100) end
  expect_eq(math.abs(measure(n).presentation_in_surface.x - 100) < 0.01, true, "landed")
  expect_error(function() n:spring(ids.property.rect, { 1, 2 }, { stiffness = 1, damping = 1 }) end, "count")
  n:destroy()
end)

-- Encodes into the turn's writer alone: what set_text writes, without the turn's other ops.
local function encode_alone(fn)
  local c = axe.commands
  local buf, size, ops = c.buf, c.size, c.op_count
  c.buf, c.size, c.op_count = buffer.create(4096), rt.HEADER_SIZE, 0
  fn()
  local out, n = c:finish(ids.WIRE_VERSION)
  local bytes = buffer.readstring(out, 0, n)
  c.buf, c.size, c.op_count = buf, size, ops
  return bytes
end

test("set_text encodes as ui_cmd_set_text", function()
  local a, b = { handle = { index = 1, generation = 1 } }, { handle = { index = 2, generation = 3 } }
  local n = axe.node(surface())
  local bytes = encode_alone(function()
    axe._set_text(a.handle, "h\u{e9}llo \u{1f600} world", {
      families = { "Noto Sans", "sans-serif" },
      size = 16,
      weight = 400,
      align = ids.text_align.center,
      line_height = 1.4,
      max_lines = 2,
      ellipsis = "...",
      spans = {
        { start = 1, stop = 3, weight = 700 },
        { start = 7, stop = 11, families = { "Noto Serif" }, language = "ro", size = 20 },
      },
    })
    axe._set_text(b.handle, "Open", {})
  end)
  expect_eq(bytes == buffer.tostring(c_buffer("text")), true)
  n:destroy()
end)

test("long text continues in append_text ops", function()
  local big = string.rep("abcdefghijklmnopqrstuvwxyz", 7693):sub(1, 200000)
  local bytes = encode_alone(function()
    axe._set_text({ index = 1, generation = 1 }, big, { ellipsis = false })
  end)
  expect_eq(bytes == buffer.tostring(c_buffer("long_text")), true)
end)

test("set_text checks its options and writes nothing when they're wrong", function()
  local n = axe.node(surface())
  local ops = axe.commands.op_count
  expect_error(function() n:set_text("abc", { spans = { { start = 2, stop = 1 } } }) end, "spans")
  expect_error(function() n:set_text("abc", { spans = { { start = 0, stop = 2 }, { start = 1, stop = 3 } } }) end, "spans")
  expect_error(function() n:set_text("abc", { spans = { { start = 0, stop = 4 } } }) end, "spans")
  expect_error(function() n:set_text("abc", { size = 0 }) end, "size")
  expect_error(function() n:set_text("abc", { language = "no spaces" }) end, "language")
  expect_error(function() n:set_text("abc", { families = {} }) end, "families")
  expect_error(function() n:set_text("abc", { families = { "\255" } }) end, "strings")
  expect_error(function() n:set_text("abc", { ellipsis = "a\0b" }) end, "ellipsis")
  expect_error(function() n:set_text("abc", { ellipsis = "\255" }) end, "ellipsis")
  expect_eq(axe.commands.op_count, ops)
  n:destroy()
end)

test("text that isn't UTF-8 reaches the core, which replaces what's wrong", function()
  local n = axe.node(surface(), { styles = { axe.style({ decls = { font_size = axe.px(20), width = axe.px(300) } }) } })
  n:set_text("a\255b\xed\xa0\x80c", { spans = { { start = 2, stop = 3, weight = 700 } } })
  step(16)
  step(16)
  expect_eq(measure(n).layout_in_surface.height > 15, true)
  n:destroy()
end)

test("text lays out with the node's styles", function()
  local n = axe.node(surface(), { styles = { axe.style({ decls = { font_size = axe.px(20), width = axe.px(300) } }) } })
  n:set_text("Hello from Luau")
  step(16)
  step(16)
  local layout = measure(n)
  expect_eq(layout.layout_in_surface.height > 15, true, "the text has height")
  n:destroy()
end)

test("nothing in this file was reported as an error", function()
  -- Long tasks can be: a sanitizer's build is slow enough to pass the threshold.
  for _, h in ipairs(hooks()) do expect_eq(h.kind == testing.Hook.long_task, true, h.text) end
end)

testing.run()
