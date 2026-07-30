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
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

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

    bool compressed;
    uint8_t *burst;
    size_t burst_len;
    int compressed_rate;
    int64_t compressed_pts;
    int64_t compressed_packets;
    bool compressed_failed;
    bool compressed_eof;
    bool compressed_paused;

    AVFormatContext *hls;
    AVStream *hls_stream;
    bool hls_header;
    char *hls_dir;
    uint64_t hls_generation;
    dispatch_source_t feed_timer;

    AVPlayer *player;
    float player_volume;
    bool player_muted;
    double player_progress;
    int64_t player_progress_ns;

    int http_listener;
    int http_dir;
    int http_port;
    dispatch_queue_t http_queue;
    dispatch_source_t http_source;
    dispatch_group_t http_clients;
    dispatch_semaphore_t http_cancelled;
};

static char http_queue_key;

static int64_t CMTimeGetNanoseconds(CMTime time)
{
    time = CMTimeConvertScale(time, 1000000000, kCMTimeRoundingMethod_Default);
    return time.value;
}

static CMTime CMTimeFromNanoseconds(int64_t time)
{
    return CMTimeMake(time, 1000000000);
}

static int eac3_profile(const uint8_t *data, size_t size)
{
    int profile = AV_PROFILE_UNKNOWN;
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_EAC3);
    AVCodecContext *codec = avcodec_alloc_context3(decoder);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (codec && packet && frame && av_new_packet(packet, size) >= 0) {
        memcpy(packet->data, data, size);
        // The public parser does not expose the JOC flag. Decode only this
        // first frame to inspect its profile; playback remains compressed.
        if (avcodec_open2(codec, decoder, NULL) >= 0 &&
            avcodec_send_packet(codec, packet) >= 0 &&
            avcodec_receive_frame(codec, frame) >= 0)
            profile = codec->profile;
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    return profile;
}

static bool http_send_all(int fd, const void *data, size_t size)
{
    const uint8_t *pos = data;
    while (size) {
        ssize_t written = send(fd, pos, size, 0);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (!written)
            return false;
        pos += written;
        size -= written;
    }
    return true;
}

static void http_reply(int fd, int status, const char *reason)
{
    char response[256];
    int len = snprintf(response, sizeof(response),
                      "HTTP/1.1 %d %s\r\nContent-Length: 0\r\n"
                      "Connection: close\r\n\r\n", status, reason);
    http_send_all(fd, response, len);
}

static bool http_generated_name(const char *name)
{
    const char *pos;
    const char *suffix;
    bool segment = false;
    if (strncmp(name, "stream", 6) == 0) {
        pos = name + 6;
        suffix = ".m3u8";
    } else if (strncmp(name, "init", 4) == 0) {
        pos = name + 4;
        suffix = ".mp4";
    } else if (strncmp(name, "seg", 3) == 0) {
        pos = name + 3;
        suffix = ".m4s";
        segment = true;
    } else {
        return false;
    }
    if (!*pos)
        return false;
    while (*pos >= '0' && *pos <= '9')
        pos++;
    if (segment) {
        if (*pos++ != '-' || *pos < '0' || *pos > '9')
            return false;
        while (*pos >= '0' && *pos <= '9')
            pos++;
    }
    return strcmp(pos, suffix) == 0;
}

static void http_serve(struct priv *p, int fd)
{
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));

    char request[2048];
    ssize_t size;
    do {
        size = recv(fd, request, sizeof(request) - 1, 0);
    } while (size < 0 && errno == EINTR);
    if (size <= 0)
        goto done;
    request[size] = '\0';

    char method[8], target[1024], version[16];
    if (sscanf(request, "%7s %1023s %15s", method, target, version) != 3) {
        http_reply(fd, 400, "Bad Request");
        goto done;
    }
    if (strcmp(method, "GET") != 0) {
        http_reply(fd, 405, "Method Not Allowed");
        goto done;
    }
    if (target[0] != '/' || strchr(target + 1, '/') ||
        strchr(target, '\\') || strchr(target, '%') || strstr(target, ".."))
    {
        http_reply(fd, 403, "Forbidden");
        goto done;
    }
    char *query = strchr(target, '?');
    if (query)
        *query = '\0';
    const char *name = target + 1;
    if (!http_generated_name(name)) {
        http_reply(fd, 404, "Not Found");
        goto done;
    }

    int file = openat(p->http_dir, name, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (file < 0 || fstat(file, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (file >= 0)
            close(file);
        http_reply(fd, 404, "Not Found");
        goto done;
    }

    const char *type = strncmp(name, "stream", 6) == 0
        ? "application/vnd.apple.mpegurl" : "video/mp4";
    char header[512];
    int header_size = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
        "Content-Length: %lld\r\nConnection: close\r\n\r\n",
        type, (long long)st.st_size);
    if (http_send_all(fd, header, header_size)) {
        uint8_t buf[16384];
        ssize_t got;
        while ((got = read(file, buf, sizeof(buf))) > 0) {
            if (!http_send_all(fd, buf, got))
                break;
        }
    }
    close(file);

done:
    close(fd);
}

static bool http_start(struct ao *ao)
{
    struct priv *p = ao->priv;
    p->http_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (p->http_listener < 0) {
        MP_FATAL(ao, "failed to create loopback HTTP socket: %s\n",
                 mp_strerror(errno));
        return false;
    }

    int flags = fcntl(p->http_listener, F_GETFL);
    if (flags < 0 || fcntl(p->http_listener, F_SETFL, flags | O_NONBLOCK) < 0) {
        MP_FATAL(ao, "failed to configure loopback HTTP socket: %s\n",
                 mp_strerror(errno));
        return false;
    }
    int yes = 1;
    setsockopt(p->http_listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(p->http_listener, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_len = sizeof(addr),
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(p->http_listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(p->http_listener, 8) < 0)
    {
        MP_FATAL(ao, "failed to bind/listen on 127.0.0.1: %s\n",
                 mp_strerror(errno));
        return false;
    }

    socklen_t addr_len = sizeof(addr);
    if (getsockname(p->http_listener, (struct sockaddr *)&addr, &addr_len) < 0) {
        MP_FATAL(ao, "failed to read loopback HTTP port: %s\n",
                 mp_strerror(errno));
        return false;
    }
    p->http_port = ntohs(addr.sin_port);
    p->http_queue = dispatch_queue_create("mpv avfoundation http",
                                          DISPATCH_QUEUE_SERIAL);
    p->http_clients = dispatch_group_create();
    p->http_cancelled = dispatch_semaphore_create(0);
    if (!p->http_queue || !p->http_clients || !p->http_cancelled) {
        MP_FATAL(ao, "failed to create loopback HTTP queues\n");
        return false;
    }
    dispatch_queue_set_specific(p->http_queue, &http_queue_key, p, NULL);

    int listener = p->http_listener;
    p->http_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
                                            listener, 0, p->http_queue);
    if (!p->http_source) {
        MP_FATAL(ao, "failed to create loopback HTTP listener\n");
        return false;
    }
    dispatch_source_set_event_handler(p->http_source, ^{
        for (;;) {
            int client = accept(listener, NULL, NULL);
            if (client < 0) {
                if (errno == EINTR)
                   continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                   MP_ERR(ao, "loopback HTTP accept failed: %s\n",
                          mp_strerror(errno));
                break;
            }
            int client_flags = fcntl(client, F_GETFL);
            if (client_flags >= 0)
                fcntl(client, F_SETFL, client_flags & ~O_NONBLOCK);
            dispatch_group_async(p->http_clients,
                dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                   http_serve(p, client);
                });
        }
    });
    dispatch_semaphore_t cancelled = p->http_cancelled;
    dispatch_retain(cancelled);
    dispatch_source_set_cancel_handler(p->http_source, ^{
        close(listener);
        dispatch_semaphore_signal(cancelled);
        dispatch_release(cancelled);
    });
    dispatch_resume(p->http_source);
    MP_VERBOSE(ao, "serving compressed audio on http://127.0.0.1:%d/\n",
               p->http_port);
    return true;
}

static void http_stop(struct priv *p)
{
    dispatch_source_t source = p->http_source;
    dispatch_semaphore_t cancelled = p->http_cancelled;
    if (source) {
        p->http_source = NULL;
        p->http_cancelled = NULL;
        p->http_listener = -1;
        dispatch_source_cancel(source);
        dispatch_release(source);
        if (dispatch_get_specific(&http_queue_key) != p)
            dispatch_semaphore_wait(cancelled, DISPATCH_TIME_FOREVER);
    } else if (p->http_listener >= 0) {
        close(p->http_listener);
        p->http_listener = -1;
    }
    if (cancelled) {
        p->http_cancelled = NULL;
        dispatch_release(cancelled);
    }
    if (p->http_clients)
        dispatch_group_wait(p->http_clients, DISPATCH_TIME_FOREVER);
    if (p->http_dir >= 0) {
        close(p->http_dir);
        p->http_dir = -1;
    }
}

static void compressed_remove_files(struct ao *ao)
{
    struct priv *p = ao->priv;
    NSString *dir = [NSString stringWithUTF8String:p->hls_dir];
    NSFileManager *manager = [NSFileManager defaultManager];
    NSError *error = nil;
    NSArray *files = [manager contentsOfDirectoryAtPath:dir error:&error];
    if (!files && error) {
        MP_ERR(ao, "failed to list HLS directory: %s\n",
               error.localizedDescription.UTF8String);
        return;
    }
    for (NSString *file in files) {
        if (![manager removeItemAtPath:[dir stringByAppendingPathComponent:file]
                                 error:&error])
        {
            MP_ERR(ao, "failed to remove old HLS file: %s\n",
                  error.localizedDescription.UTF8String);
            error = nil;
        }
    }
}

static bool compressed_close_muxer(struct ao *ao, bool report_error)
{
    struct priv *p = ao->priv;
    if (!p->hls)
        return true;
    bool ok = true;
    if (p->hls_header) {
        int ret = av_write_trailer(p->hls);
        if (ret < 0 && report_error) {
            MP_ERR(ao, "failed to finalize E-AC-3 HLS: %s\n",
                  av_err2str(ret));
            ok = false;
        } else if (ret < 0) {
            MP_VERBOSE(ao, "ignoring E-AC-3 HLS reset trailer error: %s\n",
                       av_err2str(ret));
        }
    }
    if (!(p->hls->oformat->flags & AVFMT_NOFILE))
        avio_closep(&p->hls->pb);
    avformat_free_context(p->hls);
    p->hls = NULL;
    p->hls_stream = NULL;
    p->hls_header = false;
    return ok;
}

static bool compressed_open_muxer(struct ao *ao, const struct eac3_frame *fr,
                                  const uint8_t *data, size_t size)
{
    struct priv *p = ao->priv;
    if (p->hls)
        return p->compressed_rate == fr->rate;

    unsigned long long generation = ++p->hls_generation;
    char *playlist = talloc_asprintf(NULL, "%s/stream%06llu.m3u8",
                                     p->hls_dir, generation);
    char *segments = talloc_asprintf(NULL, "%s/seg%06llu-%%06d.m4s",
                                     p->hls_dir, generation);
    char *init = talloc_asprintf(NULL, "init%06llu.mp4", generation);
    int ret = avformat_alloc_output_context2(&p->hls, NULL, "hls", playlist);
    if (ret < 0 || !p->hls) {
        MP_FATAL(ao, "failed to create E-AC-3 HLS muxer: %s\n",
                 av_err2str(ret));
        goto error;
    }

    p->hls_stream = avformat_new_stream(p->hls, NULL);
    if (!p->hls_stream) {
        MP_FATAL(ao, "failed to create E-AC-3 HLS stream\n");
        goto error;
    }
    p->hls_stream->time_base = (AVRational){1, fr->rate};
    AVCodecParameters *par = p->hls_stream->codecpar;
    par->codec_type = AVMEDIA_TYPE_AUDIO;
    par->codec_id = AV_CODEC_ID_EAC3;
    par->sample_rate = fr->rate;
    par->frame_size = fr->samples;
    par->bit_rate = size * 8LL * fr->rate / fr->samples;
    av_channel_layout_default(&par->ch_layout, fr->channels);

    if (!(p->hls->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&p->hls->pb, playlist, AVIO_FLAG_WRITE);
        if (ret < 0) {
            MP_FATAL(ao, "failed to open E-AC-3 HLS output: %s\n",
                    av_err2str(ret));
            goto error;
        }
    }

    AVDictionary *options = NULL;
    av_dict_set(&options, "hls_segment_type", "fmp4", 0);
    av_dict_set(&options, "hls_time", "0.5", 0);
    av_dict_set(&options, "hls_list_size", "32", 0);
    av_dict_set(&options, "hls_delete_threshold", "32", 0);
    av_dict_set(&options, "hls_flags", "delete_segments+temp_file", 0);
    av_dict_set(&options, "hls_fmp4_init_filename", init, 0);
    av_dict_set(&options, "hls_segment_filename", segments, 0);
    ret = avformat_write_header(p->hls, &options);
    av_dict_free(&options);
    if (ret < 0) {
        MP_FATAL(ao, "failed to start E-AC-3 HLS muxer: %s\n",
                 av_err2str(ret));
        goto error;
    }
    p->hls_header = true;
    p->compressed_rate = fr->rate;
    bool atmos = eac3_profile(data, size) == AV_PROFILE_EAC3_DDP_ATMOS;
    MP_VERBOSE(ao, "compressed route: E-AC-3%s %d Hz, %d channels via HLS/AVPlayer\n",
               atmos ? "+JOC Atmos" : "", fr->rate, fr->channels);
    talloc_free(playlist);
    talloc_free(segments);
    talloc_free(init);
    return true;

error:
    talloc_free(playlist);
    talloc_free(segments);
    talloc_free(init);
    compressed_close_muxer(ao, false);
    return false;
}

static bool compressed_write_packet(struct ao *ao, const struct eac3_frame *fr,
                                   const uint8_t *data, size_t size)
{
    struct priv *p = ao->priv;
    if (!compressed_open_muxer(ao, fr, data, size)) {
        if (p->hls && p->compressed_rate != fr->rate)
            MP_FATAL(ao, "E-AC-3 sample rate changed inside the stream\n");
        return false;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet || av_new_packet(packet, size) < 0) {
        MP_FATAL(ao, "failed to allocate E-AC-3 HLS packet\n");
        av_packet_free(&packet);
        return false;
    }
    memcpy(packet->data, data, size);
    packet->stream_index = p->hls_stream->index;
    packet->pts = packet->dts = p->compressed_pts;
    packet->duration = fr->samples;
    packet->flags = AV_PKT_FLAG_KEY;
    int ret = av_interleaved_write_frame(p->hls, packet);
    av_packet_free(&packet);
    if (ret < 0) {
        MP_FATAL(ao, "failed to mux E-AC-3 HLS packet: %s\n",
                 av_err2str(ret));
        return false;
    }
    p->compressed_pts += fr->samples;
    p->compressed_packets++;
    return true;
}

static void compressed_start_player(struct ao *ao, bool eof)
{
    struct priv *p = ao->priv;
    if (p->player || !p->hls_dir)
        return;
    char *startup_segment = talloc_asprintf(
        NULL, "%s/seg%06llu-%06d.m4s", p->hls_dir,
        (unsigned long long)p->hls_generation, eof ? 0 : 3);
    bool ready = access(startup_segment, R_OK) == 0;
    talloc_free(startup_segment);
    if (!ready)
        return;

    NSString *url = [NSString
        stringWithFormat:@"http://127.0.0.1:%d/stream%06llu.m3u8",
        p->http_port, (unsigned long long)p->hls_generation];
    AVPlayerItem *item = [AVPlayerItem playerItemWithURL:[NSURL URLWithString:url]];
    item.preferredForwardBufferDuration = 1.0;
    p->player = [[AVPlayer alloc] initWithPlayerItem:item];
    if (!p->player) {
        MP_FATAL(ao, "failed to create AVPlayer for E-AC-3 HLS\n");
        p->compressed_failed = true;
        return;
    }
    p->player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
    p->player.volume = p->player_volume;
    p->player.muted = p->player_muted;
    p->player_progress = 0;
    p->player_progress_ns = mp_time_ns();
    if (!p->compressed_paused)
        [p->player playImmediatelyAtRate:1.0];
    MP_VERBOSE(ao, "AVPlayer opened the E-AC-3 HLS playlist\n");
}

static double compressed_player_time(struct priv *p)
{
    if (!p->player)
        return 0;
    CMTime time = p->player.currentTime;
    if (!CMTIME_IS_NUMERIC(time))
        return 0;
    double seconds = CMTimeGetSeconds(time);
    return isfinite(seconds) && seconds > 0 ? seconds : 0;
}

static bool compressed_submit_burst(struct ao *ao, const uint8_t *payload,
                                   size_t payload_len)
{
    uint8_t *raw = talloc_size(NULL, payload_len);
    iec61937_unswap(raw, payload, payload_len);

    bool ok = true;
    size_t pos = 0;
    while (pos < payload_len) {
        struct eac3_frame fr;
        size_t packet_size;
        if (!eac3_parse_packet(raw + pos, payload_len - pos, &fr, &packet_size))
            break;
        if (!compressed_write_packet(ao, &fr, raw + pos, packet_size)) {
            ok = false;
            break;
        }
        pos += packet_size;
    }

    talloc_free(raw);
    return ok;
}

static void compressed_check_player(struct ao *ao)
{
    struct priv *p = ao->priv;
    AVPlayerItem *item = p->player.currentItem;
    NSError *error = item.error ?: p->player.error;
    if (!item || (item.status != AVPlayerItemStatusFailed && !error) ||
        p->compressed_failed)
        return;
    p->compressed_failed = true;
    MP_FATAL(ao, "AVPlayer rejected the E-AC-3 HLS stream: %s\n",
             error ? error.localizedDescription.UTF8String : "unknown error");
    MP_FATAL(ao, "retry without --audio-spdif to decode locally instead\n");
}

static void compressed_drain_failed(struct ao *ao)
{
    struct priv *p = ao->priv;
    if (p->compressed_eof || p->compressed_paused)
        return;

    int samples = ao->samplerate / 50;
    void *chunk = talloc_size(NULL, samples * ao->sstride);
    void *data[] = {chunk};
    bool eof = false;
    ao_read_data(ao, data, samples,
                 mp_time_ns() + MP_TIME_S_TO_NS(samples / (double)ao->samplerate),
                 &eof, false, true);
    talloc_free(chunk);
    if (eof) {
        p->compressed_eof = true;
        ao_stop_streaming(ao);
    }
}

static void feed_compressed(struct ao *ao)
{
    struct priv *p = ao->priv;
    compressed_check_player(ao);
    if (p->compressed_failed) {
        compressed_drain_failed(ao);
        return;
    }
    if (p->compressed_eof || p->compressed_paused)
        return;

    double player_time = compressed_player_time(p);
    double muxed_time = p->compressed_rate
        ? p->compressed_pts / (double)p->compressed_rate : 0;
    double queued = MPMAX(0, muxed_time - player_time);
    int64_t now = mp_time_ns();
    if (player_time > p->player_progress + 0.01) {
        if (p->player_progress == 0)
            MP_VERBOSE(ao, "AVPlayer started compressed audio playback\n");
        p->player_progress = player_time;
        p->player_progress_ns = now;
    }

    int64_t stalled_for = p->player_progress_ns
                        ? now - p->player_progress_ns : 0;
    if (stalled_for >= MP_TIME_S_TO_NS(15) && queued >= 6.0) {
        p->compressed_failed = true;
        MP_FATAL(ao, "AVPlayer %s for 15 seconds with %.1f seconds of E-AC-3 "
                 "buffered\n", p->player ? "made no progress" : "did not start",
                 queued);
        MP_FATAL(ao, "retry without --audio-spdif to decode locally instead\n");
        return;
    }

    double queue_limit = !p->player || stalled_for >= MP_TIME_S_TO_NS(1)
                       ? 8.0 : 2.0;
    if (queued >= queue_limit)
        return;

    int request_samples = ao->samplerate / 20;
    int buffer_size = request_samples * ao->sstride;
    void *chunk = talloc_size(NULL, buffer_size);
    void *data[] = {chunk};
    int64_t out_time = mp_time_ns() +
        MP_TIME_S_TO_NS(queued + request_samples / (double)ao->samplerate);
    bool eof = false;
    int got = ao_read_data(ao, data, request_samples, out_time,
                          &eof, false, true);

    if (got > 0) {
        size_t add = (size_t)got * ao->sstride;
        p->burst = talloc_realloc_size(ao, p->burst, p->burst_len + add);
        memcpy(p->burst + p->burst_len, chunk, add);
        p->burst_len += add;

        size_t pos = 0;
        int data_type, payload_size;
        while (iec61937_find_burst(p->burst, p->burst_len, &pos, &data_type,
                                  &payload_size))
        {
            if (data_type != IEC61937_DATA_TYPE_EAC3) {
                pos += 2;
                continue;
            }
            if (payload_size <= 0 ||
                p->burst_len - pos - IEC61937_HEADER_BYTES <
                   (size_t)payload_size)
                break;
            if (!compressed_submit_burst(
                   ao, p->burst + pos + IEC61937_HEADER_BYTES, payload_size))
            {
                p->compressed_failed = true;
                MP_FATAL(ao, "retry without --audio-spdif to decode locally instead\n");
                break;
            }
            pos += IEC61937_HEADER_BYTES + payload_size;
        }
        if (pos) {
            memmove(p->burst, p->burst + pos, p->burst_len - pos);
            p->burst_len -= pos;
        }
        compressed_start_player(ao, false);
    }

    if (eof && !p->compressed_failed) {
        if (!compressed_close_muxer(ao, true)) {
            p->compressed_failed = true;
            talloc_free(chunk);
            return;
        }
        compressed_start_player(ao, true);
        p->compressed_eof = true;
        ao_stop_streaming(ao);
        MP_VERBOSE(ao, "finished E-AC-3 HLS stream after %lld packets\n",
                  (long long)p->compressed_packets);
    }
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

    if (p->compressed) {
        dispatch_sync(p->queue, ^{
            if (p->compressed_eof) {
                [p->player pause];
                [p->player replaceCurrentItemWithPlayerItem:nil];
                [p->player release];
                p->player = nil;
                compressed_remove_files(ao);
                p->compressed_pts = 0;
                p->compressed_packets = 0;
                p->compressed_rate = 0;
                p->compressed_eof = false;
                p->compressed_failed = false;
                p->player_progress = 0;
                p->player_progress_ns = 0;
            }
            p->compressed_paused = false;
            if (p->player) {
                p->player_progress = compressed_player_time(p);
                p->player_progress_ns = mp_time_ns();
                [p->player playImmediatelyAtRate:1.0];
            } else if (!p->player_progress_ns) {
                p->player_progress_ns = mp_time_ns();
            }
            if (!p->feed_timer) {
                p->feed_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
                                                        0, 0, p->queue);
                if (!p->feed_timer) {
                    MP_FATAL(ao, "failed to create compressed audio feed timer\n");
                    p->compressed_failed = true;
                    return;
                }
                dispatch_source_set_timer(p->feed_timer, DISPATCH_TIME_NOW,
                                          20 * NSEC_PER_MSEC,
                                          2 * NSEC_PER_MSEC);
                dispatch_source_set_event_handler(p->feed_timer, ^{
                    feed_compressed(ao);
                });
                dispatch_resume(p->feed_timer);
            }
        });
        return;
    }

    p->end_time_av = -1;
    [p->synchronizer setRate:1];
    [p->renderer requestMediaDataWhenReadyOnQueue:p->queue usingBlock:^{
        feed(ao);
    }];
}

static void stop(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->compressed) {
        dispatch_sync(p->queue, ^{
            if (p->feed_timer) {
                dispatch_source_cancel(p->feed_timer);
                dispatch_release(p->feed_timer);
                p->feed_timer = NULL;
            }
            [p->player pause];
            [p->player replaceCurrentItemWithPlayerItem:nil];
            [p->player release];
            p->player = nil;
            compressed_close_muxer(ao, false);
            talloc_free(p->burst);
            p->burst = NULL;
            p->burst_len = 0;
            p->compressed_rate = 0;
            p->compressed_pts = 0;
            p->compressed_packets = 0;
            p->compressed_failed = false;
            p->compressed_eof = false;
            p->compressed_paused = false;
            p->player_progress = 0;
            p->player_progress_ns = 0;
            compressed_remove_files(ao);
        });
        return;
    }

    dispatch_sync(p->queue, ^{
        [p->renderer stopRequestingMediaData];
        [p->renderer flush];
        [p->synchronizer setRate:0];
    });
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;

    if (p->compressed) {
        dispatch_sync(p->queue, ^{
            p->compressed_paused = paused;
            if (paused)
                [p->player pause];
            else {
                p->player_progress = compressed_player_time(p);
                p->player_progress_ns = mp_time_ns();
                [p->player playImmediatelyAtRate:1.0];
            }
        });
        return true;
    }

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

    if (p->compressed) {
        __block bool muted;
        __block float volume;
        switch (cmd) {
        case AOCONTROL_GET_MUTE:
            dispatch_sync(p->queue, ^{ muted = p->player_muted; });
            *(bool *)arg = muted;
            return CONTROL_OK;
        case AOCONTROL_GET_VOLUME:
            dispatch_sync(p->queue, ^{ volume = p->player_volume; });
            *(float *)arg = volume * 100;
            return CONTROL_OK;
        case AOCONTROL_SET_MUTE:
            muted = *(bool *)arg;
            dispatch_sync(p->queue, ^{
                p->player_muted = muted;
                p->player.muted = muted;
            });
            return CONTROL_OK;
        case AOCONTROL_SET_VOLUME:
            volume = *(float *)arg / 100;
            dispatch_sync(p->queue, ^{
                p->player_volume = volume;
                p->player.volume = volume;
            });
            return CONTROL_OK;
        default:
            return CONTROL_UNKNOWN;
        }
    }

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
    p->http_listener = -1;
    p->http_dir = -1;
    p->player_volume = 1.0;

#if TARGET_OS_IPHONE
    AVAudioSession *instance = AVAudioSession.sharedInstance;
    NSInteger maxChannels = instance.maximumOutputNumberOfChannels;
    NSInteger prefChannels = MIN(maxChannels, ao->channels.num);
    [instance setCategory:AVAudioSessionCategoryPlayback error:nil];
    [instance setMode:AVAudioSessionModeMoviePlayback error:nil];
    [instance setActive:YES error:nil];
    [instance setPreferredOutputNumberOfChannels:prefChannels error:nil];
#endif

    if ((p->queue = dispatch_queue_create(
        "avfoundation event",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0)
    )) == NULL) {
        MP_FATAL(ao, "failed to create dispatch queue\n");
        MP_VERBOSE(ao, "dispatch_queue_create failed\n");
        goto error;
    }

    if (af_fmt_is_spdif(ao->format)) {
        if (ao->format != AF_FORMAT_S_EAC3) {
            MP_FATAL(ao, "avfoundation passthrough supports E-AC-3 only\n");
#if HAVE_COREAUDIO
            MP_FATAL(ao, "please use coreaudio_exclusive instead\n");
#endif
            goto error;
        }
        if (ao->device && ao->device[0]) {
            MP_FATAL(ao, "AVPlayer cannot use explicit audio device '%s'\n",
                     ao->device);
            goto error;
        }
        p->compressed = true;

        NSString *path = [NSTemporaryDirectory()
            stringByAppendingPathComponent:@"mpv-avfoundation-XXXXXX"];
        char *template = talloc_strdup(NULL, path.fileSystemRepresentation);
        if (!mkdtemp(template)) {
            MP_FATAL(ao, "failed to create E-AC-3 HLS directory: %s\n",
                     mp_strerror(errno));
            talloc_free(template);
            goto error;
        }
        p->hls_dir = talloc_strdup(ao, template);
        talloc_free(template);
        p->http_dir = open(p->hls_dir, O_RDONLY | O_DIRECTORY);
        if (p->http_dir < 0) {
            MP_FATAL(ao, "failed to open E-AC-3 HLS directory: %s\n",
                     mp_strerror(errno));
            goto error;
        }
        if (!http_start(ao))
            goto error;

        ao->device_buffer = ao->samplerate * 2;
        return CONTROL_OK;
    }

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

    if (ao->device && ao->device[0]) {
        [p->renderer setAudioOutputDeviceUniqueID:(NSString*)cfstr_from_cstr(ao->device)];
    }

    [p->synchronizer addRenderer:p->renderer];
#if HAVE_MACOS_11_3_FEATURES
    if (@available(tvOS 14.5, iOS 14.5, macOS 11.3, *)) {
        [p->synchronizer setDelaysRateChangeUntilHasSufficientMediaData:NO];
    }
#endif

#if HAVE_MACOS_12_FEATURES
    if (@available(tvOS 15.0, iOS 15.0, macOS 12.0, *)) {
        // Let the renderer spatialize whatever it decodes. The default already
        // allows multichannel; asking for mono/stereo too means a downmixed or
        // 2-channel presentation still gets spatialized rather than played flat.
        [p->renderer setAllowedAudioSpatializationFormats:
            AVAudioSpatializationFormatMonoStereoAndMultichannel];
    }
#endif

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
    http_stop(p);
    if (p->http_clients) dispatch_release(p->http_clients);
    if (p->http_queue) dispatch_release(p->http_queue);
    if (p->hls_dir) {
        compressed_remove_files(ao);
        rmdir(p->hls_dir);
    }
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

    if (p->compressed) {
        http_stop(p);
        compressed_remove_files(ao);
        if (rmdir(p->hls_dir) < 0)
            MP_WARN(ao, "failed to remove E-AC-3 HLS directory: %s\n",
                    mp_strerror(errno));
        if (p->http_clients) dispatch_release(p->http_clients);
        if (p->http_queue) dispatch_release(p->http_queue);
        dispatch_release(p->queue);
#if TARGET_OS_IPHONE
        [AVAudioSession.sharedInstance setActive:NO
            withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
            error:nil
        ];
#endif
        return;
    }

    [p->renderer release];
    [p->synchronizer release];
    dispatch_release(p->queue);
    if (p->format_description) CFRelease(p->format_description);

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
    .description    = "AVFoundation audio renderer",
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
