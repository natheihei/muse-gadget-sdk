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

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "lvgl.h"

#include "muse_board.h"
#include "muse_input.h"
#include "muse_lua_priv.h"
#include "muse_mem.h"
#include "muse_pixel.h"
#include "muse_state.h"
#include "muse_ui.h"

static const char *TAG = "muse_lua_ui";

/*
 * A layer over everything on the screen: a top bar the firmware owns (the
 * app's title, a mic that also talks while held, Muse's state and an X that
 * closes the app) over the app's area, where its widgets go.
 *
 * Widgets are made and changed only by the runner, holding the display lock,
 * and only once the Lua side of a call is done: an error raised while holding
 * the lock would leave it held. LVGL's event callbacks only queue events for
 * the runner (muse_lua_post_ui).
 */

#define BAR_H 36
#define MIC_PX 20
#define CLOSE_W 56
#define WIDGETS_MAX 200
#define LOCK_MS 2000
#define WIDGET_MT "muse.widget"

#define COLOR_BAR 0x140f22
#define COLOR_TITLE 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_TRACK 0x2a2540
#define COLOR_FILL 0x7c5cff
#define COLOR_ERROR 0xff8a80

typedef enum { W_LABEL, W_BUTTON, W_RECT, W_BAR, W_ARC } widget_kind_t;

typedef struct {
    uint32_t wid;      /* 0: free */
    lv_obj_t *obj;
    lv_obj_t *text;    /* a button's label */
    uint8_t kind;
} slot_t;

typedef struct {
    uint32_t wid;
    uint16_t slot;
} widget_ud_t;

/* A widget's fields, read from Lua before taking the display lock. */
typedef struct {
    bool has_x, has_y, has_w, has_h, has_text, has_size, has_color, has_bg;
    bool has_align, has_radius, has_value, has_width, has_hidden;
    int x, y, w, h, size, radius, width;
    uint32_t color, bg;
    lv_text_align_t align;
    float value;
    bool hidden;
    const char *text;   /* on the Lua stack, cleaned up (see clean_text) */
} props_t;

/* Runner task, and LVGL's for what its callbacks read. */
static lv_obj_t *s_layer, *s_bar, *s_title, *s_mic, *s_state, *s_close, *s_area;
static lv_timer_t *s_bar_timer;
static volatile uint32_t s_app;
static EXT_RAM_BSS_ATTR slot_t s_slots[WIDGETS_MAX];
static uint32_t s_next_wid = 1;
static int s_callbacks = LUA_NOREF;   /* wid -> on_press */
static int s_on_tap = LUA_NOREF;
static int s_on_swipe = LUA_NOREF;

/* LVGL task only. */
static bool s_gestured;     /* this touch was a swipe: not a tap or a press too */
static bool s_mic_held;
static int s_shown_mode = -1;

static bool display_lock(void)
{
    return muse_board->display_lock(LOCK_MS);
}

void muse_lua_ui_size(int *w, int *h)
{
    *w = muse_board->width;
    *h = muse_board->height - BAR_H;
}

/* ---- Text --------------------------------------------------------------------- */

/* Montserrat as built in has ASCII, the degree sign and the bullet. Typography
 * a recipe is likely to have becomes ASCII; anything else is left alone. */
static const char *ascii_for(uint32_t cp)
{
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return "-";
    case 0x2026: return "...";
    case 0x00BD: return "1/2";
    case 0x00BC: return "1/4";
    case 0x00BE: return "3/4";
    case 0x2153: return "1/3";
    case 0x2154: return "2/3";
    case 0x215B: return "1/8";
    case 0x00D7: return "x";
    case 0x00A0: case 0x2009: case 0x202F: return " ";
    case 0x00BA: return "\xC2\xB0";   /* the ordinal o, often typed for degrees */
    default: return NULL;
    }
}

/* Pushes the string at idx with ascii_for() applied; returns it. */
static const char *clean_text(lua_State *L, int idx)
{
    size_t len;
    const unsigned char *s = (const unsigned char *)luaL_tolstring(L, idx, &len);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (size_t i = 0; i < len;) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + n > len) {
            n = 1;
        }
        uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n; k++) {
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        const char *r = n > 1 ? ascii_for(cp) : NULL;
        if (r) {
            luaL_addstring(&b, r);
        } else {
            luaL_addlstring(&b, (const char *)s + i, n);
        }
        i += n;
    }
    luaL_pushresult(&b);
    lua_remove(L, -2);   /* tolstring's copy */
    return lua_tostring(L, -1);
}

#if CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
#endif

/* The Montserrat closest to px. With CONFIG_MUSE_CJK_FONT, a copy that falls
 * back to Unifont's 16x16 CJK (as captions do, muse_ui.c): Chinese and
 * Japanese show at 16 px whatever the size. */
static const lv_font_t *font_for(int px)
{
    static const struct {
        int px;
        const lv_font_t *font;
    } fonts[] = {
        { 14, &lv_font_montserrat_14 },
        { 16, &lv_font_montserrat_16 },
        { 20, &lv_font_montserrat_20 },
        { 28, &lv_font_montserrat_28 },
#if LV_FONT_MONTSERRAT_48
        { 48, &lv_font_montserrat_48 },
#endif
    };
    enum { N = sizeof(fonts) / sizeof(fonts[0]) };
    size_t best = 0;
    for (size_t i = 1; i < N; i++) {
        if (abs(fonts[i].px - px) < abs(fonts[best].px - px)) {
            best = i;
        }
    }
#if CONFIG_MUSE_CJK_FONT
    static lv_font_t copies[N];
    if (!copies[best].get_glyph_dsc) {
        copies[best] = *fonts[best].font;
        copies[best].fallback = &muse_font_cjk_16;
    }
    return &copies[best];
#else
    return fonts[best].font;
#endif
}

/* ---- The layer ---------------------------------------------------------------- */

static void on_close(lv_event_t *e)
{
    (void)e;
    muse_lua_post_ui(s_app, MUSE_LUA_UI_CLOSE, 0, 0, 0);
}

static void on_mic(lv_event_t *e)
{
    bool down = lv_event_get_code(e) == LV_EVENT_PRESSED;
    if (down != s_mic_held) {
        s_mic_held = down;
        muse_input_touch_talk(down);
    }
}

/* The mic lights up while Muse listens; Muse's state beside it. */
static void bar_tick(lv_timer_t *t)
{
    (void)t;
    muse_mode_t mode = muse_state_mode(NULL);
    if ((int)mode == s_shown_mode) {
        return;
    }
    s_shown_mode = (int)mode;
    muse_ui_mic_color(s_mic, mode == MUSE_MODE_LISTENING ? muse_pixel_accent(mode) : COLOR_DIM);
    const char *text = mode == MUSE_MODE_LISTENING ? "LISTENING"
                     : mode == MUSE_MODE_THINKING  ? "THINKING"
                     : mode == MUSE_MODE_SPEAKING  ? "SPEAKING"
                                                   : "";
    lv_label_set_text(s_state, text);
    lv_obj_set_style_text_color(s_state, lv_color_hex(muse_pixel_accent(mode)), 0);
}

static void on_area(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_gestured = false;
        break;
    case LV_EVENT_GESTURE:
        if (indev && !s_gestured) {
            s_gestured = true;
            lv_indev_wait_release(indev);
            muse_lua_post_ui(s_app, MUSE_LUA_UI_SWIPE, 0, lv_indev_get_gesture_dir(indev), 0);
        }
        break;
    case LV_EVENT_CLICKED:
        if (indev && !s_gestured) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            lv_area_t a;
            lv_obj_get_coords(s_area, &a);
            muse_lua_post_ui(s_app, MUSE_LUA_UI_TAP, 0, p.x - a.x1, p.y - a.y1);
        }
        break;
    default:
        break;
    }
}

static void on_button(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_gestured = false;
        break;
    case LV_EVENT_CLICKED:
        if (!s_gestured) {
            muse_lua_post_ui(s_app, MUSE_LUA_UI_PRESS, (uint32_t)(uintptr_t)lv_event_get_user_data(e), 0, 0);
        }
        break;
    default:
        break;
    }
}

static lv_obj_t *plain_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void build_layer(void)
{
    int w = muse_board->width, h = muse_board->height;

    /* Clickable, so no touch falls through to the face or settings. */
    s_layer = plain_obj(lv_screen_active());
    lv_obj_set_size(s_layer, w, h);
    lv_obj_set_style_bg_color(s_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_CLICKABLE);

    s_bar = plain_obj(s_layer);
    lv_obj_set_size(s_bar, w, BAR_H);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_BAR), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_CLICKABLE);

    /* The mic where Muse's own header has it, by the talk button. */
    const muse_button_hint_t *t = &muse_board->talk_hint;
    s_mic = muse_ui_make_mic(s_bar, MIC_PX);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_mic); i++) {
        lv_obj_remove_flag(lv_obj_get_child(s_mic, i), LV_OBJ_FLAG_CLICKABLE);   /* presses go to the mic */
    }
    lv_obj_add_flag(s_mic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_mic, 14);
    lv_obj_add_event_cb(s_mic, on_mic, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_mic, on_mic, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_mic, on_mic, LV_EVENT_PRESS_LOST, NULL);
    if (t->align == LV_ALIGN_TOP_MID || t->align == LV_ALIGN_TOP_LEFT || t->align == LV_ALIGN_TOP_RIGHT) {
        lv_obj_align(s_mic, t->align, t->x, (BAR_H - MIC_PX) / 2);
    } else {
        lv_obj_align(s_mic, LV_ALIGN_RIGHT_MID, -CLOSE_W - 100, 0);
    }
    muse_ui_mic_color(s_mic, COLOR_DIM);

    s_title = lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_title, font_for(20), 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(COLOR_TITLE), 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_update_layout(s_bar);
    int title_w = lv_obj_get_x(s_mic) - 12 - 16;
    lv_obj_set_width(s_title, title_w > 40 ? title_w : 40);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 12, 0);
    lv_label_set_text(s_title, "");

    s_state = lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_state, font_for(14), 0);
    lv_label_set_text(s_state, "");
    lv_obj_align_to(s_state, s_mic, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    s_close = plain_obj(s_bar);
    lv_obj_set_size(s_close, CLOSE_W, BAR_H);
    lv_obj_align(s_close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(s_close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_close, on_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x = lv_label_create(s_close);
    lv_obj_set_style_text_font(x, font_for(20), 0);
    lv_obj_set_style_text_color(x, lv_color_hex(COLOR_TITLE), 0);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);

    s_area = plain_obj(s_layer);
    lv_obj_set_pos(s_area, 0, BAR_H);
    lv_obj_set_size(s_area, w, h - BAR_H);
    lv_obj_set_style_bg_color(s_area, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_area, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_area, on_area, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_area, on_area, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_area, on_area, LV_EVENT_CLICKED, NULL);

    s_shown_mode = -1;
    s_bar_timer = lv_timer_create(bar_tick, 100, NULL);
    bar_tick(NULL);
}

static void forget_widgets(void)
{
    memset(s_slots, 0, sizeof(s_slots));
}

bool muse_lua_ui_open(uint32_t app, const char *title)
{
    if (!display_lock()) {
        return false;
    }
    if (!s_layer) {
        build_layer();
    } else {
        lv_obj_clean(s_area);
        lv_obj_set_style_bg_color(s_area, lv_color_black(), 0);
    }
    lv_label_set_text(s_title, title ? title : "");
    s_app = app;
    muse_ui_set_covered(true);
    muse_board->display_unlock();
    forget_widgets();
    muse_state_set_keep_awake(true);
    muse_state_set_asleep(false);
    muse_state_poke();
    return true;
}

void muse_lua_ui_title(const char *title)
{
    if (s_layer && display_lock()) {
        lv_label_set_text(s_title, title);
        muse_board->display_unlock();
    }
}

void muse_lua_ui_error(const char *message)
{
    if (!s_layer || !display_lock()) {
        return;
    }
    lv_obj_clean(s_area);
    lv_obj_set_style_bg_color(s_area, lv_color_black(), 0);
    int w = muse_board->width - 32;
    lv_obj_t *head = lv_label_create(s_area);
    lv_obj_set_style_text_font(head, font_for(20), 0);
    lv_obj_set_style_text_color(head, lv_color_hex(COLOR_ERROR), 0);
    lv_label_set_text(head, "This app stopped");
    lv_obj_set_pos(head, 16, 14);
    lv_obj_t *body = lv_label_create(s_area);
    lv_obj_set_style_text_font(body, font_for(14), 0);
    lv_obj_set_style_text_color(body, lv_color_hex(COLOR_TITLE), 0);
    lv_obj_set_width(body, w);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(body, message);
    lv_obj_set_pos(body, 16, 48);
    lv_obj_t *hint = lv_label_create(s_area);
    lv_obj_set_style_text_font(hint, font_for(14), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_DIM), 0);
    lv_label_set_text(hint, "Ask Muse to fix it, or tap " LV_SYMBOL_CLOSE " to close.");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 16, -14);
    muse_board->display_unlock();
    forget_widgets();
    muse_state_set_keep_awake(false);
}

void muse_lua_ui_close(void)
{
    if (!s_layer || !display_lock()) {
        return;
    }
    lv_timer_delete(s_bar_timer);
    lv_obj_delete(s_layer);
    s_layer = s_bar = s_title = s_mic = s_state = s_close = s_area = NULL;
    s_bar_timer = NULL;
    s_app = 0;
    if (s_mic_held) {
        s_mic_held = false;
        muse_input_touch_talk(false);
    }
    muse_ui_set_covered(false);
    muse_board->display_unlock();
    forget_widgets();
    muse_state_set_keep_awake(false);
    muse_state_poke();   /* Muse's face gets its usual time before sleeping */
}

/* ---- The ui library -------------------------------------------------------- */

static bool get_number(lua_State *L, int t, const char *k, int *out)
{
    lua_getfield(L, t, k);
    bool has = !lua_isnil(L, -1);
    if (has) {
        if (!lua_isnumber(L, -1)) {
            luaL_error(L, "%s must be a number, not %s", k, luaL_typename(L, -1));
        }
        *out = (int)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    return has;
}

/* Reads a widget's fields from the table at t. Leaves the cleaned-up text, if
 * any, on the stack: the caller keeps it there until done. */
static void read_props(lua_State *L, int t, props_t *p)
{
    memset(p, 0, sizeof(*p));
    if (lua_isnoneornil(L, t)) {
        return;
    }
    luaL_checktype(L, t, LUA_TTABLE);
    p->has_x = get_number(L, t, "x", &p->x);
    p->has_y = get_number(L, t, "y", &p->y);
    p->has_w = get_number(L, t, "w", &p->w);
    p->has_h = get_number(L, t, "h", &p->h);
    p->has_size = get_number(L, t, "size", &p->size);
    p->has_radius = get_number(L, t, "radius", &p->radius);
    p->has_width = get_number(L, t, "width", &p->width);
    int v;
    if ((p->has_color = get_number(L, t, "color", &v))) {
        p->color = (uint32_t)v & 0xFFFFFF;
    }
    if ((p->has_bg = get_number(L, t, "bg", &v))) {
        p->bg = (uint32_t)v & 0xFFFFFF;
    }
    lua_getfield(L, t, "value");
    if ((p->has_value = !lua_isnil(L, -1))) {
        p->value = (float)luaL_checknumber(L, -1);
        p->value = p->value < 0 ? 0 : p->value > 100 ? 100 : p->value;
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "hidden");
    if ((p->has_hidden = !lua_isnil(L, -1))) {
        p->hidden = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "align");
    if ((p->has_align = !lua_isnil(L, -1))) {
        const char *a = luaL_checkstring(L, -1);
        p->align = !strcmp(a, "center") ? LV_TEXT_ALIGN_CENTER
                 : !strcmp(a, "right")  ? LV_TEXT_ALIGN_RIGHT
                 : !strcmp(a, "left")   ? LV_TEXT_ALIGN_LEFT
                                        : (luaL_error(L, "align is \"left\", \"center\" or \"right\""), 0);
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "text");
    if ((p->has_text = !lua_isnil(L, -1))) {
        p->text = clean_text(L, -1);   /* replaces the field on the stack */
    } else {
        lua_pop(L, 1);
        lua_pushnil(L);   /* the slot the caller pops either way */
    }
}

static void style_text(lv_obj_t *o, const props_t *p, bool create)
{
    if (create || p->has_size) {
        lv_obj_set_style_text_font(o, font_for(p->has_size ? p->size : 20), 0);
    }
    if (create || p->has_color) {
        lv_obj_set_style_text_color(o, lv_color_hex(p->has_color ? p->color : 0xFFFFFF), 0);
    }
    if (p->has_align) {
        lv_obj_set_style_text_align(o, p->align, 0);
    }
}

/* Applies the fields to a widget; on creation, the defaults for the rest. */
static void apply(slot_t *s, const props_t *p, bool create)
{
    lv_obj_t *o = s->obj;
    if (p->has_x || p->has_y || create) {
        lv_obj_set_pos(o, p->has_x ? p->x : lv_obj_get_x(o), p->has_y ? p->y : lv_obj_get_y(o));
    }
    switch (s->kind) {
    case W_LABEL:
        style_text(o, p, create);
        if (p->has_w) {
            lv_obj_set_width(o, p->w);
            lv_label_set_long_mode(o, p->has_h ? LV_LABEL_LONG_MODE_DOTS : LV_LABEL_LONG_MODE_WRAP);
        }
        if (p->has_h) {
            lv_obj_set_height(o, p->h);
        }
        if (p->has_bg) {
            lv_obj_set_style_bg_color(o, lv_color_hex(p->bg), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        }
        if (p->has_text || create) {
            lv_label_set_text(o, p->has_text ? p->text : "");
        }
        break;
    case W_BUTTON:
        style_text(s->text, p, create);
        if (create || p->has_w || p->has_h) {
            lv_obj_set_size(o, p->has_w ? p->w : (create ? 140 : lv_obj_get_width(o)),
                            p->has_h ? p->h : (create ? 48 : lv_obj_get_height(o)));
        }
        if (create || p->has_bg) {
            lv_obj_set_style_bg_color(o, lv_color_hex(p->has_bg ? p->bg : COLOR_TRACK), 0);
        }
        if (create || p->has_radius) {
            lv_obj_set_style_radius(o, p->has_radius ? p->radius : 10, 0);
        }
        if (p->has_text || create) {
            lv_label_set_text(s->text, p->has_text ? p->text : "");
        }
        break;
    case W_RECT:
        if (create || p->has_w || p->has_h) {
            lv_obj_set_size(o, p->has_w ? p->w : (create ? 100 : lv_obj_get_width(o)),
                            p->has_h ? p->h : (create ? 100 : lv_obj_get_height(o)));
        }
        if (create || p->has_color) {
            lv_obj_set_style_bg_color(o, lv_color_hex(p->has_color ? p->color : 0xFFFFFF), 0);
        }
        if (p->has_radius) {
            lv_obj_set_style_radius(o, p->radius, 0);
        }
        break;
    case W_BAR:
        if (create || p->has_w || p->has_h) {
            lv_obj_set_size(o, p->has_w ? p->w : (create ? 200 : lv_obj_get_width(o)),
                            p->has_h ? p->h : (create ? 16 : lv_obj_get_height(o)));
        }
        if (create || p->has_color) {
            lv_obj_set_style_bg_color(o, lv_color_hex(p->has_color ? p->color : COLOR_FILL), LV_PART_INDICATOR);
        }
        if (create || p->has_bg) {
            lv_obj_set_style_bg_color(o, lv_color_hex(p->has_bg ? p->bg : COLOR_TRACK), LV_PART_MAIN);
        }
        if (p->has_radius) {
            lv_obj_set_style_radius(o, p->radius, LV_PART_MAIN);
            lv_obj_set_style_radius(o, p->radius, LV_PART_INDICATOR);
        }
        if (create || p->has_value) {
            lv_bar_set_value(o, (int32_t)(p->value * 10), LV_ANIM_OFF);
        }
        break;
    case W_ARC:
        if (create || p->has_w || p->has_h) {
            lv_obj_set_size(o, p->has_w ? p->w : (create ? 120 : lv_obj_get_width(o)),
                            p->has_h ? p->h : (create ? 120 : lv_obj_get_height(o)));
        }
        if (create || p->has_color) {
            lv_obj_set_style_arc_color(o, lv_color_hex(p->has_color ? p->color : COLOR_FILL), LV_PART_INDICATOR);
        }
        if (create || p->has_bg) {
            lv_obj_set_style_arc_color(o, lv_color_hex(p->has_bg ? p->bg : COLOR_TRACK), LV_PART_MAIN);
        }
        if (create || p->has_width) {
            int width = p->has_width ? p->width : 12;
            lv_obj_set_style_arc_width(o, width, LV_PART_MAIN);
            lv_obj_set_style_arc_width(o, width, LV_PART_INDICATOR);
        }
        if (create || p->has_value) {
            lv_arc_set_value(o, (int32_t)(p->value * 10));
        }
        break;
    }
    if (p->has_hidden) {
        lv_obj_set_flag(o, LV_OBJ_FLAG_HIDDEN, p->hidden);
    }
}

static lv_obj_t *create_obj(widget_kind_t kind, uint32_t wid, lv_obj_t **text)
{
    lv_obj_t *o;
    *text = NULL;
    switch (kind) {
    case W_LABEL:
        o = lv_label_create(s_area);
        break;
    case W_BUTTON:
        o = lv_button_create(s_area);
        lv_obj_set_style_shadow_width(o, 0, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* a swipe starting on it still counts */
        lv_obj_add_event_cb(o, on_button, LV_EVENT_PRESSED, (void *)(uintptr_t)wid);
        lv_obj_add_event_cb(o, on_button, LV_EVENT_CLICKED, (void *)(uintptr_t)wid);
        *text = lv_label_create(o);
        lv_obj_center(*text);
        break;
    case W_RECT:
        o = plain_obj(s_area);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        break;
    case W_BAR:
        o = lv_bar_create(s_area);
        lv_bar_set_range(o, 0, 1000);
        break;
    case W_ARC:
    default:
        o = lv_arc_create(s_area);
        lv_arc_set_rotation(o, 270);
        lv_arc_set_bg_angles(o, 0, 360);
        lv_arc_set_range(o, 0, 1000);
        lv_obj_remove_style(o, NULL, LV_PART_KNOB);
        lv_obj_set_style_arc_rounded(o, false, LV_PART_INDICATOR);
        break;
    }
    if (kind != W_BUTTON) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);   /* taps go to the area */
    }
    return o;
}

static void callbacks_table(lua_State *L)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_callbacks);
}

static int make_widget(lua_State *L, widget_kind_t kind)
{
    if (!s_area) {
        return luaL_error(L, "the app's screen is gone");
    }
    props_t p;
    read_props(L, 1, &p);   /* pushes the text slot */
    bool press = false;
    if (kind == W_BUTTON && lua_istable(L, 1)) {
        lua_getfield(L, 1, "on_press");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TFUNCTION);
            press = true;
        }
        lua_pop(L, 1);
    }
    int slot = -1;
    for (int i = 0; i < WIDGETS_MAX; i++) {
        if (!s_slots[i].wid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return luaL_error(L, "too many widgets (%d): change them with :set{} rather than making new ones",
                          WIDGETS_MAX);
    }
    uint32_t wid = s_next_wid++;
    widget_ud_t *ud = lua_newuserdatauv(L, sizeof(*ud), 0);
    ud->wid = wid;
    ud->slot = (uint16_t)slot;
    luaL_setmetatable(L, WIDGET_MT);
    if (press) {
        callbacks_table(L);
        lua_getfield(L, 1, "on_press");
        lua_rawseti(L, -2, wid);
        lua_pop(L, 1);
    }

    /* Nothing below raises. */
    if (!display_lock()) {
        return luaL_error(L, "the display is busy");
    }
    slot_t *s = &s_slots[slot];
    s->kind = kind;
    s->obj = create_obj(kind, wid, &s->text);
    s->wid = wid;
    apply(s, &p, true);
    muse_board->display_unlock();
    return 1;
}

static int l_label(lua_State *L) { return make_widget(L, W_LABEL); }
static int l_button(lua_State *L) { return make_widget(L, W_BUTTON); }
static int l_rect(lua_State *L) { return make_widget(L, W_RECT); }
static int l_bar(lua_State *L) { return make_widget(L, W_BAR); }
static int l_arc(lua_State *L) { return make_widget(L, W_ARC); }

static slot_t *check_widget(lua_State *L)
{
    widget_ud_t *ud = luaL_checkudata(L, 1, WIDGET_MT);
    slot_t *s = &s_slots[ud->slot];
    if (s->wid != ud->wid || !s->obj) {
        luaL_error(L, "this widget was deleted (by w:delete() or ui.clear())");
    }
    return s;
}

static int l_widget_set(lua_State *L)
{
    slot_t *s = check_widget(L);
    props_t p;
    read_props(L, 2, &p);
    if (!display_lock()) {
        return luaL_error(L, "the display is busy");
    }
    apply(s, &p, false);
    muse_board->display_unlock();
    return 0;
}

static int l_widget_delete(lua_State *L)
{
    widget_ud_t *ud = luaL_checkudata(L, 1, WIDGET_MT);
    slot_t *s = &s_slots[ud->slot];
    if (s->wid != ud->wid || !s->obj) {
        return 0;
    }
    callbacks_table(L);
    lua_pushnil(L);
    lua_rawseti(L, -2, s->wid);
    lua_pop(L, 1);
    if (!display_lock()) {
        return luaL_error(L, "the display is busy");
    }
    lv_obj_delete(s->obj);
    muse_board->display_unlock();
    memset(s, 0, sizeof(*s));
    return 0;
}

static int l_clear(lua_State *L)
{
    uint32_t bg = (uint32_t)luaL_optinteger(L, 1, 0) & 0xFFFFFF;
    lua_newtable(L);
    lua_rawseti(L, LUA_REGISTRYINDEX, s_callbacks);
    if (!s_area) {
        return 0;
    }
    if (!display_lock()) {
        return luaL_error(L, "the display is busy");
    }
    lv_obj_clean(s_area);
    lv_obj_set_style_bg_color(s_area, lv_color_hex(bg), 0);
    muse_board->display_unlock();
    forget_widgets();
    return 0;
}

static int set_handler(lua_State *L, int *ref)
{
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, *ref);
    *ref = LUA_NOREF;
    if (!lua_isnoneornil(L, 1)) {
        lua_pushvalue(L, 1);
        *ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static int l_on_tap(lua_State *L) { return set_handler(L, &s_on_tap); }
static int l_on_swipe(lua_State *L) { return set_handler(L, &s_on_swipe); }

void muse_lua_ui_install(lua_State *L)
{
    static const luaL_Reg widget[] = { { "set", l_widget_set }, { "delete", l_widget_delete }, { NULL, NULL } };
    luaL_newmetatable(L, WIDGET_MT);
    lua_newtable(L);
    luaL_setfuncs(L, widget, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_newtable(L);
    s_callbacks = luaL_ref(L, LUA_REGISTRYINDEX);
    s_on_tap = s_on_swipe = LUA_NOREF;

    static const luaL_Reg ui[] = {
        { "label", l_label }, { "button", l_button }, { "rect", l_rect }, { "bar", l_bar },
        { "arc", l_arc },     { "clear", l_clear },   { "on_tap", l_on_tap }, { "on_swipe", l_on_swipe },
        { NULL, NULL },
    };
    lua_newtable(L);
    luaL_setfuncs(L, ui, 0);
    int w, h;
    muse_lua_ui_size(&w, &h);
    lua_pushinteger(L, w);
    lua_setfield(L, -2, "W");
    lua_pushinteger(L, h);
    lua_setfield(L, -2, "H");
    lua_setglobal(L, "ui");
}

void muse_lua_ui_forget(void)
{
    s_callbacks = s_on_tap = s_on_swipe = LUA_NOREF;
    forget_widgets();
}

void muse_lua_ui_event(lua_State *L, muse_lua_ui_kind_t kind, uint32_t wid, int x, int y)
{
    switch (kind) {
    case MUSE_LUA_UI_PRESS:
        callbacks_table(L);
        lua_rawgeti(L, -1, wid);
        lua_remove(L, -2);
        if (lua_isfunction(L, -1)) {
            muse_lua_call(L, 0);
        } else {
            lua_pop(L, 1);
        }
        break;
    case MUSE_LUA_UI_TAP:
        if (s_on_tap != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, s_on_tap);
            lua_pushinteger(L, x);
            lua_pushinteger(L, y);
            muse_lua_call(L, 2);
        }
        break;
    case MUSE_LUA_UI_SWIPE:
        if (s_on_swipe != LUA_NOREF) {
            const char *dir = x == LV_DIR_LEFT ? "left" : x == LV_DIR_RIGHT ? "right" : x == LV_DIR_TOP ? "up" : "down";
            lua_rawgeti(L, LUA_REGISTRYINDEX, s_on_swipe);
            lua_pushstring(L, dir);
            muse_lua_call(L, 1);
        }
        break;
    default:
        break;
    }
    ESP_LOGD(TAG, "event %d done", kind);
}
