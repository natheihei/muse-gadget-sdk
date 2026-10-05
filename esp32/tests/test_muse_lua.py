# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The guide Muse gets for Lua apps (components/muse/muse_lua_api.md): its
example runs on the vendored Lua 5.4 with the board's libraries, against a mock
of the app API (tests/lua/muse_lua_mock.lua), and behaves as the recipe demo
needs: Next starts a step's timer, Back resets it, Next again moves on."""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LUA_SRC = ROOT / "components" / "lua" / "src"
API = ROOT / "components" / "muse" / "muse_lua_api.md"
MOCK = ROOT / "tests" / "lua" / "muse_lua_mock.lua"


def example() -> str:
    """The indented block under "## Example", as Lua."""
    text = API.read_text()
    block = text[text.index("## Example"):].split("\n", 1)[1]
    return "\n".join(l[4:] for l in block.splitlines() if l.startswith("    ") or not l.strip()) + "\n"


class MuseLuaExample(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.lua = Path(cls.tmp.name) / "lua_host"
        cc = shlex.split(os.environ.get("CC", "cc"))
        sources = sorted(str(p) for p in LUA_SRC.glob("*.c"))
        subprocess.run(cc + ["-O1", "-w", "-DLUAI_MAXCCALLS=80", f"-I{LUA_SRC}", "-o", str(cls.lua),
                             str(ROOT / "tests" / "lua_host_harness.c")] + sources + ["-lm"], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_app(self, driver: str) -> subprocess.CompletedProcess:
        d = Path(self.tmp.name)
        (d / "app.lua").write_text(example())
        (d / "driver.lua").write_text(driver)
        return subprocess.run([str(self.lua), str(MOCK), str(d / "app.lua"), str(d / "driver.lua")],
                              capture_output=True, text=True)

    def assert_runs(self, driver: str):
        r = self.run_app(driver)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_fits_in_a_link_invoke(self):
        self.assertLess(len(example().encode()), 8 * 1024)

    def test_steps_and_timer(self):
        self.assert_runs("""
local count, body, clock = _T.widgets[1], _T.widgets[2], _T.widgets[3]
_T.eq(_T.title, "Pasta", "title")
_T.eq(count.props.text, "Step 1 of 3", "first step")
_T.eq(clock.props.hidden, true, "no clock without a timer")

_T.press("Next")
_T.eq(count.props.text, "Step 2 of 3", "next step")
_T.eq(clock.props.text, "10:00", "the timer's length, not started")
_T.advance(5000)
_T.eq(clock.props.text, "10:00", "not counting before Next")

_T.press("Next")
_T.eq(count.props.text, "Step 2 of 3", "Next on a timer step starts it")
_T.advance(60000)
_T.eq(clock.props.text, "9:00", "a minute in")

_T.press("Back")
_T.eq(count.props.text, "Step 2 of 3", "Back while counting stays")
_T.eq(clock.props.text, "10:00", "Back resets the timer")
_T.advance(60000)
_T.eq(clock.props.text, "10:00", "and it stays reset")

_T.press("Next")
_T.advance(599000)
_T.eq(clock.props.text, "0:01", "a second left")
_T.eq(#_T.beeps, 0, "no beep yet")
_T.advance(1250)
_T.eq(clock.props.text, "0:00", "done")
_T.eq(clock.props.color, 0x3DDC84, "green when done")
_T.eq(#_T.beeps, 1, "beeps once")
_T.eq(_T.beeps[1].count, 3, "three times")
_T.advance(60000)
_T.eq(#_T.beeps, 1, "and only once")

_T.press("Next")
_T.eq(count.props.text, "Step 3 of 3", "Next after the timer moves on")
_T.eq(clock.props.hidden, true, "no clock on the last step")
_T.press("Next")
_T.eq(count.props.text, "Step 3 of 3", "stays on the last step")

_T.swipe("right")
_T.eq(count.props.text, "Step 2 of 3", "swipe right goes back")
_T.eq(clock.props.text, "10:00", "to the timer, not started")
_T.swipe("left")
_T.advance(2000)
_T.eq(clock.props.text, "9:58", "swipe left starts it")
_T.swipe("right")
_T.swipe("right")
_T.eq(count.props.text, "Step 1 of 3", "back to the start")
""")

    def test_voice_controls(self):
        self.assert_runs("""
local count, clock = _T.widgets[1], _T.widgets[3]
_T.call("next")
_T.eq(count.props.text, "Step 2 of 3", "lua.call next")
local s = _T.call("step")
_T.eq(s.step, 2, "step reports where it is")
_T.eq(s.of, 3, "and how many")
_T.eq(s.text, "Cook the pasta", "and the step")
_T.call("next")
_T.advance(1000)
_T.eq(clock.props.text, "9:59", "next by voice starts the timer too")
_T.call("back")
_T.eq(clock.props.text, "10:00", "back by voice resets it")
""")

    def test_mock_rejects_what_the_guide_does_not_allow(self):
        r = self.run_app('ui.label{x = 1, font = 20}')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('no field "font"', r.stderr)

    def test_board_libraries_only(self):
        self.assert_runs("""
assert(io == nil and os == nil and require == nil, "no io, os or require")
assert(load == nil and dofile == nil and loadfile == nil, "no loading code")
assert(string and table and math and utf8 and coroutine, "the standard ones")
""")

    def test_errors_name_the_line(self):
        r = self.run_app("local x = nil\nlocal y = x.field\n")
        self.assertRegex(r.stderr, r"app:2: attempt to index a nil value")


if __name__ == "__main__":
    unittest.main()
