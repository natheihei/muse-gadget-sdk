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

#include "muse_lua.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "muse_chat.h"
#include "muse_lua_priv.h"
#include "muse_mem.h"
#include "muse_voice.h"

static const char *TAG = "muse_lua";

/*
 * One runner task owns the Lua state and runs everything in it: the script's
 * top level, then its callbacks, one event at a time from its queue. Other
 * tasks only queue events: Link's session (lua.run, lua.stop), esp_timer's
 * task (timers) and LVGL's (touch). So Lua never runs anywhere else, and
 * nothing waits on the runner but the queue.
 *
 * An app that runs too long is stopped from the count hook, which raises an
 * error: never by deleting the task, which could be holding the display lock.
 */

#define QUEUE_LEN 32
#define RUNNER_STACK (32 * 1024)   /* in PSRAM; the parser's recursion is capped below that */
#define RUNNER_PRIO 3              /* under the voice, input and LVGL tasks */
#define MEM_MAX (1024 * 1024)
#define TOP_MS 3000                /* the script's top level */
#define CALLBACK_MS 500            /* each callback */
#define CLOSE_MS 1000              /* app.on_exit and __gc while closing */
#define HOOK_EVERY 1000            /* instructions between looks at the clock */
#define FIRST_REPLY_MS 1000        /* lua.run waits this long for early errors */
#define TIMERS_MAX 32
#define TIMEOUT_MIN_MS 1
#define INTERVAL_MIN_MS 50
#define LOG_LINES 48
#define LOG_TEXT 200
#define STATUS_LOGS 30
#define ERROR_MAX 600
#define TITLE_MAX 64
#define SOURCE_MAX 160
#define CALLS_MAX 256              /* the names app.expose gave, comma separated */
#define CALL_NAME_MAX 64
#define JSON_DEPTH 16

typedef enum { MSG_RUN, MSG_STOP, MSG_TIMER, MSG_UI, MSG_CALL } msg_type_t;

typedef struct {
    uint8_t type;
    union {
        struct {
            char *script;
            size_t len;
            char *title;
            char *source;
            muse_lua_reply_fn reply;
            void *ctx;
        } run;
        struct {
            uint32_t app;   /* the X of this app; 0: whichever is running */
            const char *reason;
            muse_lua_reply_fn reply;
            void *ctx;
        } stop;
        struct {
            uint32_t id;
        } timer;
        struct {
            char *name;
            char *args;   /* JSON, or NULL */
            muse_lua_reply_fn reply;
            void *ctx;
        } call;
        struct {
            uint32_t app, wid;
            int16_t x, y;
            uint8_t kind;
        } ui;
    };
} msg_t;

typedef struct {
    uint32_t id;               /* 0: free */
    esp_timer_handle_t handle;
    bool repeat;
    volatile bool queued;      /* one firing waits in the queue at most */
    int ref;
} lua_timer_t;

typedef struct {
    uint32_t seq;
    uint32_t app;
    char text[LOG_TEXT];
} log_line_t;

typedef enum { APP_IDLE, APP_RUNNING, APP_ERROR } app_state_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_mutex;   /* the status and the log, read by other tasks */

/* Runner task only. */
static lua_State *s_L;
static size_t s_mem;
static int64_t s_deadline_us;
static const char *s_deadline_what;
static int s_deadline_ms;
static bool s_failed;
static bool s_exit_requested;
static int s_on_exit_ref = LUA_NOREF;
static int s_expose_ref = LUA_NOREF;   /* app.expose's table, for lua.call */
static lua_timer_t s_timers[TIMERS_MAX];
static uint32_t s_next_timer_id = 1;
static EXT_RAM_BSS_ATTR char s_fail_msg[ERROR_MAX];
static struct {
    muse_lua_reply_fn fn;
    void *ctx;
    int64_t due_us;
    uint32_t log_from;
} s_pending;

/* Set by other tasks: the running code is being replaced or stopped. */
static volatile bool s_abort;
/* A lua.call is queued or running: one at a time, so a burst of them (Muse
 * retrying, or acting on what it read before) can't pile up behind the app. */
static atomic_bool s_call_busy;

/* Under s_mutex. */
static EXT_RAM_BSS_ATTR struct {
    app_state_t state;
    uint32_t app;
    char title[TITLE_MAX];
    char source[SOURCE_MAX];   /* where Muse keeps the script */
    char calls[CALLS_MAX];     /* what lua.call can call */
    int64_t started_us;
    char error[ERROR_MAX];
    char last[48];   /* how the last app ended */
} s_status;
static log_line_t *s_log;
static uint32_t s_log_seq;

extern const char api_md_start[] asm("_binary_muse_lua_api_md_start");

/* ---- Status and log --------------------------------------------------------- */

static void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static void add_log(uint32_t app, const char *text)
{
    ESP_LOGI(TAG, "app %u: %s", (unsigned)app, text);
    lock();
    log_line_t *l = &s_log[s_log_seq % LOG_LINES];
    l->seq = s_log_seq++;
    l->app = app;
    strlcpy(l->text, text, sizeof(l->text));
    unlock();
}

/* The log lines of `app` from `from` on, the newest `max`. Under s_mutex. */
static cJSON *logs_json_locked(uint32_t app, uint32_t from, int max)
{
    cJSON *arr = cJSON_CreateArray();
    uint32_t first = s_log_seq > LOG_LINES ? s_log_seq - LOG_LINES : 0;
    if (from < first) {
        from = first;   /* older lines have been written over */
    }
    uint32_t seqs[LOG_LINES];
    int count = 0;
    for (uint32_t s = from; s < s_log_seq; s++) {
        if (s_log[s % LOG_LINES].app == app) {
            seqs[count++] = s;
        }
    }
    for (int i = count > max ? count - max : 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_log[seqs[i] % LOG_LINES].text));
    }
    return arr;
}

static void set_status(app_state_t state, const char *error, const char *last)
{
    lock();
    s_status.state = state;
    if (error) {
        strlcpy(s_status.error, error, sizeof(s_status.error));
    }
    if (last) {
        strlcpy(s_status.last, last, sizeof(s_status.last));
    }
    unlock();
}

/* ---- Results ------------------------------------------------------------------ */

static cJSON *result_ok(cJSON *payload)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    if (payload) {
        cJSON_AddItemToObject(r, "payload", payload);
    }
    return r;
}

static cJSON *result_error(const char *code, const char *message, cJSON *payload)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    if (payload) {
        cJSON_AddItemToObject(r, "payload", payload);
    }
    return r;
}

/* app.expose's names, for lua.call. Under s_mutex. */
static cJSON *calls_json_locked(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (const char *p = s_status.calls; *p;) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char name[CALL_NAME_MAX];
        snprintf(name, sizeof(name), "%.*s", (int)n, p);
        cJSON_AddItemToArray(arr, cJSON_CreateString(name));
        p += n + (end ? 1 : 0);
    }
    return arr;
}

/* lua.run's payload: the app and its log since it started. */
static cJSON *run_payload(const char *state, uint32_t log_from)
{
    cJSON *p = cJSON_CreateObject();
    lock();
    cJSON_AddNumberToObject(p, "app", s_status.app);
    cJSON_AddStringToObject(p, "state", state);
    cJSON_AddItemToObject(p, "logs", logs_json_locked(s_status.app, log_from, LOG_LINES));
    cJSON_AddItemToObject(p, "calls", calls_json_locked());
    bool kept = s_status.source[0];
    bool voiced = s_status.calls[0];
    unlock();
    if (s_L) {
        cJSON_AddNumberToObject(p, "mem_kb", (double)(s_mem / 1024));
    }
#if CONFIG_MUSE_CJK_FONT
    cJSON_AddBoolToObject(p, "cjk", true);   /* as lua.help says: Chinese and Japanese show */
#endif
    /* Muse reads every reply, but may have read lua.help long ago. */
    char note[640] = "";
    if (!kept) {
        strlcat(note,
                "The gadget doesn't keep apps. Save this script as a file in your workspace (such as "
                "gadget-apps/<name>.lua), and send its path as a source parameter alongside script in lua.run, "
                "so you can edit and resend it later rather than rewrite it. lua.status reports the running "
                "app's source. ",
                sizeof(note));
    }
    if (!voiced && s_L) {
        strlcat(note,
                "The person can talk to you while the app runs. Name its actions with app.expose{next = "
                "next_step, ...} so you can do them with lua.call {name, args} when asked, such as \"next "
                "step\".",
                sizeof(note));
    }
    if (note[0]) {
        cJSON_AddStringToObject(p, "note", note);
    }
    return p;
}

/* Answers a lua.run still waiting for its first second. */
static void reply_pending(const char *state, const char *error)
{
    if (!s_pending.fn) {
        return;
    }
    cJSON *payload = run_payload(state, s_pending.log_from);
    s_pending.fn(s_pending.ctx, error ? result_error("lua_error", error, payload) : result_ok(payload));
    s_pending.fn = NULL;
}

/* ---- The Lua state ------------------------------------------------------------ */

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    size_t old = ptr ? osize : 0;   /* without ptr, osize is the object's type */
    if (nsize == 0) {
        heap_caps_free(ptr);
        s_mem -= old;
        return NULL;
    }
    if (nsize > old && s_mem - old + nsize > MEM_MAX) {
        return NULL;   /* Lua raises "not enough memory" */
    }
    void *p = heap_caps_realloc(ptr, nsize, MUSE_BIG_CAPS);
    if (p) {
        s_mem = s_mem - old + nsize;
    }
    return p;
}

static void set_deadline(const char *what, int ms)
{
    s_deadline_what = what;
    s_deadline_ms = ms;
    s_deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

static void hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (s_abort) {
        luaL_error(L, "stopped");
    }
    if (esp_timer_get_time() > s_deadline_us) {
        luaL_error(L, "%s ran over %d ms: don't wait in a loop, use timer.setTimeout or timer.setInterval",
                   s_deadline_what, s_deadline_ms);
    }
}

/* The error and a few lines of where it happened, for Muse to fix. */
static int msgh(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    if (!msg) {
        msg = luaL_tolstring(L, 1, NULL);
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

/* Keeps the message and the traceback's lines in the app's own code. */
static void keep_error(const char *raw, char *out, size_t cap)
{
    const char *tb = strstr(raw, "\nstack traceback:");
    size_t head = tb ? (size_t)(tb - raw) : strlen(raw);
    size_t n = head < cap - 1 ? head : cap - 1;
    memcpy(out, raw, n);
    out[n] = '\0';
    if (!tb) {
        return;
    }
    int lines = 0;
    for (const char *p = strchr(tb + 1, '\n'); p && lines < 4; p = strchr(p + 1, '\n')) {
        const char *line = p + 1;
        while (*line == '\t' || *line == ' ') {
            line++;
        }
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (strncmp(line, "app:", 4) != 0) {
            continue;   /* "[C]: in ?" and the like */
        }
        if (n + len + 4 >= cap) {
            break;
        }
        n += snprintf(out + n, cap - n, "\n  %.*s", (int)len, line);
        lines++;
    }
}

static void note_failure(lua_State *L)
{
    if (!s_abort && !s_failed) {
        keep_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "error", s_fail_msg, sizeof(s_fail_msg));
        s_failed = true;
    }
}

static int pcall_timed(lua_State *L, int nargs, int nresults, const char *what, int ms)
{
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, msgh);
    lua_insert(L, base);
    set_deadline(what, ms);
    int rc = lua_pcall(L, nargs, nresults, base);
    if (rc != LUA_OK) {
        note_failure(L);
        lua_pop(L, 1);
    }
    lua_remove(L, base);
    return rc;
}

bool muse_lua_call(lua_State *L, int nargs)
{
    return pcall_timed(L, nargs, 0, "a callback", CALLBACK_MS) == LUA_OK;
}

/* ---- Libraries: log, app, timer, time, sound, json ------------------------- */

static int l_log(lua_State *L)
{
    int n = lua_gettop(L);   /* before buffinit, which pushes a placeholder */
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            luaL_addchar(&b, ' ');
        }
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    add_log(s_status.app, lua_tostring(L, -1));
    return 0;
}

static int l_app_title(lua_State *L)
{
    const char *t = luaL_checkstring(L, 1);
    lock();
    strlcpy(s_status.title, t, sizeof(s_status.title));
    unlock();
    muse_lua_ui_title(t);
    return 0;
}

static int l_app_exit(lua_State *L)
{
    (void)L;
    s_exit_requested = true;
    return 0;
}

static int l_app_on_exit(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    luaL_unref(L, LUA_REGISTRYINDEX, s_on_exit_ref);
    lua_pushvalue(L, 1);
    s_on_exit_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_app_expose(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);   /* a copy: what lua.call can reach doesn't change behind its back */
    char names[CALLS_MAX] = "";
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) != LUA_TSTRING || !lua_isfunction(L, -1)) {
            return luaL_error(L, "app.expose takes names and functions, such as app.expose{next = next_step}");
        }
        if (names[0]) {
            strlcat(names, ",", sizeof(names));
        }
        strlcat(names, lua_tostring(L, -2), sizeof(names));
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, 2);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, s_expose_ref);
    s_expose_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lock();
    strlcpy(s_status.calls, names, sizeof(s_status.calls));
    unlock();
    return 0;
}

static void timer_fired(void *arg)
{
    lua_timer_t *t = &s_timers[(uintptr_t)arg];
    uint32_t id = t->id;
    if (!id || t->queued) {
        return;   /* gone, or the last firing hasn't run yet: coalesce */
    }
    t->queued = true;
    msg_t m = { .type = MSG_TIMER, .timer.id = id };
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        t->queued = false;
    }
}

static void timer_free(lua_State *L, lua_timer_t *t)
{
    esp_timer_stop(t->handle);
    esp_timer_delete(t->handle);
    if (L) {
        luaL_unref(L, LUA_REGISTRYINDEX, t->ref);
    }
    t->handle = NULL;
    t->id = 0;
    t->queued = false;
}

static int timer_start(lua_State *L, bool repeat)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_Integer min = repeat ? INTERVAL_MIN_MS : TIMEOUT_MIN_MS;
    if (ms < min) {
        ms = min;
    }
    int slot = -1;
    for (int i = 0; i < TIMERS_MAX; i++) {
        if (!s_timers[i].id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return luaL_error(L, "too many timers (%d): clear the ones you're done with", TIMERS_MAX);
    }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_timer_t *t = &s_timers[slot];
    const esp_timer_create_args_t args = {
        .callback = timer_fired,
        .arg = (void *)(uintptr_t)slot,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lua",
    };
    if (esp_timer_create(&args, &t->handle) != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "no memory for a timer");
    }
    t->id = s_next_timer_id++;
    t->repeat = repeat;
    t->queued = false;
    t->ref = ref;
    uint64_t us = (uint64_t)ms * 1000;
    if ((repeat ? esp_timer_start_periodic(t->handle, us) : esp_timer_start_once(t->handle, us)) != ESP_OK) {
        timer_free(L, t);
        return luaL_error(L, "couldn't start the timer");
    }
    lua_pushinteger(L, t->id);
    return 1;
}

static int l_set_timeout(lua_State *L)
{
    return timer_start(L, false);
}

static int l_set_interval(lua_State *L)
{
    return timer_start(L, true);
}

static int l_timer_clear(lua_State *L)
{
    lua_Integer id = luaL_optinteger(L, 1, 0);
    for (int i = 0; id > 0 && i < TIMERS_MAX; i++) {
        if (s_timers[i].id == (uint32_t)id) {
            timer_free(L, &s_timers[i]);
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

static void timers_clear(lua_State *L)
{
    for (int i = 0; i < TIMERS_MAX; i++) {
        if (s_timers[i].id) {
            timer_free(L, &s_timers[i]);
        }
    }
}

static void timer_event(lua_State *L, uint32_t id)
{
    for (int i = 0; i < TIMERS_MAX; i++) {
        lua_timer_t *t = &s_timers[i];
        if (t->id != id) {
            continue;
        }
        t->queued = false;
        lua_rawgeti(L, LUA_REGISTRYINDEX, t->ref);
        if (!t->repeat) {
            timer_free(L, t);   /* first, so timer.clear(id) in it says false */
        }
        muse_lua_call(L, 0);
        return;
    }
}

static int l_time_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static int opt_field(lua_State *L, int t, const char *k, int def, int lo, int hi)
{
    if (lua_type(L, t) != LUA_TTABLE) {
        return def;
    }
    lua_getfield(L, t, k);
    int v = lua_isnil(L, -1) ? def : (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return v < lo ? lo : v > hi ? hi : v;
}

static int l_beep(lua_State *L)
{
    int freq = opt_field(L, 1, "freq", 880, 100, 4000);
    int ms = opt_field(L, 1, "ms", 150, 20, 2000);
    int count = opt_field(L, 1, "count", 1, 1, 10);
    int gap = opt_field(L, 1, "gap", 120, 0, 2000);
    muse_voice_request_beep(freq, ms, count, gap);
    return 0;
}

/* NULL with *err set on a value JSON can't hold: the caller frees what's built
 * and raises, so no half-built tree is left behind by a longjmp. */
static cJSON *to_json(lua_State *L, int idx, int depth, const char **err)
{
    idx = lua_absindex(L, idx);
    if (depth > JSON_DEPTH) {
        *err = "nested too deep";
        return NULL;
    }
    luaL_checkstack(L, 4, "json.encode");
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return cJSON_CreateNull();
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER:
        return cJSON_CreateNumber(lua_tonumber(L, idx));
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE: {
        lua_Unsigned n = lua_rawlen(L, idx);
        size_t keys = 0;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            keys++;
            lua_pop(L, 1);
        }
        bool array = keys == n && n > 0;
        cJSON *out = array ? cJSON_CreateArray() : cJSON_CreateObject();
        if (array) {
            for (lua_Unsigned i = 1; i <= n; i++) {
                lua_rawgeti(L, idx, (lua_Integer)i);
                cJSON *item = to_json(L, -1, depth + 1, err);
                lua_pop(L, 1);
                if (!item) {
                    cJSON_Delete(out);
                    return NULL;
                }
                cJSON_AddItemToArray(out, item);
            }
            return out;
        }
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            int kt = lua_type(L, -2);
            if (kt != LUA_TSTRING && kt != LUA_TNUMBER) {
                lua_pop(L, 2);
                cJSON_Delete(out);
                *err = "object keys must be strings or numbers";
                return NULL;
            }
            lua_pushvalue(L, -2);   /* a copy to convert: tostring changes number keys in place */
            const char *k = lua_tostring(L, -1);
            cJSON *item = to_json(L, -2, depth + 1, err);
            if (!item) {
                lua_pop(L, 3);
                cJSON_Delete(out);
                return NULL;
            }
            cJSON_AddItemToObject(out, k, item);
            lua_pop(L, 2);
        }
        return out;
    }
    default:
        *err = "it holds only nil, booleans, numbers, strings and tables";
        return NULL;
    }
}

static int l_json_encode(lua_State *L)
{
    luaL_checkany(L, 1);
    const char *err = NULL;
    cJSON *j = to_json(L, 1, 0, &err);
    if (!j) {
        return luaL_error(L, "json.encode: %s", err ? err : "out of memory");
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) {
        return luaL_error(L, "json.encode: out of memory");
    }
    lua_pushstring(L, s);
    cJSON_free(s);
    return 1;
}

static void push_json(lua_State *L, const cJSON *j, int depth)
{
    luaL_checkstack(L, 4, "json.decode");
    if (depth > JSON_DEPTH) {
        lua_pushnil(L);
    } else if (cJSON_IsBool(j)) {
        lua_pushboolean(L, cJSON_IsTrue(j));
    } else if (cJSON_IsNumber(j)) {
        double d = j->valuedouble;
        if (d == (double)(lua_Integer)d) {
            lua_pushinteger(L, (lua_Integer)d);
        } else {
            lua_pushnumber(L, d);
        }
    } else if (cJSON_IsString(j)) {
        lua_pushstring(L, j->valuestring);
    } else if (cJSON_IsArray(j)) {
        lua_createtable(L, cJSON_GetArraySize(j), 0);
        int i = 1;
        for (const cJSON *c = j->child; c; c = c->next) {
            push_json(L, c, depth + 1);
            lua_rawseti(L, -2, i++);
        }
    } else if (cJSON_IsObject(j)) {
        lua_newtable(L);
        for (const cJSON *c = j->child; c; c = c->next) {
            push_json(L, c, depth + 1);
            lua_setfield(L, -2, c->string);
        }
    } else {
        lua_pushnil(L);
    }
}

static int l_json_decode(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    cJSON *j = cJSON_ParseWithLength(s, len);
    if (!j) {
        lua_pushnil(L);
        lua_pushstring(L, "json.decode: not valid JSON");
        return 2;
    }
    /* push_json only raises on a full stack; parse depth is bounded first. */
    push_json(L, j, 0);
    cJSON_Delete(j);
    return 1;
}

static void add_lib(lua_State *L, const char *name, const luaL_Reg *fns)
{
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, name);
}

static void open_libs(lua_State *L)
{
    static const luaL_Reg std[] = {
        { LUA_GNAME, luaopen_base },        { LUA_COLIBNAME, luaopen_coroutine },
        { LUA_STRLIBNAME, luaopen_string }, { LUA_TABLIBNAME, luaopen_table },
        { LUA_MATHLIBNAME, luaopen_math },  { LUA_UTF8LIBNAME, luaopen_utf8 },
    };
    for (size_t i = 0; i < sizeof(std) / sizeof(std[0]); i++) {
        luaL_requiref(L, std[i].name, std[i].func, 1);
        lua_pop(L, 1);
    }
    /* Files don't exist here, and binary chunks could crash the VM. */
    static const char *const gone[] = { "dofile", "loadfile", "load" };
    for (size_t i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
        lua_pushnil(L);
        lua_setglobal(L, gone[i]);
    }
    lua_register(L, "log", l_log);
    lua_register(L, "print", l_log);

    static const luaL_Reg app[] = {
        { "title", l_app_title }, { "exit", l_app_exit }, { "on_exit", l_app_on_exit }, { "expose", l_app_expose },
        { NULL, NULL }
    };
    static const luaL_Reg timer[] = {
        { "setTimeout", l_set_timeout }, { "setInterval", l_set_interval }, { "clear", l_timer_clear }, { NULL, NULL }
    };
    static const luaL_Reg time[] = { { "ms", l_time_ms }, { NULL, NULL } };
    static const luaL_Reg sound[] = { { "beep", l_beep }, { NULL, NULL } };
    static const luaL_Reg json[] = { { "encode", l_json_encode }, { "decode", l_json_decode }, { NULL, NULL } };
    add_lib(L, "app", app);
    add_lib(L, "timer", timer);
    add_lib(L, "time", time);
    add_lib(L, "sound", sound);
    add_lib(L, "json", json);
    muse_lua_ui_install(L);
}

/* ---- Lifecycle (runner task) ----------------------------------------------- */

static void close_state(bool run_on_exit)
{
    lua_State *L = s_L;
    if (!L) {
        return;
    }
    if (run_on_exit && s_on_exit_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_on_exit_ref);
        pcall_timed(L, 0, 0, "app.on_exit", CLOSE_MS);
    }
    timers_clear(L);
    muse_lua_ui_forget();
    set_deadline("closing the app", CLOSE_MS);   /* __gc metamethods run in lua_close */
    lua_close(L);
    s_L = NULL;
    s_on_exit_ref = LUA_NOREF;
    s_expose_ref = LUA_NOREF;
    s_failed = false;
    s_exit_requested = false;
    ESP_LOGI(TAG, "state closed, %u bytes left", (unsigned)s_mem);
    s_mem = 0;
}

/* s_fail_msg in the log, its start if it's longer than a line. */
static void log_failure(uint32_t app)
{
    char line[LOG_TEXT] = "error: ";
    strlcat(line, s_fail_msg, sizeof(line));
    add_log(app, line);
}

/* An error stopped the app: it shows under the top bar until the X. */
static void app_fail(void)
{
    log_failure(s_status.app);
    close_state(false);
    set_status(APP_ERROR, s_fail_msg, "error");
    muse_lua_ui_error(s_fail_msg);
    reply_pending("error", s_fail_msg);
}

/* Closes the app and shows Muse again; false if there was none. */
static bool app_stop(const char *reason)
{
    lock();
    app_state_t state = s_status.state;
    uint32_t app = s_status.app;
    unlock();
    if (state == APP_IDLE) {
        return false;
    }
    char line[64];
    snprintf(line, sizeof(line), "closed (%s)", reason);
    add_log(app, line);
    close_state(state == APP_RUNNING);
    muse_lua_ui_close();
    set_status(APP_IDLE, NULL, reason);
    reply_pending("closed", NULL);
    return true;
}

/* After each event: an error or app.exit() ends the app. */
static void settle(void)
{
    if (s_failed) {
        app_fail();
    } else if (s_exit_requested) {
        app_stop("app.exit");
    }
}

static void app_run(msg_t *m)
{
    s_abort = false;
    if (s_pending.fn) {
        reply_pending("replaced", NULL);
    }
    lock();
    bool running = s_status.state == APP_RUNNING;
    unlock();
    if (running) {
        close_state(true);
    }

    lock();
    uint32_t app = ++s_status.app;
    s_status.state = APP_RUNNING;
    strlcpy(s_status.title, m->run.title ? m->run.title : "", sizeof(s_status.title));
    strlcpy(s_status.source, m->run.source ? m->run.source : "", sizeof(s_status.source));
    s_status.calls[0] = '\0';
    s_status.started_us = esp_timer_get_time();
    s_status.error[0] = '\0';
    uint32_t log_from = s_log_seq;
    unlock();
    ESP_LOGI(TAG, "app %u: %u bytes, \"%s\"", (unsigned)app, (unsigned)m->run.len, s_status.title);

    s_mem = 0;
    s_failed = s_exit_requested = false;
    lua_State *L = lua_newstate(l_alloc, NULL);
    if (!L) {
        set_status(APP_IDLE, "out of memory", "error");
        muse_lua_ui_close();
        m->run.reply(m->run.ctx, result_error("out_of_memory", "no memory for a Lua state", NULL));
        return;
    }
    s_L = L;
    lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
    set_deadline("opening the libraries", TOP_MS);
    open_libs(L);

    int rc = luaL_loadbufferx(L, m->run.script, m->run.len, "=app", "t");
    if (rc != LUA_OK) {
        /* It never ran: no screen of its own, just the error for Muse. */
        keep_error(lua_tostring(L, -1), s_fail_msg, sizeof(s_fail_msg));
        log_failure(app);
        close_state(false);
        muse_lua_ui_close();
        set_status(APP_IDLE, s_fail_msg, "error");
        m->run.reply(m->run.ctx, result_error("lua_error", s_fail_msg, run_payload("error", log_from)));
        return;
    }
    if (!muse_lua_ui_open(app, s_status.title)) {
        close_state(false);
        set_status(APP_IDLE, "display busy", "error");
        m->run.reply(m->run.ctx, result_error("busy", "the display stayed busy", NULL));
        return;
    }
    s_pending.fn = m->run.reply;
    s_pending.ctx = m->run.ctx;
    s_pending.log_from = log_from;
    s_pending.due_us = esp_timer_get_time() + FIRST_REPLY_MS * 1000LL;
    pcall_timed(L, 0, 0, "the script's top level", TOP_MS);
    settle();
}

typedef struct {
    const char *name;
    const cJSON *args;
    bool found;
} call_prep_t;

/* In a pcall, so running out of the app's memory can't panic: pushes the
 * exposed function and lua.call's arguments (a JSON array's items, or one
 * value), or nothing if the app exposes no such function. */
static int l_call_prepare(lua_State *L)
{
    call_prep_t *c = lua_touserdata(L, 1);
    lua_settop(L, 0);
    if (s_expose_ref == LUA_NOREF) {
        return 0;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_expose_ref);
    lua_getfield(L, 1, c->name);   /* app.expose's own table: no metatable */
    lua_remove(L, 1);
    if (!lua_isfunction(L, 1)) {
        return 0;
    }
    c->found = true;
    if (cJSON_IsArray(c->args)) {
        for (const cJSON *a = c->args->child; a; a = a->next) {
            push_json(L, a, 1);
        }
    } else if (c->args) {
        push_json(L, c->args, 0);
    }
    return lua_gettop(L);
}

static cJSON *call_payload(cJSON *result, uint32_t log_from)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddItemToObject(p, "result", result ? result : cJSON_CreateNull());
    lock();
    cJSON_AddItemToObject(p, "logs", logs_json_locked(s_status.app, log_from, LOG_LINES));
    unlock();
    return p;
}

/* Every lua.call reply goes through here: the next call may come as soon as
 * Muse has this one's answer. */
static void call_done(muse_lua_reply_fn reply, void *ctx, cJSON *result)
{
    atomic_store(&s_call_busy, false);
    reply(ctx, result);
}

/* lua.call: one of app.expose's functions. An error goes back to Muse and the
 * app runs on: a misheard "next" shouldn't end a recipe halfway. */
static void app_call(msg_t *m)
{
    muse_lua_reply_fn reply = m->call.reply;
    void *ctx = m->call.ctx;
    lock();
    bool running = s_status.state == APP_RUNNING;
    uint32_t app = s_status.app, log_from = s_log_seq;
    char calls[CALLS_MAX];
    strlcpy(calls, s_status.calls, sizeof(calls));
    unlock();
    if (!s_L || !running) {
        call_done(reply, ctx, result_error("no_app", "no Lua app is running", NULL));
        return;
    }
    cJSON *args = NULL;
    if (m->call.args && !(args = cJSON_Parse(m->call.args))) {
        call_done(reply, ctx, result_error("bad_args", "args isn't JSON: send a JSON array, such as [2]", NULL));
        return;
    }
    lua_State *L = s_L;
    int top = lua_gettop(L);
    call_prep_t prep = { .name = m->call.name, .args = args };
    lua_pushcfunction(L, l_call_prepare);
    lua_pushlightuserdata(L, &prep);
    int rc = lua_pcall(L, 1, LUA_MULTRET, 0);
    cJSON_Delete(args);
    if (rc != LUA_OK || !prep.found) {
        lua_settop(L, top);
        char msg[CALLS_MAX + 128];
        snprintf(msg, sizeof(msg), "the app has no \"%s\" to call; it exposes: %s", m->call.name,
                 calls[0] ? calls : "nothing (an app offers functions with app.expose{...})");
        call_done(reply, ctx, rc != LUA_OK ? result_error("out_of_memory", "no memory in the app for the call", NULL)
                                : result_error("not_found", msg, NULL));
        return;
    }
    char line[LOG_TEXT];
    snprintf(line, sizeof(line), "lua.call %s", m->call.name);
    add_log(app, line);
    if (pcall_timed(L, lua_gettop(L) - top - 1, 1, "lua.call", CALLBACK_MS) != LUA_OK) {
        const char *msg = s_failed ? s_fail_msg : "stopped: the app was closed or replaced";
        s_failed = false;   /* Muse handles it; the app runs on */
        strlcpy(line, "lua.call error: ", sizeof(line));
        strlcat(line, msg, sizeof(line));
        add_log(app, line);
        call_done(reply, ctx, result_error("lua_error", msg, call_payload(NULL, log_from)));
    } else {
        lua_pushcfunction(L, l_json_encode);
        lua_insert(L, -2);
        cJSON *result = NULL;
        if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
            result = cJSON_Parse(lua_tostring(L, -1));
        }
        call_done(reply, ctx, result_ok(call_payload(result, log_from)));
    }
    lua_settop(L, top);
    if (s_exit_requested) {
        app_stop("app.exit");
    }
}

static void runner_task(void *arg)
{
    (void)arg;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_pending.fn) {
            int64_t left_us = s_pending.due_us - esp_timer_get_time();
            wait = left_us > 0 ? pdMS_TO_TICKS(left_us / 1000) + 1 : 0;
        }
        msg_t m;
        if (xQueueReceive(s_queue, &m, wait) == pdTRUE) {
            switch (m.type) {
            case MSG_RUN:
                app_run(&m);
                free(m.run.script);
                free(m.run.title);
                free(m.run.source);
                break;
            case MSG_STOP: {
                s_abort = false;
                if (m.stop.app && m.stop.app != s_status.app) {
                    break;   /* the X of an app that's already gone */
                }
                bool stopped = app_stop(m.stop.reason);
                if (m.stop.reply) {
                    cJSON *p = cJSON_CreateObject();
                    cJSON_AddBoolToObject(p, "stopped", stopped);
                    m.stop.reply(m.stop.ctx, result_ok(p));
                }
                break;
            }
            case MSG_TIMER:
                if (s_L && s_status.state == APP_RUNNING) {
                    timer_event(s_L, m.timer.id);
                    settle();
                }
                break;
            case MSG_CALL:
                app_call(&m);
                free(m.call.name);
                free(m.call.args);
                break;
            case MSG_UI:
                if (s_L && s_status.state == APP_RUNNING && m.ui.app == s_status.app) {
                    muse_lua_ui_event(s_L, (muse_lua_ui_kind_t)m.ui.kind, m.ui.wid, m.ui.x, m.ui.y);
                    settle();
                }
                break;
            }
        }
        if (s_pending.fn && esp_timer_get_time() >= s_pending.due_us) {
            reply_pending(s_status.state == APP_RUNNING ? "running" : "closed", NULL);
        }
    }
}

/* ---- From other tasks ------------------------------------------------------ */

void muse_lua_post_ui(uint32_t app, muse_lua_ui_kind_t kind, uint32_t wid, int x, int y)
{
    msg_t m;
    if (kind == MUSE_LUA_UI_CLOSE) {
        m = (msg_t){ .type = MSG_STOP, .stop = { .app = app, .reason = "closed on the gadget" } };
        s_abort = true;
    } else {
        m = (msg_t){ .type = MSG_UI, .ui = { .app = app, .wid = wid, .x = (int16_t)x, .y = (int16_t)y, .kind = kind } };
    }
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full: dropped a touch");
    }
}

esp_err_t muse_lua_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    s_log = heap_caps_calloc(LOG_LINES, sizeof(log_line_t), MUSE_BIG_CAPS);
    if (!s_mutex || !s_queue || !s_log) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(s_status.last, "none yet", sizeof(s_status.last));
    if (xTaskCreatePinnedToCoreWithCaps(runner_task, "muse_lua", RUNNER_STACK, NULL, RUNNER_PRIO, NULL, tskNO_AFFINITY,
                                        MUSE_BIG_CAPS) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Lua apps ready (%s)", LUA_RELEASE);
    return ESP_OK;
}

static char *dup_big(const char *s, size_t n)
{
    char *d = heap_caps_malloc(n + 1, MUSE_BIG_CAPS);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static cJSON *status_json(void)
{
    cJSON *p = cJSON_CreateObject();
    static const char *const names[] = { [APP_IDLE] = "idle", [APP_RUNNING] = "running", [APP_ERROR] = "error" };
    lock();
    cJSON_AddStringToObject(p, "state", names[s_status.state]);
    cJSON_AddNumberToObject(p, "app", s_status.app);
    cJSON_AddStringToObject(p, "title", s_status.title);
    if (s_status.source[0]) {
        cJSON_AddStringToObject(p, "source", s_status.source);
    }
    if (s_status.state == APP_RUNNING) {
        cJSON_AddNumberToObject(p, "running_ms", (double)((esp_timer_get_time() - s_status.started_us) / 1000));
        cJSON_AddItemToObject(p, "calls", calls_json_locked());
    }
    if (s_status.error[0]) {
        cJSON_AddStringToObject(p, "error", s_status.error);
    } else {
        cJSON_AddNullToObject(p, "error");
    }
    if (s_status.state == APP_IDLE) {
        cJSON_AddStringToObject(p, "last_ended", s_status.last);
    }
    cJSON_AddItemToObject(p, "logs", logs_json_locked(s_status.app, 0, STATUS_LOGS));
    unlock();
    return p;
}

static cJSON *help_json(void)
{
    int w, h;
    muse_lua_ui_size(&w, &h);
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "api", api_md_start);
    cJSON *screen = cJSON_AddObjectToObject(p, "app_area");
    cJSON_AddNumberToObject(screen, "w", w);
    cJSON_AddNumberToObject(screen, "h", h);
    cJSON *limits = cJSON_AddObjectToObject(p, "limits");
    cJSON_AddNumberToObject(limits, "script_kb", 8);
    cJSON_AddNumberToObject(limits, "memory_kb", MEM_MAX / 1024);
    cJSON_AddNumberToObject(limits, "timers", TIMERS_MAX);
    cJSON_AddStringToObject(p, "lua", LUA_RELEASE);
#if CONFIG_MUSE_CJK_FONT
    cJSON_AddBoolToObject(p, "cjk", true);   /* Chinese and Japanese text shows */
#else
    cJSON_AddBoolToObject(p, "cjk", false);
#endif
    return p;
}

static bool post_or_fail(msg_t *m, muse_lua_reply_fn reply, void *ctx)
{
    s_abort = true;   /* the code running now gives way */
    if (xQueueSend(s_queue, m, pdMS_TO_TICKS(500)) == pdTRUE) {
        return true;
    }
    reply(ctx, result_error("busy", "the app runner is busy", NULL));
    return false;
}

bool muse_lua_command(const char *command, const cJSON *params, muse_lua_reply_fn reply, void *ctx)
{
    if (strncmp(command, "lua.", 4) != 0) {
        return false;
    }
    if (!s_queue) {
        reply(ctx, result_error("unavailable", "Lua apps are still starting", NULL));
        return true;
    }
    if (!strcmp(command, "lua.help")) {
        reply(ctx, result_ok(help_json()));
        return true;
    }
    if (!strcmp(command, "lua.status")) {
        reply(ctx, result_ok(status_json()));
        return true;
    }
    if (!strcmp(command, "lua.stop")) {
        msg_t m = { .type = MSG_STOP, .stop = { .reason = "lua.stop", .reply = reply, .ctx = ctx } };
        post_or_fail(&m, reply, ctx);
        return true;
    }
    if (!strcmp(command, "lua.call")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "args");
        if (!cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring) >= CALL_NAME_MAX) {
            reply(ctx, result_error("missing_param", "name is required: one of the functions the app exposes", NULL));
            return true;
        }
        /* args: a JSON array as a string, or the JSON itself. A string that
         * isn't JSON is one string argument. */
        char *json = NULL;
        if (cJSON_IsString(args)) {
            cJSON *parsed = cJSON_Parse(args->valuestring);
            json = parsed ? cJSON_PrintUnformatted(parsed) : NULL;
            cJSON_Delete(parsed);
            if (!json) {
                cJSON *one = cJSON_CreateArray();
                cJSON_AddItemToArray(one, cJSON_CreateString(args->valuestring));
                json = cJSON_PrintUnformatted(one);
                cJSON_Delete(one);
            }
        } else if (args && !cJSON_IsNull(args)) {
            json = cJSON_PrintUnformatted(args);
        }
        msg_t m = { .type = MSG_CALL };
        m.call.name = dup_big(name->valuestring, strlen(name->valuestring));
        m.call.args = json ? dup_big(json, strlen(json)) : NULL;
        cJSON_free(json);
        m.call.reply = reply;
        m.call.ctx = ctx;
        if (atomic_exchange(&s_call_busy, true)) {
            free(m.call.name);
            free(m.call.args);
            reply(ctx, result_error("busy", "another lua.call is still running: send this one once it has replied",
                                    NULL));
            return true;
        }
        /* Unlike run and stop, a call doesn't interrupt the app: it waits its turn. */
        if (!m.call.name || xQueueSend(s_queue, &m, pdMS_TO_TICKS(500)) != pdTRUE) {
            free(m.call.name);
            free(m.call.args);
            call_done(reply, ctx, result_error("busy", "the app runner is busy", NULL));
        }
        return true;
    }
    if (strcmp(command, "lua.run") != 0) {
        return false;
    }
    const cJSON *script = cJSON_GetObjectItemCaseSensitive(params, "script");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(params, "title");
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(params, "source");
    if (!cJSON_IsString(script) || !script->valuestring[0]) {
        reply(ctx, result_error("missing_param", "script is required: the app's Lua source", NULL));
        return true;
    }
    size_t len = strlen(script->valuestring);
    if (len > MUSE_LUA_SCRIPT_MAX) {
        reply(ctx, result_error("too_long", "the script is too long", NULL));
        return true;
    }
    msg_t m = { .type = MSG_RUN };
    m.run.script = dup_big(script->valuestring, len);
    m.run.len = len;
    m.run.title = cJSON_IsString(title) ? dup_big(title->valuestring, strnlen(title->valuestring, TITLE_MAX - 1)) : NULL;
    m.run.source = cJSON_IsString(source) ? dup_big(source->valuestring, strnlen(source->valuestring, SOURCE_MAX - 1))
                                          : NULL;
    m.run.reply = reply;
    m.run.ctx = ctx;
    if (!m.run.script) {
        free(m.run.title);
        free(m.run.source);
        reply(ctx, result_error("out_of_memory", "no memory for the script", NULL));
        return true;
    }
    if (!post_or_fail(&m, reply, ctx)) {
        free(m.run.script);
        free(m.run.title);
        free(m.run.source);
    }
    return true;
}

/* ---- Console (tools/muse/lua.py) --------------------------------------------- */

static void console_reply(void *ctx, cJSON *result)
{
    (void)ctx;
    char *s = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (s) {
        printf("@lua %s\n", s);
        fflush(stdout);
        cJSON_free(s);
    }
}

static char *s_console_script;
static size_t s_console_len;

static void console_line_error(const char *message)
{
    free(s_console_script);
    s_console_script = NULL;
    console_reply(NULL, result_error("console", message, NULL));
}

bool muse_lua_console(char *line, bool whole)
{
    if (strncmp(line, "lua", 3) != 0) {
        return false;
    }
    const char *cmd = line;
    if (!strcmp(cmd, "lua.stop") || !strcmp(cmd, "lua.status") || !strcmp(cmd, "lua.help")) {
        return muse_lua_command(cmd, NULL, console_reply, NULL);
    }
    if (!strncmp(cmd, "lua.call=", 9)) {
        cJSON *params = cJSON_Parse(cmd + 9);   /* {"name":..., "args":[...]} */
        if (!params) {
            console_reply(NULL, result_error("console", "lua.call= takes {\"name\":..., \"args\":[...]}", NULL));
            return true;
        }
        muse_lua_command("lua.call", params, console_reply, NULL);
        cJSON_Delete(params);
        return true;
    }
    bool last = !strncmp(line, "lua=", 4);
    if (!last && strncmp(line, "lua+=", 5) != 0) {
        return false;
    }
    char *piece = line + (last ? 4 : 5);
    size_t n = muse_hatch_unescape(piece);
    if (!whole) {
        console_line_error("line too long");
        return true;
    }
    if (!s_console_script) {
        s_console_script = heap_caps_malloc(MUSE_LUA_SCRIPT_MAX + 1, MUSE_BIG_CAPS);
        s_console_len = 0;
        if (!s_console_script) {
            console_line_error("out of memory");
            return true;
        }
    }
    if (s_console_len + n > MUSE_LUA_SCRIPT_MAX) {
        console_line_error("script too long");
        return true;
    }
    memcpy(s_console_script + s_console_len, piece, n);
    s_console_len += n;
    s_console_script[s_console_len] = '\0';
    printf("@lua {\"ack\":%u}\n", (unsigned)s_console_len);
    fflush(stdout);
    if (last) {
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "script", s_console_script);
        free(s_console_script);
        s_console_script = NULL;
        muse_lua_command("lua.run", params, console_reply, NULL);
        cJSON_Delete(params);
    }
    return true;
}
