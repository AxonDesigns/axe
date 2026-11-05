-- A minimal imperative API over the wire, on the Luau runtime host (README.md): surfaces, nodes,
-- text, styles and tokens, springs, timers, measurement. No signals and no components.
--
--   local axe = require("axe")
--   axe.on_surface(function(surface)
--     local node = axe.node(surface)
--     node:set_rect(10, 10, 100, 100)
--   end)

local ids = require("axe/wire/ids")
local Writer = require("axe/wire/commands")
local events = require("axe/wire/events")
local host = require("axe/host")
local rt = require("axe/wire/runtime")
local styles = require("axe/wire/styles")

local axe = {}
axe.ids = ids
axe.args = host.args

-- ---- The turn (docs/script.md, "The turn") ------------------------------------------------------

-- Every op goes here; the host sends it to the core when the turn ends.
local commands = Writer.new(ids.MAGIC_COMMANDS)
axe.commands = commands

local listeners, turn_start, turn_end = {}, {}, {}

local function add(list, fn)
  table.insert(list, fn)
  return function()
    local i = table.find(list, fn)
    if i then table.remove(list, i) end
  end
end

local function each(list, ...)
  for _, fn in ipairs(table.clone(list)) do fn(...) end
end

local function on(kind, fn)
  listeners[kind] = listeners[kind] or {}
  return add(listeners[kind], fn)
end

host.set_dispatcher(function(b, kind, at)
  if kind == ids.event_type.clock then
    each(turn_start)
    return
  end
  local list = listeners[kind]
  if not list then return end
  local event = events.decode(b, kind, at)
  if event then each(list, event) end
end)

host.set_flush(function()
  each(turn_end)
  local b, size = commands:finish(ids.WIRE_VERSION)
  if b then host.submit(b, size) end
end)

-- Calls `fn` at the start of every turn, before its events; returns a function that removes it.
function axe.on_turn_start(fn) return add(turn_start, fn) end
-- Calls `fn` at the end of every turn, before its commands are sent.
function axe.on_turn_end(fn) return add(turn_end, fn) end
-- The animation clock at this turn, in ms since the app started.
function axe.time() return host.animation_time() / 1e6 end

-- ---- Timers (docs/script.md, "Timers") ------------------------------------------------------------

function axe.set_timeout(fn, ms, ...)
  local args = table.pack(...)
  return host.set_timer(function() fn(table.unpack(args, 1, args.n)) end, ms, false)
end

function axe.set_interval(fn, ms, ...)
  local args = table.pack(...)
  return host.set_timer(function() fn(table.unpack(args, 1, args.n)) end, ms, true)
end

function axe.clear_timer(id) host.clear_timer(id) end

-- ---- Surfaces --------------------------------------------------------------------------------

local Surface = {}
Surface.__index = Surface

local surfaces, surface_order, surface_listeners = {}, {}, {} -- by handle index, and in order

function Surface:on_resize(fn) return add(self.resize, fn) end
function Surface:on_frame(fn) return add(self.frame, fn) end
function Surface:on_close_requested(fn) return add(self.close, fn) end

local function surface_of(h)
  local s = surfaces[h.index]
  return s and s.handle.generation == h.generation and s or nil
end

on(ids.event_type.surface_resized, function(e)
  local s = surface_of(e.surface)
  local first = not s
  if not s then
    s = setmetatable({ handle = e.surface, width = 0, height = 0, scale = 1, resize = {}, frame = {}, close = {},
                       children = {} }, Surface)
    surfaces[e.surface.index] = s
    table.insert(surface_order, s)
  end
  s.width, s.height, s.scale = e.width, e.height, e.scale_120 / 120
  each(s.resize, s)
  if first then each(surface_listeners, s) end
end)
on(ids.event_type.frame_committed, function(e)
  local s = surface_of(e.surface)
  if s then each(s.frame, { frame_id = e.frame_id, present_time = e.predicted_present_ns / 1e6 }) end
end)
on(ids.event_type.surface_close_requested, function(e)
  local s = surface_of(e.surface)
  if s then
    s.close_requested = true
    each(s.close, s)
  end
end)

-- Calls `fn` with every surface, those that exist now first, then each new one, with its size
-- known. Returns a function that stops it.
function axe.on_surface(fn)
  for _, s in ipairs(surface_order) do fn(s) end
  return add(surface_listeners, fn)
end

function axe.surfaces() return table.clone(surface_order) end

-- ---- Values, tokens and styles (docs/styles.md) --------------------------------------------------

local kind = ids.value_kind

function axe.px(n) return { kind = kind.px, number = n } end
function axe.percent(n) return { kind = kind.percent, number = n } end
function axe.number(n) return { kind = kind.number, number = n } end
function axe.rgba(r, g, b, a) return { kind = kind.color, color = { r, g, b, a or 1 } } end
function axe.keyword(k) return { kind = kind.keyword, keyword = k } end
function axe.string(s) return { kind = kind.string, string = s } end
function axe.spring(stiffness, damping) return { kind = kind.spring, stiffness = stiffness, damping = damping } end
axe.auto = table.freeze({ kind = kind.auto })
axe.none = table.freeze({ kind = kind.none })
axe.inherit = table.freeze({ kind = kind.inherit })
axe.initial = table.freeze({ kind = kind.initial })
axe.unset = table.freeze({ kind = kind.unset })

local Token = {}
Token.__index = Token

local function value_of(v)
  return getmetatable(v) == Token and { kind = kind.token, token = v.id } or v
end

local function write_token(id, v)
  v = value_of(v)
  if not styles.token_value_valid(v) then error(`token {id}: a token can't hold that value`, 3) end
  commands:set_token(id, { property = 0, value = v })
end

local next_token = 1

-- A theme token: a value styles refer to by id. `value` is a value or another token; nil makes a
-- local token, with no global value, only what scopes declare.
function axe.token(value)
  local t = setmetatable({ id = next_token }, Token)
  next_token += 1
  if value ~= nil then write_token(t.id, value) end
  return t
end

function Token:set(value) write_token(self.id, value) end
function Token:unset() commands:set_token(self.id, { property = 0, value = axe.unset }) end

-- This token's value in a scope (a style's `tokens`, node:set_tokens).
function Token:with(value)
  value = value_of(value)
  if not styles.token_value_valid(value) then error(`token {self.id}: a token can't hold that value`, 2) end
  return { token = self.id, value = value }
end

-- Declarations go in id order: a table has no order of its own, unlike a JS object. So a logical
-- property, whose ids are the higher, always wins over its physical side in one block.
local function add_entries(out, decls, transition, override)
  local list = {}
  for name, v in pairs(decls) do
    local id = ids.style_prop[name]
    if not id then error(`unknown style property "{name}"`, 4) end
    table.insert(list, { id = id, name = name, value = v })
  end
  table.sort(list, function(a, b) return a.id < b.id end)
  for _, d in ipairs(list) do
    local property = transition and ids.transition_of(d.id) or d.id
    local value = value_of(d.value)
    if not styles.value_valid(property, value, override) then
      error(`{transition and "transition of " or ""}{d.name}: invalid value`, 4)
    end
    table.insert(out, { property = property, value = value })
  end
end

local function entries(decls, transitions, override)
  local out = {}
  if decls then add_entries(out, decls, false, override) end
  if transitions then add_entries(out, transitions, true, override) end
  return out
end

local function token_arrays(decls)
  local tokens, values = {}, {}
  for _, d in ipairs(decls) do
    table.insert(tokens, d.token)
    table.insert(values, { property = 0, value = d.value })
  end
  return tokens, values
end

local function block(states, b)
  local out = { states = states, entries = entries(b.decls, b.transitions, false) }
  if b.tokens and #b.tokens > 0 then out.tokens, out.token_values = token_arrays(b.tokens) end
  return out
end

local function define(id, desc)
  local blocks = { block(0, desc) }
  for _, b in ipairs(desc.states or {}) do
    if not ids.is_state(b.when) or b.when == 0 then error("a state block needs state bits (axe.ids.state)", 3) end
    table.insert(blocks, block(b.when, b))
  end
  commands:define_style(id, blocks)
end

local Style = {}
Style.__index = Style

local next_style = 1

-- An interned style, applied by id: { decls, transitions, tokens, states = { { when, ... } } }.
function axe.style(desc)
  local s = setmetatable({ id = next_style }, Style)
  next_style += 1
  define(s.id, desc)
  return s
end

function Style:redefine(desc) define(self.id, desc) end
function Style:destroy() commands:destroy_style(self.id) end

-- ---- Text (docs/text.md) ---------------------------------------------------------------------------

local MAX_TEXT = 16 * 1024 * 1024
local MAX_SPANS = 1024
local MAX_STRINGS = 64
local MAX_ELLIPSIS = 64
local FAMILY_DEFAULT = 0xffff
local field = ids.text_field

local function check(ok, what)
  if not ok then error(`set_text: invalid {what}`, 3) end
end

local function whole(v, lo, hi) return v >= lo and v <= hi and v == math.floor(v) end

local function intern(list, s)
  local i = table.find(list, s)
  if i then return i - 1 end
  table.insert(list, s)
  return #list - 1
end

-- A style as the wire has it: fields that come from the node are placeholders, flagged in
-- from_node. `language` is false for none.
local function wire_style(o, families, languages)
  local set, style = 0, { family = FAMILY_DEFAULT, weight = 400, slant = 0, language = 0, size = 16, color = { 0, 0, 0, 1 } }
  if o.families ~= nil then
    check(#o.families > 0, "families")
    set = bit32.bor(set, field.family)
    style.family = intern(families, table.concat(o.families, ", "))
  end
  if o.size ~= nil then
    check(o.size > 0 and o.size <= 4096, "size")
    set = bit32.bor(set, field.size)
    style.size = o.size
  end
  if o.weight ~= nil then
    check(whole(o.weight, 1, 1000), "weight")
    set = bit32.bor(set, field.weight)
    style.weight = o.weight
  end
  if o.slant ~= nil then
    check(whole(o.slant, 0, 2), "slant")
    set = bit32.bor(set, field.slant)
    style.slant = o.slant
  end
  if o.language ~= nil then
    set = bit32.bor(set, field.language)
    if o.language then
      check(string.find(o.language, "^[%w-]+$") ~= nil and #o.language <= 35, "language")
      style.language = intern(languages, o.language) + 1 -- + the families, below
    end
  end
  if o.color ~= nil then
    check(#o.color == 4, "color")
    for _, c in ipairs(o.color) do check(math.abs(c) < math.huge, "color") end
    set = bit32.bor(set, field.color)
    style.color = o.color
  end
  style.from_node = bit32.band(bit32.bnot(set), field.span_all)
  return style, set
end

local function string_region(families, languages)
  local parts = {}
  for _, list in ipairs({ families, languages }) do
    for _, s in ipairs(list) do
      check(not string.find(s, "\0", 1, true), "families or languages (NUL)")
      table.insert(parts, s .. "\0")
    end
  end
  check(#parts <= MAX_STRINGS, "families and languages (at most 64)")
  local region = table.concat(parts)
  return region .. string.rep("\0", rt.pad4(#region) - #region), #parts
end

-- Spans are { start, stop, ...a style } in UTF-8 byte offsets, which is what the wire has and how a
-- Luau string counts: `stop` is exclusive. Unlike JS, `ellipsis` nil is the default U+2026 and
-- false or "" is none; a span's or the text's `language` false is no language.
local function set_text(node, text, o)
  check(#text <= MAX_TEXT, "length (at most 16 MiB)")
  local families, languages = {}, {}
  local style, set = wire_style(o, families, languages)
  local line_height, align, direction = 0, 0, 0
  if o.line_height ~= nil then
    check(o.line_height >= 0 and o.line_height <= 100, "line_height")
    set = bit32.bor(set, field.line_height)
    line_height = o.line_height
  end
  if o.align ~= nil then
    check(whole(o.align, 0, 5), "align")
    set = bit32.bor(set, field.align)
    align = o.align
  end
  if o.direction ~= nil then
    check(whole(o.direction, 0, 2), "direction")
    set = bit32.bor(set, field.direction)
    direction = o.direction
  end
  local max_lines = o.max_lines or 0
  check(whole(max_lines, 0, 0xffff), "max_lines")

  local spans, previous = {}, 0
  check(#(o.spans or {}) <= MAX_SPANS, "spans (at most 1024)")
  for _, sp in ipairs(o.spans or {}) do
    check(sp.start < sp.stop and sp.start >= previous and sp.stop <= #text, "spans (sorted, not overlapping, within the text)")
    previous = sp.stop
    table.insert(spans, { start = sp.start, ["end"] = sp.stop, style = (wire_style(sp, families, languages)) })
  end
  -- Languages are numbered after the families in the string region.
  local function relabel(s)
    if s.language ~= 0 then s.language += #families end
  end
  relabel(style)
  for _, sp in ipairs(spans) do relabel(sp.style) end
  local region, string_count = string_region(families, languages)
  local ellipsis = o.ellipsis or ""
  check(#ellipsis <= MAX_ELLIPSIS, "ellipsis (at most 64 bytes)")
  local header = 72 + #spans * 36 + #region + rt.pad4(#ellipsis)
  check(header <= rt.MAX_PAYLOAD and #region <= 0xffff, "text options (too large for one op)")
  local room = (rt.MAX_PAYLOAD - header) // 4 * 4
  local chunk = math.min(#text, room)

  -- Text longer than one op continues in append_text ops; all of it applies, or none.
  local mark = commands:mark()
  local ok, err = pcall(function()
    commands:set_text({
      node = node,
      text_bytes = #text,
      string_count = string_count,
      max_lines = max_lines,
      direction = direction,
      flags = o.ellipsis ~= nil and 1 or 0, -- custom ellipsis (false: none)
      style = style,
      line_height = line_height,
      align = align,
      from_node = bit32.band(bit32.bnot(set), field.all),
    }, spans, region, ellipsis, string.sub(text, 1, chunk))
    local max = (rt.MAX_PAYLOAD - 12) // 4 * 4
    local sent = chunk
    while sent < #text do
      commands:append_text(node, string.sub(text, sent + 1, sent + max))
      sent += max
    end
  end)
  if not ok then
    commands:rollback(mark)
    error(err, 0)
  end
end

axe._set_text = set_text -- on a handle, for the tests

-- ---- Nodes -----------------------------------------------------------------------------------
-- Handles are allocated here, as the C allocator does: generations, a LIFO free list. Children
-- are mirrored, to free a subtree's handles when it is destroyed. Ownership is explicit: nothing
-- reports a node dropped without destroy(), since Luau can't run code when a table is collected.

local Node = {}
Node.__index = Node

local generation, free_slots, used = {}, {}, 0
local live = {} -- by slot

local function new_slot()
  local slot = table.remove(free_slots)
  if not slot then
    slot = used
    used += 1
  end
  local g = ((generation[slot] or 0) + 1) % 0x100000000
  generation[slot] = g == 0 and 1 or g
  return slot
end

local function alive(n) return live[n.slot] == n end

local function checked(n, what)
  if not alive(n) then error(`can't {what} node {n.handle.index}:{n.handle.generation}: it was destroyed`, 3) end
end

-- A new node, last among `parent`'s children (a surface's, for a root-level node).
function axe.node(parent, options)
  if getmetatable(parent) == Node then checked(parent, "create a child of") end
  local slot = new_slot()
  local n = setmetatable({ slot = slot, handle = table.freeze({ index = slot, generation = generation[slot] }),
                           parent = parent, children = {} }, Node)
  local ok, err = pcall(commands.create_node, commands, n.handle, parent.handle)
  if not ok then
    table.insert(free_slots, slot)
    error(err, 2)
  end
  live[slot] = n
  table.insert(parent.children, n)
  if options and options.styles then n:set_styles(table.unpack(options.styles)) end
  return n
end

-- False once the node (or an ancestor) was destroyed.
function Node:alive() return alive(self) end

local function unlink(n)
  table.remove(n.parent.children, table.find(n.parent.children, n))
end

local function release(n)
  live[n.slot] = nil
  table.insert(free_slots, n.slot)
  for _, c in ipairs(n.children) do release(c) end
end

-- Destroys the node and its subtree; their handles are free for reuse.
function Node:destroy()
  checked(self, "destroy")
  commands:destroy_node(self.handle)
  unlink(self)
  release(self)
end

-- Moves the node, with its subtree, under `parent`, before `before` (a child of `parent`; nil: last).
function Node:move_to(parent, before)
  checked(self, "move")
  if getmetatable(parent) == Node then
    checked(parent, "move a node under")
    local p = parent
    while getmetatable(p) == Node do
      if p == self then error("a node can't move under itself or its descendants", 2) end
      p = p.parent
    end
  end
  if before then
    checked(before, "move a node before")
    if before == self or before.parent ~= parent then error("`before` must be another child of `parent`", 2) end
  end
  commands:move_node(self.handle, parent.handle, before and before.handle or rt.NULL_HANDLE)
  unlink(self)
  self.parent = parent
  table.insert(parent.children, before and table.find(parent.children, before) or #parent.children + 1, self)
end

function Node:set_styles(...)
  checked(self, "style")
  local list = {}
  for _, s in ipairs({ ... }) do table.insert(list, s.id) end
  commands:set_styles(self.handle, list)
end

-- Per-node overrides, which win over every style; axe.unset removes one.
function Node:set_props(decls, transitions)
  checked(self, "set properties of")
  commands:set_props(self.handle, entries(decls, transitions, true))
end

-- The node's own token values (token:with(value)), for it and its subtree.
function Node:set_tokens(...)
  checked(self, "set tokens of")
  commands:set_token_overrides(self.handle, token_arrays({ ... }))
end

function Node:unset_tokens(...)
  checked(self, "unset tokens of")
  local list, values = {}, {}
  for _, t in ipairs({ ... }) do
    table.insert(list, t.id)
    table.insert(values, { property = 0, value = axe.unset })
  end
  commands:set_token_overrides(self.handle, list, values)
end

function Node:set_state(mask, value)
  checked(self, "set the state of")
  commands:set_state(self.handle, mask, value)
end

function Node:set_text(text, options)
  checked(self, "set the text of")
  set_text(self.handle, text, options or {})
end

function Node:set_rect(x, y, width, height)
  checked(self, "set the rect of")
  commands:set_rect(self.handle, x, y, width, height)
end

function Node:set_color(r, g, b, a)
  checked(self, "set the color of")
  commands:set_color(self.handle, r, g, b, a or 1)
end

function Node:set_corner_radius(radius)
  checked(self, "set the corner radius of")
  commands:set_corner_radius(self.handle, radius)
end

function Node:set_clip(clip)
  checked(self, "clip")
  commands:set_clip(self.handle, clip and 1 or 0)
end

-- Starts or retargets a spring on a property (axe.ids.property), keeping its velocity.
function Node:spring(property, target, options)
  checked(self, "animate")
  commands:start_spring(self.handle, property, options.stiffness, options.damping, target)
end

-- ---- Measurement -------------------------------------------------------------------------------

local next_request = 1
local requests = {}

on(ids.event_type.layout_measured, function(e)
  local r = requests[e.request_id]
  if not r then return end
  requests[e.request_id] = nil
  if e.result ~= 0 then
    r.fn(nil, `node {r.node.index}:{r.node.generation} no longer exists`)
    return
  end
  r.fn({
    layout_in_parent = e.layout_in_parent,
    layout_in_surface = e.layout_in_surface,
    presentation_in_parent = e.presentation_in_parent,
    presentation_in_surface = e.presentation_in_surface,
  })
end)

-- Calls fn(layout) once the frame that applies this request has laid the node out, or
-- fn(nil, message) if the node is gone by then. A callback, since Luau has no promises.
function Node:measure(fn)
  checked(self, "measure")
  local id = next_request
  next_request = id >= 0xffffffff and 1 or id + 1
  commands:request_layout(self.handle, id)
  requests[id] = { fn = fn, node = self.handle }
end

return axe
