-- Loads DirkSimple's game.lua (https://github.com/icculus/DirkSimple, zlib
-- licence, by Ryan C. Gordon) as DirkSimple does, with its host functions
-- stubbed, and writes its scenes and rows as JSON, every time evaluated as
-- DirkSimple evaluates it (milliseconds from the disc's content start; -1:
-- no seek):
--
--   lua tools/game/game_to_json.lua GAME_LUA > game.json

local path = arg[1]
if path == nil then
  io.stderr:write("usage: lua game_to_json.lua GAME_LUA\n")
  os.exit(2)
end

-- The host API game.lua expects; nothing here runs the game.
DirkSimple = {
  log = function() end,
  play_sound = function() end,
  start_clip = function() end,
  show_single_frame = function() end,
  draw_sprite = function() end,
  to_int = function(v) return math.tointeger(tonumber(v)) end,
  to_bool = function(v) return v == "true" end,
}

-- The file keeps its scenes, its scene manager (the rows) and its functions
-- local: made global, they can be read and the interrupts named.
local f = assert(io.open(path, "rb"))
local source = f:read("a")
f:close()
source = source:gsub("local scenes = nil", "scenes = nil")
source = source:gsub("local scene_manager = nil", "scene_manager = nil")
source = source:gsub("local function ", "function ")
assert(load(source, "=game.lua"))()

local function_names = {}
for name, value in pairs(_G) do
  if type(value) == "function" then function_names[value] = name end
end

local function json(value, indent)
  local t = type(value)
  if t == "nil" then return "null" end
  if t == "boolean" then return tostring(value) end
  if t == "number" then
    if math.type(value) == "integer" then return tostring(value) end
    return string.format("%.6f", value)
  end
  if t == "string" then return string.format("%q", value) end
  if t == "function" then return string.format("%q", function_names[value] or "?") end
  -- A table: an array when its keys are 1..n.
  local n = #value
  local keys = {}
  for k in pairs(value) do keys[#keys + 1] = k end
  local inner = indent .. "  "
  local parts = {}
  if n > 0 and #keys == n then
    for i = 1, n do parts[#parts + 1] = inner .. json(value[i], inner) end
    return "[\n" .. table.concat(parts, ",\n") .. "\n" .. indent .. "]"
  end
  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
  for _, k in ipairs(keys) do
    parts[#parts + 1] = inner .. string.format("%q", tostring(k)) .. ": " .. json(value[k], inner)
  end
  return "{\n" .. table.concat(parts, ",\n") .. "\n" .. indent .. "}"
end

local out = {
  title = DirkSimple.gametitle,
  rows = scene_manager.rows,
  scenes = {},
}
for name, scene in pairs(scenes) do
  out.scenes[name] = scene
end
io.write(json(out, ""), "\n")
