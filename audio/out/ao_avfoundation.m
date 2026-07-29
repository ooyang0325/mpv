/*
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

#include "ao.h"
#include "audio/format.h"
#include "audio/out/ao_coreaudio_chmap.h"
#include "audio/out/ao_coreaudio_utils.h"
#include "common/common.h"
#include "common/msg.h"
#include "internal.h"
#include "osdep/timer.h"
#include "ta/ta_talloc.h"

#include <libavcodec/avcodec.h>

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#import <CoreAudioTypes/CoreAudioTypes.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include "ao_avfoundation_eac3.h"


@interface AVObserver : NSObject {
    struct ao *ao;
}
- (void)handleRestartNotification:(NSNotification*)notification;
@end

struct priv {
    AVSampleBufferAudioRenderer *renderer;
    AVSampleBufferRenderSynchronizer *synchronizer;
    dispatch_queue_t queue;
    CMAudioFormatDescriptionRef format_description;
    AVObserver *observer;
    int64_t end_time_av;

    // Compressed (E-AC-3) passthrough state. When set, ao_read_data() hands us
    // an IEC 61937 byte stream from ad_spdif, which we unwrap back into raw
    // elementary frames for AVSampleBufferAudioRenderer to decode itself.
    bool compressed;
    uint8_t *burst;             // accumulates across feed() calls
    size_t burst_len;
    int compressed_rate;        // real sample rate, not the 192 kHz carrier
    int64_t compressed_pts;     // in compressed_rate units
    int64_t compressed_packets; // enqueued so far, for diagnostics
    bool compressed_failed;     // renderer.error already reported
};

static int64_t CMTimeGetNanoseconds(CMTime time)
{
    time = CMTimeConvertScale(time, 1000000000, kCMTimeRoundingMethod_Default);
    return time.value;
}

static CMTime CMTimeFromNanoseconds(int64_t time)
{
    return CMTimeMake(time, 1000000000);
}

// Build the 'ec-3' format description once the first real frame tells us the
// sample rate and channel count. Deferred because ad_spdif reports the IEC
// 61937 carrier (192 kHz stereo), not the actual audio.
static int eac3_profile(const uint8_t *data, size_t size)
{
    int profile = AV_PROFILE_UNKNOWN;
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_EAC3);
    AVCodecContext *codec = avcodec_alloc_context3(NULL);
    if (parser && codec) {
        parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
        uint8_t *out = NULL;
        int out_size = 0;
        if (av_parser_parse2(parser, codec, &out, &out_size, data, size,
                             AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0) > 0)
            profile = codec->profile;
    }
    avcodec_free_context(&codec);
    if (parser)
        av_parser_close(parser);
    return profile;
}

static bool compressed_make_format(struct ao *ao, const struct eac3_frame *fr,
                                   const uint8_t *data, size_t size)
{
    struct priv *p = ao->priv;
    if (p->format_description)
        return true;

    bool atmos = eac3_profile(data, size) == AV_PROFILE_EAC3_DDP_ATMOS;
    uint8_t cookie[EAC3_DEC3_COOKIE_MAX_BYTES];
    size_t cookie_size = eac3_make_dec3_cookie(fr, size, atmos, cookie);
    if (!cookie_size)
        MP_WARN(ao, "E-AC-3 dependent substreams have no dec3 metadata; "
                    "using the bitstream layout\n");

    AudioStreamBasicDescription asbd = {
        .mSampleRate       = fr->rate,
        .mFormatID         = kAudioFormatEnhancedAC3,
        .mFramesPerPacket  = fr->samples,
        .mChannelsPerFrame = fr->channels,
    };
    AudioChannelLayout layout = {0};
    const AudioChannelLayout *layout_ptr = NULL;
    size_t layout_size = 0;

    AudioFormatInfo info = {
        .mASBD = asbd,
        .mMagicCookie = cookie_size ? cookie : NULL,
        .mMagicCookieSize = cookie_size,
    };
    UInt32 list_size = 0;
    OSStatus err = AudioFormatGetPropertyInfo(kAudioFormatProperty_FormatList,
                                               sizeof(info), &info, &list_size);
    AudioFormatListItem *formats = err == noErr ? malloc(list_size) : NULL;
    if (formats) {
        err = AudioFormatGetProperty(kAudioFormatProperty_FormatList,
                                     sizeof(info), &info, &list_size, formats);
        UInt32 count = list_size / sizeof(*formats);
        UInt32 selected = 0;
        UInt32 selected_size = sizeof(selected);
        if (err == noErr && count &&
            AudioFormatGetProperty(kAudioFormatProperty_FirstPlayableFormatFromList,
                                   list_size, formats, &selected_size,
                                   &selected) == noErr &&
            selected < count)
        {
            asbd = formats[selected].mASBD;
            layout.mChannelLayoutTag = formats[selected].mChannelLayoutTag;
            layout_ptr = &layout;
            layout_size = offsetof(AudioChannelLayout, mChannelDescriptions);
        }
    }
    free(formats);
    if (!layout_ptr)
        MP_WARN(ao, "CoreAudio could not derive the E-AC-3 format list; "
                    "using the bitstream layout\n");

    err = CMAudioFormatDescriptionCreate(NULL, &asbd, layout_size, layout_ptr,
                                         cookie_size,
                                         cookie_size ? cookie : NULL, NULL,
                                         &p->format_description);
    if (err != noErr) {
        MP_FATAL(ao, "failed to create compressed audio format description\n");
        MP_VERBOSE(ao, "CMAudioFormatDescriptionCreate returned %d\n", (int)err);
        return false;
    }

    p->compressed_rate = fr->rate;
    MP_VERBOSE(ao, "compressed passthrough: E-AC-3%s %d Hz, %d channels\n",
               atmos ? "+JOC Atmos" : "", fr->rate,
               asbd.mChannelsPerFrame);
    return true;
}

// Hand one packet (an independent frame plus any dependent substreams that
// follow it) to the renderer.
static bool compressed_enqueue(struct ao *ao, const uint8_t *data, size_t size,
                               int samples)
{
    struct priv *p = ao->priv;
    CMBlockBufferRef bb = NULL;
    CMSampleBufferRef sb = NULL;
    bool ok = false;
    OSStatus err;

    if ((err = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, size, kCFAllocatorDefault,
                                                  NULL, 0, size, 0, &bb)) != noErr)
    {
        MP_ERR(ao, "failed to create block buffer (%d)\n", (int)err);
        goto done;
    }
    if ((err = CMBlockBufferReplaceDataBytes(data, bb, 0, size)) != noErr) {
        MP_ERR(ao, "failed to fill block buffer (%d)\n", (int)err);
        goto done;
    }

    CMSampleTimingInfo timing = {
        .duration              = CMTimeMake(samples, p->compressed_rate),
        .presentationTimeStamp = CMTimeMake(p->compressed_pts, p->compressed_rate),
        .decodeTimeStamp       = kCMTimeInvalid,
    };
    size_t sample_size = size;
    if ((err = CMSampleBufferCreateReady(NULL, bb, p->format_description, 1, 1,
                                         &timing, 1, &sample_size, &sb)) != noErr)
    {
        MP_ERR(ao, "failed to create compressed sample buffer (%d)\n", (int)err);
        goto done;
    }

    [p->renderer enqueueSampleBuffer:sb];
    p->compressed_pts += samples;
    if (!p->compressed_packets)
        MP_VERBOSE(ao, "compressed passthrough: first packet enqueued\n");
    p->compressed_packets++;
    ok = true;

done:
    if (bb) CFRelease(bb);
    if (sb) CFRelease(sb);
    return ok;
}

// Unwrap one IEC 61937 burst payload (16-bit byte swapped) and enqueue the
// elementary frames inside it. Returns false on a fatal error.
static bool compressed_submit_burst(struct ao *ao, const uint8_t *payload,
                                    size_t payload_len)
{
    // Recover the original elementary-stream byte order.
    uint8_t *raw = talloc_size(NULL, payload_len + 1);
    iec61937_unswap(raw, payload, payload_len);

    bool ok = true;
    size_t pos = 0;
    while (pos < payload_len) {
        struct eac3_frame fr;
        if (!eac3_parse_frame(raw + pos, payload_len - pos, &fr))
            break;      // trailing padding, or a frame we do not understand

        // Gather any dependent substreams into the same packet.
        size_t pkt = fr.size;
        int samples = fr.samples;
        struct eac3_frame next;
        while (pos + pkt < payload_len &&
               eac3_parse_frame(raw + pos + pkt, payload_len - pos - pkt, &next) &&
               !next.independent)
        {
            pkt += next.size;
        }

        if (!samples)
            break;      // a dependent frame with no independent parent

        if (!compressed_make_format(ao, &fr, raw + pos, pkt)) {
            ok = false;
            break;
        }
        if (!compressed_enqueue(ao, raw + pos, pkt, samples)) {
            ok = false;
            break;
        }
        pos += pkt;
    }

    talloc_free(raw);
    return ok;
}

// Pull from the audio chain and forward compressed frames to the renderer.
static void feed_compressed(struct ao *ao)
{
    struct priv *p = ao->priv;

    int request_sample_count = ao->samplerate / 10;
    int buffer_size = request_sample_count * ao->sstride;
    void *chunk = talloc_size(NULL, buffer_size);
    void *data[] = {chunk};

    int64_t cur_time_av = CMTimeGetNanoseconds([p->synchronizer currentTime]);
    int64_t cur_time_mp = mp_time_ns();
    int64_t end_time_av = MPMAX(p->end_time_av, cur_time_av);
    int64_t time_delta = CMTimeGetNanoseconds(CMTimeMake(request_sample_count,
                                                         ao->samplerate));
    bool eof;
    int got = ao_read_data(ao, data, request_sample_count,
                           end_time_av - cur_time_av + cur_time_mp + time_delta,
                           &eof, false, true);
    if (eof) {
        [p->renderer stopRequestingMediaData];
        ao_stop_streaming(ao);
    }
    if (got <= 0)
        goto done;

    // Burst boundaries do not line up with the chunks the audio chain hands
    // out, so carry a partial burst over to the next call.
    size_t add = (size_t)got * ao->sstride;
    p->burst = talloc_realloc_size(ao, p->burst, p->burst_len + add);
    memcpy(p->burst + p->burst_len, chunk, add);
    p->burst_len += add;

    size_t pos = 0;
    int data_type, pd;
    while (iec61937_find_burst(p->burst, p->burst_len, &pos, &data_type, &pd)) {
        // Pd is a byte count for E-AC-3 (it is a bit count for some other
        // data types, which we never see here).
        if (data_type != IEC61937_DATA_TYPE_EAC3) {
            pos += 2;
            continue;
        }
        if (p->burst_len - pos - IEC61937_HEADER_BYTES < (size_t)pd)
            break;      // wait for the rest of the payload

        if (!compressed_submit_burst(ao, p->burst + pos + IEC61937_HEADER_BYTES, pd)) {
            ao_request_reload(ao);
            goto done;
        }
        pos += IEC61937_HEADER_BYTES + pd;
    }

    if (pos) {
        memmove(p->burst, p->burst + pos, p->burst_len - pos);
        p->burst_len -= pos;
    }

    // A rejected compressed stream fails here rather than at init, and the
    // renderer just stops consuming -- which is silent. Say so loudly.
    if (p->renderer.error && !p->compressed_failed) {
        p->compressed_failed = true;
        MP_FATAL(ao, "AVFoundation rejected the compressed stream: %s\n",
                 p->renderer.error.localizedDescription.UTF8String);
        // ponytail: no automatic fallback to PCM -- mpv only re-negotiates
        // passthrough at init. Re-run without --audio-spdif to decode locally.
        MP_FATAL(ao, "retry without --audio-spdif to decode locally instead\n");
    }

    // Account for wall-clock pacing in carrier-rate terms, as the PCM path does.
    p->end_time_av = end_time_av +
                     CMTimeGetNanoseconds(CMTimeMake(got, ao->samplerate));

done:
    talloc_free(chunk);
}

static void feed(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->compressed) {
        feed_compressed(ao);
        return;
    }

    int samplerate = ao->samplerate;
    int sstride = ao->sstride;

    CMBlockBufferRef block_buffer = NULL;
    CMSampleBufferRef sample_buffer = NULL;
    OSStatus err;

    int request_sample_count = samplerate / 10;
    int buffer_size = request_sample_count * sstride;
    void *data[] = {CFAllocatorAllocate(NULL, buffer_size, 0)};

    int64_t cur_time_av = CMTimeGetNanoseconds([p->synchronizer currentTime]);
    int64_t cur_time_mp = mp_time_ns();
    int64_t end_time_av = MPMAX(p->end_time_av, cur_time_av);
    int64_t time_delta = CMTimeGetNanoseconds(CMTimeMake(request_sample_count, samplerate));
    bool eof;
    int real_sample_count = ao_read_data(ao, data, request_sample_count, end_time_av - cur_time_av + cur_time_mp + time_delta, &eof, false, true);
    if (eof) {
        [p->renderer stopRequestingMediaData];
        ao_stop_streaming(ao);
    }
    if (real_sample_count == 0) {
        goto finish;
    }

    if ((err = CMBlockBufferCreateWithMemoryBlock(
        NULL,
        data[0],
        buffer_size,
        NULL,
        NULL,
        0,
        real_sample_count * sstride,
        0,
        &block_buffer
    )) != noErr) {
        MP_FATAL(ao, "failed to create block buffer\n");
        MP_VERBOSE(ao, "CMBlockBufferCreateWithMemoryBlock returned %d\n", err);
        goto error;
    }
    data[0] = NULL;

    CMSampleTimingInfo sample_timing_into[] = {(CMSampleTimingInfo) {
        .duration = CMTimeMake(1, samplerate),
        .presentationTimeStamp = CMTimeFromNanoseconds(end_time_av),
        .decodeTimeStamp = kCMTimeInvalid
    }};
    size_t sample_size_array[] = {sstride};
    if ((err = CMSampleBufferCreateReady(
        NULL,
        block_buffer,
        p->format_description,
        real_sample_count,
        1,
        sample_timing_into,
        1,
        sample_size_array,
        &sample_buffer
    )) != noErr) {
        MP_FATAL(ao, "failed to create sample buffer\n");
        MP_VERBOSE(ao, "CMSampleBufferCreateReady returned %d\n", err);
        goto error;
    }

    [p->renderer enqueueSampleBuffer:sample_buffer];

    time_delta = CMTimeGetNanoseconds(CMTimeMake(real_sample_count, samplerate));
    p->end_time_av = end_time_av + time_delta;

    goto finish;

error:
    ao_request_reload(ao);
finish:
    if (data[0]) CFAllocatorDeallocate(NULL, data[0]);
    if (block_buffer) CFRelease(block_buffer);
    if (sample_buffer) CFRelease(sample_buffer);
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;

    p->end_time_av = -1;
    [p->synchronizer setRate:1];
    [p->renderer requestMediaDataWhenReadyOnQueue:p->queue usingBlock:^{
        feed(ao);
    }];
}

static void stop(struct ao *ao)
{
    struct priv *p = ao->priv;

    dispatch_sync(p->queue, ^{
        [p->renderer stopRequestingMediaData];
        [p->renderer flush];
        [p->synchronizer setRate:0];
        if (p->compressed) {
            talloc_free(p->burst);
            p->burst = NULL;
            p->burst_len = 0;
            CMTime now = [p->synchronizer currentTime];
            p->compressed_pts = p->compressed_rate > 0 && CMTIME_IS_NUMERIC(now)
                              ? CMTimeConvertScale(now, p->compressed_rate,
                                                   kCMTimeRoundingMethod_Default).value
                              : 0;
        }
    });
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;

    if (paused) {
        [p->synchronizer setRate:0];
    } else {
        [p->synchronizer setRate:1];
    }

    return true;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    struct priv *p = ao->priv;

    switch (cmd) {
    case AOCONTROL_GET_MUTE:
        *(bool*)arg = [p->renderer isMuted];
        return CONTROL_OK;
    case AOCONTROL_GET_VOLUME:
        *(float*)arg = [p->renderer volume] * 100;
        return CONTROL_OK;
    case AOCONTROL_SET_MUTE:
        [p->renderer setMuted:*(bool*)arg];
        return CONTROL_OK;
    case AOCONTROL_SET_VOLUME:
        [p->renderer setVolume:*(float*)arg / 100];
        return CONTROL_OK;
    default:
        return CONTROL_UNKNOWN;
    }
}

@implementation AVObserver
- (instancetype)initWithAO:(struct ao*)_ao {
    self = [super init];
    if (self) {
        ao = _ao;
    }
    return self;
}
- (void)handleRestartNotification:(NSNotification*)notification {
    char *name = cfstr_get_cstr((CFStringRef)notification.name);
    MP_WARN(ao, "restarting due to system notification; this will cause desync\n");
    MP_VERBOSE(ao, "notification name: %s\n", name);
    talloc_free(name);
    stop(ao);
    start(ao);
}
@end

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;
    AudioChannelLayout *layout = NULL;

#if TARGET_OS_IPHONE
    AVAudioSession *instance = AVAudioSession.sharedInstance;
    NSInteger maxChannels = instance.maximumOutputNumberOfChannels;
    NSInteger prefChannels = MIN(maxChannels, ao->channels.num);
    [instance setCategory:AVAudioSessionCategoryPlayback error:nil];
    [instance setMode:AVAudioSessionModeMoviePlayback error:nil];
    [instance setActive:YES error:nil];
    [instance setPreferredOutputNumberOfChannels:prefChannels error:nil];
#endif

    if ((p->renderer = [[AVSampleBufferAudioRenderer alloc] init]) == nil) {
        MP_FATAL(ao, "failed to create audio renderer\n");
        MP_VERBOSE(ao, "AVSampleBufferAudioRenderer failed to initialize\n");
        goto error;
    }
    if ((p->synchronizer = [[AVSampleBufferRenderSynchronizer alloc] init]) == nil) {
        MP_FATAL(ao, "failed to create rendering synchronizer\n");
        MP_VERBOSE(ao, "AVSampleBufferRenderSynchronizer failed to initialize\n");
        goto error;
    }
    if ((p->queue = dispatch_queue_create(
        "avfoundation event",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0)
    )) == NULL) {
        MP_FATAL(ao, "failed to create dispatch queue\n");
        MP_VERBOSE(ao, "dispatch_queue_create failed\n");
        goto error;
    }

    if (ao->device && ao->device[0]) {
        [p->renderer setAudioOutputDeviceUniqueID:(NSString*)cfstr_from_cstr(ao->device)];
    }

    [p->synchronizer addRenderer:p->renderer];
#if HAVE_MACOS_11_3_FEATURES
    if (@available(tvOS 14.5, iOS 14.5, macOS 11.3, *)) {
        [p->synchronizer setDelaysRateChangeUntilHasSufficientMediaData:NO];
    }
#endif

    if (af_fmt_is_spdif(ao->format)) {
        // Hand the compressed stream to AVFoundation instead of decoding it.
        // Apple's renderer decodes E-AC-3 including its JOC extension, so the
        // Atmos objects survive to its spatializer -- FFmpeg would have thrown
        // them away and left us with the 5.1 bed.
        //
        // ponytail: E-AC-3 only. AC-3 and DTS carry no objects (nothing to
        // gain over normal decoding), and CoreAudio cannot decode TrueHD at
        // all, so there is no format ID to ask for.
        if (ao->format != AF_FORMAT_S_EAC3) {
            MP_FATAL(ao, "avfoundation passthrough supports E-AC-3 only\n");
#if HAVE_COREAUDIO
            MP_FATAL(ao, "please use coreaudio_exclusive instead\n");
#endif
            goto error;
        }
        p->compressed = true;
    }

#if HAVE_MACOS_12_FEATURES
    if (@available(tvOS 15.0, iOS 15.0, macOS 12.0, *)) {
        // Let the renderer spatialize whatever it decodes. The default already
        // allows multichannel; asking for mono/stereo too means a downmixed or
        // 2-channel presentation still gets spatialized rather than played flat.
        [p->renderer setAllowedAudioSpatializationFormats:
            AVAudioSpatializationFormatMonoStereoAndMultichannel];
    }
#endif

    if (p->compressed) {
        // The format description needs the real rate and channel count, which
        // only the first frame reveals; feed_compressed() builds it then.
        // AVSampleBufferAudioRenderer read ahead aggressively
        ao->device_buffer = ao->samplerate * 2;
        goto skip_pcm_format;
    }

    // AVSampleBufferAudioRenderer only supports interleaved formats
    ao->format = af_fmt_from_planar(ao->format);
    if (af_fmt_is_planar(ao->format)) {
        MP_FATAL(ao, "planar audio formats are unsupported\n");
        goto error;
    }

    AudioStreamBasicDescription asbd;
    ca_fill_asbd(ao, &asbd);
    size_t layout_size;
    layout = ca_get_acl(ao, &layout_size);

    OSStatus err;
    if ((err = CMAudioFormatDescriptionCreate(
        NULL,
        &asbd,
        layout_size,
        layout,
        0,
        NULL,
        NULL,
        &p->format_description
    )) != noErr) {
        MP_FATAL(ao, "failed to create audio format description\n");
        MP_VERBOSE(ao, "CMAudioFormatDescriptionCreate returned %d\n", err);
        goto error;
    }
    talloc_free(layout);
    layout = NULL;

    // AVSampleBufferAudioRenderer read ahead aggressively
    ao->device_buffer = ao->samplerate * 2;

skip_pcm_format:
    p->observer = [[AVObserver alloc] initWithAO:ao];
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
#if HAVE_MACOS_12_FEATURES
    if (@available(tvOS 15.0, iOS 15.0, macOS 12.0, *)) {
        [center addObserver:p->observer selector:@selector(handleRestartNotification:) name:AVSampleBufferAudioRendererOutputConfigurationDidChangeNotification object:p->renderer];
    }
#endif
    [center addObserver:p->observer selector:@selector(handleRestartNotification:) name:AVSampleBufferAudioRendererWasFlushedAutomaticallyNotification object:p->renderer];

    return CONTROL_OK;

error:
    talloc_free(layout);
    if (p->renderer) [p->renderer release];
    if (p->synchronizer) [p->synchronizer release];
    if (p->queue) dispatch_release(p->queue);
    if (p->format_description) CFRelease(p->format_description);

#if TARGET_OS_IPHONE
    [AVAudioSession.sharedInstance setActive:NO
        withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
        error:nil
    ];
#endif

    return CONTROL_ERROR;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    stop(ao);

    [p->renderer release];
    [p->synchronizer release];
    dispatch_release(p->queue);
    CFRelease(p->format_description);

    [[NSNotificationCenter defaultCenter] removeObserver:p->observer];
    [p->observer release];

#if TARGET_OS_IPHONE
    [AVAudioSession.sharedInstance setActive:NO
        withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
        error:nil
    ];
#endif
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_avfoundation = {
    .description    = "AVFoundation AVSampleBufferAudioRenderer",
    .name           = "avfoundation",
    .uninit         = uninit,
    .init           = init,
    .control        = control,
    .reset          = stop,
    .start          = start,
    .set_pause      = set_pause,
    .list_devs      = ca_get_device_list,
    .priv_size      = sizeof(struct priv),
};
