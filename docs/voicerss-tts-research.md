# VoiceRSS TTS — research for AiPi Lite spoken replies

**Checked:** 2026-10-19. Sources: https://voicerss.org/api (official docs, read today),
https://voicerss.org/ (plans page, read today).

## Why VoiceRSS
ElevenLabs is OUT at David's explicit request (signup too complicated, 2026-10-19).
VoiceRSS was proposed as the replacement; David understood the API-key concept but had
NOT yet confirmed the switch — the build decision is pending his go-ahead + his key.

## Facts verified
- Free plan: **$0, 350 requests/day**, plain text only, 100KB max per request. No credit card.
- Signup: free account on voicerss.org; key is shown after signup. **Account must be
  activated via the email they send, or requests fail.**
- One voice per request, one simple GET — no SDK, no JSON body, no auth headers.
- On error the API returns plain text starting with `ERROR: ` (NOT audio). The firmware
  must check the first bytes before decoding.

## Exact request format (verified against docs today)
```
http://api.voicerss.org/?key=KEY&hl=en-us&src=Hello,%20world!
```
- Plain **HTTP** is documented (https:// also supported). Plain HTTP avoids TLS
  certificate work on the ESP32 — use `http://api.voicerss.org/...`.
- Mandatory params: `key`, `src` (URL-encoded reply text), `hl` (e.g. `en-us`).
- Optional: `c=MP3` (codec; also WAV, AAC, OGG, CAF), `f=44khz_16bit_mono`
  (44kHz/16-bit/mono — good match for the little speaker; many formats available),
  `r` (rate -10..10, default 0), `v` (voice).
- en-us voices (free): Linda (female), Amy, Mary, John, Mike.
- Docs examples: `?key=YOUR_VOICERSS_KEY_HERE&hl=en-us&c=MP3&src=Hello, world!`

## Build sketch (when David confirms)
1. Firmware takes the reply text it already displays, URL-encodes it, appends to the
   URL above with his baked-in key (same pattern as the existing SDK token bake-in;
   key used transiently, never stored in chat or repo).
2. HTTP GET → read response → if it starts with `ERROR: `, keep text-only reply and
   log it (serial); never feed it to the MP3 decoder.
3. Otherwise pipe the MP3 bytes into the existing MP3 decode + speaker playback
   plumbing (the `start_tts()` hook and speaker path are already in the build).
4. Batch preference: David asked for firmware changes batched — this build should land
   as one OTA update, no iterative rounds.

## Free-tier math for David's use
350 requests/day ≈ ~14-15 hours of one reply per 2.5 minutes. Normal chat volume will
never hit it. Text is limited to 100KB per request (replies are ~200 bytes) — fine.

## Still needed from David
1. His go-ahead ("let's do VoiceRSS").
2. His VoiceRSS API key (he signs up free, activates via email, copies the key here —
   treat like his other keys: used transiently, never stored, and he was reminded
   never to send sensitive IDs in chat — have him paste it into the same pattern as
   the gadget token handoff).
Then: build → he flashes (or OTA), tells him when it's ready to test.
