/*
 * Authored Blu-ray menu sound effects.
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MPV_AUDIO_BD_SFX_H
#define MPV_AUDIO_BD_SFX_H

#include <stdbool.h>
#include <stdint.h>

// libbluray hands us short authored menu clips through BD_EVENT_SOUND_EFFECT
// (fetched with bd_get_sound_effect()): always 48 kHz, 16-bit LPCM, 1 or 2
// channels. We overlay them on top of the ordinary decoded PCM that is already
// on its way to the audio output, reusing mpv's normal aframe / channel-map /
// resampling machinery (libswresample, exactly what f_swresample uses) instead
// of standing up a second audio path.
//
// Threading: everything here is driven from the player core thread. Both the
// producer (mp_handle_nav polling the stream) and the consumer (ao_process
// mixing into the outgoing frame) run there, so no locking is needed inside.
// The BLURAY* stays on the stream/demux thread; it copies the PCM out and the
// core thread only ever sees owned buffers.

struct mp_aframe;
struct mp_log;
struct mp_bd_sfx;

#define MP_BD_SFX_SRC_RATE     48000  // libbluray effects are always 48 kHz
#define MP_BD_SFX_MAX_PENDING  8      // hard cap on queued clips (drop-on-full)
// Overlay attenuation. Menu beeps ride on top of program audio that may already
// be near full scale, so pull the effect down (~-6 dB); saturation is the
// backstop, this keeps it from being the common case.
#define MP_BD_SFX_HEADROOM     0.5f

struct mp_bd_sfx *mp_bd_sfx_create(void *ta_parent, struct mp_log *log);

// Copy an authored effect (interleaved if stereo) into the bounded pending
// queue. Returns false if the clip was rejected (invalid, or the queue is at
// MP_BD_SFX_MAX_PENDING); nothing is retained in that case.
bool mp_bd_sfx_add(struct mp_bd_sfx *sfx, const int16_t *samples,
                   int num_frames, int num_channels);

// Overlay the next slice of the active/queued effect onto an outgoing aframe,
// converting to the frame's rate / channel map / sample format on the way.
// No-op (and the whole queue is flushed) whenever overlaying would break
// bit-perfect delivery: `ao_bit_exact` reports the AO carrier (PCM-to-DSD,
// non-mixable exclusive, ...), while SPDIF / DoP frames are recognised from the
// frame format itself.
void mp_bd_sfx_mix(struct mp_bd_sfx *sfx, struct mp_aframe *af,
                   bool ao_bit_exact);

// Drop the active effect and everything queued (route change, reset, or the
// user leaving the menu).
void mp_bd_sfx_flush(struct mp_bd_sfx *sfx);

// Number of clips currently queued (excludes the one being mixed). For
// inspection and tests.
int mp_bd_sfx_num_pending(struct mp_bd_sfx *sfx);

// True while the mixer still has something to emit: a clip is being mixed or at
// least one is queued. Used to decide whether to synthesize silence to drain an
// effect on a menu with no program audio, and to bound that synthesis.
bool mp_bd_sfx_has_output(struct mp_bd_sfx *sfx);

// --- Pure helpers, unit-tested in isolation (no libbluray/AO/thread state) ---

// True when overlaying decoded PCM effects is safe for this output route.
// out_format is an af_format; ao_bit_exact is the AO carrier flag.
bool mp_bd_sfx_route_allows_mix(int out_format, bool ao_bit_exact);

// Saturating overlay of mix-domain float `src` (nominally [-1, 1]) onto `n`
// packed destination samples at `gain`. One entry point per supported output
// sample format so the convert + add + clip path is exercised directly.
void mp_bd_sfx_mix_f32(float *dst, const float *src, int n, float gain);
void mp_bd_sfx_mix_s16(int16_t *dst, const float *src, int n, float gain);
void mp_bd_sfx_mix_s32(int32_t *dst, const float *src, int n, float gain);

#endif // MPV_AUDIO_BD_SFX_H
