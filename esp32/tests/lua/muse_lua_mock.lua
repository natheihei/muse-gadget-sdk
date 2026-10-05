-- Copyright (c) Meta Platforms, Inc. and affiliates.
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- The board's app API (components/muse/muse_lua_api.md) on the host, strict
-- about what the guide allows, with a clock the test moves (_T.advance) and
-- touches it makes (_T.press, _T.swipe). Load before an app.

_T = { now = 0, widgets = {}, beeps = {}, logs = {}, timers = {}, next_id = 1 }

local FIELDS = {
  x = "number", y = "number", w = "number", h = "number", text = "string",
  size = "number", color = "number", bg = "number", align = "string",
  radius = "number", value = "number", width = "number", hidden = "boolean",
  on_press = "function",
}
local SIZES = { [14] = true, [16] = true, [20] = true, [28] = true, [48] = true }

local function check(fields, kind)
  for k, v in pairs(fields or {}) do
    local want = FIELDS[k]
    assert(want, ("%s: no field %q"):format(kind, tostring(k)))
    if k == "text" and type(v) == "number" then v = tostring(v) end
    assert(type(v) == want, ("%s.%s: %s, not %s"):format(kind, k, want, type(v)))
    if k == "on_press" then assert(kind == "button", "on_press is for buttons") end
    if k == "size" then assert(SIZES[v], "size " .. v .. " isn't a font size") end
    if k == "align" then assert(v == "left" or v == "center" or v == "right", "align " .. v) end
  end
end

local Widget = {}
Widget.__index = Widget
function Widget:set(fields)
  assert(not self.deleted, "set on a deleted widget")
  check(fields, self.kind)
  assert(fields.on_press == nil, "set can't change on_press")
  for k, v in pairs(fields) do self.props[k] = v end
end
function Widget:delete() self.deleted = true end

ui = { W = 480, H = 284 }
for _, kind in ipairs({ "label", "button", "rect", "bar", "arc" }) do
  ui[kind] = function(fields)
    check(fields, kind)
    local w = setmetatable({ kind = kind, props = {} }, Widget)
    for k, v in pairs(fields or {}) do w.props[k] = v end
    table.insert(_T.widgets, w)
    return w
  end
end
function ui.clear() for _, w in ipairs(_T.widgets) do w.deleted = true end end
function ui.on_tap(fn) _T.on_tap = fn end
function ui.on_swipe(fn) _T.on_swipe = fn end

app = {}
function app.title(t) _T.title = t end
function app.exit() _T.exited = true end
function app.on_exit(fn) _T.on_exit = fn end
function app.expose(t)
  assert(type(t) == "table", "app.expose{name = fn}")
  for k, v in pairs(t) do assert(type(k) == "string" and type(v) == "function", "app.expose{name = fn}") end
  _T.exposed = t
end

function log(...)
  local parts = {}
  for i = 1, select("#", ...) do parts[#parts + 1] = tostring(select(i, ...)) end
  table.insert(_T.logs, table.concat(parts, " "))
end
print = log

time = { ms = function() return _T.now end }

local function add_timer(ms, fn, every)
  assert(type(ms) == "number" and type(fn) == "function", "timer(ms, fn)")
  if every then ms = math.max(ms, 50) end
  local id = _T.next_id
  _T.next_id = id + 1
  _T.timers[id] = { due = _T.now + ms, every = every and ms, fn = fn }
  return id
end
timer = {
  setTimeout = function(ms, fn) return add_timer(ms, fn, false) end,
  setInterval = function(ms, fn) return add_timer(ms, fn, true) end,
  clear = function(id) local had = _T.timers[id] ~= nil; _T.timers[id] = nil; return had end,
}

sound = { beep = function(o) table.insert(_T.beeps, o or {}) end }

-- Runs every timer due by now + ms, in time order, the clock stepping to each.
function _T.advance(ms)
  local stop = _T.now + ms
  while true do
    local id, t = nil, nil
    for i, c in pairs(_T.timers) do
      if c.due <= stop and (not t or c.due < t.due or (c.due == t.due and i < id)) then id, t = i, c end
    end
    if not t then break end
    _T.now = t.due
    if t.every then t.due = t.due + t.every else _T.timers[id] = nil end
    t.fn()
  end
  _T.now = stop
end

function _T.press(text)
  for _, w in ipairs(_T.widgets) do
    if w.kind == "button" and not w.deleted and w.props.text == text and not w.props.hidden then
      return w.props.on_press()
    end
  end
  error("no button " .. text)
end

function _T.swipe(dir) return _T.on_swipe(dir) end

-- lua.call
function _T.call(name, ...) return assert(_T.exposed and _T.exposed[name], "not exposed: " .. name)(...) end

function _T.eq(got, want, what)
  if got ~= want then error(("%s: got %s, want %s"):format(what, tostring(got), tostring(want)), 2) end
end
