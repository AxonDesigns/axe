-- spring_rect in Luau: the C example (demo_logic.c) on the binding's API. One rounded rect springs
-- between four anchors every period, changing color; resizing the window springs it to its
-- anchor's new place.
--
--   spring_rect_luau [-- --period S --stall]
--   spring_rect_luau --headless --at S --out f.ppm
--
-- --stall blocks Luau for 500 ms after each switch: the spring keeps moving (frames never wait for
-- the language).

local axe = require("axe")
local property = axe.ids.property

local SPRING = { stiffness = 170, damping = 18.25 }
local anchors = { { 0.25, 0.3 }, { 0.75, 0.3 }, { 0.75, 0.7 }, { 0.25, 0.7 } }
local colors = {
  { 0.95, 0.45, 0.25, 1 }, -- orange
  { 0.25, 0.7, 0.95, 1 },  -- sky
  { 0.55, 0.85, 0.35, 1 }, -- green
  { 0.85, 0.35, 0.8, 1 },  -- magenta
}

local period_arg = table.find(axe.args, "--period")
local period_ms = period_arg and tonumber(axe.args[period_arg + 1]) * 1000 or 2000
local stall = table.find(axe.args, "--stall") ~= nil

local function rect(anchor, width, height)
  local c = anchors[anchor + 1]
  local w, h = width * 0.3, height * 0.3
  return c[1] * width - w * 0.5, c[2] * height - h * 0.5, w, h
end

axe.on_surface(function(surface)
  local anchor = 0
  local node = axe.node(surface)
  node:set_color(table.unpack(colors[1]))
  node:set_rect(rect(anchor, surface.width, surface.height))
  node:set_corner_radius(math.min(surface.width, surface.height) * 0.08)
  surface:on_resize(function(s)
    node:spring(property.rect, { rect(anchor, s.width, s.height) }, SPRING)
    node:set_corner_radius(math.min(s.width, s.height) * 0.08)
  end)
  local off
  off = surface:on_frame(function()
    off()
    axe.set_interval(function()
      anchor = (anchor + 1) % #anchors
      node:spring(property.rect, { rect(anchor, surface.width, surface.height) }, SPRING)
      node:spring(property.color, colors[anchor + 1], SPRING)
      if stall then
        print("stalling Luau for 500 ms")
        local stop = os.clock() + 0.5
        while os.clock() < stop do end
      end
    end, period_ms)
  end)
end)
