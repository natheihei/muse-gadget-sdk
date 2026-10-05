/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lua apps that Muse sends (CONFIG_MUSE_LUA). One runs at a time, in a Lua 5.4
 * state of its own on the runner task, and draws under a top bar the firmware
 * owns, whose X closes it (muse_lua_ui.c). muse_lua_api.md, which lua.help
 * returns, is the contract with Muse.
 */

/* Most bytes of script lua.run takes. Muse is told 8 KB: a link.invoke has to
 * fit one ~12 KB frame, escaped. The console takes up to this. */
#define MUSE_LUA_SCRIPT_MAX (64 * 1024)

/* Takes the command's result, {"ok":..., "payload"|"error":...}, as Link's
 * command handlers return it. Called once, from any task. */
typedef void (*muse_lua_reply_fn)(void *ctx, cJSON *result);

/* Starts the runner task. After muse_ui_start(). */
esp_err_t muse_lua_start(void);

/* lua.run, lua.call, lua.stop, lua.status or lua.help, with its params (may be NULL).
 * False if `command` is none of them; otherwise `reply` runs once, perhaps
 * before this returns. */
bool muse_lua_command(const char *command, const cJSON *params, muse_lua_reply_fn reply, void *ctx);

/* A console line after '>' (muse_input.c): "lua+=PIECE" adds a piece of a
 * script and "lua=PIECE" adds the last and runs it (escaped as chat lines
 * are), "lua.call={"name":...,"args":[...]}", "lua.stop", "lua.status" and
 * "lua.help". Replies are "@lua JSON"
 * lines (tools/muse/lua.py). False if the line isn't one of these. */
bool muse_lua_console(char *line, bool whole);

#ifdef __cplusplus
}
#endif
