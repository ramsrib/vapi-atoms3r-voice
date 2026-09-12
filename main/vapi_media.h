/* Audio pipeline for the Vapi AtomS3R voice client.
 *
 * Deliberately named vapi_media.h rather than media_sys.h: the vendored
 * esp-webrtc-solution ships its own solutions/common/media_sys.h, which
 * includes esp_webrtc.h. We do not use esp_webrtc here (Vapi's websocket
 * transport carries raw PCM), and a same-named header on the include path
 * would resolve unpredictably.
 */

#pragma once

#include <stdint.h>
#include "esp_capture.h"
#include "av_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up the capture (mic → AEC → PCM) and render (PCM → speaker) systems. */
int vapi_media_buildup(void);

/**
 * @brief  Start capturing mic audio into the primary sink.
 *
 * Configures the sink for raw PCM at VAPI_SAMPLE_RATE. Because both the source
 * and the sink are PCM, esp_capture bypasses the encoder entirely — the frames
 * that come out of vapi_media_read_frame() are exactly what goes on the wire.
 */
int vapi_media_capture_start(void);

/**
 * @brief  Set the microphone analog input gain, in dB.
 *
 * This is the AEC's main tuning knob on this board — see VAPI_MIC_GAIN_DB.
 */
int vapi_media_set_mic_gain(float db);

/**
 * @brief  Set the post-AEC digital makeup gain, in dB (0..40).
 *
 * Applied after cancellation, so it lifts the talker without lifting the echo.
 * See VAPI_MIC_DIGITAL_GAIN_DB.
 */
int vapi_media_set_digital_gain(float db);

/**
 * @brief  Attenuate the mic while the assistant is audible, in dB (0 = off).
 *
 * Delay-compensated against the render FIFO. See VAPI_ECHO_GATE — this costs
 * barge-in, and on this board it is what actually stops the echo.
 */
int vapi_media_set_echo_gate(float atten_db);

/** Stop capture and release the sink path. */
int vapi_media_capture_stop(void);

/**
 * @brief  Pull one captured PCM frame, if one is ready.
 *
 * Non-blocking. Returns the number of bytes written into @p buf, 0 when no
 * frame is pending, or negative on error.
 */
int vapi_media_read_frame(uint8_t *buf, int buf_size);

/** Open the speaker path for inbound PCM. */
int vapi_media_play_start(void);

/** Flush and close the speaker path. */
int vapi_media_play_stop(void);

/** Queue inbound PCM (16 kHz mono s16le) for playback. */
int vapi_media_write_frame(const uint8_t *data, int size);

/** Local mic → speaker loopback, for checking the hardware without a call. */
int test_capture_to_player(void);

#ifdef __cplusplus
}
#endif
