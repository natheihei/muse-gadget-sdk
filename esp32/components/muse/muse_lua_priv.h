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

/* Between the runner (muse_lua.c) and the app's screen (muse_lua_ui.c). */

#include <stdbool.h>
#include <stdint.h>

#include "lua.h"

/* What the screen posts to the runner. */
typedef enum {
    MUSE_LUA_UI_PRESS,   /* a button: wid */
    MUSE_LUA_UI_TAP,     /* the app's area: x, y */
    MUSE_LUA_UI_SWIPE,   /* the app's area: x is an lv_dir_t */
    MUSE_LUA_UI_CLOSE,   /* the top bar's X */
} muse_lua_ui_kind_t;

/* ---- The runner's side, from the LVGL task (event callbacks) ---- */

/* Queues an event for the runner, stamped with the app it's for (the one
 * muse_lua_ui_open was given): a later app ignores it. Never blocks. */
void muse_lua_post_ui(uint32_t app, muse_lua_ui_kind_t kind, uint32_t wid, int x, int y);

/* ---- The runner's side, from the runner task (the ui library) ---- */

/* Calls the function under its nargs arguments on top of the stack, popping
 * them, within the callback time limit. On an error the app is stopped once
 * the event is done; false then. */
bool muse_lua_call(lua_State *L, int nargs);

/* ---- The screen's side, all from the runner task ---- */

/* The app's area under the top bar. */
void muse_lua_ui_size(int *w, int *h);
/* Covers Muse's face with the top bar and an empty app area, or empties the
 * one showing. Keeps the screen awake. False if the display stayed busy. */
bool muse_lua_ui_open(uint32_t app, const char *title);
void muse_lua_ui_title(const char *title);
/* Replaces the app's widgets with the error, under the top bar, and lets the
 * screen sleep again. */
void muse_lua_ui_error(const char *message);
/* Takes the top bar and the app away: Muse's face again. */
void muse_lua_ui_close(void);
/* Adds the ui library to a new state. */
void muse_lua_ui_install(lua_State *L);
/* The state is closing: forget its widgets and callbacks (the widgets stay
 * on screen until muse_lua_ui_open, _error or _close). */
void muse_lua_ui_forget(void);
/* Runs the app's callback for an event, through muse_lua_call. */
void muse_lua_ui_event(lua_State *L, muse_lua_ui_kind_t kind, uint32_t wid, int x, int y);
