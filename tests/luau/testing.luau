-- The tests' side of axe-test-luau (runner.c): tests run in order, each in a coroutine that can
-- wait for the stepped clock, since Luau has no promises.

local axe = require("axe")
local runner = require("axe/test")

local testing = {}
local tests = {}

testing.Hook = { exception = 0, rejection = 1, long_task = 2, leak = 3 }
testing.hooks = runner.hooks
testing.c_buffer = runner.c_buffer
testing.c_events = runner.c_events

function testing.test(name: string, fn: () -> ())
  table.insert(tests, { name = name, fn = fn })
end

-- A timer at the target time guarantees a turn there: a stepped clock with nothing to draw sends
-- no events otherwise. Steps the paused clock by `ms` and returns in the turn at that time.
function testing.step(ms: number)
  local co = coroutine.running()
  runner.request_step(ms)
  axe.set_timeout(function() coroutine.resume(co) end, ms)
  coroutine.yield()
end

-- Returns in a later turn, without moving the clock.
function testing.next_turn()
  local co = coroutine.running()
  axe.set_timeout(function() coroutine.resume(co) end, 0)
  coroutine.yield()
end

local function show(v: any): string
  if type(v) ~= "table" then return tostring(v) end
  local keys = {}
  for k in pairs(v) do table.insert(keys, k) end
  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
  local parts = {}
  for _, k in ipairs(keys) do table.insert(parts, `{k}={show(v[k])}`) end
  return `\{{table.concat(parts, ", ")}}`
end

function testing.expect_eq(actual: any, expected: any, what: string?)
  local a, e = show(actual), show(expected)
  if a ~= e then error(`{what and what .. ": " or ""}expected {e}, got {a}`, 2) end
end

function testing.expect_error(fn: () -> (), part: string)
  local ok, err = pcall(fn)
  if ok then error(`expected an error mentioning "{part}"`, 2) end
  if not string.find(tostring(err), part, 1, true) then error(`expected "{part}" in: {err}`, 2) end
end

-- Runs the tests once the surface exists, and reports to the runner.
function testing.run()
  local started = false
  axe.on_surface(function()
    if started then return end
    started = true
    coroutine.wrap(function()
      local failed = 0
      for _, t in ipairs(tests) do
        local ok, err = xpcall(t.fn, debug.traceback)
        if not ok then
          failed += 1
          print(`FAIL {t.name}: {err}`)
        end
      end
      runner.done(failed, #tests)
    end)()
  end)
end

return testing
