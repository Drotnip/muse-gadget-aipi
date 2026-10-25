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

#include "csi_sentry.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"

#include "muse_chat.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "wifi_mgr.h"

static const char *TAG = "link.csi_sentry";

#define CSI_MAX_LEN   512   // big enough for any HT20/HT40 CSI buffer
#define QUEUE_LEN     32
#define MAX_PAIRS     192   // max I/Q pairs tracked per frame
#define CALIB_SECONDS 20
#define PING_INTERVAL_MS 50 // 20 Hz: steadier on power and the shared radio
                            // than the standalone project's 50 Hz, still
                            // ~30 CSI frames/s with the AP's beacons

#define ALERT_SUSTAIN_US (2 * 1000000LL)     // motion this long before alerting
#define ALERT_COOLDOWN_US (60 * 1000000LL)   // at most one chat alert a minute
#define ALERT_CAPTION_S 12
#define CALIB_CAPTION_S (CALIB_SECONDS + 10)
#define READY_CAPTION_S 6
#define NO_SIGNAL_AFTER_US (5 * 1000000LL)   // frames stalled this long: say so
#define NO_SIGNAL_CAPTION_S 10

typedef enum {
    SENTRY_EMPTY = 0,
    SENTRY_PRESENT,      // someone in the room, not moving
    SENTRY_MOTION,       // active movement right now
    SENTRY_CALIBRATING,  // learning the empty-room baseline
    SENTRY_NO_WIFI,
    SENTRY_OFF,          // turned off in settings
} sentry_state_t;

typedef struct {
    uint16_t len;
    int8_t rssi;
    bool skip_first4;
    int8_t buf[CSI_MAX_LEN];
} csi_frame_t;

typedef struct {
    uint32_t magic;
    float jitter_thresh;
    float wander_thresh;
} calib_blob_t;
#define CALIB_MAGIC 0xC51CA1
#define CALIB_NS "csi_sense"

static QueueHandle_t s_queue;
static esp_ping_handle_t s_ping;
static bool s_inited;
static bool s_radio_on;      // CSI enabled and the gateway ping running

// --- metric state (ported from the standalone CSI motion sense project) ---
static float s_prev_amp[MAX_PAIRS];
static float s_base_amp[MAX_PAIRS];   // slow baseline of "empty room"
static float s_jitter;               // fast: frame-to-frame change  -> motion
static float s_wander;               // slow: deviation from baseline -> presence
static float s_jitter_thresh = 4.0f;
static float s_wander_thresh = 8.0f;
static bool s_calibrated;
static bool s_calibrating;
static float s_calib_progress;
static sentry_state_t s_state = SENTRY_NO_WIFI;
static uint32_t s_frames_total, s_frames_this_sec, s_fps;
static int64_t s_fps_window_start;

// --- calibration accumulators ---
static double s_cal_jit_sum, s_cal_jit_sq, s_cal_wan_sum, s_cal_wan_sq;
static uint32_t s_cal_frames;
static int64_t s_cal_start_us;

// --- detection hysteresis ---
static uint32_t s_motion_streak;
static int64_t s_motion_latch_us;
static uint32_t s_present_frames, s_empty_frames;

// --- alerting ---
static int64_t s_motion_since_us;
static int64_t s_last_alert_us;
static bool s_calib_captioned;

// --- recalibration requests (counted in the settings, polled here) ---
static uint32_t s_cal_requests_seen;

// --- frame flow: a silent CSI path must not look like an empty room ---
static int64_t s_last_frame_us;
static bool s_no_signal;   // the NO SIGNAL caption is up; re-arms on a frame

// --- caption ownership (the caption is shared with the voice pipeline) ---
static uint32_t s_cap_ver;
static bool s_cap_mine;
static int64_t s_cap_until_us;

static inline int64_t now_us(void) { return esp_timer_get_time(); }

static void set_state(sentry_state_t st)
{
    if (st != s_state) {
        s_state = st;
        ESP_LOGI(TAG, "state -> %d (jitter=%.2f thr=%.2f, wander=%.2f thr=%.2f, %u fps)",
                 st, s_jitter, s_jitter_thresh, s_wander, s_wander_thresh, (unsigned)s_fps);
    }
}

// ---------------- captions ----------------

// Shows a caption only while the gadget is idle, so a sentry note never
// talks over a conversation, and remembers it is ours so it can be cleared
// later if nobody else has replaced it.
static void caption_show(const char *text, int hold_s)
{
    if (muse_state_mode(NULL) != MUSE_MODE_IDLE) {
        return;
    }
    muse_state_set_caption("%s", text);
    char buf[MUSE_CAPTION_MAX];
    while (muse_state_caption(buf, sizeof(buf), &s_cap_ver)) {
        // advance my version tracker to the caption I just set
    }
    s_cap_mine = true;
    s_cap_until_us = now_us() + (int64_t)hold_s * 1000000;
}

static void caption_poll(int64_t now)
{
    if (!s_cap_mine || now < s_cap_until_us) {
        return;
    }
    s_cap_mine = false;
    char buf[MUSE_CAPTION_MAX];
    if (!muse_state_caption(buf, sizeof(buf), &s_cap_ver)) {
        muse_state_set_caption("%s", "");   // unchanged since I set it: still mine
    }
}

// ---------------- NVS calibration storage ----------------

static void save_calibration(void)
{
    nvs_handle_t h;
    if (nvs_open(CALIB_NS, NVS_READWRITE, &h) != ESP_OK) return;
    calib_blob_t b = { .magic = CALIB_MAGIC,
                       .jitter_thresh = s_jitter_thresh,
                       .wander_thresh = s_wander_thresh };
    nvs_set_blob(h, "calib", &b, sizeof(b));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "calibration saved: jitter_thr=%.2f wander_thr=%.2f",
             s_jitter_thresh, s_wander_thresh);
}

static void load_calibration(void)
{
    nvs_handle_t h;
    calib_blob_t b = {0};
    size_t len = sizeof(b);
    if (nvs_open(CALIB_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "calib", &b, &len) == ESP_OK && b.magic == CALIB_MAGIC) {
            s_jitter_thresh = b.jitter_thresh;
            s_wander_thresh = b.wander_thresh;
            s_calibrated = true;
            ESP_LOGI(TAG, "calibration loaded: jitter_thr=%.2f wander_thr=%.2f",
                     s_jitter_thresh, s_wander_thresh);
        }
        nvs_close(h);
    }
}

static void erase_calibration(void)
{
    nvs_handle_t h;
    if (nvs_open(CALIB_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "calib");
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "stored calibration erased");
}

// ---------------- CSI plumbing ----------------

static void csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    if (!info || !info->buf || info->len == 0) return;
    csi_frame_t f;
    uint16_t n = info->len > CSI_MAX_LEN ? CSI_MAX_LEN : info->len;
    memcpy(f.buf, info->buf, n);
    f.len = n;
    f.rssi = info->rx_ctrl.rssi;
    f.skip_first4 = info->first_word_invalid;
    // Never block the Wi-Fi task: drop the frame if the queue is full.
    xQueueSend(s_queue, &f, 0);
}

static bool csi_enable(void)
{
    wifi_csi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.lltf_en = true;            // legacy frames (beacons / management)
    cfg.htltf_en = true;           // HT frames (our ping replies, HT20/HT40)
    cfg.stbc_htltf2_en = true;
    cfg.ltf_merge_en = true;
    cfg.channel_filter_en = true;
    cfg.manu_scale = false;        // automatic scaling
    cfg.shift = 0;
    cfg.dump_ack_en = false;
    if (esp_wifi_set_csi_config(&cfg) != ESP_OK) return false;
    if (esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL) != ESP_OK) return false;
    if (esp_wifi_set_csi(true) != ESP_OK) return false;
    ESP_LOGI(TAG, "CSI enabled");
    return true;
}

static void ping_ok_cb(esp_ping_handle_t h, void *args)      { (void)h; (void)args; }
static void ping_timeout_cb(esp_ping_handle_t h, void *args) { (void)h; (void)args; }
static void ping_end_cb(esp_ping_handle_t h, void *args)     { (void)h; (void)args; }

static bool ping_start(void)
{
    esp_netif_t *netif = wifi_mgr_get_netif();
    esp_netif_ip_info_t ip;
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.gw.addr == 0) {
        ESP_LOGW(TAG, "no gateway yet, ping deferred");
        return false;
    }
    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = ip.gw.addr;  // endian-safe copy

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = PING_INTERVAL_MS;
    cfg.timeout_ms = 200;
    cfg.data_size = 32;
    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_ok_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end = ping_end_cb,
        .cb_args = NULL,
    };
    if (esp_ping_new_session(&cfg, &cbs, &s_ping) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ping_new_session failed");
        s_ping = NULL;
        return false;
    }
    esp_ping_start(s_ping);
    ESP_LOGI(TAG, "pinging gateway " IPSTR " every %d ms", IP2STR(&ip.gw), PING_INTERVAL_MS);
    return true;
}

static void ping_stop(void)
{
    if (s_ping) {
        esp_ping_stop(s_ping);
        esp_ping_delete_session(s_ping);
        s_ping = NULL;
    }
}

// Radio on: CSI up, then the ping that feeds it frames. If either step
// fails the sentry stays off and the reconcile loop retries; a sentry
// problem must never take the gadget down with it.
static void radio_start(void)
{
    if (!csi_enable()) {
        ESP_LOGW(TAG, "CSI enable failed; will retry");
        return;
    }
    if (!ping_start()) {
        esp_wifi_set_csi(false);
        return;
    }
    s_radio_on = true;
    s_last_frame_us = now_us();   // grace period before the stall check bites
    s_no_signal = false;
}

static void radio_stop(void)
{
    ping_stop();
    esp_wifi_set_csi(false);
    s_radio_on = false;
    ESP_LOGI(TAG, "sentry radio off");
}

// ---------------- detection (ported) ----------------

static void finish_calibration(void)
{
    double jm = s_cal_jit_sum / s_cal_frames;
    double jv = s_cal_jit_sq / s_cal_frames - jm * jm;
    double wm = s_cal_wan_sum / s_cal_frames;
    double wv = s_cal_wan_sq / s_cal_frames - wm * wm;
    if (jv < 0) jv = 0;
    if (wv < 0) wv = 0;
    s_jitter_thresh = (float)(jm + 5.0 * sqrt(jv));
    s_wander_thresh = (float)(wm + 5.0 * sqrt(wv));
    if (s_jitter_thresh < 2.0f) s_jitter_thresh = 2.0f;  // sanity floors
    if (s_wander_thresh < 4.0f) s_wander_thresh = 4.0f;
    s_calibrated = true;
    s_calibrating = false;
    s_calib_progress = 1.0f;
    memcpy(s_base_amp, s_prev_amp, sizeof(s_base_amp));  // re-anchor baseline
    s_motion_streak = s_present_frames = s_empty_frames = 0;
    save_calibration();
    set_state(SENTRY_EMPTY);
    ESP_LOGI(TAG, "calibration complete");
    if (s_calib_captioned) {
        s_calib_captioned = false;
        s_cap_until_us = 0;   // let caption_poll clear it if it is still ours
    }
    caption_show("SENTRY READY", READY_CAPTION_S);
}

static void calib_feed(void)
{
    int64_t now = now_us();
    if (s_cal_start_us == 0) s_cal_start_us = now;
    s_cal_jit_sum += s_jitter;
    s_cal_jit_sq += (double)s_jitter * s_jitter;
    s_cal_wan_sum += s_wander;
    s_cal_wan_sq += (double)s_wander * s_wander;
    s_cal_frames++;
    s_calib_progress = (float)(now - s_cal_start_us) / (CALIB_SECONDS * 1000000.0f);
    if (s_calib_progress > 1.0f) s_calib_progress = 1.0f;
    if (now - s_cal_start_us >= (int64_t)CALIB_SECONDS * 1000000 && s_cal_frames > 50) {
        finish_calibration();
    }
}

static void start_calibration(void)
{
    s_cal_jit_sum = s_cal_jit_sq = 0;
    s_cal_wan_sum = s_cal_wan_sq = 0;
    s_cal_frames = 0;
    s_cal_start_us = 0;
    s_calib_progress = 0;
    s_calibrating = true;
    set_state(SENTRY_CALIBRATING);
    ESP_LOGI(TAG, "calibration started: keep the room empty for %d s", CALIB_SECONDS);
    caption_show("SENTRY CALIBRATING - KEEP ROOM EMPTY", CALIB_CAPTION_S);
    s_calib_captioned = s_cap_mine;
}

static void detect(int64_t now)
{
    bool motion_now = s_calibrated && (s_jitter > s_jitter_thresh);
    bool present_now = s_calibrated && (s_wander > s_wander_thresh);

    if (motion_now) {
        s_motion_streak++;
        s_present_frames = s_empty_frames = 0;
        if (s_motion_streak >= 3) {
            set_state(SENTRY_MOTION);
            s_motion_latch_us = now;
        }
        return;
    }
    s_motion_streak = 0;

    if (s_state == SENTRY_MOTION) {
        if (now - s_motion_latch_us < 4000000) return;  // linger 4 s in MOTION
        // fall through to re-evaluate
    }
    if (present_now) {
        s_present_frames++;
        s_empty_frames = 0;
        if (s_present_frames > 100) set_state(SENTRY_PRESENT);  // ~2 s
    } else {
        s_empty_frames++;
        s_present_frames = 0;
        if (s_empty_frames > 500) set_state(SENTRY_EMPTY);      // ~10 s
    }
}

static void process_frame(const csi_frame_t *f, bool wifi_ok)
{
    int64_t now = now_us();
    s_last_frame_us = now;
    if (s_no_signal) {
        s_no_signal = false;   // re-arm the stall warning
        ESP_LOGI(TAG, "CSI frames flowing again");
    }
    int offset = f->skip_first4 ? 4 : 0;
    int pairs = (f->len - offset) / 2;
    if (pairs <= 0) return;
    if (pairs > MAX_PAIRS) pairs = MAX_PAIRS;

    float jit_sum = 0.0f, wan_sum = 0.0f;
    bool first = (s_frames_total == 0);
    for (int i = 0; i < pairs; i++) {
        float amp = fabsf((float)f->buf[offset + 2 * i])
                  + fabsf((float)f->buf[offset + 2 * i + 1]);
        float dj = first ? 0.0f : fabsf(amp - s_prev_amp[i]);
        jit_sum += dj;
        s_prev_amp[i] = amp;
        // The baseline must keep representing the *empty* room: adapt it at a
        // normal rate while EMPTY, barely at all while someone is present
        // (otherwise a perfectly still person would fade into the baseline),
        // and freeze it entirely during strong motion.
        float adapt = (s_state == SENTRY_EMPTY) ? 0.002f : 0.0001f;
        if (s_jitter < s_jitter_thresh * 3.0f) {
            s_base_amp[i] += adapt * (amp - s_base_amp[i]);
        }
        wan_sum += fabsf(amp - s_base_amp[i]);
    }
    float jit_inst = jit_sum / pairs;
    float wan_inst = wan_sum / pairs;
    s_jitter += 0.25f * (jit_inst - s_jitter);
    s_wander += 0.05f * (wan_inst - s_wander);

    s_frames_total++;
    s_frames_this_sec++;
    if (now - s_fps_window_start >= 1000000) {
        s_fps = s_frames_this_sec;
        s_frames_this_sec = 0;
        s_fps_window_start = now;
    }

    if (s_calibrating) {
        calib_feed();
        return;
    }
    if (!wifi_ok) {
        set_state(SENTRY_NO_WIFI);
        return;
    }
    if (!s_calibrated) {
        set_state(SENTRY_CALIBRATING);
        return;
    }
    detect(now);
}

// ---------------- frame-flow watchdog ----------------

// No frames is not the same as an empty room: if the CSI path silently
// stops delivering (driver, radio, power save), say so once instead of
// standing guard over nothing. Re-arms when a frame arrives.
static void nosignal_poll(int64_t now)
{
    if (s_no_signal || !s_last_frame_us) return;
    if (now - s_last_frame_us < NO_SIGNAL_AFTER_US) return;
    s_no_signal = true;
    ESP_LOGI(TAG, "no CSI frames for 5 s (fps=%u); sentry has no signal", (unsigned)s_fps);
    caption_show("SENTRY: NO SIGNAL", NO_SIGNAL_CAPTION_S);
}

// ---------------- recalibration requests ----------------

// The menu's "Recalibrate sentry" row and the BLE "sentry_cal" command ask
// through the settings; honor a new request by dropping the stored
// thresholds (in NVS and in RAM) so the auto-calibration path below starts
// a fresh 20 s learn as soon as frames flow.
static void cal_request_poll(void)
{
    uint32_t req = muse_settings_sentry_calibration_requests();
    if (req == s_cal_requests_seen) return;
    s_cal_requests_seen = req;
    ESP_LOGI(TAG, "recalibration requested");
    erase_calibration();
    s_calibrated = false;
    s_calibrating = false;
    s_motion_streak = s_present_frames = s_empty_frames = 0;
    s_motion_since_us = 0;
}

// ---------------- alerting ----------------

static void alert_poll(int64_t now)
{
    if (s_state != SENTRY_MOTION) {
        s_motion_since_us = 0;
        return;
    }
    if (!s_motion_since_us) {
        s_motion_since_us = now;
        return;
    }
    if (now - s_motion_since_us < ALERT_SUSTAIN_US) return;
    if (now - s_last_alert_us < ALERT_COOLDOWN_US) return;
    s_last_alert_us = now;

    ESP_LOGI(TAG, "ALERT: motion sensed (jitter=%.2f thr=%.2f)", s_jitter, s_jitter_thresh);
    caption_show("MOTION SENSED NEARBY", ALERT_CAPTION_S);
#if CONFIG_MUSE_HATCH
    // The typed-turn API is the chat session's supported way for the device
    // to put a message into the Muse chat. It takes ownership of the
    // string. If a voice turn is running the session refuses it and the
    // alert is dropped (the caption above still shows); the cooldown keeps
    // later motion from spamming retries.
    char *msg = strdup("Motion sensed near the little blue guy.");
    if (msg) {
        muse_hatch_text_turn(msg);
    }
#endif
}

// ---------------- task ----------------

static void sentry_task(void *arg)
{
    (void)arg;
    s_fps_window_start = now_us();
    csi_frame_t f;
    while (1) {
        bool on = muse_settings_sentry_on();
        bool up = wifi_mgr_is_connected();

        cal_request_poll();

        if (on && up && !s_inited) {
            // First enable with Wi-Fi up: the event loop and NVS are long
            // since ready (Wi-Fi joining runs after both).
            s_queue = xQueueCreate(QUEUE_LEN, sizeof(csi_frame_t));
            load_calibration();
            s_inited = s_queue != NULL;
            if (!s_inited) {
                ESP_LOGE(TAG, "queue creation failed; sentry disabled");
                vTaskDelete(NULL);
            }
            if (!s_calibrated) {
                ESP_LOGI(TAG, "no calibration stored; will calibrate once frames flow");
            }
        }
        if (on && up && s_inited && !s_radio_on) {
            radio_start();
        } else if ((!on || !up) && s_radio_on) {
            radio_stop();
            set_state(on ? SENTRY_NO_WIFI : SENTRY_OFF);
            s_motion_since_us = 0;
        }
        if (!on && s_state != SENTRY_OFF && !s_radio_on) {
            set_state(SENTRY_OFF);
        }
        if (!s_inited) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (s_radio_on && !s_calibrated && !s_calibrating && s_frames_total > 10) {
            start_calibration();
        }

        if (xQueueReceive(s_queue, &f, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (s_radio_on) {
                process_frame(&f, up);
            } // else: frames left over from before the radio stopped; drop
        } else if (now_us() - s_fps_window_start >= 1000000) {
            s_fps = s_frames_this_sec;
            s_frames_this_sec = 0;
            s_fps_window_start = now_us();
        }

        if (s_radio_on) {
            nosignal_poll(now_us());
            alert_poll(now_us());
        }
        caption_poll(now_us());
    }
}

void csi_sentry_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    if (xTaskCreate(sentry_task, "csi_sentry", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the sentry task");
    }
}
