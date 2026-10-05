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

/* The vendored Lua (components/lua) with the libraries an app gets on the
 * board (muse_lua.c's open_libs): runs each file named in turn, in one state,
 * and exits 1 with the error on the first that fails. */

#include <stdio.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

int main(int argc, char **argv)
{
    lua_State *L = luaL_newstate();
    static const luaL_Reg std[] = {
        { LUA_GNAME, luaopen_base },        { LUA_COLIBNAME, luaopen_coroutine },
        { LUA_STRLIBNAME, luaopen_string }, { LUA_TABLIBNAME, luaopen_table },
        { LUA_MATHLIBNAME, luaopen_math },  { LUA_UTF8LIBNAME, luaopen_utf8 },
    };
    for (size_t i = 0; i < sizeof(std) / sizeof(std[0]); i++) {
        luaL_requiref(L, std[i].name, std[i].func, 1);
        lua_pop(L, 1);
    }
    static const char *const gone[] = { "dofile", "loadfile", "load" };
    for (size_t i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
        lua_pushnil(L);
        lua_setglobal(L, gone[i]);
    }
    for (int i = 1; i < argc; i++) {
        /* "=app" names the chunk as the board does, so errors read "app:12:". */
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "can't open %s\n", argv[i]);
            return 1;
        }
        static char buf[256 * 1024];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        if (luaL_loadbufferx(L, buf, n, "=app", "t") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
            fprintf(stderr, "%s: %s\n", argv[i], lua_tostring(L, -1));
            return 1;
        }
    }
    lua_close(L);
    return 0;
}
