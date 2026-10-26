/*
 * Rooster crow alarm clock for the little blue guy.
 *
 * A daily alarm, set from a Muse reply ("[ALARM:07:30]", "[ALARM:OFF]") or
 * toggled from the on-screen menu ("Rooster alarm"). At the set time it
 * crows three times through the gadget's speaker and shows
 * COCK-A-DOODLE-DOO on screen. The time comes from SNTP over Wi-Fi, in
 * America/Chicago time, so it only fires once the clock is actually synced.
 * Boards without audio hardware never crow: the task checks the board's
 * audio support before playing anything.
 */

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "wifi_mgr.h"

static const char *TAG = "link.rooster_alarm";

#include "rooster_crow_data.h"

#define CROW_REPEATS 3
#define CROW_GAP_MS 1000
#define CHECK_MS 10000
/* Before the clock syncs, time() sits near the 1970 epoch. Anything below
 * this (2023) means "no real time yet", so the alarm cannot fire. */
#define SYNCED_EPOCH_MIN 1700000000

static void crow_once(void)
{
    const int16_t *pcm = rooster_crow_pcm;
    size_t frames = rooster_crow_pcm_frames;
    while (frames) {
        size_t n = frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames;
        if (muse_audio_write(pcm, n) != ESP_OK) {
            return;
        }
        pcm += n;
        frames -= n;
    }
}

static void crow_alarm(void)
{
    if (!muse_board || !muse_board->audio_init) {
        return;  // display-only board: nothing to crow through
    }
    /* The voice loop powers the codecs down while the gadget rests on
     * battery, and writes need power on: crowing through a powered-down
     * speaker fails silently. So open the codecs first, and put them back
     * to sleep afterwards — unless a conversation started mid-crow, in
     * which case the voice loop owns the audio now. */
    bool power_was_on = muse_audio_is_powered();
    muse_audio_power(true);
    /* An alarm that stays silent because the speaker was muted is a broken
     * alarm: crow through the mute, then put the setting back. */
    bool was_on = muse_settings_speaker_on();
    if (!was_on) {
        muse_settings_set_speaker_on(true);
    }
    muse_state_set_caption("%s", "COCK-A-DOODLE-DOO");
    for (int i = 0; i < CROW_REPEATS; i++) {
        if (muse_state_mode(NULL) != MUSE_MODE_IDLE) {
            break;  // a conversation started: stop crowing over it
        }
        crow_once();
        if (i + 1 < CROW_REPEATS) {
            vTaskDelay(pdMS_TO_TICKS(CROW_GAP_MS));
        }
    }
    muse_state_set_caption("%s", "");
    if (!was_on) {
        muse_settings_set_speaker_on(false);
    }
    if (!power_was_on && muse_state_mode(NULL) == MUSE_MODE_IDLE) {
        muse_audio_power(false);
    }
}

static void alarm_task(void *arg)
{
    (void)arg;
    /* Chicago time, daylight saving aware. */
    setenv("TZ", "CST6CDT,M3.2.0,M11.1.0", 1);
    tzset();
    /* SNTP needs a network interface, so wait for Wi-Fi before starting it. */
    while (!wifi_mgr_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        esp_netif_sntp_start();
    } else {
        ESP_LOGW(TAG, "SNTP init failed; the alarm will not fire without a clock");
    }
    int last_fired_min = -1;  // minute-of-day already crowed, so it fires once
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
        if (!muse_settings_alarm_on()) {
            continue;
        }
        time_t now = time(NULL);
        if (now < SYNCED_EPOCH_MIN) {
            continue;  // clock not synced yet: cannot know the time
        }
        struct tm tm;
        localtime_r(&now, &tm);
        int minute_of_day = tm.tm_hour * 60 + tm.tm_min;
        if (minute_of_day == last_fired_min) {
            continue;
        }
        if (tm.tm_hour == muse_settings_alarm_hour() && tm.tm_min == muse_settings_alarm_min()) {
            last_fired_min = minute_of_day;
            ESP_LOGI(TAG, "rooster alarm firing at %02d:%02d", tm.tm_hour, tm.tm_min);
            crow_alarm();
        }
    }
}

void alarm_clock_start(void)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    if (xTaskCreate(alarm_task, "rooster_alarm", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the alarm task");
    }
}
