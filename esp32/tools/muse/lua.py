#!/usr/bin/env python3
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

"""Run Lua apps on a board over USB, as Muse does with lua.run.

    python3 tools/muse/lua.py [--port PORT] run APP.lua   run it; print lua.run's reply
    python3 tools/muse/lua.py [--port PORT] call NAME [ARGS]  lua.call: NAME(ARGS), ARGS a JSON array
    python3 tools/muse/lua.py [--port PORT] stop          close the app
    python3 tools/muse/lua.py [--port PORT] status        lua.status
    python3 tools/muse/lua.py [--port PORT] help          lua.help's guide, as Muse gets it

The board takes the script a console line at a time ("lua+=", then "lua="
runs it; components/muse/muse_lua.h) and answers with "@lua" lines, so this
needs no Muse, token or network. Needs firmware with CONFIG_MUSE_LUA and
pyserial.
"""
import argparse
import json
import sys
import time

from chat import Board, BoardError, escape, pick_port

LINE_BYTES = 480   # as chat.py: escaped, a line stays under the board's 1024
ACK_S = 3.0
REPLY_S = 20.0     # lua.run answers a second after the top level, which may take 3 s


def lua_lines(script):
    data = script.replace("\0", "").encode("utf-8")
    starts = range(0, len(data), LINE_BYTES) if data else [0]
    return [(b">lua" + (b"+=" if i + LINE_BYTES < len(data) else b"=") + escape(data[i:i + LINE_BYTES]) + b"\n",
             min(i + LINE_BYTES, len(data))) for i in starts]


def expect(board, secs, want=None):
    """The next "@lua" frame (with key `want`, if given); raises at the deadline."""
    deadline = time.monotonic() + secs
    while True:
        f = board.frame(deadline, "@lua")
        if f is None:
            raise BoardError("The board didn't answer. Is its firmware built with CONFIG_MUSE_LUA? "
                             + board.recent_log())
        if want is None or want in f:
            return f
        if f.get("ok") is False:
            return f


def run(board, script):
    for attempt in range(2):
        board.wake()
        board.write(b"\n")
        board.drain(0.2)
        for line, total in lua_lines(script):
            board.write(line)
            f = expect(board, ACK_S, "ack")
            if f.get("ack") != total:
                break   # a line went missing: start over
        else:
            return expect(board, REPLY_S, "ok")
    raise BoardError("The board didn't take the script.")


def command(board, name):
    board.wake()
    board.write(b"\n")
    board.drain(0.2)
    board.write_line(name)
    return expect(board, ACK_S + 2, "ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("action", choices=["run", "call", "stop", "status", "help"])
    ap.add_argument("file", nargs="?", help="the app, for run; the function's name, for call")
    ap.add_argument("args", nargs="?", help="call: the arguments as a JSON array")
    ap.add_argument("--port")
    args = ap.parse_args()
    try:
        with Board(args.port or pick_port()) as board:
            if args.action == "run":
                if not args.file:
                    ap.error("run needs the app's file")
                with open(args.file, encoding="utf-8") as f:
                    result = run(board, f.read())
            elif args.action == "call":
                if not args.file:
                    ap.error("call needs the function's name")
                params = {"name": args.file}
                if args.args:
                    params["args"] = json.loads(args.args)
                result = command(board, "lua.call=" + json.dumps(params))
            else:
                result = command(board, "lua." + args.action)
            if args.action == "help" and result.get("ok"):
                print(result["payload"].pop("api"))
            print(json.dumps(result, indent=2))
            if not result.get("ok"):
                sys.exit(1)
    except BoardError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
