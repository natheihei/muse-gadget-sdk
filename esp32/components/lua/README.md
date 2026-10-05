# Lua 5.4.6

The Lua interpreter that runs apps Muse sends to the board
(`components/muse/muse_lua.c`, `CONFIG_MUSE_LUA`). MIT licensed: see `LICENSE`.

`src/` is the unmodified `src/` of the Lua 5.4.6 release (lua.org, 2 May 2023)
without the standalone programs (`lua.c`, `luac.c`), `lua.hpp`, and the
libraries apps don't get: `linit.c`, `liolib.c`, `loslib.c`, `loadlib.c` and
`ldblib.c`. `muse_lua.c` opens base, coroutine, string, table, math and utf8
itself.

To update, copy the new release's `src/` files over these, leaving out the same
ones, and keep the files unmodified.
