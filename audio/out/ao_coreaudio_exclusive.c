/*
 * CoreAudio audio output driver for macOS
 *
 * original copyright (C) Timothy J. Wood - Aug 2000
 * ported to MPlayer libao2 by Dan Christiansen
 *
 * Chris Roccati
 * Stefano Pigozzi
 *
 * The S/PDIF part of the code is based on the auhal audio output
 * module from VideoLAN:
 * Copyright (c) 2006 Derk-Jan Hartman <hartman at videolan dot org>
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

/*
 * The macOS CoreAudio framework doesn't mesh as simply as some
 * simpler frameworks do.  This is due to the fact that CoreAudio pulls
 * audio samples rather than having them pushed at it (which is nice
 * when you are wanting to do good buffering of audio).
 */

#include <stdatomic.h>
#include <math.h>
#include <string.h>

#include <CoreAudio/HostTime.h>

#include <libavutil/intreadwrite.h>
#include <libavutil/intfloat.h>

#include "ao.h"
#include "internal.h"
#include "audio/dop.h"
#include "audio/format.h"
#include "osdep/timer.h"
#include "options/m_option.h"
#include "common/msg.h"
#include "audio/out/ao_coreaudio_chmap.h"
#include "audio/out/ao_coreaudio_properties.h"
#include "audio/out/ao_coreaudio_utils.h"
#include "osdep/mac/compat.h"

struct priv {
    // This must be put in the front
    struct coreaudio_cb_sem sem;

    AudioDeviceID device;   // selected device

    bool paused;

    // audio render callback
    AudioDeviceIOProcID render_cb;

    // pid set for hog mode, (-1) means that hog mode on the device was
    // released. hog mode is exclusive access to a device
    pid_t hog_pid;

    AudioStreamID stream;

    // stream index in an AudioBufferList
    int stream_idx;

    // format we changed the stream to, and the original format to restore
    AudioStreamBasicDescription stream_asbd;
    // The physical format this output installed. macOS re-derives the virtual format from
    // it asynchronously, so the virtual format is not a reliable signal of the device
    // having been taken away from us.
    AudioStreamBasicDescription physical_asbd;
    AudioStreamBasicDescription original_asbd;
    AudioStreamBasicDescription original_virtual_asbd;
    bool original_asbd_sanitized;
    bool changed_virtual_format;
    bool dop_high_aligned;
    bool dop_float_hack;
    bool dop_fa_marker;

    // Output s16 physical format, float32 virtual format, ac3/dts mpv format
    bool spdif_hack;

    bool changed_mixing;
    bool restore_default_device;
    bool needs_warmup;
    bool changed_volume;
    float original_volume;

    atomic_bool reload_requested;
    atomic_bool warming_up;

    uint64_t hw_latency_ns;
};

static int get_volume(struct ao *ao, float *vol);
static int set_volume(struct ao *ao, float *vol);
static int get_mute(struct ao *ao, bool *muted);
static int set_mute(struct ao *ao, bool *muted);

static void restore_dop_volume(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (p->changed_volume) {
        set_volume(ao, &p->original_volume);
        p->changed_volume = false;
    }
}

static OSStatus property_listener_cb(
    AudioObjectID object, uint32_t n_addresses,
    const AudioObjectPropertyAddress addresses[],
    void *data)
{
    struct ao *ao = data;
    struct priv *p = ao->priv;

    // Check whether the device was taken away from us. Compare the physical format, which
    // is the one this output installed: setting a non-mixable physical format makes macOS
    // re-derive the virtual format a moment later, so watching the virtual format sees our
    // own change arrive as if it were somebody else's and reloads the output. The reload
    // installs the format again, which fires this callback again, and playback never
    // starts. Nothing but a seek breaks the cycle, which is what made it look like the
    // play button had stopped working after switching outputs.
    AudioStreamBasicDescription f;
    OSErr err = CA_GET(p->stream, kAudioStreamPropertyPhysicalFormat, &f);
    CHECK_CA_WARN("could not get stream format");
    if (err != noErr || !ca_asbd_equals(&p->physical_asbd, &f)) {
        if (atomic_compare_exchange_strong(&p->reload_requested,
                                           &(bool){false}, true))
        {
            ao_request_reload(ao);
            MP_INFO(ao, "Stream format changed! Reloading.\n");
        }
    }

    return noErr;
}

static OSStatus enable_property_listener(struct ao *ao, bool enabled)
{
    struct priv *p = ao->priv;

    uint32_t selectors[] = {kAudioDevicePropertyDeviceHasChanged,
                            kAudioHardwarePropertyDevices};
    AudioDeviceID devs[] = {p->device,
                            kAudioObjectSystemObject};
    static_assert(MP_ARRAY_SIZE(selectors) == MP_ARRAY_SIZE(devs), "");

    OSStatus status = noErr;
    for (int n = 0; n < MP_ARRAY_SIZE(devs); n++) {
        AudioObjectPropertyAddress addr = {
            .mScope    = kAudioObjectPropertyScopeGlobal,
            .mElement  = kAudioObjectPropertyElementMain,
            .mSelector = selectors[n],
        };
        AudioDeviceID device = devs[n];

        OSStatus status2;
        if (enabled) {
            status2 = AudioObjectAddPropertyListener(
                device, &addr, property_listener_cb, ao);
        } else {
            status2 = AudioObjectRemovePropertyListener(
                device, &addr, property_listener_cb, ao);
        }
        if (status == noErr)
            status = status2;
    }

    return status;
}

// This is a hack for passing through AC3/DTS on drivers which don't support it.
// The goal is to have the driver output the AC3 data bitexact, so basically we
// feed it float data by converting the AC3 data to float in the reverse way we
// assume the driver outputs it.
// Input: data_as_int16[0..samples]
// Output: data_as_float[0..samples]
// The conversion is done in-place.
static void bad_hack_mygodwhy(char *data, int samples)
{
    // In reverse, so we can do it in-place.
    for (int n = samples - 1; n >= 0; n--) {
        int16_t val = AV_RN16(data + n * 2);
        float fval = val / (float)(1 << 15);
        uint32_t ival = av_float2int(fval);
        AV_WN32(data + n * 4, ival);
    }
}

static void dop_hack_mygodwhy(char *data, int frames, int read_frames,
                              int channels, bool as_float, bool high_aligned,
                              bool *fa_marker)
{
    for (int frame = 0; frame < frames; frame++) {
        uint32_t marker = *fa_marker ? 0xfa : 0x05;
        for (int channel = 0; channel < channels; channel++) {
            int index = frame * channels + channel;
            uint32_t word = frame < read_frames ? AV_RN32(data + index * 4)
                                                : 0x6969;
            word = (word & 0xffff) | marker << 16;

            if (as_float) {
                AV_WN32(data + index * 4,
                        av_float2int(mp_dop_word_to_float(word)));
            } else if (high_aligned) {
                AV_WN32(data + index * 4, word << 8);
            } else {
                AV_WN32(data + index * 4,
                        word | (word & 0x800000 ? 0xff000000 : 0));
            }
        }
        *fa_marker = !*fa_marker;
    }
}

static OSStatus render_cb_compressed(
        AudioDeviceID device, const AudioTimeStamp *ts,
        const void *in_data, const AudioTimeStamp *in_ts,
        AudioBufferList *out_data, const AudioTimeStamp *out_ts, void *ctx)
{
    struct ao *ao    = ctx;
    struct priv *p   = ao->priv;
    AudioBuffer buf  = out_data->mBuffers[p->stream_idx];
    int requested    = buf.mDataByteSize;
    int sstride      = p->spdif_hack ? 4 * ao->channels.num : ao->sstride;

    int pseudo_frames = requested / sstride;

    // we expect the callback to read full frames, which are aligned accordingly
    if (pseudo_frames * sstride != requested) {
        MP_ERR(ao, "Unsupported unaligned read of %d bytes.\n", requested);
        return kAudioHardwareUnspecifiedError;
    }

    int64_t end = mp_time_ns();
    end += p->hw_latency_ns + ca_get_latency(ts)
        + ca_frames_to_ns(ao, pseudo_frames);

    bool warming_up = atomic_load_explicit(&p->warming_up, memory_order_relaxed);
    int read_frames = 0;
    if (warming_up) {
        memset(buf.mData, 0, requested);
    } else {
        read_frames =
            ao_read_data(ao, &buf.mData, pseudo_frames, end, NULL, true, true);
    }

    if (ao->format == AF_FORMAT_S_DOP)
        dop_hack_mygodwhy(buf.mData, pseudo_frames, read_frames,
                          ao->channels.num, p->dop_float_hack,
                          p->dop_high_aligned, &p->dop_fa_marker);

    if (p->spdif_hack)
        bad_hack_mygodwhy(buf.mData, pseudo_frames * ao->channels.num);

    return noErr;
}

// Apparently, audio devices can have multiple sub-streams. It's not clear to
// me what devices with multiple streams actually do. So only select the first
// one that fulfills some minimum requirements.
// If this is not sufficient, we could duplicate the device list entries for
// each sub-stream, and make it explicit.
static int select_stream(struct ao *ao)
{
    struct priv *p = ao->priv;

    AudioStreamID *streams;
    size_t n_streams;
    OSStatus err;

    /* Get a list of all the streams on this device. */
    err = CA_GET_ARY_O(p->device, kAudioDevicePropertyStreams,
                       &streams, &n_streams);
    CHECK_CA_ERROR("could not get number of streams");
    for (int i = 0; i < n_streams; i++) {
        uint32_t direction;
        err = CA_GET(streams[i], kAudioStreamPropertyDirection, &direction);
        CHECK_CA_WARN("could not get stream direction");
        if (err == noErr && direction != 0) {
            MP_VERBOSE(ao, "Substream %d is not an output stream.\n", i);
            continue;
        }

        if (af_fmt_is_pcm(ao->format) || ao->format == AF_FORMAT_S_DOP ||
            p->spdif_hack ||
            ca_stream_supports_compressed(ao, streams[i]))
        {
            MP_VERBOSE(ao, "Using substream %d/%zd.\n", i, n_streams);
            p->stream = streams[i];
            p->stream_idx = i;
            break;
        }
    }

    talloc_free(streams);

    if (p->stream_idx < 0) {
        MP_ERR(ao, "No usable substream found.\n");
        goto coreaudio_error;
    }

    return 0;

coreaudio_error:
    return -1;
}

static bool dop_physical_asbd_is_supported(
    const AudioStreamBasicDescription *asbd, int samplerate, int channels)
{
    uint32_t flags = asbd->mFormatFlags;
    return asbd->mFormatID == kAudioFormatLinearPCM &&
           fabs(asbd->mSampleRate - samplerate) < 1.0 &&
           asbd->mChannelsPerFrame == channels &&
           asbd->mBytesPerFrame == 4 * channels &&
           asbd->mFramesPerPacket == 1 &&
           (asbd->mBitsPerChannel == 24 || asbd->mBitsPerChannel == 32) &&
           (flags & kAudioFormatFlagIsSignedInteger) &&
           !(flags & (kAudioFormatFlagIsFloat |
                      kAudioFormatFlagIsBigEndian |
                      kAudioFormatFlagIsNonInterleaved));
}

static bool dop_virtual_asbd_is_supported(
    const AudioStreamBasicDescription *asbd, int samplerate, int channels)
{
    uint32_t flags = asbd->mFormatFlags;
    bool sample_format =
        ((flags & kAudioFormatFlagIsFloat) && asbd->mBitsPerChannel == 32) ||
        ((flags & kAudioFormatFlagIsSignedInteger) &&
         (asbd->mBitsPerChannel == 24 || asbd->mBitsPerChannel == 32));
    return asbd->mFormatID == kAudioFormatLinearPCM &&
           fabs(asbd->mSampleRate - samplerate) < 1.0 &&
           asbd->mChannelsPerFrame == channels &&
           asbd->mBytesPerFrame == 4 * channels &&
           asbd->mFramesPerPacket == 1 && sample_format &&
           !(flags & (kAudioFormatFlagIsBigEndian |
                      kAudioFormatFlagIsNonInterleaved));
}

static int find_best_format(struct ao *ao, AudioStreamBasicDescription *out_fmt)
{
    struct priv *p = ao->priv;

    // Build ASBD for the input format
    AudioStreamBasicDescription asbd;
    ca_fill_asbd(ao, &asbd);
    ca_print_asbd(ao, "our format:", &asbd);

    *out_fmt = (AudioStreamBasicDescription){0};

    AudioStreamRangedDescription *formats;
    size_t n_formats;
    OSStatus err;
    bool prefer_mixable =
        af_fmt_is_pcm(ao->format) && ao->format != AF_FORMAT_S_DOP &&
        !p->spdif_hack;
    bool found_mixable = false;

    err = CA_GET_ARY(p->stream, kAudioStreamPropertyAvailablePhysicalFormats,
                     &formats, &n_formats);
    CHECK_CA_ERROR("could not get number of stream formats");

    for (int j = 0; j < n_formats; j++) {
        AudioStreamBasicDescription candidate = formats[j].mFormat;
        AudioStreamBasicDescription *stream_asbd = &candidate;

        if (prefer_mixable) {
            bool nonmixable =
                stream_asbd->mFormatFlags & kAudioFormatFlagIsNonMixable;
            if (nonmixable && found_mixable)
                continue;
            if (!nonmixable && !found_mixable) {
                *out_fmt = (AudioStreamBasicDescription){0};
                found_mixable = true;
            }
        }

        if (ao->format == AF_FORMAT_S_DOP) {
            AudioValueRange range = formats[j].mSampleRateRange;
            if (asbd.mSampleRate >= range.mMinimum &&
                asbd.mSampleRate <= range.mMaximum)
                stream_asbd->mSampleRate = asbd.mSampleRate;
            if (!dop_physical_asbd_is_supported(stream_asbd, ao->samplerate,
                                                ao->channels.num))
                continue;
        }

        ca_print_asbd(ao, "- ", stream_asbd);

        if (!out_fmt->mFormatID || ca_asbd_is_better(&asbd, out_fmt, stream_asbd))
            *out_fmt = *stream_asbd;
    }

    talloc_free(formats);

    if (!out_fmt->mFormatID) {
        MP_ERR(ao, "no format found\n");
        return -1;
    }

    return 0;
coreaudio_error:
    return -1;
}

// If a previous crash or failed teardown stranded a USB DAC in a non-mixable
// format, recording that as the "original" would restore the broken state forever.
// Prefer the identical mixable variant the device advertises at the same rate.
static void sanitize_original_format(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!(p->original_asbd.mFormatFlags & kAudioFormatFlagIsNonMixable))
        return;

    AudioStreamRangedDescription *formats = NULL;
    size_t count = 0;
    if (CA_GET_ARY(p->stream, kAudioStreamPropertyAvailablePhysicalFormats,
                   &formats, &count) != noErr)
        return;

    uint32_t ignored = kAudioFormatFlagIsNonMixable;
    for (int n = 0; n < count; n++) {
        AudioStreamBasicDescription candidate = formats[n].mFormat;
        if (candidate.mFormatFlags & kAudioFormatFlagIsNonMixable)
            continue;
        if (fabs(candidate.mSampleRate - p->original_asbd.mSampleRate) < 1.0 &&
            candidate.mFormatID == p->original_asbd.mFormatID &&
            (candidate.mFormatFlags & ~ignored) ==
                (p->original_asbd.mFormatFlags & ~ignored) &&
            candidate.mBitsPerChannel == p->original_asbd.mBitsPerChannel &&
            candidate.mBytesPerFrame == p->original_asbd.mBytesPerFrame &&
            candidate.mChannelsPerFrame == p->original_asbd.mChannelsPerFrame)
        {
            MP_WARN(ao, "Original device format was non-mixable; restoring its mixable counterpart.\n");
            p->original_asbd = candidate;
            p->original_asbd_sanitized = true;
            break;
        }
    }
    talloc_free(formats);
}

static void restore_default_device(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!p->restore_default_device)
        return;
    OSStatus err = CA_SET(kAudioObjectSystemObject,
                          kAudioHardwarePropertyDefaultOutputDevice, &p->device);
    CHECK_CA_WARN("could not restore the default output device");
    p->restore_default_device = false;
}

static void restore_stream_formats(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (!p->original_asbd.mFormatID)
        return;

    if (!ca_change_physical_format_sync(ao, p->stream, p->original_asbd))
        MP_WARN(ao, "can't revert to original device format\n");
    p->original_asbd = (AudioStreamBasicDescription){0};

    if (p->changed_virtual_format && !p->original_asbd_sanitized) {
        OSStatus err = CA_SET(p->stream, kAudioStreamPropertyVirtualFormat,
                              &p->original_virtual_asbd);
        CHECK_CA_WARN("can't revert to original virtual format");
        p->changed_virtual_format = false;
    }
}

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;
    int original_format = ao->format;

    OSStatus err = ca_select_device(ao, ao->device, &p->device);
    CHECK_CA_ERROR_L(coreaudio_error_nounlock, "failed to select device");

    ao->format = af_fmt_from_planar(ao->format);

    if (!af_fmt_is_pcm(ao->format) && !af_fmt_is_spdif(ao->format)) {
        MP_ERR(ao, "Unsupported format.\n");
        goto coreaudio_error_nounlock;
    }

    if (af_fmt_is_pcm(ao->format))
        p->spdif_hack = false;

    if (p->spdif_hack) {
        if (af_fmt_to_bytes(ao->format) != 2) {
            MP_ERR(ao, "HD formats not supported with spdif hack.\n");
            goto coreaudio_error_nounlock;
        }
        // Let the pure evil begin!
        ao->format = AF_FORMAT_S16;
    }

    uint32_t is_alive = 1;
    err = CA_GET(p->device, kAudioDevicePropertyDeviceIsAlive, &is_alive);
    CHECK_CA_WARN("could not check whether device is alive");

    if (!is_alive)
        MP_WARN(ao, "device is not alive\n");

    // Hogging a default USB DAC makes macOS move the system default elsewhere. Remember
    // whether this was the default before taking it so auto-selection still means the same
    // device after exclusive mode is turned off or the app quits.
    AudioDeviceID default_device = kAudioObjectUnknown;
    if (CA_GET(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
               &default_device) == noErr)
        p->restore_default_device = default_device == p->device;

    // Take the device. A previous output of our own on the way out can still hold it for
    // a moment, so do not give up on the first refusal; this is the common case when the
    // user turns exclusive mode on while something is already playing.
    for (int attempt = 0; attempt < 20; attempt++) {
        err = ca_lock_device(p->device, &p->hog_pid);
        if (err == noErr)
            break;
        mp_sleep_ns(MP_TIME_MS_TO_NS(25));
    }
    CHECK_CA_WARN("failed to set hogmode");
    // Exclusive output exists to own the device outright. Carrying on without it means
    // reprogramming the stream will be refused, which is only a warning further down, so
    // the output would run believing it had installed a format it never got: the samples
    // are then interpreted at the wrong width or rate and come out as bursts of noise.
    // Refusing here instead keeps the device in a state that still plays correctly.
    if (err != noErr) {
        // Keep playing rather than going silent: give the device up, drop the exclusive
        // request and hand back to the shared output, which does not need to own the
        // device. The next time the output is rebuilt the request is made again, so a
        // moment of contention does not cost exclusive mode for the whole session.
        MP_WARN(ao, "Device is owned by another process, using shared output.\n");
        ao->init_flags &= ~AO_INIT_EXCLUSIVE;
        ao->redirect = "coreaudio";
        goto coreaudio_error;
    }

    if (af_fmt_is_pcm(original_format) && original_format != AF_FORMAT_S_DOP) {
        err = ca_enable_mixing(ao, p->device, true);
        CHECK_CA_WARN("failed to keep PCM mixing enabled");
    } else {
        err = ca_disable_mixing(ao, p->device, &p->changed_mixing);
        CHECK_CA_WARN("failed to disable mixing");
    }

    if (select_stream(ao) < 0)
        goto coreaudio_error;

    AudioStreamBasicDescription hwfmt;
    if (find_best_format(ao, &hwfmt) < 0)
        goto coreaudio_error;
    p->needs_warmup = true;

    err = CA_GET(p->stream, kAudioStreamPropertyPhysicalFormat,
                 &p->original_asbd);
    CHECK_CA_ERROR("could not get stream's original physical format");
    err = CA_GET(p->stream, kAudioStreamPropertyVirtualFormat,
                 &p->original_virtual_asbd);
    CHECK_CA_ERROR("could not get stream's original virtual format");
    sanitize_original_format(ao);

    bool changed_physical = ca_change_physical_format_sync(ao, p->stream, hwfmt);
    // Exclusive output only means anything if the stream really is running the format it
    // asked for. Continuing after a refusal leaves the device on whatever it had, while
    // the samples handed to it are built for the format that was requested, which is
    // heard as bursts of noise. Fail instead and let mpv fall back to shared output.
    if (!changed_physical) {
        MP_ERR(ao, "Could not install the requested device format.\n");
        goto coreaudio_error;
    }

    if (original_format == AF_FORMAT_S_DOP) {
        AudioStreamBasicDescription physical = {0};
        err = CA_GET(p->stream, kAudioStreamPropertyPhysicalFormat, &physical);
        CHECK_CA_ERROR("could not get DoP physical format");
        if (!dop_physical_asbd_is_supported(&physical, ao->samplerate,
                                            ao->channels.num))
        {
            MP_ERR(ao, "Device has no exact integer PCM carrier for DoP.\n");
            goto coreaudio_error;
        }

        AudioStreamBasicDescription virtual = {0};
        err = CA_GET(p->stream, kAudioStreamPropertyVirtualFormat, &virtual);
        CHECK_CA_ERROR("could not get DoP virtual format");
        if (fabs(virtual.mSampleRate - ao->samplerate) >= 1.0) {
            virtual.mSampleRate = ao->samplerate;
            err = CA_SET(p->stream, kAudioStreamPropertyVirtualFormat, &virtual);
            CHECK_CA_ERROR("could not set DoP virtual sample rate");
            p->changed_virtual_format = true;
        }
    }

    if (!ca_init_chmap(ao, p->device))
        goto coreaudio_error;

    err = CA_GET(p->stream, kAudioStreamPropertyVirtualFormat, &p->stream_asbd);
    CHECK_CA_ERROR("could not get stream's virtual format");

    ca_print_asbd(ao, "virtual format", &p->stream_asbd);

    if (p->stream_asbd.mChannelsPerFrame > MP_NUM_CHANNELS) {
        MP_ERR(ao, "unsupported number of channels: %d > %d.\n",
               p->stream_asbd.mChannelsPerFrame, MP_NUM_CHANNELS);
        goto coreaudio_error;
    }

    if (original_format == AF_FORMAT_S_DOP) {
        if (!dop_virtual_asbd_is_supported(&p->stream_asbd, ao->samplerate,
                                           ao->channels.num))
        {
            MP_ERR(ao, "Device has no usable virtual PCM carrier for DoP.\n");
            goto coreaudio_error;
        }
        p->dop_float_hack =
            p->stream_asbd.mFormatFlags & kAudioFormatFlagIsFloat;
        p->dop_high_aligned =
            !p->dop_float_hack &&
            (p->stream_asbd.mBitsPerChannel == 32 ||
             (p->stream_asbd.mFormatFlags & kAudioFormatFlagIsAlignedHigh));
        ao->format = AF_FORMAT_S_DOP;
    } else {
        int new_format = ca_asbd_to_mp_format(&p->stream_asbd);

        // If both old and new formats are spdif, avoid changing it due to the
        // imperfect mapping between mp and CA formats.
        if (!(af_fmt_is_spdif(ao->format) && af_fmt_is_spdif(new_format)))
            ao->format = new_format;
    }

    if (!ao->format || af_fmt_is_planar(ao->format)) {
        MP_ERR(ao, "hardware format not supported\n");
        goto coreaudio_error;
    }

    ao->samplerate = p->stream_asbd.mSampleRate;

    if (ao->channels.num != p->stream_asbd.mChannelsPerFrame) {
        ca_get_active_chmap(ao, p->device, p->stream_asbd.mChannelsPerFrame,
                            &ao->channels);
    }
    if (!ao->channels.num) {
        MP_ERR(ao, "number of channels changed, and unknown channel layout!\n");
        goto coreaudio_error;
    }

    if (p->spdif_hack) {
        AudioStreamBasicDescription physical_format = {0};
        err = CA_GET(p->stream, kAudioStreamPropertyPhysicalFormat,
                     &physical_format);
        CHECK_CA_ERROR("could not get stream's physical format");
        int ph_format = ca_asbd_to_mp_format(&physical_format);
        if (ao->format != AF_FORMAT_FLOAT || ph_format != AF_FORMAT_S16) {
            MP_ERR(ao, "Wrong parameters for spdif hack (%d / %d)\n",
                   ao->format, ph_format);
        }
        ao->format = original_format; // pretend AC3 or DTS *evil laughter*
        MP_WARN(ao, "Using spdif passthrough hack. This could produce noise.\n");
    }

    p->hw_latency_ns = ca_get_device_latency_ns(ao, p->device);
    MP_VERBOSE(ao, "base latency: %lld nanoseconds\n", p->hw_latency_ns);

    // Read the physical format back as late as possible, so the listener below compares
    // against the format the device actually settled on rather than the one we asked for.
    err = CA_GET(p->stream, kAudioStreamPropertyPhysicalFormat, &p->physical_asbd);
    CHECK_CA_ERROR("could not get the installed physical format");

    err = enable_property_listener(ao, true);
    CHECK_CA_ERROR("cannot install format change listener during init");

    err = AudioDeviceCreateIOProcID(p->device,
                                    (AudioDeviceIOProc)render_cb_compressed,
                                    (void *)ao,
                                    &p->render_cb);
    CHECK_CA_ERROR("failed to register audio render callback");

    if (ao->format == AF_FORMAT_S_DOP) {
        float volume = 100;
        if (get_volume(ao, &p->original_volume) == CONTROL_TRUE) {
            p->changed_volume = p->original_volume != volume;
            if (set_volume(ao, &volume) != CONTROL_TRUE)
                MP_WARN(ao, "Could not force unity volume for DoP.\n");
        }
    }

    return CONTROL_TRUE;

coreaudio_error:
    err = enable_property_listener(ao, false);
    CHECK_CA_WARN("can't remove format change listener");
    restore_dop_volume(ao);
    restore_stream_formats(ao);
    err = ca_unlock_device(p->device, &p->hog_pid);
    CHECK_CA_WARN("can't release hog mode");
    restore_default_device(ao);
coreaudio_error_nounlock:
    return CONTROL_ERROR;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;
    OSStatus err = noErr;

    err = enable_property_listener(ao, false);
    CHECK_CA_WARN("can't remove device listener, this may cause a crash");

    err = AudioDeviceStop(p->device, p->render_cb);
    CHECK_CA_WARN("failed to stop audio device");

    err = AudioDeviceDestroyIOProcID(p->device, p->render_cb);
    CHECK_CA_WARN("failed to remove device render callback");

    bool original_mute = false;
    bool used_mute = get_mute(ao, &original_mute) == CONTROL_TRUE;
    if (used_mute) {
        bool muted = true;
        used_mute = set_mute(ao, &muted) == CONTROL_TRUE;
    }
    float restore_volume = 100;
    bool lowered_volume = false;
    if (!used_mute && get_volume(ao, &restore_volume) == CONTROL_TRUE) {
        if (p->changed_volume)
            restore_volume = p->original_volume;
        float silent = 0;
        lowered_volume = set_volume(ao, &silent) == CONTROL_TRUE;
    }

    restore_stream_formats(ao);
    mp_sleep_ns(MP_TIME_S_TO_NS(3));
    if (p->changed_volume) {
        set_volume(ao, &p->original_volume);
        p->changed_volume = false;
    } else if (lowered_volume) {
        set_volume(ao, &restore_volume);
    }
    if (used_mute)
        set_mute(ao, &original_mute);

    err = ca_enable_mixing(ao, p->device, p->changed_mixing);
    CHECK_CA_WARN("can't re-enable mixing");

    err = ca_unlock_device(p->device, &p->hog_pid);
    CHECK_CA_WARN("can't release hog mode");
    restore_default_device(ao);
}

static void audio_pause(struct ao *ao)
{
    struct priv *p = ao->priv;

    OSStatus err = AudioDeviceStop(p->device, p->render_cb);
    CHECK_CA_WARN("can't stop audio device");
    p->dop_fa_marker = false;
}

static void audio_resume(struct ao *ao)
{
    struct priv *p = ao->priv;
    bool original_mute = false;
    bool used_mute = false;
    float original_volume = 100;
    bool lowered_volume = false;

    if (p->needs_warmup) {
        // Hardware formats can be visible before the device clock is ready. Feed
        // silence through the transition instead of leaking it as noise.
        atomic_store_explicit(&p->warming_up, true, memory_order_relaxed);
        if (af_fmt_is_pcm(ao->format) && ao->format != AF_FORMAT_S_DOP) {
            used_mute = get_mute(ao, &original_mute) == CONTROL_TRUE;
            if (used_mute) {
                bool muted = true;
                used_mute = set_mute(ao, &muted) == CONTROL_TRUE;
            }
            if (!used_mute &&
                get_volume(ao, &original_volume) == CONTROL_TRUE) {
                float silent = 0;
                lowered_volume = set_volume(ao, &silent) == CONTROL_TRUE;
            }
        }
        MP_INFO(ao, "Warming up exclusive output before releasing audio.\n");
    }

    OSStatus err = AudioDeviceStart(p->device, p->render_cb);
    CHECK_CA_WARN("can't start audio device");

    if (p->needs_warmup && err == noErr)
        mp_sleep_ns(MP_TIME_S_TO_NS(3));
    if (lowered_volume)
        set_volume(ao, &original_volume);
    if (used_mute)
        set_mute(ao, &original_mute);
    if (p->needs_warmup && err == noErr) {
        p->needs_warmup = false;
        atomic_store_explicit(&p->warming_up, false, memory_order_relaxed);
        MP_INFO(ao, "Exclusive output clock settled.\n");
    }
}

// Volume is handled by the device itself rather than by scaling the samples, since the
// whole point of this output is to hand the source's own bits to the hardware. Devices
// expose either a single main volume control or one per channel, and plenty (HDMI and
// most S/PDIF among them) expose none at all, in which case these report failure and the
// player falls back to its software gain.

static bool ca_volume_elements(struct ao *ao, AudioObjectPropertyElement *elements,
                               int *count)
{
    struct priv *p = ao->priv;
    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioDevicePropertyVolumeScalar,
        .mScope    = kAudioDevicePropertyScopeOutput,
        .mElement  = kAudioObjectPropertyElementMain,
    };

    if (AudioObjectHasProperty(p->device, &addr)) {
        elements[0] = kAudioObjectPropertyElementMain;
        *count = 1;
        return true;
    }

    // No main control, so drive the channels the device nominates as its stereo pair.
    uint32_t stereo[2] = {1, 2};
    AudioObjectPropertyAddress pref = {
        .mSelector = kAudioDevicePropertyPreferredChannelsForStereo,
        .mScope    = kAudioDevicePropertyScopeOutput,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    uint32_t size = sizeof(stereo);
    AudioObjectGetPropertyData(p->device, &pref, 0, NULL, &size, stereo);

    *count = 0;
    for (int n = 0; n < 2; n++) {
        addr.mElement = stereo[n];
        if (AudioObjectHasProperty(p->device, &addr))
            elements[(*count)++] = stereo[n];
    }
    return *count > 0;
}

static int get_volume(struct ao *ao, float *vol)
{
    struct priv *p = ao->priv;
    AudioObjectPropertyElement elements[2];
    int count;

    if (!ca_volume_elements(ao, elements, &count))
        return CONTROL_FALSE;

    // Report the loudest channel, so a device left with unbalanced channels does not read
    // back as quieter than it is.
    float scalar = 0;
    for (int n = 0; n < count; n++) {
        AudioObjectPropertyAddress addr = {
            .mSelector = kAudioDevicePropertyVolumeScalar,
            .mScope    = kAudioDevicePropertyScopeOutput,
            .mElement  = elements[n],
        };
        float channel;
        uint32_t size = sizeof(channel);
        OSStatus err = AudioObjectGetPropertyData(p->device, &addr, 0, NULL, &size,
                                                  &channel);
        if (err != noErr)
            return CONTROL_FALSE;
        if (channel > scalar)
            scalar = channel;
    }

    *vol = scalar * 100.0;
    return CONTROL_OK;
}

static int set_volume(struct ao *ao, float *vol)
{
    struct priv *p = ao->priv;
    AudioObjectPropertyElement elements[2];
    int count;

    if (!ca_volume_elements(ao, elements, &count))
        return CONTROL_FALSE;

    float scalar = MPCLAMP(*vol / 100.0, 0.0, 1.0);
    for (int n = 0; n < count; n++) {
        AudioObjectPropertyAddress addr = {
            .mSelector = kAudioDevicePropertyVolumeScalar,
            .mScope    = kAudioDevicePropertyScopeOutput,
            .mElement  = elements[n],
        };
        OSStatus err = AudioObjectSetPropertyData(p->device, &addr, 0, NULL,
                                                  sizeof(scalar), &scalar);
        if (err != noErr) {
            CHECK_CA_WARN("could not set device volume");
            return CONTROL_FALSE;
        }
    }
    return CONTROL_OK;
}

static int get_mute(struct ao *ao, bool *muted)
{
    struct priv *p = ao->priv;
    uint32_t value;
    OSStatus err = CA_GET_O(p->device, kAudioDevicePropertyMute, &value);
    if (err != noErr)
        return CONTROL_FALSE;
    *muted = value;
    return CONTROL_OK;
}

static int set_mute(struct ao *ao, bool *muted)
{
    struct priv *p = ao->priv;
    uint32_t value = *muted;
    OSStatus err = CA_SET_O(p->device, kAudioDevicePropertyMute, &value);
    if (err != noErr) {
        CHECK_CA_WARN("could not set device mute");
        return CONTROL_FALSE;
    }
    return CONTROL_OK;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    switch (cmd) {
    case AOCONTROL_GET_VOLUME:
        if (ao->format == AF_FORMAT_S_DOP) {
            *(float *)arg = 100;
            return CONTROL_TRUE;
        }
        return get_volume(ao, arg);
    case AOCONTROL_SET_VOLUME:
        if (ao->format == AF_FORMAT_S_DOP) {
            float volume = 100;
            return set_volume(ao, &volume);
        }
        return set_volume(ao, arg);
    case AOCONTROL_GET_MUTE:
        return get_mute(ao, arg);
    case AOCONTROL_SET_MUTE:
        return set_mute(ao, arg);
    }
    return CONTROL_UNKNOWN;
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_coreaudio_exclusive = {
    .description = "CoreAudio Exclusive Mode",
    .name      = "coreaudio_exclusive",
    .uninit    = uninit,
    .init      = init,
    .control   = control,
    .reset     = audio_pause,
    .start     = audio_resume,
    .list_devs = ca_get_device_list,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv){
        .sem = (struct coreaudio_cb_sem){
            .mutex = MP_STATIC_MUTEX_INITIALIZER,
            .cond = MP_STATIC_COND_INITIALIZER,
        },
        .hog_pid = -1,
        .stream = 0,
        .stream_idx = -1,
        .changed_mixing = false,
    },
    .options = (const struct m_option[]){
        {"spdif-hack", OPT_BOOL(spdif_hack)},
        {0}
    },
    .options_prefix = "coreaudio",
};
