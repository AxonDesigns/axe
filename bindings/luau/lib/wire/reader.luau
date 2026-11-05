-- Reading an event buffer: the framing (docs/command-buffer.md, "Header", "Op framing") over the
-- generated decoders. Unknown events (from a newer core) are skipped; a known event shorter than
-- its original size is malformed. The host frames the buffer it dispatches; this is for code that
-- holds a whole one.

local events = require("axe/wire/events")
local ids = require("axe/wire/ids")
local rt = require("axe/wire/runtime")

local EventReader = {}
EventReader.__index = EventReader

function EventReader.new(b: buffer)
  local size = buffer.len(b)
  if size % 4 ~= 0 or size < rt.HEADER_SIZE then error("not an event buffer: bad size", 2) end
  if buffer.readu32(b, 0) ~= ids.MAGIC_EVENTS or buffer.readu16(b, 4) ~= ids.WIRE_VERSION or buffer.readu32(b, 8) ~= size then
    error("not an event buffer of this version: bad header", 2)
  end
  return setmetatable({ b = b, at = rt.HEADER_SIZE, stop = size, remaining = buffer.readu32(b, 12) }, EventReader)
end

-- The next event, or nil after the last.
function EventReader:next()
  while true do
    if self.at == self.stop then
      if self.remaining ~= 0 then error("malformed event buffer: fewer events than its header says", 2) end
      return nil
    end
    if self.stop - self.at < rt.OP_HEADER_SIZE or self.remaining == 0 then error("malformed event buffer: framing", 2) end
    local kind, size = buffer.readu16(self.b, self.at), buffer.readu16(self.b, self.at + 2)
    if size < rt.OP_HEADER_SIZE or size % 4 ~= 0 or size > self.stop - self.at then error("malformed event buffer: op size", 2) end
    local payload = self.at + rt.OP_HEADER_SIZE
    self.at += size
    self.remaining -= 1
    local original = events.original_size[kind]
    if original then -- nil: unknown, from a newer core
      if size - rt.OP_HEADER_SIZE < original then error(`malformed event buffer: event {kind} too short`, 2) end
      return events.decode(self.b, kind, payload)
    end
  end
end

return EventReader
