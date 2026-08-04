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

#include <math.h>
#include <string.h>

#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "mpv_talloc.h"
#include "audio/aframe.h"
#include "audio/chmap.h"
#include "audio/chmap_avchannel.h"
#include "audio/format.h"
#include "common/common.h"
#include "common/msg.h"

#include "bd_sfx.h"

// A copied authored clip waiting to be played: interleaved 48 kHz S16 LPCM.
struct bd_sfx_clip {
    int16_t *samples;   // owned (talloc child of the mixer)
    int num_frames;
    int num_channels;   // 1 (mono) or 2 (stereo)
};

// The clip currently being overlaid, already resampled to the output route's
// rate / channel map and converted to the mix-domain float format.
struct bd_sfx_active {
    float *pcm;         // owned; interleaved, `channels` planes
    int total_frames;
    int pos;            // frames already mixed
    int channels;
};

struct mp_bd_sfx {
    struct mp_log *log;

    // Bounded FIFO of pending clips (drop-on-full, never grows).
    struct bd_sfx_clip pending[MP_BD_SFX_MAX_PENDING];
    int num_pending;

    struct bd_sfx_active active;

    // Cached resampler and the output parameters it is configured for. Rebuilt
    // only when the clip channel count or the output route changes.
    struct SwrContext *swr;
    int swr_in_channels;
    int cfg_out_rate;
    struct mp_chmap cfg_out_chmap;
};

static void sfx_destroy(void *p)
{
    struct mp_bd_sfx *sfx = p;
    swr_free(&sfx->swr);
}

struct mp_bd_sfx *mp_bd_sfx_create(void *ta_parent, struct mp_log *log)
{
    struct mp_bd_sfx *sfx = talloc_zero(ta_parent, struct mp_bd_sfx);
    sfx->log = log;
    talloc_set_destructor(sfx, sfx_destroy);
    return sfx;
}

int mp_bd_sfx_num_pending(struct mp_bd_sfx *sfx)
{
    return sfx->num_pending;
}

bool mp_bd_sfx_add(struct mp_bd_sfx *sfx, const int16_t *samples,
                   int num_frames, int num_channels)
{
    if (!samples || num_frames <= 0 ||
        (num_channels != 1 && num_channels != 2))
        return false;

    if (sfx->num_pending >= MP_BD_SFX_MAX_PENDING) {
        // Rapid button mashing must stay bounded: drop the newest rather than
        // grow without limit. The clips already queued still play.
        if (sfx->log)
            MP_TRACE(sfx, "sound effect queue full, dropping clip\n");
        return false;
    }

    size_t n = (size_t)num_frames * num_channels;
    int16_t *copy = talloc_array(sfx, int16_t, n);
    memcpy(copy, samples, n * sizeof(int16_t));
    sfx->pending[sfx->num_pending++] = (struct bd_sfx_clip){
        .samples = copy,
        .num_frames = num_frames,
        .num_channels = num_channels,
    };
    return true;
}

static void drop_active(struct mp_bd_sfx *sfx)
{
    talloc_free(sfx->active.pcm);
    sfx->active = (struct bd_sfx_active){0};
}

void mp_bd_sfx_flush(struct mp_bd_sfx *sfx)
{
    drop_active(sfx);
    for (int n = 0; n < sfx->num_pending; n++)
        talloc_free(sfx->pending[n].samples);
    sfx->num_pending = 0;
}

// (Re)configure the resampler for `in_channels` -> the current output route.
static bool ensure_swr(struct mp_bd_sfx *sfx, int in_channels, int out_rate,
                       const struct mp_chmap *out_chmap)
{
    if (sfx->swr && sfx->swr_in_channels == in_channels &&
        sfx->cfg_out_rate == out_rate &&
        mp_chmap_equals(&sfx->cfg_out_chmap, out_chmap))
        return true;

    AVChannelLayout in_layout = {0}, out_layout = {0};
    av_channel_layout_default(&in_layout, in_channels);
    mp_chmap_to_av_layout(&out_layout, out_chmap);

    struct SwrContext *s = NULL;
    int rc = swr_alloc_set_opts2(&s, &out_layout, AV_SAMPLE_FMT_FLT, out_rate,
                                 &in_layout, AV_SAMPLE_FMT_S16,
                                 MP_BD_SFX_SRC_RATE, 0, NULL);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    if (rc < 0 || !s) {
        swr_free(&s);
        if (sfx->log)
            MP_WARN(sfx, "could not allocate sound effect resampler\n");
        return false;
    }
    if (swr_init(s) < 0) {
        swr_free(&s);
        if (sfx->log)
            MP_WARN(sfx, "could not init sound effect resampler\n");
        return false;
    }

    swr_free(&sfx->swr);
    sfx->swr = s;
    sfx->swr_in_channels = in_channels;
    sfx->cfg_out_rate = out_rate;
    sfx->cfg_out_chmap = *out_chmap;
    return true;
}

// Pop the next pending clip, resample it to the output route once, and make it
// the active effect. Returns false when nothing could be prepared.
static bool activate_next(struct mp_bd_sfx *sfx, int out_rate,
                          const struct mp_chmap *out_chmap)
{
    if (sfx->num_pending <= 0)
        return false;

    struct bd_sfx_clip clip = sfx->pending[0];
    memmove(&sfx->pending[0], &sfx->pending[1],
            (sfx->num_pending - 1) * sizeof(sfx->pending[0]));
    sfx->num_pending--;

    if (!ensure_swr(sfx, clip.num_channels, out_rate, out_chmap)) {
        talloc_free(clip.samples);
        return false;
    }

    int out_ch = out_chmap->num;
    int max_out = swr_get_out_samples(sfx->swr, clip.num_frames);
    if (max_out < clip.num_frames)
        max_out = (int)av_rescale_rnd(clip.num_frames, out_rate,
                                      MP_BD_SFX_SRC_RATE, AV_ROUND_UP);
    max_out += 64; // resampler latency headroom

    float *pcm = talloc_array(sfx, float, (size_t)max_out * out_ch);
    const uint8_t *in[1] = { (const uint8_t *)clip.samples };
    uint8_t *out[1] = { (uint8_t *)pcm };
    int got = swr_convert(sfx->swr, out, max_out, in, clip.num_frames);
    if (got >= 0 && got < max_out) {
        out[0] = (uint8_t *)(pcm + (size_t)got * out_ch);
        int more = swr_convert(sfx->swr, out, max_out - got, NULL, 0);
        if (more > 0)
            got += more;
    }
    talloc_free(clip.samples); // fully consumed by the resampler

    if (got <= 0) {
        talloc_free(pcm);
        return false;
    }

    sfx->active = (struct bd_sfx_active){
        .pcm = pcm,
        .total_frames = got,
        .pos = 0,
        .channels = out_ch,
    };
    return true;
}

void mp_bd_sfx_mix(struct mp_bd_sfx *sfx, struct mp_aframe *af,
                   bool ao_bit_exact)
{
    // Fast path: nothing to play and nothing queued. This is the common case on
    // every audio frame once a menu is up, so keep it free of aframe queries.
    if (!sfx->active.pcm && sfx->num_pending == 0)
        return;

    int format = mp_aframe_get_format(af);

    if (!mp_bd_sfx_route_allows_mix(format, ao_bit_exact)) {
        // A blocked route (passthrough, DoP, PCM-to-DSD, bit-exact carrier)
        // must never carry an overlay; drop anything queued so it does not
        // surface later on the same non-mixable stream.
        mp_bd_sfx_flush(sfx);
        return;
    }

    // Overlay directly into the packed PCM the AO consumes. Real Core Audio
    // routes are float or int; anything else (planar, U8, 64-bit, double) is
    // left untouched rather than guessed at.
    if (format != AF_FORMAT_FLOAT && format != AF_FORMAT_S16 &&
        format != AF_FORMAT_S32)
        return;
    if (mp_aframe_get_planes(af) != 1)
        return;

    int n = mp_aframe_get_size(af);
    if (n <= 0)
        return;
    int rate = mp_aframe_get_rate(af);
    struct mp_chmap chmap;
    if (!mp_aframe_get_chmap(af, &chmap) || chmap.num < 1)
        return;

    // If the output route changed under an in-flight effect, stop it; the next
    // clip is resampled fresh for the new format.
    if (sfx->active.pcm &&
        (sfx->active.channels != chmap.num || sfx->cfg_out_rate != rate ||
         !mp_chmap_equals(&sfx->cfg_out_chmap, &chmap)))
        drop_active(sfx);

    if (!sfx->active.pcm && !activate_next(sfx, rate, &chmap))
        return;

    uint8_t **data = mp_aframe_get_data_rw(af);
    if (!data || !data[0])
        return;

    int ch = sfx->active.channels;
    int cnt = MPMIN(n, sfx->active.total_frames - sfx->active.pos);
    if (cnt <= 0) {
        drop_active(sfx);
        return;
    }

    const float *src = sfx->active.pcm + (size_t)sfx->active.pos * ch;
    int nsamp = cnt * ch;
    switch (format) {
    case AF_FORMAT_FLOAT:
        mp_bd_sfx_mix_f32((float *)data[0], src, nsamp, MP_BD_SFX_HEADROOM);
        break;
    case AF_FORMAT_S16:
        mp_bd_sfx_mix_s16((int16_t *)data[0], src, nsamp, MP_BD_SFX_HEADROOM);
        break;
    case AF_FORMAT_S32:
        mp_bd_sfx_mix_s32((int32_t *)data[0], src, nsamp, MP_BD_SFX_HEADROOM);
        break;
    }

    sfx->active.pos += cnt;
    if (sfx->active.pos >= sfx->active.total_frames)
        drop_active(sfx); // finished; a queued clip starts on the next frame
}

// --- Pure helpers (unit-tested) -------------------------------------------

bool mp_bd_sfx_route_allows_mix(int out_format, bool ao_bit_exact)
{
    // af_fmt_is_pcm() is false for every IEC61937 passthrough format and for
    // AF_FORMAT_S_DOP, so SPDIF and DoP are rejected here. ao_bit_exact covers
    // the routes that still look like PCM at this stage but are carried
    // bit-for-bit (PCM-to-DSD, non-mixable exclusive).
    return af_fmt_is_pcm(out_format) && !af_fmt_is_spdif(out_format) &&
           !ao_bit_exact;
}

void mp_bd_sfx_mix_f32(float *dst, const float *src, int n, float gain)
{
    for (int i = 0; i < n; i++)
        dst[i] = MPCLAMP(dst[i] + src[i] * gain, -1.0f, 1.0f);
}

void mp_bd_sfx_mix_s16(int16_t *dst, const float *src, int n, float gain)
{
    for (int i = 0; i < n; i++) {
        float v = dst[i] * (1.0f / 32768.0f) + src[i] * gain;
        long r = lrintf(MPCLAMP(v, -1.0f, 1.0f) * 32768.0f);
        dst[i] = MPCLAMP(r, -32768, 32767);
    }
}

void mp_bd_sfx_mix_s32(int32_t *dst, const float *src, int n, float gain)
{
    for (int i = 0; i < n; i++) {
        double v = dst[i] * (1.0 / 2147483648.0) + (double)src[i] * gain;
        long long r = llrint(MPCLAMP(v, -1.0, 1.0) * 2147483648.0);
        dst[i] = MPCLAMP(r, INT32_MIN, INT32_MAX);
    }
}
