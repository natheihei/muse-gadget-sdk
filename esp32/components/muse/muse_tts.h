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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Text-to-speech over OpenAI's POST /v1/audio/speech, or any server that takes
 * the same request (CONFIG_MUSE_TTS_URL). One request at a time, on a task of
 * its own so the chat session's loop never blocks on it: the MP3 comes back
 * through a stream buffer that's read without waiting.
 */

typedef enum {
    MUSE_TTS_PENDING,   /* queued, not started */
    MUSE_TTS_RUNNING,   /* MP3 arriving, or some still to read */
    MUSE_TTS_DONE,      /* all of it arrived and has been read */
    MUSE_TTS_FAILED,    /* stopped early (or replaced); whatever came has been read */
} muse_tts_status_t;

esp_err_t muse_tts_init(void);

/* A request can go: there's a key, or the endpoint isn't OpenAI's. */
bool muse_tts_configured(void);

/* Asks for `text` spoken, replacing any request in progress. Returns the
 * request's id, 0 if it couldn't be queued. */
uint32_t muse_tts_start(const char *text);

/* Up to `cap` bytes of request `id`'s MP3; 0 if none is waiting. Doesn't block. */
size_t muse_tts_read(uint32_t id, uint8_t *buf, size_t cap);

muse_tts_status_t muse_tts_status(uint32_t id);

/* Stops the request in progress. */
void muse_tts_cancel(void);

#ifdef __cplusplus
}
#endif
