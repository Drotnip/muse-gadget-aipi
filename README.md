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
- **Pond Sentry** — Wi-Fi CSI motion sensing (see below, and read the
  known issue first).

## ⚠️ Known issue — Pond Sentry motion caption is BROKEN

Plainly: the sentry's motion caption does not work right now. When
motion is sensed, **no caption appears on screen**. The voice gadget
itself works fine — only the sentry caption is broken, and the fix is
still owed. Do not treat the sentry as fully working until this note
is removed.

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
