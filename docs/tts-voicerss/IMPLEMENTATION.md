# VoiceRSS TTS for the Muse Gadget (AiPi Lite) — implementation note

**Status:** code prepared and staged, NOT built, NOT flashed. Staged 2026-10-22.
**Folder:** `~/workspace/muse-gadget-aipi/tts-voicerss/` — the SDK tree itself
is untouched; nothing here changes the existing build until the patches are
applied and a key is set.

## What was already in the code (findings)

The spoken-reply plumbing was never missing — only the provider was wrong:

- `sdk/esp32/components/muse/muse_chat_session.cpp` already has the full TTS
  state machine: a finished reply message is marked `TTS_QUEUED` in
  `message_done()` (~line 1401), the chat task's poll loop calls `start_tts()`
  while `P_WAIT_REPLY` (~line 2080), and `start_tts()` (~line 1588) calls
  `tts_fetch(text)`; streamed MP3 lands in `tts_data()` → `decode()` (minimp3)
  → resampled to 16 kHz mono → `s_out` stream buffer → `muse_hatch_turn_read()`
  (~line 2276) → the voice task plays it through `muse_audio` to the speaker.
- Captions are independent of the audio: `show_reply_start()` emits the opening
  line and `muse_hatch_caption_at()` paces the rest to playback position. With
  no TTS audio, `start_tts()` already falls back to silent pacing at reading
  pace — that is today's behaviour, and it stays the fallback for every
  failure below.
- **The catch:** the existing `tts_fetch()` (an uncommitted local modification,
  like the Kconfig entry beside it) speaks to **ElevenLabs** — POST JSON to
  `api.elevenlabs.io`, key from `CONFIG_MUSE_TTS_API_KEY`. ElevenLabs is out
  (David's explicit call, 2026-10-19), so that block is replaced wholesale.
  Note: `build-muse-aipi/sdkconfig` currently holds a non-empty value under
  the old ElevenLabs symbol (value not inspected). Once the Kconfig patch
  lands, that symbol no longer exists and the value is inert; it is never
  copied anywhere by this work.

## What was added / changed (staged)

| File | What it is |
|---|---|
| `voicerss_fetch.inc` | The complete new fetch block, human-readable: URL encoding, chunk splitter, HTTP handler with error-page gating, `tts_fetch_one()`, `tts_fetch()`. |
| `muse_chat_session.cpp.patch` | Unified diff against the **current working tree**: swaps the ElevenLabs block for the VoiceRSS block and fixes the one "ElevenLabs" mention in `start_tts()`'s comment. Apply from `sdk/esp32/` with `patch -p1` or `git apply`. |
| `muse_Kconfig.patch` | Replaces `config MUSE_TTS_API_KEY` (ElevenLabs) with `config VOICERSS_API_KEY` and `config VOICERSS_VOICE`, both `default ""`, both `depends on MUSE_HATCH`. |
| `IMPLEMENTATION.md` | This note. |

No other files change. `start_tts()`, `tts_data()`, `decode()`, the caption
path and the OTA code are byte-identical to today.

## Design

- **Trigger:** unchanged. Reply text (already capped at `TEXT_MAX` = 1024
  chars per message in `s_turn.texts`) is handed to `tts_fetch()` by
  `start_tts()`; speech and captions run together, as the pipeline intends.
- **Request:** `GET https://api.voicerss.org/?key=<KEY>&hl=en-us[&v=<VOICE>]&src=<url-encoded>&c=MP3&f=44khz_16bit_stereo`
  over `esp_http_client` with the TLS cert bundle — the same client pattern
  the old block used. `&v=` is included only when `CONFIG_VOICERSS_VOICE` is
  non-empty (voice choice still TBD; free-tier en-us names: Linda, Amy, Mary,
  John, Mike).
- **Streaming:** the HTTP event handler feeds each chunk of MP3 into
  `tts_data()` and calls `decode()` immediately, so speech starts while the
  rest downloads — same as before.
- **Long replies / chunking:** text is split into ≤400-char chunks at a
  sentence end (else a word boundary) and fetched in sequence into the same
  playback buffer; minimp3 resyncs per frame, so concatenated streams play as
  one. 400 chars ≈ 30 s of speech, which keeps one chunk's MP3 comfortably
  inside the 512 KB `MP3_BUF` even before decode drains any of it — a single
  1024-char request at this format could exceed it mid-stream. A normal reply
  is one request; the longest possible is three. Free tier (350 req/day,
  100 KB/request) is nowhere near either limit.
- **Error handling:**
  - *Empty key* → `tts_fetch()` returns false immediately → captions only,
    exactly as today. This is also the state of any build made before a key
    is set: the feature is simply off.
  - *VoiceRSS errors* come back as HTTP 200 with a plain-text body starting
    `ERROR` (bad key, unactivated account, over quota). The handler holds
    back the first 8 bytes of every response and checks them before anything
    reaches the decoder; an error body is swallowed whole, the fetch reports
    failure, and the reply falls back to silent caption pacing.
  - *HTTP errors / timeouts* (15 s, same as before) → fetch fails → same
    caption fallback. If a *later* chunk fails after audio already arrived,
    the speech just stops early and captions carry on (`tts_fetch()` returns
    true when any audio was fed).
- **Key handling:** the key is a build-time Kconfig string, same pattern as
  the gadget's SDK token: set it in `build-muse-aipi/sdkconfig` when
  building, never commit it, never put it in this folder or any doc.

## Where the API key goes

At build time only, in `sdk/esp32/build-muse-aipi/sdkconfig`:

```
CONFIG_VOICERSS_API_KEY="<David's VoiceRSS key>"
CONFIG_VOICERSS_VOICE=""        # or a name once he picks one
```

(or via `idf.py -B build-muse-aipi menuconfig` → "Muse" menu). The account
must be activated from VoiceRSS's email first, or every request returns an
`ERROR` body and replies stay text-only.

## Build steps (verified against the project, not just this brief)

From `sdk/esp32/AGENTS.md` (AiPi Lite row: manual `idf.py`, overlays
`devices/sdkconfig.muse;devices/sdkconfig.muse-aipi`) — the same command the
2026-10-18 build used, per `build.log`:

```sh
cd ~/workspace/muse-gadget-aipi/sdk/esp32
# 1. apply the two patches (one time)
patch -p1 < ../../tts-voicerss/muse_chat_session.cpp.patch
patch -p1 < ../../tts-voicerss/muse_Kconfig.patch
# 2. set CONFIG_VOICERSS_API_KEY in build-muse-aipi/sdkconfig (see above)
# 3. build
source ~/esp/esp-idf-v6/export.sh
idf.py -B build-muse-aipi -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-aipi/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-aipi" \
  build
# 4. merge into one flashable image. Offsets are from
#    build-muse-aipi/flash_args and partitions_muse.csv (otadata 0x1D000,
#    ota_0 0x20000, 4 MB) — both checked for this note:
python -m esptool --chip esp32s3 merge-bin -o ../../firmware/muse-gadget-aipi-lite-voicerss.bin \
  0x0     build-muse-aipi/bootloader/bootloader.bin \
  0x10000 build-muse-aipi/partition_table/partition-table.bin \
  0x1d000 build-muse-aipi/ota_data_initial.bin \
  0x20000 build-muse-aipi/muse-gadget.bin
# 5. sanity check: partition table magic at 0x10000 must read `aa 50`
xxd -s 0x10000 -l 2 ../../firmware/muse-gadget-aipi-lite-voicerss.bin
```

Delivery after that is the standing route (David flashes at 0x0, or OTA —
OTA itself is untouched by this change).

## Verification done on the staged code

- Both patches dry-run and apply cleanly onto pristine copies of the current
  working-tree files and reproduce the intended edited files byte-for-byte.
- The new block compiles under `g++ -std=c++17 -Wall -Wextra` against stubbed
  IDF HTTP types, and a smoke run of `tts_fetch()` over a >400-char reply and
  an empty string terminates correctly (chunk loop makes progress, no hang).
- The patched sources contain zero remaining "ElevenLabs" references.
- No firmware build was run and nothing was flashed, per the prep-only brief.

## Blockers / watch-outs

- **Patch base is a dirty tree.** `muse_chat_session.cpp`, `Kconfig` and
  `muse_ui.c` already carry uncommitted local changes (the ElevenLabs block,
  caption/avatar work). The patches are diffs against that exact state; if
  the tree changes before build day, re-check with `patch --dry-run` first.
- **Format choice:** the brief's `f=44khz_16bit_stereo` is implemented as
  specified. `decode()` already downmixes stereo to mono, so it plays fine;
  the earlier research note suggested `44khz_16bit_mono` as a lighter stream.
  If download weight ever matters, that is a one-word change in
  `tts_fetch_one()` — not done here, per the brief.
- **Very fast downloads** can still outrun realtime playback on replies near
  the 1024-char cap (`tts_data()` drops overflow with a log warning, as it
  does today). The 400-char chunking bounds the burst per request; speech
  gaps on extreme replies are possible but captions are unaffected.
- Nothing here signs David up for VoiceRSS or handles his key — that stays a
  build-day input from him, as with the SDK token.
