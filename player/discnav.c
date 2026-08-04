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

#include "core.h"
#include "command.h"

#include "common/common.h"
#include "common/msg.h"
#include "input/input.h"

#include "stream/stream.h"
#include "stream/discnav.h"

#include "demux/demux.h"

#include "sub/osd.h"

struct mp_nav_state {
    struct mp_log *log;

    struct mp_nav_state_info st; // last polled snapshot (also read by properties)
    int applied_overlay_change_id;
    struct sub_bitmaps *authored; // owned, in authored coords, for re-scaling
    int overlay_w, overlay_h;     // authored size paired with `authored`
    struct mp_osd_res last_res;
};

// Return the bluray stream if the current demuxer is a disc menu stream.
static struct stream *get_nav_stream(struct MPContext *mpctx)
{
    struct demuxer *demuxer = mpctx->demuxer;
    if (!mpctx->playback_initialized || !demuxer || !demuxer->stream ||
        !demuxer->stream->info)
        return NULL;
    const char *name = demuxer->stream->info->name;
    if (strcmp(name, "bd") != 0 && strcmp(name, "bdmv/bluray") != 0)
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
    if (!mpctx->nav_state)
        return;
    osd_set_nav(mpctx->osd, NULL);
    talloc_free(mpctx->nav_state);
    mpctx->nav_state = NULL;
    mp_notify_property(mpctx, "disc-menu-active");
    mp_notify_property(mpctx, "disc-menu-popup-available");
    mp_notify_property(mpctx, "disc-mouse-on-button");
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
        MP_VERBOSE(mpctx->nav_state, "enabling disc menu navigation\n");
    }
    struct mp_nav_state *nav = mpctx->nav_state;

    struct mp_nav_state_info old = nav->st;
    nav->st = info;
    if (old.menu_active != info.menu_active)
        mp_notify_property(mpctx, "disc-menu-active");
    if (old.popup_available != info.popup_available)
        mp_notify_property(mpctx, "disc-menu-popup-available");
    if (old.mouse_over_button != info.mouse_over_button)
        mp_notify_property(mpctx, "disc-mouse-on-button");

    struct mp_osd_res res = osd_get_vo_res(mpctx->osd);
    if (info.overlay_change_id != nav->applied_overlay_change_id) {
        // Fetch bitmaps, their generation id and authored size together, and
        // trust the fetched id: a publish between the state poll and this fetch
        // cannot leave us applying a mismatched generation.
        struct mp_nav_overlay ov = {0};
        stream_control(s, STREAM_CTRL_GET_NAV_OVERLAY, &ov);
        talloc_free(nav->authored);
        nav->authored = talloc_steal(nav, ov.imgs); // may be NULL
        nav->overlay_w = ov.w;
        nav->overlay_h = ov.h;
        nav->applied_overlay_change_id = ov.change_id;
        apply_overlay(mpctx);
        nav->last_res = res;
    } else if (nav->authored && !osd_res_equals(res, nav->last_res)) {
        apply_overlay(mpctx);
        nav->last_res = res;
    }

    // Poll promptly (but without busy-spinning) while a menu or still is shown,
    // so overlay animations and hover feedback stay responsive.
    if (info.menu_active || info.overlay_visible || info.still_seconds)
        mp_set_timeout(mpctx, 0.05);
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
// osd_rescale_bitmaps()).
static void window_to_authored(struct MPContext *mpctx, int wx, int wy,
                               int *ax, int *ay)
{
    struct mp_nav_state *nav = mpctx->nav_state;
    int fw = nav && nav->st.overlay_w > 0 ? nav->st.overlay_w : 1920;
    int fh = nav && nav->st.overlay_h > 0 ? nav->st.overlay_h : 1080;
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
    if (!s)
        return;
    struct mp_nav_cmd cmd = { .action = parse_action(action) };
    if (cmd.action == MP_NAV_ACTION_NONE)
        return;
    if (cmd.action == MP_NAV_ACTION_MOUSE_MOVE ||
        cmd.action == MP_NAV_ACTION_MOUSE_CLICK)
    {
        int x, y, hover;
        mp_input_get_mouse_pos(mpctx->input, &x, &y, &hover);
        window_to_authored(mpctx, x, y, &cmd.x, &cmd.y);
    }
    stream_control(s, STREAM_CTRL_NAV_CMD, &cmd);
    mp_wakeup_core(mpctx);
}

bool mp_nav_menu_active(struct MPContext *mpctx)
{
    return mpctx->nav_state && mpctx->nav_state->st.menu_active;
}

bool mp_nav_popup_available(struct MPContext *mpctx)
{
    return mpctx->nav_state && mpctx->nav_state->st.popup_available;
}

bool mp_nav_mouse_on_button(struct MPContext *mpctx)
{
    return mpctx->nav_state && mpctx->nav_state->st.mouse_over_button;
}
