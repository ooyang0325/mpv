/*
 * SACD ISO demuxer
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 */

#include <sacd_bridge.h>

#include "audio/chmap.h"
#include "common/common.h"
#include "common/msg.h"
#include "common/tags.h"
#include "demux.h"
#include "options/m_config.h"
#include "options/options.h"
#include "packet.h"
#include "stheader.h"
#include "stream/stream.h"

#define SACD_FRAME_RATE 75
#define SACD_MAX_FRAME_SIZE (64 * 1024)

struct priv {
    iina_sacd *sacd;
    struct sh_stream *stream;
    iina_sacd_area_info area;
    uint8_t frame[SACD_MAX_FRAME_SIZE];
};

static bool read_packet(struct demuxer *demuxer, struct demux_packet **out)
{
    struct priv *p = demuxer->priv;
    size_t size = 0;
    int dst = 0;
    double pts = 0;
    int result = iina_sacd_read_frame(p->sacd, p->frame, sizeof(p->frame),
                                      &size, &dst, &pts);
    if (result <= 0) {
        if (result < 0)
            MP_ERR(demuxer, "Invalid SACD audio frame.\n");
        return false;
    }
    if (!!dst != !!p->area.dst_encoded) {
        MP_ERR(demuxer, "SACD area changed encoding unexpectedly.\n");
        return false;
    }

    if (size > sizeof(p->frame)) {
        MP_ERR(demuxer, "SACD frame larger than the buffer it was read into.\n");
        return false;
    }

    struct demux_packet *packet =
        new_demux_packet(demuxer->packet_pool, size);
    if (!packet)
        return false;
    memcpy(packet->buffer, p->frame, size);

    packet->stream = p->stream->index;
    packet->pts = packet->dts = pts;
    packet->duration = 1.0 / SACD_FRAME_RATE;
    packet->keyframe = true;
    *out = packet;
    return true;
}

static void seek(struct demuxer *demuxer, double seconds, int flags)
{
    struct priv *p = demuxer->priv;
    if (flags & SEEK_FACTOR)
        seconds *= p->area.duration;
    if (!iina_sacd_seek(p->sacd, MPCLAMP(seconds, 0, p->area.duration)))
        MP_ERR(demuxer, "Could not seek in SACD image.\n");
}

static int open_sacd(struct demuxer *demuxer, enum demux_check check)
{
    if (check != DEMUX_CHECK_FORCE &&
        !bstr_case_endswith(bstr0(demuxer->filename), bstr0(".iso")))
        return -1;

    char *path = mp_file_get_path(demuxer, bstr0(demuxer->filename));
    if (!path)
        return -1;

    iina_sacd *sacd = iina_sacd_open(path);
    if (!sacd)
        return -1;

    int areas = iina_sacd_area_count(sacd);
    if (areas < 1) {
        iina_sacd_close(sacd);
        return -1;
    }

    for (int n = 0; n < areas; n++) {
        iina_sacd_area_info info;
        if (!iina_sacd_get_area(sacd, n, &info))
            continue;
        struct demux_edition edition = {
            .demuxer_id = n,
            .default_edition = n == 0,
            .metadata = talloc_zero(demuxer, struct mp_tags),
        };
        char *title = talloc_asprintf(
            demuxer, "%s area (%d channels)",
            info.channels == 2 ? "Stereo" : "Multichannel", info.channels);
        mp_tags_set_str(edition.metadata, "TITLE", title);
        MP_TARRAY_APPEND(demuxer, demuxer->editions, demuxer->num_editions,
                         edition);
    }

    struct MPOpts *opts =
        mp_get_config_group(NULL, demuxer->global, &mp_opt_root);
    int selected = opts->edition_id >= 0 && opts->edition_id < areas
        ? opts->edition_id : 0;
    talloc_free(opts);

    struct priv *p = talloc_zero(demuxer, struct priv);
    demuxer->priv = p;
    p->sacd = sacd;
    if (!iina_sacd_get_area(sacd, selected, &p->area) ||
        !iina_sacd_select_area(sacd, selected)) {
        iina_sacd_close(sacd);
        p->sacd = NULL;
        return -1;
    }

    demuxer->edition = selected;
    demuxer->duration = p->area.duration;
    demuxer->seekable = true;
    demuxer->filetype = "sacd";

    struct sh_stream *sh = demux_alloc_sh_stream(STREAM_AUDIO);
    p->stream = sh;
    sh->default_track = true;
    sh->title = talloc_asprintf(
        sh, "%s area (%d channels)",
        p->area.channels == 2 ? "Stereo" : "Multichannel", p->area.channels);
    sh->codec->codec = p->area.dst_encoded ? "dst" : "dsd_msbf";
    sh->codec->codec_desc = p->area.dst_encoded
        ? "DST (Digital Stream Transfer)" : "Direct Stream Digital";
    sh->codec->format_name = p->area.dst_encoded ? "DST" : "DSD";
    sh->codec->samplerate = 352800;
    sh->codec->native_tb_num = 1;
    sh->codec->native_tb_den = SACD_FRAME_RATE;
    sh->codec->bitrate = 2822400 * p->area.channels;
    sh->codec->duration = p->area.duration;
    mp_chmap_from_channels(&sh->codec->channels, p->area.channels);
    sh->codec->force_channels = true;
    demux_add_sh_stream(demuxer, sh);

    for (int n = 0; n < p->area.track_count; n++) {
        iina_sacd_track_info track;
        if (!iina_sacd_get_track(sacd, selected, n, &track))
            continue;
        char *title;
        if (track.title && track.title[0] && track.artist && track.artist[0])
            title = talloc_asprintf(demuxer, "%s - %s", track.artist,
                                    track.title);
        else if (track.title && track.title[0])
            title = talloc_strdup(demuxer, track.title);
        else
            title = talloc_asprintf(demuxer, "Track %d", n + 1);
        demuxer_add_chapter(demuxer, title, track.start, n);
    }

    demux_close_stream(demuxer);
    return 0;
}

static void close_sacd(struct demuxer *demuxer)
{
    struct priv *p = demuxer->priv;
    if (p)
        iina_sacd_close(p->sacd);
}

const demuxer_desc_t demuxer_desc_sacd = {
    .name = "sacd",
    .desc = "Scarlet Book SACD ISO",
    .open = open_sacd,
    .read_packet = read_packet,
    .seek = seek,
    .close = close_sacd,
};
