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
 * One request: POST CONFIG_MUSE_TTS_URL with
 *   {"model", "input", "voice", "response_format": "mp3", ["instructions"]}
 * and the MP3 streams back chunked, written to s_mp3 as it arrives. A full
 * buffer holds the socket back, so the decoder sets the pace.
 *
 * Ids hand requests over without a lock on the buffer: s_want is the request
 * last asked for (0 once cancelled), s_active the one whose MP3 is in s_mp3.
 * The task empties s_mp3 before it moves s_active on, and stops writing as soon
 * as s_want moves away, so a reader asking by id never sees another's audio.
 */
#include "muse_tts.h"

#include <stdatomic.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "muse_settings.h"

static const char *TAG = "muse_tts";

#define TEXT_MAX 1024                 /* a reply message's text, as the chat session keeps it */
#define MP3_BYTES (32 * 1024)         /* ~4 s of OpenAI's MP3 between the socket and the decoder */
#define READ_BYTES 2048
#define TIMEOUT_MS 15000
#define SEND_WAIT_MS 100
#define OPENAI_PREFIX "https://api.openai.com/"

static TaskHandle_t s_task;
static StreamBufferHandle_t s_mp3;
static SemaphoreHandle_t s_lock;      /* s_text and s_next */
static char *s_text;                  /* the text for s_want */
static uint32_t s_next;
static atomic_uint s_want;
static atomic_uint s_active;
static atomic_int s_state = MUSE_TTS_DONE;   /* s_active's: RUNNING, DONE or FAILED */

static void key(char out[MUSE_TTS_KEY_MAX + 1])
{
    muse_settings_tts_key(out);
    if (!out[0]) {
        strlcpy(out, CONFIG_MUSE_TTS_API_KEY, MUSE_TTS_KEY_MAX + 1);
    }
}

static void voice(char out[MUSE_TTS_VOICE_MAX + 1])
{
    muse_settings_tts_voice(out);
    if (!out[0]) {
        strlcpy(out, CONFIG_MUSE_TTS_VOICE, MUSE_TTS_VOICE_MAX + 1);
    }
}

static char *request_body(const char *text)
{
    char v[MUSE_TTS_VOICE_MAX + 1];
    voice(v);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", CONFIG_MUSE_TTS_MODEL);
    cJSON_AddStringToObject(req, "input", text);
    cJSON_AddStringToObject(req, "voice", v);
    cJSON_AddStringToObject(req, "response_format", "mp3");
    if (CONFIG_MUSE_TTS_INSTRUCTIONS[0]) {
        cJSON_AddStringToObject(req, "instructions", CONFIG_MUSE_TTS_INSTRUCTIONS);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* Hands `len` bytes to the reader, waiting while the buffer is full. False once replaced. */
static bool pass_on(uint32_t id, const uint8_t *data, size_t len)
{
    while (len) {
        if (atomic_load(&s_want) != id) {
            return false;
        }
        size_t n = xStreamBufferSend(s_mp3, data, len, pdMS_TO_TICKS(SEND_WAIT_MS));
        data += n;
        len -= n;
    }
    return true;
}

/* Sends the request on `c` and streams the reply to the reader. */
static bool exchange(esp_http_client_handle_t c, uint32_t id, const char *body, uint8_t *buf)
{
    int64_t t0 = esp_timer_get_time(), first = 0;
    int len = (int)strlen(body);
    esp_err_t err = esp_http_client_open(c, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't reach %s: %s", CONFIG_MUSE_TTS_URL, esp_err_to_name(err));
        return false;
    }
    if (esp_http_client_write(c, body, len) != len || esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGW(TAG, "request failed");
        return false;
    }
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        int n = esp_http_client_read(c, (char *)buf, 200);
        buf[n > 0 ? n : 0] = '\0';
        ESP_LOGW(TAG, "HTTP %d: %s", status, (char *)buf);
        return false;
    }
    size_t total = 0;
    for (;;) {
        int n = esp_http_client_read(c, (char *)buf, READ_BYTES);
        if (n < 0) {
            ESP_LOGW(TAG, "read failed after %u bytes", (unsigned)total);
            return false;
        }
        if (n == 0) {
            break;
        }
        if (!first) {
            first = esp_timer_get_time();
        }
        total += n;
        if (!pass_on(id, buf, n)) {
            ESP_LOGI(TAG, "request %u replaced after %u bytes", (unsigned)id, (unsigned)total);
            return false;
        }
    }
    if (!esp_http_client_is_complete_data_received(c)) {
        ESP_LOGW(TAG, "cut off after %u bytes", (unsigned)total);
        return false;
    }
    ESP_LOGI(TAG, "%u bytes of MP3: first after %d ms, all in %d ms", (unsigned)total,
             first ? (int)((first - t0) / 1000) : -1, (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

static bool fetch(uint32_t id, const char *text, uint8_t *buf)
{
    char *body = request_body(text);
    if (!body) {
        return false;
    }
    char k[MUSE_TTS_KEY_MAX + 1];
    char auth[sizeof("Bearer ") + MUSE_TTS_KEY_MAX];
    key(k);
    snprintf(auth, sizeof(auth), "Bearer %s", k);

    const esp_http_client_config_t cfg = {
        .url = CONFIG_MUSE_TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_MS,
        .buffer_size = READ_BYTES,
        .buffer_size_tx = 1024,   /* the Authorization header alone is ~180 bytes */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (c) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_header(c, "Accept", "audio/mpeg");
        if (k[0]) {
            esp_http_client_set_header(c, "Authorization", auth);
        }
        ok = exchange(c, id, body, buf);
        esp_http_client_cleanup(c);
    }
    cJSON_free(body);
    return ok;
}

static void tts_task(void *arg)
{
    (void)arg;
    char *text = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *buf = heap_caps_malloc(READ_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text || !buf) {
        ESP_LOGE(TAG, "out of memory");
        vTaskSuspend(NULL);
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t id = atomic_load(&s_want);
        strlcpy(text, s_text, TEXT_MAX);
        xSemaphoreGive(s_lock);
        if (!id || id == atomic_load(&s_active)) {
            continue;   /* cancelled, or already done */
        }
        /* Nothing reads or writes s_mp3 now: the reader only takes s_active's. */
        xStreamBufferReset(s_mp3);
        atomic_store(&s_state, MUSE_TTS_RUNNING);
        atomic_store(&s_active, id);
        bool ok = fetch(id, text, buf);
        atomic_store(&s_state, ok ? MUSE_TTS_DONE : MUSE_TTS_FAILED);
    }
}

esp_err_t muse_tts_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_text = heap_caps_calloc(1, TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mp3 = xStreamBufferCreateWithCaps(MP3_BYTES, 1, MALLOC_CAP_SPIRAM);
    /* Stack in PSRAM, as the chat session's: TLS runs here. Below its priority. */
    if (!s_lock || !s_text || !s_mp3 ||
        xTaskCreatePinnedToCoreWithCaps(tts_task, "muse_tts", 16 * 1024, NULL, 4, &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    char v[MUSE_TTS_VOICE_MAX + 1];
    voice(v);
    ESP_LOGI(TAG, "%s, %s, voice %s%s", CONFIG_MUSE_TTS_URL, CONFIG_MUSE_TTS_MODEL, v,
             muse_tts_configured() ? "" : " (no key: replies are shown, not spoken)");
    return ESP_OK;
}

bool muse_tts_configured(void)
{
    char k[MUSE_TTS_KEY_MAX + 1];
    key(k);
    return s_task && (k[0] || strncmp(CONFIG_MUSE_TTS_URL, OPENAI_PREFIX, strlen(OPENAI_PREFIX)) != 0);
}

uint32_t muse_tts_start(const char *text)
{
    if (!s_task || !text || !text[0]) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_text, text, TEXT_MAX);
    uint32_t id = ++s_next ? s_next : ++s_next;   /* never 0 */
    atomic_store(&s_want, id);
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return id;
}

size_t muse_tts_read(uint32_t id, uint8_t *buf, size_t cap)
{
    if (!id || atomic_load(&s_active) != id || atomic_load(&s_want) != id) {
        return 0;
    }
    return xStreamBufferReceive(s_mp3, buf, cap, 0);
}

muse_tts_status_t muse_tts_status(uint32_t id)
{
    if (!id || atomic_load(&s_want) != id) {
        return MUSE_TTS_FAILED;
    }
    if (atomic_load(&s_active) != id) {
        return MUSE_TTS_PENDING;
    }
    int st = atomic_load(&s_state);
    if (st != MUSE_TTS_RUNNING && !xStreamBufferIsEmpty(s_mp3)) {
        return MUSE_TTS_RUNNING;   /* ended, but there's still some to read */
    }
    return (muse_tts_status_t)st;
}

void muse_tts_cancel(void)
{
    atomic_store(&s_want, 0);
}
