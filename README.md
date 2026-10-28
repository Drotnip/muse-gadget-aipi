# Muse Gadget firmware — AiPi Lite, M5StickC Plus 2, M5StickS3

> Built with AI assistance, tested on real hardware.

ESP-IDF firmware that turns small ESP32 boards into Muse voice gadgets:
pair from the Muse app over Bluetooth, join Wi-Fi, and talk to Muse by
voice. Based on Meta's open-source Muse gadget SDK
(github.com/facebookincubator/muse-gadget-sdk), with custom builds for
the boards below.

## Boards in this repo

- **AiPi Lite** (first-gen ESP32-S3 handheld) — the "little blue" gadget
- **M5Stack StickS3**
- **M5Stack StickC Plus 2**

## What's in here

- **Voice gadget** — Bluetooth pairing, Wi-Fi, Muse chat, on-screen
  avatar and captions.
- **VoiceRSS text-to-speech** — spoken replies (voice "Mike", en-us).
  See `docs/tts-voicerss/` for how the TTS swap was done.
- **Pond Sentry** — Wi-Fi CSI motion sensing (see below).
- **Rooster crow alarm** — a daily alarm clock: set it from a Muse
  reply (`[ALARM:07:30]`, `[ALARM:OFF]`) or the on-screen menu
  ("Rooster alarm"); at the set time the gadget crows three times.
  v4 fixes silent crowing while the gadget rests on battery: the alarm
  now powers the speaker up before crowing, then lets it sleep again.
  v5 adds a "Rooster time" menu row after "Rooster alarm" that steps
  the alarm time in 15-minute steps (wrapping 23:45 to 00:00) without
  changing on/off, so the time can be set with buttons alone.
  v6 adds a short idle tune (C, E, G, high C, about one second) at
  09:00, 13:00 and 18:00, only while the gadget is already awake and
  idle — it never wakes the gadget from standby to play.

## Pond Sentry motion caption — fixed in v3, awaiting on-device test

The sentry caption path is fixed: a caption that fires while Muse is
talking is now held and shown when the gadget goes idle, instead of
being dropped silently, and a radio that never starts now says
SENTRY: NO SIGNAL on screen instead of failing silently. The fix is
in `main/csi_sentry.c` (v3 build). It has not yet been confirmed on
the gadget itself — that check happens when the v3 build goes out
over OTA.

What the sentry is meant to do: the gadget joins your Wi-Fi and watches
the radio signal (CSI) for movement nearby, after a 20-second
empty-room calibration. Source: `main/csi_sentry.c`.

## What you need

- **ESP-IDF v6.0.1** (other versions are unsupported)
- A USB cable that carries **data**, not just power
- A computer (Windows, macOS, or Linux)
- A **Muse SDK token** from
  [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens)
  (Account > SDK tokens)
- The **Muse app** on your phone, to pair the gadget once it's flashed

## Secrets — put your own in, never commit real ones

- **Muse SDK token:** set it in your *build directory's* `sdkconfig`
  (or with `idf.py menuconfig`):

  ```
  CONFIG_GADGET_SDK_TOKEN="YOUR_MUSE_TOKEN_HERE"
  ```

  Never commit a real token. Without a token the gadget will stop
  pairing once Muse requires tokens.
- **VoiceRSS API key:** set it in your build `sdkconfig`:

  ```
  CONFIG_VOICERSS_API_KEY="YOUR_VOICERSS_KEY_HERE"
  CONFIG_VOICERSS_VOICE="Mike"
  ```

  The repo ships placeholders only, never a real key.
- **No prebuilt `.bin` files are included in this repo on purpose:**
  some gadget builds bake tokens in, so always build from source.

## How to build and flash (plain language)

1. Install ESP-IDF v6.0.1 and activate it (`. $IDF_PATH/export.sh`
   on macOS/Linux, or open the ESP-IDF Command Prompt on Windows).
2. Pick your board's settings files (overlays), which go after
   `sdkconfig.defaults`:
   - AiPi Lite: `devices/sdkconfig.muse` +
     `devices/sdkconfig.muse-aipi`
   - StickS3: `devices/sdkconfig.muse` +
     `devices/sdkconfig.muse-m5stack-sticks3`
   - StickC Plus 2: `devices/sdkconfig.muse` +
     `devices/sdkconfig.muse-m5stack-stickc-plus2`
3. Set your SDK token and VoiceRSS key in the build `sdkconfig`
   (see "Secrets" above).
4. Build it:

   ```
   idf.py set-target esp32s3   # esp32 for the StickC Plus 2
   idf.py build
   ```

5. Plug the board in with USB and flash it:

   ```
   idf.py -p COM3 flash   # use your port number, not COM3
   ```

6. On first boot, pair the gadget from the Muse app.

**OTA updates:** for this gadget's over-the-air update, always send
the **app-only** image, never a merged full-flash `.bin` — the merged
image gets rejected by the device.

## Credit

Based on the open-source Muse gadget SDK by Meta (Apache 2.0).
Custom work in this repo: the AiPi Lite / StickS3 / StickC Plus 2
board builds, the VoiceRSS text-to-speech swap, and the Pond Sentry
CSI motion-sense variant.
