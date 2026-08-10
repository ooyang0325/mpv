/*
 * Player-side coordination for Blu-ray/disc menu navigation. This drives the
 * minimal contract exposed by stream_bluray.c: it renders menu overlays through
 * the mpv OSD, mirrors the read-only menu state into properties, and forwards
 * user input to the stream's navigation VM.
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <math.h>

#include "core.h"
#include "command.h"

#include "common/common.h"
#include "common/msg.h"
#include "input/input.h"

#include "audio/bd_sfx.h"
#include "audio/out/ao.h"

#include "filters/f_async_queue.h"

#include "stream/stream.h"
#include "stream/discnav.h"

#include "demux/demux.h"

#include "sub/dec_sub.h"
#include "sub/osd.h"

// Poll iterations the audio output must stay starved during a disc hold before
// we idle it, so a brief gap in authored menu music (or the initial buffered
// audio draining at menu entry) does not deselect a motion menu's audio. Holds
// poll at ~50ms, so this is a few hundred ms of confirmed silence.
#define NAV_AUDIO_IDLE_DEBOUNCE 8

struct mp_nav_state {
    struct mp_log *log;

    struct mp_nav_state_info st; // last polled snapshot (also read by properties)
    int applied_overlay_change_id;
    int applied_reset_id;
    struct sub_bitmaps *authored; // owned, in authored coords, for re-scaling
    int overlay_w, overlay_h;     // authored size paired with `authored`
    struct sub_bitmaps *pending_authored;
    int pending_overlay_w, pending_overlay_h;
    int64_t pending_overlay_pts;
    bool pending_overlay_valid;
    bool pending_menu_active;
    bool pending_overlay_visible;
    bool pending_mouse_over_button;
    bool overlay_wait_for_video;
    bool overlay_restart_seen;
    struct mp_osd_res last_res;
    bool audio_idled;             // AO is paused during a silent hold
    int audio_quiet_polls;        // consecutive polls the hold has been silent
    bool eof_hold;                // transient menu EOF has not resumed media yet
    int applied_audio_change_id;
    int pending_audio_change_id;
    int pending_audio_pid;
    bool authored_audio_disabled;
    int applied_subtitle_change_id;
    int pending_subtitle_change_id;
    int pending_subtitle_pid;
    bool pending_subtitle_enabled;
    bool authored_subtitle_disabled;
    struct track *authored_subtitle_track;
};

// Return the disc stream if the current demuxer is a disc menu stream
// (Blu-ray HDMV or DVD). All of the player-side handling below is media
// agnostic: it drives the shared discnav.h contract.
static struct stream *get_nav_stream(struct MPContext *mpctx)
{
    struct demuxer *demuxer = mpctx->demuxer;
    if (!mpctx->playback_initialized || !demuxer || !demuxer->stream ||
        !demuxer->stream->info)
        return NULL;
    const char *name = demuxer->stream->info->name;
    if (strcmp(name, "bd") != 0 && strcmp(name, "bdmv/bluray") != 0 &&
        strcmp(name, "dvdnav") != 0 && strcmp(name, "ifo_dvdnav") != 0)
        return NULL;
    return demuxer->stream;
}

// Rescale the cached authored overlay to the current OSD resolution and push it
// to the dedicated disc-nav OSD source (never the public overlay-add slot).
static void apply_overlay(struct MPContext *mpctx)
{
    struct mp_nav_state *nav = mpctx->nav_state;
    if (!nav->authored || !nav->authored->num_parts) {
        osd_set_nav(mpctx->osd, NULL);
        return;
    }
    struct sub_bitmaps *imgs = sub_bitmaps_copy(NULL, nav->authored);
    if (!imgs)
        return;
    int fw = nav->overlay_w > 0 ? nav->overlay_w : 1920;
    int fh = nav->overlay_h > 0 ? nav->overlay_h : 1080;
    struct mp_osd_res res = osd_get_vo_res(mpctx->osd);
    osd_rescale_bitmaps(imgs, fw, fh, res, 0);
    osd_set_nav(mpctx->osd, imgs);
    talloc_free(imgs);
}

void mp_nav_destroy(struct MPContext *mpctx)
{
    talloc_free(mpctx->bd_sfx);
    mpctx->bd_sfx = NULL;
    if (!mpctx->nav_state)
        return;
    if (mpctx->nav_state->audio_idled && mpctx->ao)
        ao_set_paused(mpctx->ao, get_internal_paused(mpctx), false);
    if (mpctx->nav_state->authored_subtitle_track &&
        mpctx->nav_state->authored_subtitle_track->d_sub)
        sub_set_forced_only(mpctx->nav_state->authored_subtitle_track->d_sub,
                            false);
    osd_set_nav(mpctx->osd, NULL);
    talloc_free(mpctx->nav_state);
    mpctx->nav_state = NULL;
    mp_notify_property(mpctx, "disc-menu-active");
    mp_notify_property(mpctx, "disc-menu-popup-available");
    mp_notify_property(mpctx, "disc-mouse-on-button");
}

// Drain authored menu sound effects from the disc stream into the mixer. Runs
// on the core thread, same as the mix point in ao_process, so the mixer needs
// no locking. The stream itself copies the PCM out of libbluray on its own
// thread; here we only ever receive owned buffers.
static void fetch_sound_effects(struct MPContext *mpctx, struct stream *s)
{
    struct mp_nav_sound_effect e;
    while (stream_control(s, STREAM_CTRL_GET_NAV_SOUND, &e) == STREAM_OK) {
        if (!mpctx->bd_sfx)
            mpctx->bd_sfx = mp_bd_sfx_create(mpctx, mpctx->log);
        mp_bd_sfx_add(mpctx->bd_sfx, e.samples, e.num_frames, e.num_channels);
        talloc_free(e.samples); // add() copies; release the transferred buffer
    }
}

static void apply_authored_audio(struct MPContext *mpctx,
                                 struct mp_nav_state_info *info)
{
    struct mp_nav_state *nav = mpctx->nav_state;
    if (info->authored_audio_change_id != nav->pending_audio_change_id &&
        info->authored_audio_change_id != nav->applied_audio_change_id)
    {
        nav->pending_audio_change_id = info->authored_audio_change_id;
        nav->pending_audio_pid = info->authored_audio_pid;
    }

    if (nav->pending_audio_change_id == nav->applied_audio_change_id)
        return;
    if (nav->authored_audio_disabled || nav->pending_audio_pid < 0)
    {
        nav->applied_audio_change_id = nav->pending_audio_change_id;
        return;
    }

    struct track *target = NULL;
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track->type == STREAM_AUDIO && !track->is_external &&
            track->demuxer == mpctx->demuxer && track->stream &&
            track->demuxer_id == nav->pending_audio_pid)
        {
            target = track;
            break;
        }
    }
    if (!target)
        return;

    if (mpctx->current_track[0][STREAM_AUDIO] != target)
        mp_switch_track(mpctx, STREAM_AUDIO, target, 0);
    nav->applied_audio_change_id = nav->pending_audio_change_id;
}

static void apply_authored_subtitle(struct MPContext *mpctx,
                                    struct mp_nav_state_info *info)
{
    struct mp_nav_state *nav = mpctx->nav_state;
    if (info->authored_subtitle_change_id !=
            nav->pending_subtitle_change_id &&
        info->authored_subtitle_change_id !=
            nav->applied_subtitle_change_id)
    {
        nav->pending_subtitle_change_id =
            info->authored_subtitle_change_id;
        nav->pending_subtitle_pid = info->authored_subtitle_pid;
        nav->pending_subtitle_enabled = info->authored_subtitle_enabled;
    }

    if (nav->pending_subtitle_change_id == nav->applied_subtitle_change_id)
        return;
    if (nav->authored_subtitle_disabled) {
        nav->applied_subtitle_change_id = nav->pending_subtitle_change_id;
        return;
    }
    if (!mpctx->restart_complete || mpctx->playback_pts == MP_NOPTS_VALUE)
        return;

    if (nav->pending_subtitle_pid < 0) {
        struct track *owned = nav->authored_subtitle_track;
        if (owned && owned->d_sub)
            sub_set_forced_only(owned->d_sub, false);
        if (mpctx->current_track[0][STREAM_SUB] == owned)
            mp_switch_track(mpctx, STREAM_SUB, NULL, 0);
        nav->authored_subtitle_track = NULL;
        nav->applied_subtitle_change_id = nav->pending_subtitle_change_id;
        return;
    }

    struct track *target = NULL;
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track->type == STREAM_SUB && !track->is_external &&
            track->demuxer == mpctx->demuxer && track->stream &&
            track->stream->codec &&
            track->demuxer_id == nav->pending_subtitle_pid &&
            !strcmp(track->stream->codec->codec, "hdmv_pgs_subtitle"))
        {
            target = track;
            break;
        }
    }
    if (!target)
        return;

    if (target->selected &&
        mpctx->current_track[0][STREAM_SUB] != target)
        return;
    if (nav->authored_subtitle_track &&
        nav->authored_subtitle_track != target &&
        nav->authored_subtitle_track->d_sub)
        sub_set_forced_only(nav->authored_subtitle_track->d_sub, false);
    if (mpctx->current_track[0][STREAM_SUB] != target)
        mp_switch_track(mpctx, STREAM_SUB, target, 0);
    if (mpctx->current_track[0][STREAM_SUB] != target)
        return;
    if (target->d_sub) {
        sub_set_forced_only(target->d_sub,
                            !nav->pending_subtitle_enabled);
        target->redraw_subs = true;
    }
    nav->authored_subtitle_track = target;
    nav->applied_subtitle_change_id = nav->pending_subtitle_change_id;
}

void mp_handle_nav(struct MPContext *mpctx)
{
    struct stream *s = get_nav_stream(mpctx);
    struct mp_nav_state_info info;
    bool is_nav = s && stream_control(s, STREAM_CTRL_GET_NAV_STATE, &info) == STREAM_OK;
    if (!is_nav) {
        if (mpctx->nav_state)
            mp_nav_destroy(mpctx);
        return;
    }

    if (!mpctx->nav_state) {
        mpctx->nav_state = talloc_zero(mpctx, struct mp_nav_state);
        mpctx->nav_state->log = mp_log_new(mpctx->nav_state, mpctx->log, "discnav");
        mpctx->nav_state->applied_overlay_change_id = -1;
        mpctx->nav_state->applied_audio_change_id = -1;
        mpctx->nav_state->pending_audio_change_id = -1;
        mpctx->nav_state->applied_subtitle_change_id = -1;
        mpctx->nav_state->pending_subtitle_change_id = -1;
        mpctx->nav_state->authored_subtitle_disabled =
            mpctx->opts->stream_id[0][STREAM_SUB] != -1;
        MP_VERBOSE(mpctx->nav_state, "enabling disc menu navigation\n");
    }
    struct mp_nav_state *nav = mpctx->nav_state;

    if (info.reset_id != nav->applied_reset_id) {
        nav->applied_reset_id = info.reset_id;
        talloc_free(nav->pending_authored);
        nav->pending_authored = NULL;
        nav->pending_overlay_valid = false;
        talloc_free(nav->authored);
        nav->authored = NULL;
        nav->overlay_wait_for_video = false;
        nav->st.menu_active = false;
        nav->st.overlay_visible = false;
        nav->st.mouse_over_button = false;
        osd_set_nav(mpctx->osd, NULL);
        demux_flush(mpctx->demuxer);
        reset_playback_state(mpctx);
        demux_start_prefetch(mpctx->demuxer);
    }

    apply_authored_audio(mpctx, &info);
    apply_authored_subtitle(mpctx, &info);

    struct mp_osd_res res = osd_get_vo_res(mpctx->osd);
    int64_t video_pts = mpctx->video_pts == MP_NOPTS_VALUE
        ? -1 : llrint(mpctx->video_pts * 90000);
    if (!nav->pending_overlay_valid &&
        info.overlay_change_id != nav->applied_overlay_change_id)
    {
        // Fetch bitmaps, their generation id and authored size together, and
        // trust the fetched id: a publish between the state poll and this fetch
        // cannot leave us applying a mismatched generation.
        struct mp_nav_overlay ov = {0};
        stream_control(s, STREAM_CTRL_GET_NAV_OVERLAY, &ov);
        nav->applied_overlay_change_id = ov.change_id;
        if (!mp_nav_overlay_due(video_pts, ov.present_pts)) {
            talloc_free(nav->pending_authored);
            nav->pending_authored = talloc_steal(nav, ov.imgs);
            nav->pending_overlay_w = ov.w;
            nav->pending_overlay_h = ov.h;
            nav->pending_overlay_pts = ov.present_pts;
            nav->pending_overlay_valid = true;
            nav->pending_menu_active = ov.menu_active;
            nav->pending_overlay_visible = ov.overlay_visible;
            nav->pending_mouse_over_button = ov.mouse_over_button;
        } else {
            talloc_free(nav->pending_authored);
            nav->pending_authored = NULL;
            nav->pending_overlay_valid = false;
            talloc_free(nav->authored);
            nav->authored = talloc_steal(nav, ov.imgs); // may be NULL
            nav->overlay_w = ov.w;
            nav->overlay_h = ov.h;
            nav->overlay_wait_for_video = ov.wait_for_video;
            nav->overlay_restart_seen = !mpctx->restart_complete;
            info.menu_active = ov.menu_active;
            info.overlay_visible = ov.overlay_visible;
            info.mouse_over_button = ov.mouse_over_button;
            if (!nav->overlay_wait_for_video) {
                apply_overlay(mpctx);
                nav->last_res = res;
            }
        }
    }

    if (nav->pending_overlay_valid &&
        mp_nav_overlay_due(video_pts, nav->pending_overlay_pts))
    {
        talloc_free(nav->authored);
        nav->authored = nav->pending_authored;
        nav->pending_authored = NULL;
        nav->pending_overlay_valid = false;
        nav->overlay_w = nav->pending_overlay_w;
        nav->overlay_h = nav->pending_overlay_h;
        nav->overlay_wait_for_video = false;
        info.menu_active = nav->pending_menu_active;
        info.overlay_visible = nav->pending_overlay_visible;
        info.mouse_over_button = nav->pending_mouse_over_button;
        apply_overlay(mpctx);
        nav->last_res = res;
    } else if (nav->pending_overlay_valid) {
        info.menu_active = nav->st.menu_active;
        info.overlay_visible = nav->st.overlay_visible;
        info.mouse_over_button = nav->st.mouse_over_button;
        mp_set_timeout(mpctx, 0.01);
    }

    if (nav->overlay_wait_for_video) {
        nav->overlay_restart_seen |= !mpctx->restart_complete;
        if (nav->overlay_restart_seen && mpctx->video_status >= STATUS_READY &&
            mpctx->video_status <= STATUS_PLAYING)
        {
            apply_overlay(mpctx);
            nav->last_res = res;
            nav->overlay_wait_for_video = false;
            nav->overlay_restart_seen = false;
        } else {
            info.menu_active = false;
            info.overlay_visible = false;
            info.mouse_over_button = false;
            mp_set_timeout(mpctx, 0.01);
        }
    } else if (nav->authored && !osd_res_equals(res, nav->last_res)) {
        apply_overlay(mpctx);
        nav->last_res = res;
    }

    struct mp_nav_state_info old = nav->st;
    nav->st = info;
    nav->eof_hold = mp_nav_eof_hold(nav->eof_hold,
        mp_nav_hold(info.menu_active, info.still_seconds) || info.wait_pending,
        mpctx->audio_status == STATUS_EOF && mpctx->video_status == STATUS_EOF);
    if (old.menu_active != info.menu_active)
        mp_notify_property(mpctx, "disc-menu-active");
    if (old.popup_available != info.popup_available)
        mp_notify_property(mpctx, "disc-menu-popup-available");
    if (old.mouse_over_button != info.mouse_over_button)
        mp_notify_property(mpctx, "disc-mouse-on-button");

    double duration;
    if (!nav->pending_overlay_valid &&
        stream_control(s, STREAM_CTRL_GET_TIME_LENGTH, &duration) == STREAM_OK &&
        duration >= 0 && duration != mpctx->demuxer->duration)
    {
        mpctx->demuxer->duration = duration;
        mp_notify(mpctx, MP_EVENT_DURATION_UPDATE, NULL);
    }

    // Idle the audio output only during a *genuinely silent* disc hold. A DVD
    // still, or a WAIT-parked / looping button menu with no program audio,
    // otherwise keeps the audio device open rendering silence for the whole
    // (often indefinite) hold, pinning a CoreAudio render thread near 100% CPU
    // and blocking the DVDNAV_WAIT audio drain; pausing the AO idles the device
    // without tearing down the selected track. But a motion menu (or still)
    // that carries authored
    // in-band background music must keep playing, so only idle once the audio
    // output has actually been starved (no program audio) for a short debounce,
    // and reselect the default track when the hold ends. Audio still plays
    // through menu intros/animations (no hold yet) and titles. The AO is
    // player-owned, so this has to live here.
    // "Program audio" here means audio actually being produced for playback:
    // the audio output chain exists and has decoded frames queued for it. This
    // is the decoded-audio buffer feeding the AO, so it reflects real in-band
    // audio regardless of the output device streaming silence when starved
    // (a CoreAudio pull device keeps "playing", so its underrun flag is not a
    // reliable silence signal). A silent hold drains this queue to empty.
    bool has_program_audio = mpctx->ao_chain && mpctx->ao_chain->ao_queue &&
        mp_async_queue_get_frames(mpctx->ao_chain->ao_queue) > 0;
    bool want_idle = mp_nav_audio_idle(info.menu_active, info.still_seconds,
                                       info.wait_pending, has_program_audio);
    nav->audio_quiet_polls = want_idle ? nav->audio_quiet_polls + 1 : 0;
    if (want_idle && nav->audio_quiet_polls >= NAV_AUDIO_IDLE_DEBOUNCE &&
        mpctx->ao)
    {
        // Reassert this while held: a user pause transition can otherwise
        // resume the AO behind us. ao_set_paused() is idempotent.
        ao_set_paused(mpctx->ao, true, false);
        nav->audio_idled = true;
    } else if (!want_idle && nav->audio_idled) {
        if (mpctx->ao)
            ao_set_paused(mpctx->ao, get_internal_paused(mpctx), false);
        nav->audio_idled = false;
    }

    // Poll promptly (but without busy-spinning) while a menu or still is shown,
    // so overlay animations and hover feedback stay responsive.
    if (info.menu_active || info.overlay_visible || info.still_seconds)
        mp_set_timeout(mpctx, 0.02);
    if (info.still_seconds > 0)
        demux_resume(mpctx->demuxer);

    fetch_sound_effects(mpctx, s);
}

static enum mp_nav_action parse_action(const char *action)
{
    if (!action)
        return MP_NAV_ACTION_NONE;
    if (!strcmp(action, "up"))          return MP_NAV_ACTION_UP;
    if (!strcmp(action, "down"))        return MP_NAV_ACTION_DOWN;
    if (!strcmp(action, "left"))        return MP_NAV_ACTION_LEFT;
    if (!strcmp(action, "right"))       return MP_NAV_ACTION_RIGHT;
    if (!strcmp(action, "select"))      return MP_NAV_ACTION_SELECT;
    if (!strcmp(action, "menu"))        return MP_NAV_ACTION_MENU;
    if (!strcmp(action, "top-menu"))    return MP_NAV_ACTION_MENU;
    if (!strcmp(action, "popup"))       return MP_NAV_ACTION_POPUP;
    if (!strcmp(action, "mouse-move"))  return MP_NAV_ACTION_MOUSE_MOVE;
    if (!strcmp(action, "mouse-click")) return MP_NAV_ACTION_MOUSE_CLICK;
    if (!strcmp(action, "mouse"))       return MP_NAV_ACTION_MOUSE_CLICK;
    if (!strcmp(action, "resume"))      return MP_NAV_ACTION_RESUME;
    return MP_NAV_ACTION_NONE;
}

// Map window/OSD coordinates to authored overlay-plane coordinates (inverse of
// osd_rescale_bitmaps()). Uses the authored size paired with the currently
// rendered overlay generation so clicks land on the buttons the user sees.
static void window_to_authored(struct MPContext *mpctx, int wx, int wy,
                               int *ax, int *ay)
{
    struct mp_nav_state *nav = mpctx->nav_state;
    int fw = nav && nav->overlay_w > 0 ? nav->overlay_w : 1920;
    int fh = nav && nav->overlay_h > 0 ? nav->overlay_h : 1080;
    struct mp_osd_res res = osd_get_vo_res(mpctx->osd);
    int vidw = res.w - res.ml - res.mr;
    int vidh = res.h - res.mt - res.mb;
    double xscale = vidw > 0 ? (double)vidw / fw : 1;
    double yscale = vidh > 0 ? (double)vidh / fh : 1;
    int cx = vidw / 2 - (int)(fw * xscale) / 2;
    int cy = vidh / 2 - (int)(fh * yscale) / 2;
    int rx = xscale ? (int)((wx - res.ml - cx) / xscale) : 0;
    int ry = yscale ? (int)((wy - res.mt - cy) / yscale) : 0;
    *ax = MPCLAMP(rx, 0, fw - 1);
    *ay = MPCLAMP(ry, 0, fh - 1);
}

void mp_nav_user_input(struct MPContext *mpctx, const char *action)
{
    struct stream *s = get_nav_stream(mpctx);
    struct mp_nav_state *nav = mpctx->nav_state;
    if (!s || (nav && (nav->pending_overlay_valid ||
                       nav->st.overlay_change_id !=
                       nav->applied_overlay_change_id)))
        return;
    struct mp_nav_cmd cmd = {
        .action = parse_action(action),
        .pts = mpctx->video_pts == MP_NOPTS_VALUE
            ? -1 : llrint(mpctx->video_pts * 90000),
        .overlay_change_id = mpctx->nav_state
            ? mpctx->nav_state->applied_overlay_change_id : -1,
        .reset_id = mpctx->nav_state
            ? mpctx->nav_state->applied_reset_id : -1,
    };
    if (cmd.action == MP_NAV_ACTION_NONE)
        return;
    if (cmd.action == MP_NAV_ACTION_MOUSE_MOVE ||
        cmd.action == MP_NAV_ACTION_MOUSE_CLICK)
    {
        int x, y, hover;
        mp_input_get_mouse_pos(mpctx->input, &x, &y, &hover);
        window_to_authored(mpctx, x, y, &cmd.x, &cmd.y);
    }
    if (stream_control(s, STREAM_CTRL_NAV_CMD, &cmd) == STREAM_OK)
        demux_resume(mpctx->demuxer);
    mp_wakeup_core(mpctx);
}

void mp_nav_disable_authored_audio(struct MPContext *mpctx)
{
    if (mpctx->nav_state)
        mpctx->nav_state->authored_audio_disabled = true;
}

void mp_nav_disable_authored_subtitle(struct MPContext *mpctx)
{
    if (!mpctx->nav_state)
        return;
    struct track *track = mpctx->nav_state->authored_subtitle_track;
    if (track && track->d_sub)
        sub_set_forced_only(track->d_sub, false);
    mpctx->nav_state->authored_subtitle_track = NULL;
    mpctx->nav_state->authored_subtitle_disabled = true;
}

bool mp_nav_menu_active(struct MPContext *mpctx)
{
    return mpctx->nav_state && mpctx->nav_state->st.menu_active;
}

bool mp_nav_eof_hold_active(struct MPContext *mpctx)
{
    return mpctx->nav_state && mpctx->nav_state->eof_hold;
}

// True while a disc menu or still is on screen, i.e. the disc stream is
// intentionally idle waiting for user input rather than starved. The player
// uses this to avoid entering the cache-buffering pause, which would otherwise
// freeze an authored still menu behind a "Buffering..." state (the demuxer
// legitimately produces no packets until the user navigates).
bool mp_nav_hold_active(struct MPContext *mpctx)
{
    struct mp_nav_state *nav = mpctx->nav_state;
    return nav && mp_nav_hold(nav->st.menu_active, nav->st.still_seconds);
}

// True while the disc stream is parked at a DVDNAV_WAIT sync point waiting for
// the player pipeline to drain to that boundary. The player keeps presenting
// (does not buffer-pause) and releases the wait once the pipeline has drained.
bool mp_nav_wait_pending(struct MPContext *mpctx)
{
    return mpctx->nav_state && mpctx->nav_state->st.wait_pending;
}

bool mp_nav_audio_idle_active(struct MPContext *mpctx)
{
    return mpctx->nav_state && mpctx->nav_state->audio_idled;
}

bool mp_nav_popup_available(struct MPContext *mpctx)
{
    return mpctx->nav_state && mpctx->nav_state->st.popup_available;
}

bool mp_nav_mouse_on_button(struct MPContext *mpctx)
{
    return mpctx->nav_state && mpctx->nav_state->st.mouse_over_button;
}
