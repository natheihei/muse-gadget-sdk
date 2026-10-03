/*
 * Copyright (c) 2026 Yueyao (natheihei@gmail.com)
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

/*
 * Waveshare ESP32-S3-Touch-LCD-3.5 (and its -C variant): ESP32-S3R8 (8 MB
 * octal PSRAM), 16 MB flash, 3.5" 320x480 ST7796 IPS LCD on SPI with FT6336
 * touch, ES8311 codec with one mic and a speaker header, AXP2101 PMU, TCA9554
 * expander. The LCD's reset is the expander's EXIO1. PWR goes to the AXP2101
 * and, per Waveshare's wiki, to EXIO6; BOOT talks and PWR is read from the
 * PMU's key latch, as on the AMOLED-1.75.
 *
 * Pins and the panel's init sequence are from Waveshare's examples
 * (waveshareteam/ESP32-S3-Touch-LCD-3.5, ESP-IDF/01_factory/components/esp_port)
 * and xiaozhi-esp32's waveshare/esp32-s3-touch-lcd-3.5 board.
 */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "muse_pmu.h"

static const char *TAG = "board";

#define LCD_W 480                  /* landscape, USB-C on the right */
#define LCD_H 320
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_5
#define LCD_MOSI GPIO_NUM_1
#define LCD_DC GPIO_NUM_3
#define LCD_BL GPIO_NUM_6
#define DRAW_BUF_LINES 80       /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_W * 4 * 2)

#define I2C_SDA GPIO_NUM_8
#define I2C_SCL GPIO_NUM_7
#define I2S_MCLK GPIO_NUM_12
#define I2S_BCLK GPIO_NUM_13
#define I2S_WS GPIO_NUM_15
#define I2S_DOUT GPIO_NUM_16
#define I2S_DIN GPIO_NUM_14

#define TALK_GPIO GPIO_NUM_0       /* BOOT */
#define PMU_KEY_EVERY 2            /* poll the PMU for PWR over I2C every 20 ms */

#define EXP_ADDR 0x20              /* TCA9554, A2-A0 low */
#define EXP_REG_OUTPUT 0x01
#define EXP_REG_CONFIG 0x03        /* 1 = input */
#define EXP_LCD_RST BIT(1)

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exp;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk;

/* Waveshare's ST7796 setup for this panel; the driver's own is for another glass. */
static const st7796_lcd_init_cmd_t s_lcd_init[] = {
    { 0x11, NULL, 0, 120 },
    { 0x3A, (uint8_t[]){ 0x05 }, 1, 0 },
    { 0xF0, (uint8_t[]){ 0xC3 }, 1, 0 },
    { 0xF0, (uint8_t[]){ 0x96 }, 1, 0 },
    { 0xB4, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xB7, (uint8_t[]){ 0xC6 }, 1, 0 },
    { 0xC0, (uint8_t[]){ 0x80, 0x45 }, 2, 0 },
    { 0xC1, (uint8_t[]){ 0x13 }, 1, 0 },
    { 0xC2, (uint8_t[]){ 0xA7 }, 1, 0 },
    { 0xC5, (uint8_t[]){ 0x0A }, 1, 0 },
    { 0xE8, (uint8_t[]){ 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33 }, 8, 0 },
    { 0xE0, (uint8_t[]){ 0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31 }, 14, 0 },
    { 0xE1, (uint8_t[]){ 0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32 }, 14, 0 },
    { 0xF0, (uint8_t[]){ 0x3C }, 1, 0 },
    { 0xF0, (uint8_t[]){ 0x69 }, 1, 120 },
    { 0x21, NULL, 0, 0 },
    { 0x29, NULL, 0, 0 },
};

static esp_err_t exp_write(uint8_t reg, uint8_t v)
{
    const uint8_t buf[2] = { reg, v };
    return i2c_master_transmit(s_exp, buf, sizeof(buf), 50);
}

static esp_err_t exp_read(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_exp, &reg, 1, v, 1, 50);
}

/* Pulses the LCD's reset (EXIO1), as Waveshare's examples do; other pins are left alone. */
static esp_err_t lcd_reset(void)
{
    uint8_t out, cfg;
    ESP_RETURN_ON_ERROR(exp_read(EXP_REG_OUTPUT, &out), TAG, "expander not responding");
    ESP_RETURN_ON_ERROR(exp_read(EXP_REG_CONFIG, &cfg), TAG, "expander config");
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_OUTPUT, out & ~EXP_LCD_RST), TAG, "lcd reset low");
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_CONFIG, cfg & ~EXP_LCD_RST), TAG, "lcd reset out");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_OUTPUT, out | EXP_LCD_RST), TAG, "lcd reset high");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t exp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &exp_cfg, &s_exp), TAG, "expander");
    ESP_RETURN_ON_ERROR(lcd_reset(), TAG, "lcd reset");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "boot button");

    /* Only the PMU sees PWR: latch its edges for poll_buttons(). */
    esp_err_t err = muse_pmu_init(s_i2c, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): battery status disabled", esp_err_to_name(err));
        return ESP_OK;
    }
    /* Keep DCDC1 (VCC3V3) and ALDO1. xiaozhi's board, which also runs the
     * camera, keeps BLDO1 and BLDO2 on as well. */
    err = muse_pmu_keep_rails(BIT(0), BIT(0));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unused rails left on (%s)", esp_err_to_name(err));
    }
    /* The MX1.25 battery has no thermistor, and Waveshare's examples say the
     * TS pin must not be measured then or charging goes wrong. Their charger
     * settings too, which the PMU forgets whenever it loses power. */
    err = muse_pmu_set_charger(200, 4100, 50, 25, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "charger left at its defaults (%s)", esp_err_to_name(err));
    }
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = GPIO_NUM_NC,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000,   /* Waveshare's clock for this panel */
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    st7796_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,   /* on the expander, pulsed in init() */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_st7796(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* Some panels stay black after a cold power-up until the chip restarts
     * once with the panel already set up; Waveshare and xiaozhi both do this. */
    if (esp_reset_reason() == ESP_RST_POWERON) {
        ESP_LOGI(TAG, "cold start: restarting once for the panel");
        esp_restart();
    }
    esp_lcd_panel_invert_color(s_panel, true);
    /* The panel scans 320x480 portrait. Swapping its axes turns it a quarter
     * turn: landscape with USB-C on the right, as xiaozhi's board draws it. */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_W,
            .ver_res = LCD_H,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }

    /* The FT6336 answers as an FT5x06; its reset and interrupt aren't wired. */
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        return NULL;
    }
    /* It reports portrait points; mirroring x and then swapping the axes
     * (the driver's order) turns them with the panel. */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H,
        .y_max = LCD_W,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .flags = { .swap_xy = 1, .mirror_x = 1 },
    };
    esp_lcd_touch_handle_t tp;
    if (esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &tp) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t lv_tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    *touch = esp_lv_adapter_register_touch(&lv_tp_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void send_sleep(void *sleep)
{
    esp_lcd_panel_io_tx_param(s_io, *(bool *)sleep ? 0x10 : 0x11, NULL, 0);   /* SLPIN / SLPOUT */
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* The FT6336 keeps scanning: with no reset or interrupt line, nothing could
 * wake it from hibernate. */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/* One ES8311 does both directions over a duplex I2S bus with MCLK, as Waveshare's example runs it. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,   /* Waveshare's examples drive no amp enable */
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    static unsigned tick;
    unsigned ev = muse_gpio_button_poll(&s_talk);   /* BOOT talks, PWR is aux */
    if (tick++ % PMU_KEY_EVERY == 0) {
        unsigned key = muse_pmu_poll_key();
        ev |= (key & MUSE_PMU_KEY_PRESS ? MUSE_BTN_AUX_PRESS : 0) |
              (key & MUSE_PMU_KEY_RELEASE ? MUSE_BTN_AUX_RELEASE : 0);
    }
    return ev;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-3.5",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = true,
    .diagonal_in = 3.5f,
    .talk_button = "boot",
    .aux_button = "pwr",
    /* BOOT, RST and PWR run along the top edge, left to right, with USB-C on
     * the right. From Waveshare's drawing: 54, 63 and 71 mm across the 92 mm
     * board, whose screen starts 9.5 mm in at 6.5 px/mm, so BOOT is at x 292
     * and PWR at 403. */
    .talk_hint = { LV_ALIGN_TOP_MID, 52, 5 },   /* on the header row */
    .aux_hint = { LV_ALIGN_TOP_MID, 163, 5 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    /* No wait_buttons: PWR is on the PMU, so it's polled. */
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
