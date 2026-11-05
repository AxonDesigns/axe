-- The runtime host as Luau sees it (docs/script.md): timers on the stepped animation clock, the
-- order within a turn, uncaught errors reported while the app goes on. The expected logs are the
-- TypeScript test's (ts/packages/core/test/host.test.ts): the timers are the same code.

local axe = require("axe")
local host = require("axe/host")
local testing = require("axe/testing")
local test, step, next_turn, expect_eq, hooks, Hook =
  testing.test, testing.step, testing.next_turn, testing.expect_eq, testing.hooks, testing.Hook

local function surface() return axe.surfaces()[1] end

-- The hooks of one kind since `from`: a sanitizer's build is slow enough for any task to be a long
-- one.
local function new_hooks(from, kind)
  local out = {}
  for i, h in ipairs(hooks()) do
    if i > from and h.kind == kind then table.insert(out, h) end
  end
  return out
end

test("axe.time follows the stepped clock", function()
  local t0 = axe.time()
  step(250)
  expect_eq(axe.time(), t0 + 250)
end)

test("timers fire at their deadlines on the animation clock, in order", function()
  local log = {}
  local t0 = axe.time()
  local function at(s) table.insert(log, `{s}@{axe.time() - t0}`) end
  axe.set_timeout(at, 20, "b")
  axe.set_timeout(at, 10, "a")
  axe.set_timeout(at, 10, "a2")
  local n = 0
  local iv
  iv = axe.set_interval(function()
    at(`i{n}`)
    n += 1
    if n == 3 then axe.clear_timer(iv) end
  end, 15)
  axe.clear_timer(axe.set_timeout(at, 5, "never"))
  for _ = 1, 8 do step(10) end
  -- Each fires in the first turn at or after its deadline: steps of 10 ms.
  expect_eq(log, { "a@10", "a2@10", "i0@20", "b@20", "i1@30", "i2@50" })
end)

test("a timer set for 0 never runs in the turn that set it", function()
  local ran = false
  axe.set_timeout(function() ran = true end, 0)
  expect_eq(ran, false)
  step(10)
  step(10)
  expect_eq(ran, true)
end)

test("an interval doesn't drift, and a clock that jumps fires it once", function()
  local count, last = 0, 0
  local t0 = axe.time()
  local iv = axe.set_interval(function()
    count += 1
    last = axe.time() - t0
  end, 100)
  step(100)
  expect_eq(count, 1)
  step(1000) -- several periods at once
  expect_eq(count, 2)
  expect_eq(last, 1100)
  step(100) -- from the current time
  expect_eq(count, 3)
  axe.clear_timer(iv)
end)

test("events reach handlers in order, between turn start and turn end", function()
  local log = {}
  local off1 = axe.on_turn_start(function() table.insert(log, "start") end)
  local off2 = surface():on_frame(function() table.insert(log, "frame") end)
  local off3 = axe.on_turn_end(function() table.insert(log, "end") end)
  local n = axe.node(surface())
  n:set_color(1, 0, 0)
  step(16)
  step(16)
  off1()
  off2()
  off3()
  n:destroy()
  local frame_at = table.find(log, "frame")
  expect_eq(frame_at ~= nil, true)
  expect_eq(log[1], "end") -- the turn the test was in
  expect_eq(log[frame_at - 1], "start")
end)

test("an uncaught error is reported and the app goes on; what it wrote is kept", function()
  local before = #hooks()
  local ran = false
  local n = axe.node(surface())
  axe.set_timeout(function()
    n:set_rect(0, 0, 10, 10)
    error("boom in a timer")
  end, 5)
  axe.set_timeout(function() ran = true end, 5)
  step(5)
  step(5)
  expect_eq(ran, true)
  local list = new_hooks(before, Hook.exception)
  expect_eq(#list, 1)
  expect_eq(string.find(list[1].text, "boom in a timer", 1, true) ~= nil, true, "the message")
  expect_eq(string.find(list[1].text, "in a timeout", 1, true) ~= nil, true, "the context")
  expect_eq(string.find(list[1].text, "host.test.luau:", 1, true) ~= nil, true, "the stack")
  n:destroy()
end)

test("an error in an event handler is reported, and later handlers still get later events", function()
  local before = #hooks()
  local frames = 0
  local off1 = surface():on_frame(function() error({ code = 1 }) end) -- not a string
  local off2 = axe.on_turn_start(function() frames += 1 end)
  local n = axe.node(surface())
  n:set_color(0, 1, 0)
  step(16)
  off1()
  step(16)
  off2()
  n:destroy()
  local list = new_hooks(before, Hook.exception)
  expect_eq(#list >= 1, true)
  expect_eq(string.find(list[1].text, "event frame_committed", 1, true) ~= nil, true)
  expect_eq(frames >= 2, true)
end)

test("a task longer than the threshold is reported", function()
  local before = #hooks()
  axe.set_timeout(function()
    local stop = os.clock() + 0.06
    while os.clock() < stop do end
  end, 1)
  step(1)
  step(1)
  expect_eq(#new_hooks(before, Hook.long_task) >= 1, true)
end)

test("the memory limit is an error, not the end of the app", function()
  local ok, err = pcall(buffer.create, 128 * 1024 * 1024)
  expect_eq(ok, false)
  expect_eq(string.find(tostring(err), "memory", 1, true) ~= nil, true, tostring(err))
  next_turn()
end)

test("one command buffer per turn", function()
  -- The flush is the host's; a second submit in the same turn is refused.
  local b = buffer.create(16)
  local submitted = false
  local off = axe.on_turn_end(function()
    if submitted then return end
    submitted = true
    buffer.writeu32(b, 0, axe.ids.MAGIC_COMMANDS)
    buffer.writeu32(b, 8, 16)
    host.submit(b, 16)
    testing.expect_error(function() host.submit(b, 16) end, "one command buffer per turn")
  end)
  next_turn()
  off()
  expect_eq(submitted, true)
end)

test("print takes any values, however long", function()
  print(string.rep("x", 3000), nil, 1, { }, string.rep("y", 3000))
  print()
end)

test("set_timer's argument checks", function()
  testing.expect_error(function() host.set_timer("not a function", 1, false) end, "function")
  testing.expect_error(function() require("nothing") end, "no module named")
end)

testing.run()
