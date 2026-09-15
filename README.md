# Vapi AtomS3R Voice

Talk to a [Vapi](https://vapi.ai) assistant from a **$20 M5Stack AtomS3R**.
Press the button, talk, press again to hang up. No phone, no browser, no app.

Mic → PCM → Vapi, and its reply → PCM → speaker, over Vapi's `vapi.websocket`
transport. Around 2,400 lines of C on ESP-IDF.

> **Status:** working on real hardware — calls connect, audio flows both ways,
> echo is handled. Built and tested on an AtomS3R + Atomic Echo Base.

## Why there is no WebRTC here

You would expect a voice device to be a WebRTC client. This one isn't, for three
reasons worth knowing before you reach for one:

**1. Vapi's WebRTC transport is Daily, and Daily has no embedded client.** Ask
the live API and it tells you the transport list:

```console
$ curl -s -X POST https://api.vapi.ai/call \
    -H "authorization: Bearer $VAPI_API_KEY" -H 'content-type: application/json' \
    -d '{"assistantId":"...","transport":{"provider":"x"}}'
{"message":["transport.provider must be one of the following values:
  daily, vapi.websocket, twilio, vonage, telnyx, vapi.sip"],...}
```

Daily's client signaling is proprietary and unspecified; their SDKs are
JS/Swift/Kotlin/Python/Rust and `daily-core` ships as a closed binary. There is
no C client and no protocol document to write one from.

**2. The SDP gateway Vapi's own 2025 workshop firmware used is gone.**
[VapiAI/vapicon-2025-hardware-workshop](https://github.com/VapiAI/vapicon-2025-hardware-workshop)
targets this exact board and POSTs a plain SDP offer to `staging-webrtc.vapi.ai`.
That host now returns Cloudflare **530** and `webrtc.vapi.ai` does not resolve,
so that firmware will not connect today.

**3. `vapi.websocket` is the path an MCU can actually reach — and it's simpler.**
No ICE, no DTLS-SRTP, no SDP, no peer connection: a TLS websocket where binary
frames are raw PCM and text frames are JSON.

The cost is uncompressed audio — 16 kHz mono s16le is **256 kbit/s each way**,
about 16× Opus. Comfortable on 802.11n.

WebRTC would also not have helped with echo: `esp_webrtc` has no AEC of its own,
and reuses the same Espressif audio front-end this firmware uses.

## Hardware

| | |
|---|---|
| Board | **M5Stack AtomS3R** (ESP32-S3-PICO-1, 8 MB flash, 8 MB octal PSRAM) |
| Audio | **Atomic Echo Base** — ES8311 codec + NS4168 amp + mic |
| Button | AtomS3R front button, GPIO41, active low |
| USB | native USB-Serial-JTAG |

> The Echo Base is **required** — the bare AtomS3R has no microphone or speaker.
> Also note M5Stack ships a plain **AtomS3** with *no PSRAM*, which this firmware
> will not run on. Check with `esptool.py --port <port> chip_id`; you want to see
> `Embedded PSRAM 8MB`.

## Quick start

You need [ESP-IDF v5.5.1+](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/)
installed, and a Vapi account.

```bash
git clone --recurse-submodules https://github.com/ramsrib/vapi-atoms3r-voice.git
cd vapi-atoms3r-voice

make setup            # creates .env, configures the target, fetches components
$EDITOR .env          # fill in WiFi + Vapi keys
make run              # build + flash + monitor
```

Already cloned without `--recurse-submodules`? `git submodule update --init`.
Espressif's `esp-webrtc-solution` is vendored under `deps/` as a submodule
pinned to an exact commit — upstream has since moved the paths this project
uses, so the pin is deliberate.

If `IDF_PATH` isn't exported in your shell, pass it: `make IDF_PATH=~/esp/esp-idf run`.

### Configuration

`.env` (gitignored) is baked into the firmware at build time:

| key | |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | required |
| `VAPI_API_KEY` | required — your Vapi **private** key |
| `VAPI_ASSISTANT_ID` | required — from the Vapi dashboard |
| `VAPI_API_URL` | optional, defaults to `https://api.vapi.ai` |
| `VAPI_FIRST_MESSAGE` | optional greeting override |
| `VAPI_MAX_DURATION_SECONDS` | optional call cap |

`idf.py menuconfig` works too; `.env` wins where both are set.

> ### ⚠ About the API key
>
> Creating a call on the websocket transport goes through `POST /call`, which
> needs your **private** key — the public key only authorizes `/call/web`, which
> returns a Daily room this device cannot join.
>
> So a private key ends up in the firmware image, and it is recoverable with
> `esptool.py read_flash`. A private key is account-wide: it can create calls,
> read transcripts, and spend money. **Fine for a device on your desk; not fine
> for anything you ship or hand to someone else.** The fix is a small proxy that
> authenticates the device and mints a short-lived credential — point
> `VAPI_API_URL` at it instead.
>
> Setting `VAPI_MAX_DURATION_SECONDS` is worth doing regardless: a call left up
> by a missed button press bills until Vapi's own timeout.

TLS certificate verification is **on**. That means the clock has to be right, so
SNTP is configured.

## How it works

```
   ┌────────── AtomS3R + Atomic Echo Base ──────────┐
   │  ES8311 mic     → esp_capture → AEC → PCM       │   ① POST https://api.vapi.ai/call
   │  ES8311 speaker ← av_render   ←────── PCM       │  ┌─── {assistantId, transport:{
   │                        │                        │  │       provider:"vapi.websocket",
   │              esp_websocket_client ──────────────┼──┘       audioFormat:{pcm_s16le,16k}}}
   │                        │                        │      ◄── {id, transport:{websocketCallUrl}}
   │      TLS 1.2 · CA bundle · cert verify ON       │
   │                        │                        │  ② wss://…vapi.ai
   └─────────────────────────────────────────────────┘  ═══ binary = PCM ═══════════►
                                                        ◄══ binary = PCM ═══════════
                                                        ◄── text = JSON control ────►
```

One HTTPS POST creates the call and returns a websocket URL. After that, binary
frames carry raw PCM in both directions and text frames carry JSON — transcripts
and status in; `end-call`, `control` and `add-message` out.

| | |
|---|---|
| `POST /call` | 201 |
| websocket open | ~470 ms |
| first audio | ~570 ms |
| inbound frame | 640 bytes = one 20 ms frame at 16 kHz mono s16le |
| hangup | `{"type":"end-call"}` → `status-update: ended` |

`end-call` is the documented `ClientInboundMessage` type (check
`https://api.vapi.ai/api-json`). Closing the socket also ends the call, which is
exactly why an undocumented spelling would *appear* to work.

**16 kHz is the right rate**, and that's measured rather than assumed: Vapi
accepts 16000/24000/48000 and honours all three, but its returned audio has no
real content above 8 kHz (36 dB down at 48 kHz). Higher rates only upsample.

### Echo: the interesting part

The speaker sits ~2 cm from the microphone. Espressif's AEC does not cope: on the
first real call the assistant heard itself and held a conversation with itself.

Gain tuning provably cannot fix it — analog and digital gain scale the echo and
the talker by the same factor, so neither changes which one the speech-to-text
latches onto. What works is **hard-muting the mic while the assistant is
audible**, timed off a *write-ahead playout clock*: we write every inbound frame
to the speaker, so tracking bytes-written against the sample rate says exactly
when it becomes audible (142–300 ms ahead on this board).

```
gate: shut 101/101 | playout lead 177 ms | mic pk POST-gate 0     ← assistant speaking
gate: shut   0/101 | playout lead 187 ms | mic pk POST-gate 1773  ← your turn
```

**The trade is barge-in**: you cannot interrupt the assistant by talking over it.

Full measurements, why three other fixes failed, and how to re-tune for a
different enclosure: **[docs/AEC-TUNING.md](docs/AEC-TUNING.md)**. If you are
building voice hardware with a close-coupled speaker and mic, that page is the
most useful thing in this repo.

### The non-obvious hardware bit

The Echo Base amplifier is **not** on a plain PA GPIO — its `codec_board` entry
says `pa: -1`. It's gated by a **PI4IOE5V6408 I/O expander at 0x43** on the
codec's I2C bus. `main/board.c` unmutes it before `init_codec()`. Skip that and
everything initialises perfectly and plays silence.

AEC gets its reference because the ES8311, though a mono codec, is opened with
`no_dac_ref = false` — register `0x44 = 0x58`, "internal reference signal
(ADCL + DACR)" — so a 2-channel record stream carries mic on the left slot and a
speaker loopback on the right. That's why playback is configured as 2-channel
even though Vapi sends mono.

## Using it

| Gesture | Action |
|---------|--------|
| **Tap** | start the call if idle, end it if active |
| **Double tap** | mute / unmute the microphone |
| **Long press** (≥0.7 s) | mute / unmute the assistant |
| **Very long press** (≥2.5 s) | hold — silence both directions, keep the call up |

The 128×128 LCD shows a colour you can read across the room plus a short label:
blue `PUSH TO CALL`, amber `CALLING`, green `TALK`, red `MIC OFF`, purple
`LISTEN ONLY`, yellow `ON HOLD`.

No LVGL — a 475-byte 5×7 bitmap font scaled 2–4× is both larger on screen and far
cheaper. Scale is picked from the longest line, so **keep labels to 10 characters
per line**.

### Console (in the `make monitor` session)

- `start` / `stop` — connect / hang up
- `say "<text>"` — inject a message into the conversation
- `control <mute-assistant|unmute-assistant|...>` — Vapi control envelopes
- `vol <+N|-N>` — speaker volume
- `gain <db>` / `dgain <db>` / `gate <db>|0` — echo tuning, see docs/AEC-TUNING.md
- `rec2play` — local mic→speaker loopback, no WiFi or Vapi needed
- `i` — task/heap status · `dump` — toggle AEC data dump

Expected on success: `POST https://api.vapi.ai/call` → `call <id> created` →
`websocket connected — start talking.` Transcripts then print as `user:` /
`assistant:`, which is the cheapest way to tell a dead microphone from a dead LLM.

> **Console commands cannot be scripted** over USB-Serial-JTAG — they must be
> typed into an interactive `make monitor`. Reading the log *can* be automated.
> Details: [docs/SERIAL-CONSOLE.md](docs/SERIAL-CONSOLE.md).

## WiFi failover

`WIFI_SSID_2` and `WIFI_SSID_3` in `.env` are backup networks, tried in order
when the one above them does not answer. Leave them blank and nothing changes.

Each network gets three attempts before the next is tried. Association on a
normal boot routinely fails twice before succeeding, so a smaller budget is
spent by healthy behaviour alone. A dropped association
and an absent AP are indistinguishable from the device's side and want opposite
responses — the first usually reconnects immediately, the second never will — so
one retry serves the transient without stranding the device on a network that is
not there. On success the current network is kept, so one that just worked is
retried first if it drops.

Note that the underlying networking helper prefers WiFi credentials stored in
NVS over the ones it is passed ("Force to use wifi config from nvs"), which
means that after a first successful connection `WIFI_SSID` would otherwise be
ignored on every later boot. The first network is therefore applied explicitly
at startup rather than left to `network_init()`.

## Layout

```
main/
  main.c          entry point, console, thread tuning
  vapi_client.c   REST call creation + websocket transport
  vapi_media.c    audio pipeline, echo gate, gain stages
  vapi_app.c      call state, button actions, display state
  board.c         codec + PI4IOE speaker unmute
  display.c       GC9107 LCD, 5x7 font
  controls.c      button gesture recognition
  aec_probe.c     boot-time echo diagnostics (off by default)
docs/
  AEC-TUNING.md     echo: measurements, the fix, what not to try
  SERIAL-CONSOLE.md USB-Serial-JTAG quirks
```

## Gotchas

- **Stale WiFi credentials in NVS win over `.env`.** Look for `NETWORK: Force to
  use wifi config from nvs` at boot. Fix: `make erase && make flash`.
- **Don't touch the codec while idle.** The button tone only plays *during* a
  call; opening the codec while idle corrupts state `av_render` needs and kills
  playback.
- **Power is a real failure mode.** The board is bus-powered and the amp draws
  current. Use a powered hub or a direct port — the reset reason prints at every
  boot, so check it before suspecting code.
- **Port auto-detect can pick the wrong device.** Other USB peripherals also
  claim `usbmodem` names; pass `PORT=` explicitly if so.

## Known limitations

- **No barge-in** — the echo gate mutes the mic while the assistant speaks.
- **No reconnect** — the call URL is single-use, so a WiFi blip ends the call
  rather than resuming it.
- **Private API key in the image** — see the warning above.
- The binary still links every audio codec even though only PCM moves;
  `esp_capture` wants an encoder present in order to decline it.
- Gesture thresholds (0.7 s / 2.5 s / 350 ms) were tuned by reading, not by use.

## Credits

- Espressif's [esp-webrtc-solution](https://github.com/espressif/esp-webrtc-solution)
  for the capture/render pipeline and board support.
- [VapiAI/vapicon-2025-hardware-workshop](https://github.com/VapiAI/vapicon-2025-hardware-workshop)
  for the AtomS3R HAL details — the PI4IOE register sequence and the LCD offset
  both came from there.

## License

MIT — see [LICENSE](LICENSE).
