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
 *
 * v6 idle tune: three times a day (09:00, 13:00, 18:00 local) the gadget
 * also plays a short ascending arpeggio -- "I'm still here, awaiting my
 * next job" -- but only while it is already awake and idle, with the
 * speaker on. It never wakes the gadget from standby to play, never plays
 * at night, and never plays more than three times a day. The tune is
 * deliberately distinct from the rooster crow so the two are never
 * confused. Played/missed state is RAM only: a reboot may replay a tune
 * that already played that day, which is harmless at three short notes.
 */

#include <math.h>
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
/* Idle tune: sine-wave arpeggio at the gadget's 16 kHz sample rate,
 * synthesised in code -- no audio file, no copyrighted melody. */
#define TUNE_SAMPLE_RATE 16000
#define TUNE_NOTE_FRAMES 4000   /* 0.25 s per note */
#define TUNE_GAP_FRAMES 800     /* 0.05 s of silence between notes */
#define TUNE_AMPLITUDE 8000     /* well below full scale: no loud burst */
#define TUNE_FADE_FRAMES 80     /* 5 ms ramp at each note edge, no clicks */
static const float TUNE_FREQS[] = { 523.25f, 659.25f, 783.99f, 1046.50f }; /* C5 E5 G5 C6 */
static const int TUNE_MINUTES[] = { 9 * 60, 13 * 60, 18 * 60 };           /* 09:00, 13:00, 18:00 */
#define TUNE_COUNT ((int)(sizeof(TUNE_MINUTES) / sizeof(TUNE_MINUTES[0])))
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

static bool tune_write_silence(size_t frames)
{
    static const int16_t silence[MUSE_AUDIO_CHUNK] = { 0 };
    while (frames) {
        size_t n = frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames;
        if (muse_audio_write(silence, n) != ESP_OK) {
            return false;
        }
        frames -= n;
    }
    return true;
}

static bool tune_play_note(float freq)
{
    /* Phase is per-note so each note starts cleanly at zero. */
    float phase = 0.0f;
    const float step = 2.0f * (float)M_PI * freq / (float)TUNE_SAMPLE_RATE;
    size_t remaining = TUNE_NOTE_FRAMES;
    size_t pos = 0;
    while (remaining) {
        size_t n = remaining > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : remaining;
        int16_t buf[MUSE_AUDIO_CHUNK];
        for (size_t i = 0; i < n; i++) {
            size_t at = pos + i;
            float env = 1.0f;
            if (at < TUNE_FADE_FRAMES) {
                env = (float)at / (float)TUNE_FADE_FRAMES;
            } else if (at >= TUNE_NOTE_FRAMES - TUNE_FADE_FRAMES) {
                env = (float)(TUNE_NOTE_FRAMES - at) / (float)TUNE_FADE_FRAMES;
            }
            buf[i] = (int16_t)(sinf(phase) * (float)TUNE_AMPLITUDE * env);
            phase += step;
            if (phase >= 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        if (muse_audio_write(buf, n) != ESP_OK) {
            return false;
        }
        pos += n;
        remaining -= n;
    }
    return true;
}

/* Guards are evaluated once, at the first poll inside a tune minute, and
 * the slot is then marked done for the day whether it played or not:
 * a gadget asleep at the scheduled time skips that tune for the day
 * instead of waking up later to play it, and a tune never interrupts a
 * voice session that started at the same minute. */
static void tune_maybe_play(void)
{
    if (!muse_board || !muse_board->audio_init) {
        return;
    }
    if (muse_state_asleep() || muse_state_mode(NULL) != MUSE_MODE_IDLE ||
        !muse_settings_speaker_on()) {
        return;
    }
    bool power_was_on = muse_audio_is_powered();
    muse_audio_power(true);
    muse_state_set_caption("%s", "I'm here");
    for (int i = 0; i < (int)(sizeof(TUNE_FREQS) / sizeof(TUNE_FREQS[0])); i++) {
        if (muse_state_mode(NULL) != MUSE_MODE_IDLE) {
            break;  // a conversation started: stop the tune over it
        }
        if (!tune_play_note(TUNE_FREQS[i])) {
            break;
        }
        if (i + 1 < (int)(sizeof(TUNE_FREQS) / sizeof(TUNE_FREQS[0]))) {
            if (!tune_write_silence(TUNE_GAP_FRAMES)) {
                break;
            }
        }
    }
    if (muse_state_mode(NULL) == MUSE_MODE_IDLE) {
        muse_state_set_caption("%s", "");
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
    int tune_day_key = -1;    // RAM only: reboot may replay a tune, harmless
    unsigned tune_done_mask = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
        time_t now = time(NULL);
        if (now < SYNCED_EPOCH_MIN) {
            continue;  // clock not synced yet: cannot know the time
        }
        struct tm tm;
        localtime_r(&now, &tm);
        int minute_of_day = tm.tm_hour * 60 + tm.tm_min;
        /* Idle tune: daytime only, once per slot per day, independent of
         * the rooster alarm's on/off state. Evaluated on the first poll
         * inside the slot minute, then marked done for the day. */
        int day_key = tm.tm_year * 1000 + tm.tm_yday;
        if (day_key != tune_day_key) {
            tune_day_key = day_key;
            tune_done_mask = 0;
        }
        for (int i = 0; i < TUNE_COUNT; i++) {
            if (minute_of_day == TUNE_MINUTES[i] && !(tune_done_mask & (1u << i))) {
                tune_done_mask |= (1u << i);
                ESP_LOGI(TAG, "idle tune at %02d:%02d", tm.tm_hour, tm.tm_min);
                tune_maybe_play();
            }
        }
        if (!muse_settings_alarm_on()) {
            continue;
        }
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
