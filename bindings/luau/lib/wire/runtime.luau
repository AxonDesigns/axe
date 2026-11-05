-- The hand-written part of the wire package: buffer management, u64 and UTF-8 helpers, and the
-- event buffer's framing. The op encoders and event decoders built on it are generated (./gen,
-- from idl/wire.idl). Numbers are doubles, so u64s behave as in TypeScript (docs/idl.md, `time`).

local rt = {}

rt.NULL_HANDLE = table.freeze({ index = 0, generation = 0 })
rt.HEADER_SIZE = 16
rt.OP_HEADER_SIZE = 4
rt.MAX_OP_SIZE = 0xfffc
rt.MAX_PAYLOAD = rt.MAX_OP_SIZE - rt.OP_HEADER_SIZE
rt.MAX_BUFFER_SIZE = 64 * 1024 * 1024
rt.NONE = table.freeze({}) -- what an optional tail array of a record is when absent

local TWO32 = 4294967296

function rt.bad(op: string, field: string)
  error(`{op}: invalid {field}`, 3)
end

function rt.pad4(n: number): number
  return (n + 3) // 4 * 4
end

function rt.sum(list: { any }, size: (any) -> number): number
  local total = 0
  for _, x in ipairs(list) do total += size(x) end
  return total
end

-- ---- u64 -----------------------------------------------------------------------------------

-- An exact u64, which must be an integer below 2^53.
function rt.write_u64(b: buffer, at: number, v: number)
  if not (v >= 0 and v <= 2 ^ 53 - 1 and v == math.floor(v)) then error(`u64 {v} is not a safe non-negative integer`, 3) end
  buffer.writeu32(b, at, v % TWO32)
  buffer.writeu32(b, at + 4, v // TWO32)
end

-- A time in ns: rounded; values past 2^64 - 1 clamp to it.
function rt.write_time(b: buffer, at: number, v: number)
  if not (v >= 0) then error(`time {v} must be >= 0`, 3) end
  local r = math.round(v)
  if r >= TWO32 * TWO32 then
    buffer.writeu32(b, at, 0xffffffff)
    buffer.writeu32(b, at + 4, 0xffffffff)
    return
  end
  local hi = r // TWO32
  buffer.writeu32(b, at, r - hi * TWO32)
  buffer.writeu32(b, at + 4, hi)
end

-- An exact u64; a value past 2^53 is an error (counters never get there).
function rt.read_u64(b: buffer, at: number): number
  local hi = buffer.readu32(b, at + 4)
  if hi >= 0x200000 then error("u64 counter past 2^53", 2) end
  return hi * TWO32 + buffer.readu32(b, at)
end

-- A time in ns: exact below 2^53, rounded above.
function rt.read_time(b: buffer, at: number): number
  return buffer.readu32(b, at + 4) * TWO32 + buffer.readu32(b, at)
end

-- ---- Bytes -----------------------------------------------------------------------------------

-- True if the bytes are well-formed UTF-8: a Luau string holds any bytes, so what TypeScript gets
-- from its string type is checked here. utf8.len is strict (Unicode §3.9).
function rt.is_utf8(s: string): boolean
  return (utf8.len(s)) ~= nil
end

-- Writes `s` at `at`, zero-padded if `pad` so that `before` + its length is a multiple of 4;
-- returns the end.
function rt.write_bytes(b: buffer, at: number, s: string, pad: boolean, before: number?): number
  local n = #s
  if n > 0 then buffer.writestring(b, at, s) end
  at += n
  if pad then
    local extra = rt.pad4((before or 0) + n) - (before or 0) - n
    if extra > 0 then buffer.fill(b, at, 0, extra) end
    at += extra
  end
  return at
end

function rt.write_f32s(b: buffer, at: number, list: { number }): number
  for i, x in ipairs(list) do buffer.writef32(b, at + (i - 1) * 4, x) end
  return at + #list * 4
end

function rt.write_u32s(b: buffer, at: number, list: { number }): number
  for i, x in ipairs(list) do buffer.writeu32(b, at + (i - 1) * 4, x) end
  return at + #list * 4
end

-- ---- Writing ---------------------------------------------------------------------------------

-- Appends ops to one buffer and frames it (docs/command-buffer.md, "Header", "Op framing"). The
-- host has no storage to write into (a Luau buffer can't wrap host memory), so the writer owns
-- its buffer and the host copies it out at the flush.
local Writer = {}
Writer.__index = Writer
rt.Writer = Writer

function Writer.new(magic: number)
  return setmetatable({ magic = magic, buf = buffer.create(4096), size = rt.HEADER_SIZE, op_count = 0 }, Writer)
end

-- Makes room for an op of `payload` bytes (a multiple of 4) and returns its payload's offset. The
-- op exists only once commit() runs.
function Writer:op(opcode: number, payload: number): number
  local op_size = rt.OP_HEADER_SIZE + payload
  if op_size > rt.MAX_OP_SIZE then error(`op {opcode}: {payload} bytes don't fit one op`, 3) end
  local start, stop = self.size, self.size + op_size
  if stop > rt.MAX_BUFFER_SIZE then error("command buffer past 64 MiB", 3) end
  local capacity = buffer.len(self.buf)
  if stop > capacity then
    while capacity < stop do capacity *= 2 end
    local grown = buffer.create(math.min(capacity, rt.MAX_BUFFER_SIZE))
    buffer.copy(grown, 0, self.buf, 0, start)
    self.buf = grown
  end
  buffer.writeu16(self.buf, start, opcode)
  buffer.writeu16(self.buf, start + 2, op_size)
  return start + rt.OP_HEADER_SIZE
end

function Writer:commit(at: number, payload: number)
  self.size = at + payload
  self.op_count += 1
end

-- A position to roll back to: ops written after it are discarded.
function Writer:mark(): { size: number, ops: number }
  return { size = self.size, ops = self.op_count }
end

function Writer:rollback(m: { size: number, ops: number })
  self.size = m.size
  self.op_count = m.ops
end

-- Writes the header and returns the buffer and its size, valid until the next write, or nil if no
-- op was written. The writer starts a new buffer.
function Writer:finish(version: number): (buffer?, number)
  if self.op_count == 0 then return nil, 0 end
  local b, size = self.buf, self.size
  buffer.writeu32(b, 0, self.magic)
  buffer.writeu32(b, 4, version) -- version, then flags 0
  buffer.writeu32(b, 8, size)
  buffer.writeu32(b, 12, self.op_count)
  self.size = rt.HEADER_SIZE
  self.op_count = 0
  return b, size
end

return rt
