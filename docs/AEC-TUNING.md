# Echo on the AtomS3R + Atomic Echo Base

The AtomS3R's speaker sits about 2 cm from its microphone, and the acoustic
echo canceller in Espressif's audio front-end does not cope with that on its
own. The first real call made this obvious: the assistant heard itself and held
a conversation with itself.

This is what was actually wrong, what the working configuration is, and — since
three of the fixes along the way were wrong — which reasoning to distrust. If
you are bringing up a different board or enclosure, this is the page to read.

## The working configuration

```c
DEFAULT_PLAYBACK_VOL        90     // echo no longer constrains this
VAPI_MIC_GAIN_DB           24.0f   // analog, before the ADC
VAPI_MIC_DIGITAL_GAIN_DB    6.0f   // post-AEC makeup
VAPI_ECHO_GATE                 1   // hard mute while the assistant is audible
VAPI_ECHO_GATE_HANGOVER_MS   400
```

Verified on hardware — during assistant speech the gate is shut on 100% of
frames and the post-gate microphone peak is **0**; between phrases it opens and
the talker comes through:

```
gate: shut 101/101 | far hits 64 | playout lead 177 ms | in pk 7758 | mic pk POST-gate 0
gate: shut   0/101 | far hits  0 | playout lead 187 ms | in pk    0 | mic pk POST-gate 1773
```

The trade is **no barge-in**: you cannot interrupt the assistant by talking over
it. Tap the button, or long-press to mute it.

## What actually fixed it

**A delay-compensated hard mute of the microphone while the assistant is
audible.** Not AEC tuning. The gain work below helps the AEC converge and is
worth keeping, but it never came close to solving this on its own.

The device has an advantage the AEC does not: *we* write every inbound frame to
the speaker, so we know exactly what the far end is about to say. The only hard
part is knowing *when* it becomes audible, because playback lags the websocket
by a few hundred milliseconds.

`av_render_get_audio_fifo_level()` looks like the answer and is not — **measured,
it returns `duration = 0` on every call**. A gate built on it mutes from *now*
for its hangover, which covers the wrong window entirely: it mutes silence, then
opens mid-sentence. Telemetry showed it shut anywhere between 12% and 99% of
frames with no relation to whether the assistant was speaking, and echo sailed
through.

What works is a **write-ahead playout clock**, which needs no API at all. Track
the wall-clock time at which the audio queued so far will finish playing:

```
on each inbound frame:
    start = max(playout_end, now)          // if it fell behind, the queue drained
    playout_end = start + frame_duration
    if frame is loud:
        far_until = max(far_until, playout_end + HANGOVER)
```

Bytes written and the sample rate are enough to know this exactly. It reports a
`playout lead` of 142–300 ms on this board — which is the real number the FIFO
API was claiming was zero.

Mute rather than attenuate. −40 dB of a peak-20000 echo still leaves ~200, and
Vapi's STT will happily transcribe that.

## Three things that were wrong along the way

Recorded because each was plausible, each cost a flash and a billed call, and
each came from reasoning ahead of measuring.

**"The mic is clipping."** It is not. Peak at volume 85 was 25355 of 32767 —
hot, not saturated. This was asserted before anything was measured.

**"Cut analog mic gain; it is better than turning the speaker down because it
keeps the speaker loud."** Half right, and the wrong half mattered. Cutting
analog gain 30 → 18 dB did let the AEC converge, and it did kill the echo — by
also killing the talker. Vapi received *nothing*: three "Are you still there?"
and zero `user:` transcripts.

**"Speaker volume is the knob that matters, because it is the only one that
changes the echo-to-voice ratio."** The ratio argument is correct (see below),
but the conclusion was still wrong: dropping 85 → 60 bought 12.5 dB and the
residual echo was *still* transcribed. esp-sr's AEC cancels too little on this
board for any gain setting to work.

## Why gain alone can never fix this

Sort the knobs by their effect on the echo-to-voice ratio at the microphone —
the only quantity that decides whether STT latches onto the assistant:

| knob | echo | talker | ratio |
|---|---|---|---|
| analog mic gain | ↓ | ↓ | **unchanged** |
| digital makeup gain | ↓ | ↓ | **unchanged** |
| speaker volume | ↓ | — | improves |
| AEC cancellation | ↓ | — | improves |
| **far-end mute (the gate)** | **→ 0** | — | **decisive** |

Both gain stages scale echo and talker together, so neither can ever separate
them; they only trade AEC convergence against absolute level. That leaves
speaker volume and the AEC — and the AEC's residual here is too shallow, while
turning the speaker down far enough to hide it makes the device inaudible.
Muting the far end outright is the only move that wins.

## What the gain settings are still for

The AEC still runs, and still helps at the gate's edges. For it to converge at
all, the echo must not dominate its reference.

**The reference is real and correct.** Register `0x44` reads back `0x58`
("internal reference signal (ADCL + DACR)"), and the right capture slot is
silent at rest and rises with a tone. It is also a **bit-exact digital copy of
the DAC input** — measured constant at peak 9000 for a 9000-amplitude tone
across the entire volume range — so it is tapped before the volume control and
untouched by analog mic gain.

Measured at speaker volume 85, echo level relative to that reference:

| mic gain | mic rms | mic peak | echo vs ref |
|---|---|---|---|
| 30 dB (codec default) | 17577 | 25572 | **+8.8 dB** — cannot converge |
| 24 dB | 8795 | 12783 | +2.8 dB |
| 18 dB | 4395 | 6376 | −3.2 dB |
| 12 dB | 2196 | 3236 | −9.2 dB |
| 0 dB | 549 | 818 | −21.3 dB |

24 dB analog with +6 dB digital makeup gives the AEC headroom while keeping the
talker at roughly the sensitivity the 30 dB default provided. The makeup gain is
applied **after** the AEC — applying it before would scale the echo straight
back up and undo the point.

## Re-measuring

`main/aec_probe.c` produces the tables above. It is compiled out by default
(`VAPI_AEC_PROBE` in `settings.h`); set it to 1 and `make run`, and it sweeps
speaker volume and mic gain at boot, printing register `0x44` and the per-slot
levels. It costs about a second of boot and makes the board chirp.

Redo it after anything that changes the acoustics — a case, moving the Echo
Base, a different speaker. Live knobs during a call, no reflash needed:

- `gate <db>|0` — mic attenuation while the assistant speaks; `gate 0` is the
  clean way to A/B how much the AEC is really doing on its own
- `gain <db>` — analog mic gain; governs whether the AEC converges
- `dgain <db>` — post-AEC makeup; governs how well Vapi hears you
- `vol <+N|-N>` — speaker volume

## Loose end

The AFE reports AGC disabled:

```
AFE_VC: wakenet_init: 0, voice_communication_agc_init: 0
```

Not a misconfiguration on our side — it is inside an `#if 0` upstream in
esp-webrtc-solution (`components/esp_capture/src/impl/capture_audio_src/capture_aud_aec_src.c`)
and references an `algo->agc_gain` that no longer exists in that function, so it
would not compile if simply enabled. Working AGC would let the mic run quieter
still while lifting a distant talker, which is exactly the tension the manual
makeup gain is papering over.
