/*
 * Copyright (C) 2010 Benjamin Zores <ben@geexbox.org>
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
 * Blu-ray parser/reader using libbluray
 *  Use 'git clone git://git.videolan.org/libbluray' to get it.
 *
 * TODO:
 *  - Add descrambled keys database support (KEYDB.cfg)
 *
 */

#include <assert.h>
#include <string.h>

#include <libbluray/bluray.h>
#include <libbluray/meta_data.h>
#include <libbluray/overlay.h>
#include <libbluray/keys.h>
#include <libbluray/bluray-version.h>
#include <libbluray/log_control.h>
#include <libavutil/common.h>

#include "config.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/thread_tools.h"
#include "options/m_config.h"
#include "options/options.h"
#include "options/path.h"
#include "stream.h"
#include "discnav.h"
#include "bluray_overlay.h"
#include "osdep/io.h"
#include "osdep/timer.h"
#include "osdep/threads.h"
#include "sub/osd.h"
#include "sub/img_convert.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"

#define BLURAY_SECTOR_SIZE     6144

// Number of overlay planes (0: Presentation Graphics, 1: Interactive Graphics).
#define BLURAY_NUM_OVERLAYS    2

// Poll interval while waiting out a still frame, to avoid busy-spinning (ns).
#define BLURAY_STILL_POLL_NS   (10 * 1000 * 1000)

// Bound on authored menu sound effects buffered for the player. Rapid button
// presses stay bounded: on overflow the oldest queued clip is dropped.
#define BLURAY_MAX_PENDING_SFX 8

// libbluray always authors sound effects as 48 kHz LPCM (see bd_sound_effect).

#define BLURAY_DEFAULT_ANGLE      0
#define BLURAY_DEFAULT_CHAPTER    0
#define BLURAY_PLAYLIST_TITLE    -3
#define BLURAY_DEFAULT_TITLE     -2
#define BLURAY_MENU_TITLE        -1

// 90khz ticks
#define BD_TIMEBASE (90000)
#define BD_TIME_TO_MP(x) ((x) / (double)(BD_TIMEBASE))
#define BD_TIME_FROM_MP(x) ((uint64_t)(x * BD_TIMEBASE))

// copied from aacs.h in libaacs
#define AACS_ERROR_CORRUPTED_DISC -1 /* opening or reading of AACS files failed */
#define AACS_ERROR_NO_CONFIG      -2 /* missing config file */
#define AACS_ERROR_NO_PK          -3 /* no matching processing key */
#define AACS_ERROR_NO_CERT        -4 /* no valid certificate */
#define AACS_ERROR_CERT_REVOKED   -5 /* certificate has been revoked */
#define AACS_ERROR_MMC_OPEN       -6 /* MMC open failed (no MMC drive ?) */
#define AACS_ERROR_MMC_FAILURE    -7 /* MMC failed */
#define AACS_ERROR_NO_DK          -8 /* no matching device key */


#define OPT_BASE_STRUCT struct mp_bluray_opts
const struct m_sub_options stream_bluray_conf = {
    .opts = (const struct m_option[]) {
        {"device", OPT_STRING(bluray_device), .flags = M_OPT_FILE},
        {"angle", OPT_INT(angle), M_RANGE(1, 999)},
        {0},
    },
    .size = sizeof(struct mp_bluray_opts),
    .defaults = &(const struct mp_bluray_opts){
        .angle = 1,
    },
};

struct bluray_overlay_plane {
    struct mp_image *image; // persistent premultiplied BGRA plane buffer
    int x, y, w, h;         // authored position/size on the overlay plane
    bool active;            // plane has been initialized
    bool hidden;            // plane should not be displayed
    bool has_content;       // something has been drawn since the last clear
};

struct bluray_priv_s {
    BLURAY *bd;
    BLURAY_TITLE_INFO *title_info;
    int num_titles;
    int current_angle;
    int current_title;
    int current_playlist;
    int current_playitem;
    uint32_t audio_stream_number;
    unsigned playlist_epoch;
    unsigned published_audio_epoch;
    uint32_t published_audio_stream;
    int authored_audio_pid;
    int authored_audio_change_id;
    bool had_media_data;
    int pending_chapter;
    bool pending_chapter_from_menu;

    int cfg_title;
    int cfg_playlist;
    char *cfg_device;

    // Navigation (menu) mode. Direct title/playlist playback leaves all of the
    // following untouched.
    bool use_nav;
    struct mp_image_pool *pool;
    mp_mutex bd_lock; // serializes libbluray reads and user input

    // Overlay planes and published bitmaps. Planes are only ever touched on the
    // demuxer read thread (inside the libbluray overlay callback / bd_read_ext).
    struct bluray_overlay_plane planes[BLURAY_NUM_OVERLAYS];

    // Shared state between the demuxer read thread and the player thread.
    // Everything below is protected by nav_lock.
    mp_mutex nav_lock;
    struct sub_bitmaps *pending_overlay; // owned; handed to the player on demand
    bool pending_overlay_wait;
    int overlay_change_id;
    int overlay_w, overlay_h;
    double duration;
    bool overlay_visible;
    bool ig_visible;             // Interactive Graphics (menu) plane is on screen
    bool in_menu;
    bool popup_available;
    bool mouse_over_button;
    int still_length;            // 0: none, -1: infinite, >0: seconds
    bool reset_pending;          // nested demuxer should re-sync
    int reset_id;
    bool reset_hold;             // do not expose new bytes before reset ack
    bool wait_pending;           // natural EOT is draining through the player
    bool wait_release;
    bool transition_overlay_pending;
    uint8_t *held_data;          // event-associated bytes replayed after reset
    int held_len, held_pos;

    // Pending authored menu sound effects (BD_EVENT_SOUND_EFFECT). Copied out
    // of libbluray on this (stream) thread and drained by the player through
    // STREAM_CTRL_GET_NAV_SOUND. Bounded ring: a stalled consumer can never
    // make this grow without limit. Each buffer is an independent talloc root
    // so ownership can cross to the player thread without touching a shared
    // parent. Protected by nav_lock.
    struct bluray_pending_sfx {
        int16_t *samples;        // owned talloc root; NULL when slot is empty
        int num_frames;
        int num_channels;
    } sfx_queue[BLURAY_MAX_PENDING_SFX];
    int sfx_head, sfx_count;

    struct mp_bluray_opts *opts;
    struct m_config_cache *opts_cache;
};

inline static int play_playlist(struct bluray_priv_s *priv, int playlist)
{
    return bd_select_playlist(priv->bd, playlist);
}

inline static int play_title(struct bluray_priv_s *priv, int title)
{
    return bd_select_title(priv->bd, title);
}

// ---- Disc menu navigation (menu mode only) -------------------------------

static void overlay_plane_release(struct bluray_overlay_plane *plane)
{
    if (plane->image)
        talloc_free(plane->image);
    *plane = (struct bluray_overlay_plane){0};
}

static void overlay_plane_alloc(struct bluray_priv_s *b,
                                struct bluray_overlay_plane *plane,
                                int x, int y, int w, int h)
{
    overlay_plane_release(plane);
    if (w < 1 || h < 1)
        return;
    struct mp_image *mpi = mp_image_pool_get(b->pool, IMGFMT_BGRA, w, h);
    if (!mpi)
        return;
    mp_image_clear(mpi, 0, 0, w, h);
    plane->image = mpi;
    plane->x = x;
    plane->y = y;
    plane->w = w;
    plane->h = h;
    plane->active = true;
    plane->hidden = false;
    plane->has_content = false;
}

static void overlay_close_all(struct bluray_priv_s *b)
{
    for (int i = 0; i < BLURAY_NUM_OVERLAYS; i++)
        overlay_plane_release(&b->planes[i]);
}

static bool overlay_rect_valid(struct bluray_overlay_plane *plane,
                               const BD_OVERLAY *bo)
{
    return plane->image && bo->x <= plane->w && bo->y <= plane->h &&
           bo->w <= plane->w - bo->x && bo->h <= plane->h - bo->y;
}

// Build a fresh sub_bitmaps snapshot (in authored coordinates) from the visible
// planes, or NULL if nothing is visible. Runs on the demuxer read thread.
// change_id is filled in by the caller under nav_lock so that the published
// generation id and the bitmaps stay in lockstep.
static struct sub_bitmaps *build_overlay_bitmaps(struct bluray_priv_s *b)
{
    int num = 0, packed_w = 0, packed_h = 0;
    for (int i = 0; i < BLURAY_NUM_OVERLAYS; i++) {
        struct bluray_overlay_plane *p = &b->planes[i];
        if (p->active && p->image && !p->hidden && p->has_content) {
            num++;
            packed_w = MPMAX(packed_w, p->w);
            packed_h += p->h;
        }
    }
    if (!num)
        return NULL;

    struct mp_image *packed = mp_image_alloc(IMGFMT_BGRA, packed_w, packed_h);
    if (!packed)
        return NULL;
    mp_image_clear(packed, 0, 0, packed_w, packed_h);

    struct sub_bitmaps *res = talloc_zero(NULL, struct sub_bitmaps);
    res->format = SUBBITMAP_BGRA;
    res->packed = talloc_steal(res, packed);
    res->packed_w = packed_w;
    res->packed_h = packed_h;
    res->parts = talloc_array(res, struct sub_bitmap, num);

    int y_off = 0, n = 0;
    for (int i = 0; i < BLURAY_NUM_OVERLAYS; i++) {
        struct bluray_overlay_plane *p = &b->planes[i];
        if (!(p->active && p->image && !p->hidden && p->has_content))
            continue;
        uint8_t *dst = (uint8_t *)packed->planes[0] + y_off * packed->stride[0];
        for (int y = 0; y < p->h; y++) {
            memcpy(dst + y * packed->stride[0],
                   (uint8_t *)p->image->planes[0] + y * p->image->stride[0],
                   p->w * 4);
        }
        res->parts[n] = (struct sub_bitmap){
            .bitmap = dst,
            .stride = packed->stride[0],
            .w = p->w, .h = p->h,
            .dw = p->w, .dh = p->h,
            .x = p->x, .y = p->y,
            .src_x = 0, .src_y = y_off,
        };
        y_off += p->h;
        n++;
    }
    res->num_parts = n;
    return res;
}

// Publish the currently visible overlay to the player side. The bitmaps are
// built outside the lock (planes are demuxer-thread only), but the change id,
// bitmaps, visibility and authored size are stored together under nav_lock so a
// consumer can never observe a bumped id without the matching bitmaps/size.
static void publish_overlay(struct bluray_priv_s *b)
{
    int w = 0, h = 0;
    for (int i = 0; i < BLURAY_NUM_OVERLAYS; i++) {
        if (b->planes[i].active) {
            w = MPMAX(w, b->planes[i].x + b->planes[i].w);
            h = MPMAX(h, b->planes[i].y + b->planes[i].h);
        }
    }

    // The Interactive Graphics plane carries the HDMV button menu. Treat it as
    // an active menu whenever it is visibly drawn, independent of BD_EVENT_MENU
    // (which is not always emitted on a title -> Top Menu transition). The
    // Presentation Graphics plane (subtitles) must not count as a menu.
    struct bluray_overlay_plane *ig = &b->planes[BD_OVERLAY_IG];
    bool ig_visible = ig->active && ig->image && !ig->hidden && ig->has_content;

    struct sub_bitmaps *imgs = build_overlay_bitmaps(b);

    mp_mutex_lock(&b->nav_lock);
    int id = ++b->overlay_change_id;
    if (imgs)
        imgs->change_id = id;
    talloc_free(b->pending_overlay);
    b->pending_overlay = imgs;
    b->pending_overlay_wait = b->transition_overlay_pending;
    b->transition_overlay_pending = false;
    b->overlay_visible = imgs != NULL;
    b->ig_visible = ig_visible;
    if (w > 0 && h > 0) {
        b->overlay_w = w;
        b->overlay_h = h;
    }
    mp_mutex_unlock(&b->nav_lock);
}

static void overlay_process(void *data, const BD_OVERLAY *const bo)
{
    stream_t *s = data;
    struct bluray_priv_s *b = s->priv;
    if (!bo) {
        overlay_close_all(b);
        publish_overlay(b);
        return;
    }
    if (bo->plane >= BLURAY_NUM_OVERLAYS)
        return;
    struct bluray_overlay_plane *plane = &b->planes[bo->plane];

    switch (bo->cmd) {
    case BD_OVERLAY_INIT:
        overlay_plane_alloc(b, plane, bo->x, bo->y, bo->w, bo->h);
        break;
    case BD_OVERLAY_CLOSE:
        overlay_plane_release(plane);
        publish_overlay(b);
        break;
    case BD_OVERLAY_CLEAR:
        if (plane->image) {
            mp_image_clear(plane->image, 0, 0, plane->w, plane->h);
            plane->has_content = false;
        }
        break;
    case BD_OVERLAY_DRAW: {
        if (!overlay_rect_valid(plane, bo) || !bo->img || !bo->palette)
            break;
        int stride_px = plane->image->stride[0] / 4;
        uint32_t *origin = (uint32_t *)plane->image->planes[0];
        uint32_t *dst = origin + stride_px * bo->y + bo->x;
        mp_bd_decode_rle(dst, stride_px, bo->w, bo->h,
                         (const struct mp_bd_palette_entry *)bo->palette,
                         (const struct mp_bd_rle_elem *)bo->img);
        plane->hidden = false;
        plane->has_content = true;
        break;
    }
    case BD_OVERLAY_WIPE: {
        if (!overlay_rect_valid(plane, bo))
            break;
        int stride_px = plane->image->stride[0] / 4;
        uint32_t *origin = (uint32_t *)plane->image->planes[0];
        for (int y = 0; y < bo->h; y++)
            memset(origin + stride_px * (y + bo->y) + bo->x, 0, 4 * bo->w);
        break;
    }
    case BD_OVERLAY_HIDE:
        plane->hidden = true;
        break;
    case BD_OVERLAY_FLUSH:
        publish_overlay(b);
        break;
    default:
        break;
    }
}

static bd_vk_key_e nav_action_to_vk(enum mp_nav_action action)
{
    switch (action) {
    case MP_NAV_ACTION_UP:     return BD_VK_UP;
    case MP_NAV_ACTION_DOWN:   return BD_VK_DOWN;
    case MP_NAV_ACTION_LEFT:   return BD_VK_LEFT;
    case MP_NAV_ACTION_RIGHT:  return BD_VK_RIGHT;
    case MP_NAV_ACTION_SELECT: return BD_VK_ENTER;
    default:                   return BD_VK_NONE;
    }
}

// Apply a single navigation command. Runs on the demuxer read thread so that
// the BLURAY* VM is only ever touched from one thread.
static void apply_nav_command(struct bluray_priv_s *b, struct mp_nav_cmd *cmd)
{
    const int64_t pts = cmd->pts;
    bd_vk_key_e key = nav_action_to_vk(cmd->action);
    if (key != BD_VK_NONE) {
        bd_user_input(b->bd, pts, key);
        return;
    }
    switch (cmd->action) {
    case MP_NAV_ACTION_MENU:
        bd_menu_call(b->bd, pts);
        break;
    case MP_NAV_ACTION_POPUP:
        bd_user_input(b->bd, pts, BD_VK_POPUP);
        break;
    case MP_NAV_ACTION_MOUSE_MOVE: {
        int over = bd_mouse_select(b->bd, pts, cmd->x, cmd->y);
        mp_mutex_lock(&b->nav_lock);
        b->mouse_over_button = over > 0;
        mp_mutex_unlock(&b->nav_lock);
        break;
    }
    case MP_NAV_ACTION_MOUSE_CLICK: {
        int over = bd_mouse_select(b->bd, pts, cmd->x, cmd->y);
        mp_mutex_lock(&b->nav_lock);
        b->mouse_over_button = over > 0;
        mp_mutex_unlock(&b->nav_lock);
        bd_user_input(b->bd, pts, BD_VK_MOUSE_ACTIVATE);
        break;
    }
    case MP_NAV_ACTION_RESUME:
        // libbluray has no generic resume; best effort is to dismiss a popup.
        if (b->popup_available)
            bd_user_input(b->bd, pts, BD_VK_POPUP);
        break;
    default:
        break;
    }
}

static void bluray_stream_close(stream_t *s)
{
    struct bluray_priv_s *priv = s->priv;
    if (!priv)
        return;

    if (priv->title_info)
        bd_free_title_info(priv->title_info);
    if (priv->use_nav) {
        if (priv->bd)
            bd_register_overlay_proc(priv->bd, NULL, NULL);
        overlay_close_all(priv);
        talloc_free(priv->pending_overlay);
        talloc_free(priv->pool);
        for (int n = 0; n < BLURAY_MAX_PENDING_SFX; n++)
            talloc_free(priv->sfx_queue[n].samples);
        mp_mutex_destroy(&priv->bd_lock);
        mp_mutex_destroy(&priv->nav_lock);
    }
    if (priv->bd)
        bd_close(priv->bd);
}

#if BLURAY_VERSION >= BLURAY_VERSION_CODE(0, 9, 0)
// Fetch an authored menu sound effect from libbluray (owned by the BLURAY* and
// only valid to touch on this thread), copy its PCM into an independent buffer,
// and queue it for the player. Runs on the stream/demux read thread.
static void queue_sound_effect(stream_t *s, uint32_t id)
{
    struct bluray_priv_s *b = s->priv;

    BLURAY_SOUND_EFFECT effect = {0};
    // bd_get_sound_effect: <0 no effects at all, 0 id out of range, 1 success.
    if (bd_get_sound_effect(b->bd, id, &effect) != 1)
        return;
    if (!effect.samples || effect.num_frames <= 0 ||
        (effect.num_channels != 1 && effect.num_channels != 2))
        return;

    // Copy out of the library-owned buffer immediately: it is only valid until
    // the next libbluray call. Use an independent talloc root so the player can
    // take ownership across the thread boundary without racing on a shared
    // parent context.
    size_t n = (size_t)effect.num_frames * effect.num_channels;
    int16_t *copy = talloc_array(NULL, int16_t, n);
    memcpy(copy, effect.samples, n * sizeof(int16_t));

    mp_mutex_lock(&b->nav_lock);
    if (b->sfx_count >= BLURAY_MAX_PENDING_SFX) {
        // Drop the oldest clip so button mashing stays bounded.
        struct bluray_pending_sfx *old = &b->sfx_queue[b->sfx_head];
        talloc_free(old->samples);
        old->samples = NULL;
        b->sfx_head = (b->sfx_head + 1) % BLURAY_MAX_PENDING_SFX;
        b->sfx_count--;
    }
    int tail = (b->sfx_head + b->sfx_count) % BLURAY_MAX_PENDING_SFX;
    b->sfx_queue[tail] = (struct bluray_pending_sfx){
        .samples = copy,
        .num_frames = effect.num_frames,
        .num_channels = effect.num_channels,
    };
    b->sfx_count++;
    mp_mutex_unlock(&b->nav_lock);
}
#endif

static void publish_authored_audio(struct bluray_priv_s *b)
{
    if (!b->use_nav)
        return;

    int pid = -1;
    if (b->title_info && b->current_playitem >= 0 &&
        b->current_playitem < b->title_info->clip_count)
    {
        BLURAY_CLIP_INFO *clip = &b->title_info->clips[b->current_playitem];
        int index = mp_nav_bluray_stream_index(b->audio_stream_number,
                                                clip->audio_stream_count);
        if (index >= 0)
            pid = clip->audio_streams[index].pid;
    }

    if (b->published_audio_epoch == b->playlist_epoch &&
        b->published_audio_stream == b->audio_stream_number &&
        b->authored_audio_pid == pid)
        return;

    b->published_audio_epoch = b->playlist_epoch;
    b->published_audio_stream = b->audio_stream_number;
    mp_mutex_lock(&b->nav_lock);
    b->authored_audio_pid = pid;
    b->authored_audio_change_id++;
    mp_mutex_unlock(&b->nav_lock);
}

static void handle_event(stream_t *s, const BD_EVENT *ev)
{
    struct bluray_priv_s *b = s->priv;
    switch (ev->event) {
    case BD_EVENT_MENU:
        if (b->use_nav) {
            mp_mutex_lock(&b->nav_lock);
            b->in_menu = ev->param;
            mp_mutex_unlock(&b->nav_lock);
        }
        break;
    case BD_EVENT_STILL:
        if (b->use_nav) {
            mp_mutex_lock(&b->nav_lock);
            b->still_length = ev->param ? -1 : 0;
            mp_mutex_unlock(&b->nav_lock);
        }
        break;
    case BD_EVENT_STILL_TIME:
        if (b->use_nav) {
            mp_mutex_lock(&b->nav_lock);
            b->still_length = ev->param ? (int)ev->param : -1;
            mp_mutex_unlock(&b->nav_lock);
        } else {
            bd_read_skip_still(b->bd);
        }
        break;
    case BD_EVENT_END_OF_TITLE:
        if (b->use_nav) {
            mp_mutex_lock(&b->nav_lock);
            b->wait_pending = true;
            b->wait_release = false;
            mp_mutex_unlock(&b->nav_lock);
        }
        break;
    case BD_EVENT_PLAYLIST:
        b->current_playlist = ev->param;
        b->current_playitem = -1;
        b->playlist_epoch++;
#if HAVE_LIBBLURAY_GPR
        if (b->pending_chapter_from_menu) {
            uint32_t chapter = bd_get_gpr(b->bd, 3);
            b->pending_chapter = chapter > 0 ? chapter : -1;
            b->pending_chapter_from_menu = false;
        }
#endif
        if (!b->use_nav)
            b->current_title = bd_get_current_title(b->bd);
        if (b->title_info)
            bd_free_title_info(b->title_info);
        b->title_info = bd_get_playlist_info(b->bd, b->current_playlist,
                                             b->current_angle);
        publish_authored_audio(b);
        if (b->use_nav) {
            mp_mutex_lock(&b->nav_lock);
            b->duration = b->title_info
                ? BD_TIME_TO_MP(b->title_info->duration) : -1;
            mp_mutex_unlock(&b->nav_lock);
        }
        if (b->pending_chapter >= 0 && b->title_info &&
            b->pending_chapter < b->title_info->chapter_count)
        {
            bd_seek_chapter(b->bd, b->pending_chapter);
            b->pending_chapter = -1;
        }
        if (b->use_nav && b->had_media_data) {
            mp_mutex_lock(&b->nav_lock);
            if (!b->reset_pending)
                b->reset_id++;
            b->reset_pending = true;
            b->reset_hold = true;
            b->transition_overlay_pending = true;
            if (b->pending_overlay)
                b->pending_overlay_wait = true;
            mp_mutex_unlock(&b->nav_lock);
        }
        break;
    case BD_EVENT_PLAYITEM:
        b->current_playitem = ev->param;
        publish_authored_audio(b);
        break;
    case BD_EVENT_AUDIO_STREAM:
        b->audio_stream_number = ev->param;
        publish_authored_audio(b);
        break;
    case BD_EVENT_TITLE: {
#if HAVE_LIBBLURAY_GPR
        int old_title = b->current_title;
#endif
        if (ev->param == BLURAY_TITLE_FIRST_PLAY) {
            b->current_title = bd_get_current_title(b->bd);
        } else
            b->current_title = ev->param;
#if HAVE_LIBBLURAY_GPR
        // ponytail: some HDMV menus hand a zero-based chapter through GPR3
        // immediately before the following PLAYLIST event.
        b->pending_chapter_from_menu =
            old_title == BLURAY_TITLE_TOP_MENU && b->current_title > 0;
#endif
        if (b->title_info) {
            bd_free_title_info(b->title_info);
            b->title_info = NULL;
        }
        if (b->use_nav) {
            mp_mutex_lock(&b->nav_lock);
            b->duration = -1;
            mp_mutex_unlock(&b->nav_lock);
        }
        break;
    }
    case BD_EVENT_ANGLE:
        b->current_angle = ev->param;
        if (b->title_info) {
            bd_free_title_info(b->title_info);
            b->title_info = bd_get_playlist_info(b->bd, b->current_playlist,
                                                 b->current_angle);
        }
        publish_authored_audio(b);
        break;
    case BD_EVENT_POPUP:
        if (b->use_nav) {
            mp_mutex_lock(&b->nav_lock);
            b->popup_available = ev->param;
            mp_mutex_unlock(&b->nav_lock);
        }
        break;
#if BLURAY_VERSION >= BLURAY_VERSION_CODE(0, 9, 0)
    case BD_EVENT_SOUND_EFFECT:
        // Only navigation (menu) mode overlays authored effects. Direct title
        // playback leaves audio untouched, and DVD menu sound is already
        // carried in-band by the program stream.
        if (b->use_nav)
            queue_sound_effect(s, ev->param);
        break;
#endif
#if BLURAY_VERSION >= BLURAY_VERSION_CODE(0, 5, 0)
    case BD_EVENT_DISCONTINUITY:
        if (b->use_nav && b->had_media_data) {
            mp_mutex_lock(&b->nav_lock);
            if (!b->reset_pending)
                b->reset_id++;
            b->reset_pending = true;
            b->reset_hold = true;
            mp_mutex_unlock(&b->nav_lock);
        }
        break;
#endif
#if BLURAY_VERSION >= BLURAY_VERSION_CODE(0, 6, 0)
    case BD_EVENT_PLAYLIST_STOP:
    case BD_EVENT_SEEK:
        // Both cross a media boundary within the same navigation session and
        // require the nested demuxer to flush and re-sync.
        if (b->use_nav && b->had_media_data) {
            mp_mutex_lock(&b->nav_lock);
            if (!b->reset_pending)
                b->reset_id++;
            b->reset_pending = true;
            b->reset_hold = true;
            mp_mutex_unlock(&b->nav_lock);
        }
        break;
#endif
    default:
        MP_TRACE(s, "Unhandled event: %d %d\n", ev->event, ev->param);
        break;
    }
}

static int bluray_stream_fill_buffer(stream_t *s, void *buf, int len)
{
    struct bluray_priv_s *b = s->priv;
    BD_EVENT event;
    while (bd_get_event(b->bd, &event))
        handle_event(s, &event);
    return bd_read(b->bd, buf, len);
}

static int bdnav_stream_fill_buffer(stream_t *s, void *buf, int len)
{
    struct bluray_priv_s *b = s->priv;
    int64_t still_start = 0;
    for (;;) {
        mp_mutex_lock(&b->nav_lock);
        bool reset_hold = b->reset_hold;
        bool wait_pending = b->wait_pending;
        bool wait_release = b->wait_release;
        mp_mutex_unlock(&b->nav_lock);
        if (reset_hold || (wait_pending && !wait_release))
            return 0;
        if (wait_pending) {
            mp_mutex_lock(&b->nav_lock);
            b->wait_pending = false;
            b->wait_release = false;
            mp_mutex_unlock(&b->nav_lock);
        }

        if (b->held_data) {
            int copy = MPMIN(len, b->held_len - b->held_pos);
            memcpy(buf, b->held_data + b->held_pos, copy);
            b->held_pos += copy;
            if (b->held_pos == b->held_len) {
                talloc_free(b->held_data);
                b->held_data = NULL;
                b->held_len = b->held_pos = 0;
            }
            return copy;
        }

        BD_EVENT event;
        mp_mutex_lock(&b->bd_lock);
        int read = bd_read_ext(b->bd, (unsigned char *)buf, len, &event);
        if (read < 0) {
            mp_mutex_unlock(&b->bd_lock);
            return -1;
        }
        // Always process a pending event, even when data was also returned, so
        // menu-state changes and resets are never dropped. The nested demuxer's
        // reset handshake (demux_disc) discards any packet that straddles the
        // resulting transition.
        if (event.event != BD_EVENT_NONE)
            handle_event(s, &event);
        mp_mutex_unlock(&b->bd_lock);

        mp_mutex_lock(&b->nav_lock);
        reset_hold = b->reset_hold;
        mp_mutex_unlock(&b->nav_lock);
        if (reset_hold) {
            if (read > 0) {
                talloc_free(b->held_data);
                b->held_data = talloc_memdup(b, buf, read);
                b->held_len = read;
                b->held_pos = 0;
            }
            return 0;
        }

        if (read > 0) {
            b->had_media_data = true;
            return read;
        }

        // read == 0: no data was produced this call.
#if BLURAY_VERSION >= BLURAY_VERSION_CODE(0, 9, 0)
        if (event.event == BD_EVENT_IDLE) {
            // Playlist is not playing but a title applet is running. libbluray
            // explicitly asks not to call bd_read*() again immediately; back off
            // so the demuxer read thread cannot busy-loop.
            if (s->cancel && mp_cancel_test(s->cancel))
                return 0;
            mp_sleep_ns(BLURAY_STILL_POLL_NS);
            still_start = 0;
            continue;
        }
#endif
        if (event.event != BD_EVENT_NONE) {
            // Some other event was handled above; try reading again.
            still_start = 0;
            continue;
        }

        // event == BD_EVENT_NONE: either a still frame or end of stream.
        mp_mutex_lock(&b->nav_lock);
        int still = b->still_length;
        mp_mutex_unlock(&b->nav_lock);

        if (still != 0) {
            if (s->cancel && mp_cancel_test(s->cancel))
                return 0;
            if (still > 0) {
                // Timed still: keep the frame until the time elapses, then let
                // libbluray continue.
                if (!still_start)
                    still_start = mp_time_ns();
                if (mp_time_ns() - still_start >= (int64_t)still * 1000000000) {
                    bd_read_skip_still(b->bd);
                    mp_mutex_lock(&b->nav_lock);
                    b->still_length = 0;
                    mp_mutex_unlock(&b->nav_lock);
                    still_start = 0;
                    continue;
                }
            }
            // Infinite still ends when a menu action turns it off; poll without
            // busy-spinning.
            mp_sleep_ns(BLURAY_STILL_POLL_NS);
            continue;
        }

        // Genuine end of stream.
        return 0;
    }
}

static int bluray_stream_control(stream_t *s, int cmd, void *arg)
{
    struct bluray_priv_s *b = s->priv;

    switch (cmd) {
    case STREAM_CTRL_GET_NUM_CHAPTERS: {
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (!ti)
            return STREAM_UNSUPPORTED;
        *((unsigned int *) arg) = ti->chapter_count;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_CHAPTER_TIME: {
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (!ti)
            return STREAM_UNSUPPORTED;
        int chapter = *(double *)arg;
        double time = MP_NOPTS_VALUE;
        if (chapter >= 0 && chapter < ti->chapter_count)
            time = BD_TIME_TO_MP(ti->chapters[chapter].start);
        if (time == MP_NOPTS_VALUE)
            return STREAM_ERROR;
        *(double *)arg = time;
        return STREAM_OK;
    }
    case STREAM_CTRL_SET_CURRENT_TITLE: {
        const uint32_t title = *((unsigned int*)arg);
        if (title >= b->num_titles || !play_title(b, title))
            return STREAM_UNSUPPORTED;
        b->current_title = title;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_CURRENT_TITLE: {
        *((unsigned int *) arg) = b->current_title;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NUM_TITLES: {
        *((unsigned int *)arg) = b->num_titles;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TIME_LENGTH: {
        if (b->use_nav) {
            mp_mutex_lock(&b->nav_lock);
            double duration = b->duration;
            mp_mutex_unlock(&b->nav_lock);
            if (duration < 0)
                return STREAM_UNSUPPORTED;
            *((double *)arg) = duration;
            return STREAM_OK;
        }
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (!ti)
            return STREAM_UNSUPPORTED;
        *((double *) arg) = BD_TIME_TO_MP(ti->duration);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_CURRENT_TIME: {
        *((double *) arg) = BD_TIME_TO_MP(bd_tell_time(b->bd));
        return STREAM_OK;
    }
    case STREAM_CTRL_SEEK_TO_TIME: {
        double pts = *((double *) arg);
        bd_seek_time(b->bd, BD_TIME_FROM_MP(pts));
        stream_drop_buffers(s);
        // API makes it hard to determine seeking success
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NUM_ANGLES: {
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (!ti)
            return STREAM_UNSUPPORTED;
        *((int *) arg) = ti->angle_count;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_ANGLE: {
        *((int *) arg) = b->current_angle;
        return STREAM_OK;
    }
    case STREAM_CTRL_SET_ANGLE: {
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (!ti)
            return STREAM_UNSUPPORTED;
        int angle = *((int *) arg);
        if (angle < 0 || angle > ti->angle_count)
            return STREAM_UNSUPPORTED;
        b->current_angle = angle;
        bd_seamless_angle_change(b->bd, angle);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TITLE_LENGTH: {
        int title = *(double *)arg;
        if (!b->bd || title < 0 || title >= b->num_titles)
            return STREAM_UNSUPPORTED;
        BLURAY_TITLE_INFO *ti = bd_get_title_info(b->bd, title, 0);
        if (!ti)
            return STREAM_UNSUPPORTED;
        *(double *)arg = BD_TIME_TO_MP(ti->duration);
        bd_free_title_info(ti);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TITLE_PLAYLIST: {
        int title = *(double *)arg;
        if (!b->bd || title < 0 || title >= b->num_titles)
            return STREAM_UNSUPPORTED;
        BLURAY_TITLE_INFO *ti = bd_get_title_info(b->bd, title, 0);
        if (!ti)
            return STREAM_UNSUPPORTED;
        *(double *)arg = ti->playlist;
        bd_free_title_info(ti);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_LANG: {
        const BLURAY_TITLE_INFO *ti = b->title_info;
        if (ti && ti->clip_count) {
            struct stream_lang_req *req = arg;
            BLURAY_STREAM_INFO *si = NULL;
            int count = 0;
            switch (req->type) {
            case STREAM_AUDIO:
                count = ti->clips[0].audio_stream_count;
                si = ti->clips[0].audio_streams;
                break;
            case STREAM_SUB:
                count = ti->clips[0].pg_stream_count;
                si = ti->clips[0].pg_streams;
                break;
            }
            for (int n = 0; n < count; n++) {
                BLURAY_STREAM_INFO *i = &si[n];
                if (i->pid == req->id) {
                    snprintf(req->name, sizeof(req->name), "%.4s", i->lang);
                    return STREAM_OK;
                }
            }
        }
        return STREAM_ERROR;
    }
    case STREAM_CTRL_GET_DISC_NAME: {
        const struct meta_dl *meta = bd_get_meta(b->bd);
        if (!meta || !meta->di_name || !meta->di_name[0])
            break;
        *(char**)arg = talloc_strdup(NULL, meta->di_name);
        return STREAM_OK;
    }
    case STREAM_CTRL_NAV_CMD: {
        if (!b->use_nav)
            return STREAM_UNSUPPORTED;
        mp_mutex_lock(&b->bd_lock);
        apply_nav_command(b, arg);
        mp_mutex_unlock(&b->bd_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NAV_STATE: {
        if (!b->use_nav)
            return STREAM_UNSUPPORTED;
        struct mp_nav_state_info *out = arg;
        mp_mutex_lock(&b->bd_lock);
        mp_mutex_lock(&b->nav_lock);
        *out = (struct mp_nav_state_info){
            // The IG plane being on screen counts as an active menu even if
            // BD_EVENT_MENU was not (yet) delivered, so callers can route arrow
            // keys to the menu instead of seeking.
            .menu_active = b->in_menu || b->ig_visible,
            .menu_transport = b->current_title == BLURAY_TITLE_TOP_MENU,
            .popup_available = b->popup_available,
            .mouse_over_button = b->mouse_over_button,
            .overlay_visible = b->overlay_visible,
            .wait_pending = b->wait_pending,
            .reset_id = b->reset_id,
            .still_seconds = b->still_length,
            .overlay_change_id = b->overlay_change_id,
            .authored_audio_pid = b->authored_audio_pid,
            .authored_audio_change_id = b->authored_audio_change_id,
        };
        mp_mutex_unlock(&b->nav_lock);
        mp_mutex_unlock(&b->bd_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NAV_OVERLAY: {
        if (!b->use_nav)
            return STREAM_UNSUPPORTED;
        struct mp_nav_overlay *out = arg;
        mp_mutex_lock(&b->bd_lock);
        mp_mutex_lock(&b->nav_lock);
        // Bitmaps, their generation id and the authored size are read together
        // so the consumer applies a self-consistent generation.
        out->imgs = b->pending_overlay; // transfer ownership
        b->pending_overlay = NULL;
        out->change_id = b->overlay_change_id;
        out->w = b->overlay_w;
        out->h = b->overlay_h;
        out->menu_active = b->in_menu || b->ig_visible;
        out->overlay_visible = b->overlay_visible;
        out->mouse_over_button = b->mouse_over_button;
        out->wait_for_video = b->pending_overlay_wait;
        out->present_pts = -1;
        mp_mutex_unlock(&b->nav_lock);
        mp_mutex_unlock(&b->bd_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NAV_RESET: {
        if (!b->use_nav)
            return STREAM_UNSUPPORTED;
        mp_mutex_lock(&b->nav_lock);
        bool pending = b->reset_pending;
        b->reset_pending = false;
        mp_mutex_unlock(&b->nav_lock);
        return pending ? STREAM_OK : STREAM_UNSUPPORTED;
    }
    case STREAM_CTRL_NAV_RESET_DONE: {
        if (!b->use_nav)
            return STREAM_UNSUPPORTED;
        mp_mutex_lock(&b->nav_lock);
        bool held = b->reset_hold;
        b->reset_hold = false;
        mp_mutex_unlock(&b->nav_lock);
        return held ? STREAM_OK : STREAM_UNSUPPORTED;
    }
    case STREAM_CTRL_NAV_WAIT_DONE: {
        if (!b->use_nav)
            return STREAM_UNSUPPORTED;
        mp_mutex_lock(&b->nav_lock);
        bool waiting = b->wait_pending;
        if (waiting)
            b->wait_release = true;
        mp_mutex_unlock(&b->nav_lock);
        return waiting ? STREAM_OK : STREAM_UNSUPPORTED;
    }
    case STREAM_CTRL_GET_NAV_WAIT: {
        if (!b->use_nav)
            return STREAM_UNSUPPORTED;
        mp_mutex_lock(&b->nav_lock);
        bool waiting = b->wait_pending, released = b->wait_release;
        mp_mutex_unlock(&b->nav_lock);
        if (arg)
            *(int *)arg = mp_nav_wait_phase(waiting, released);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NAV_SOUND: {
        if (!b->use_nav)
            return STREAM_UNSUPPORTED;
        struct mp_nav_sound_effect *out = arg;
        mp_mutex_lock(&b->nav_lock);
        if (b->sfx_count <= 0) {
            mp_mutex_unlock(&b->nav_lock);
            return STREAM_UNSUPPORTED; // nothing queued
        }
        struct bluray_pending_sfx *e = &b->sfx_queue[b->sfx_head];
        *out = (struct mp_nav_sound_effect){
            .samples = e->samples, // transfer ownership to the caller
            .num_frames = e->num_frames,
            .num_channels = e->num_channels,
        };
        e->samples = NULL;
        b->sfx_head = (b->sfx_head + 1) % BLURAY_MAX_PENDING_SFX;
        b->sfx_count--;
        mp_mutex_unlock(&b->nav_lock);
        return STREAM_OK;
    }
    default:
        break;
    }

    return STREAM_UNSUPPORTED;
}

static const char *aacs_strerr(int err)
{
    switch (err) {
    case AACS_ERROR_CORRUPTED_DISC: return "opening or reading of AACS files failed";
    case AACS_ERROR_NO_CONFIG:      return "missing config file";
    case AACS_ERROR_NO_PK:          return "no matching processing key";
    case AACS_ERROR_NO_CERT:        return "no valid certificate";
    case AACS_ERROR_CERT_REVOKED:   return "certificate has been revoked";
    case AACS_ERROR_MMC_OPEN:       return "MMC open failed (maybe no MMC drive?)";
    case AACS_ERROR_MMC_FAILURE:    return "MMC failed";
    case AACS_ERROR_NO_DK:          return "no matching device key";
    default:                        return "unknown error";
    }
}

static bool check_disc_info(stream_t *s)
{
    struct bluray_priv_s *b = s->priv;
    const BLURAY_DISC_INFO *info = bd_get_disc_info(b->bd);

    // check Blu-ray
    if (!info->bluray_detected) {
        MP_ERR(s, "Given stream is not a Blu-ray.\n");
        return false;
    }

    // check AACS
    if (info->aacs_detected) {
        if (!info->libaacs_detected) {
            MP_ERR(s, "AACS encryption detected but cannot find libaacs.\n");
            return false;
        }
        if (!info->aacs_handled) {
            MP_ERR(s, "AACS error: %s\n", aacs_strerr(info->aacs_error_code));
            return false;
        }
    }

    // check BD+
    if (info->bdplus_detected) {
        if (!info->libbdplus_detected) {
            MP_ERR(s, "BD+ encryption detected but cannot find libbdplus.\n");
            return false;
        }
        if (!info->bdplus_handled) {
            MP_ERR(s, "Cannot decrypt BD+ encryption.\n");
            return false;
        }
    }

    return true;
}

static void select_initial_title(stream_t *s, int title_guess) {
    struct bluray_priv_s *b = s->priv;

    if (b->cfg_title == BLURAY_PLAYLIST_TITLE) {
        if (!play_playlist(b, b->cfg_playlist))
            MP_WARN(s, "Couldn't start playlist '%05d'.\n", b->cfg_playlist);
        b->current_title = bd_get_current_title(b->bd);
    } else {
        int title = -1;
        if (b->cfg_title != BLURAY_DEFAULT_TITLE )
            title = b->cfg_title;
        else
            title = title_guess;
        if (title < 0)
            return;

        if (play_title(b, title))
            b->current_title = title;
        else {
            MP_WARN(s, "Couldn't start title '%d'.\n", title);
            b->current_title = bd_get_current_title(b->bd);
        }
    }
}

static int bluray_stream_open_internal(stream_t *s)
{
    struct bluray_priv_s *b = s->priv;

    struct m_config_cache *opts_cache =
        m_config_cache_alloc(s, s->global, &stream_bluray_conf);

    b->opts_cache = opts_cache;
    b->opts = opts_cache->opts;

    int ret = 0;
    char *device = NULL;
    /* find the requested device */
    if (b->cfg_device && b->cfg_device[0]) {
        device = b->cfg_device;
    } else if (b->opts->bluray_device && b->opts->bluray_device[0]) {
        device = b->opts->bluray_device;
    } else {
        device = DEFAULT_OPTICAL_DEVICE;
    }

    if (!device || !device[0]) {
        MP_ERR(s, "No Blu-ray device/location was specified ...\n");
        ret = STREAM_UNSUPPORTED;
        goto err;
    }

    if (!mp_msg_test(s->log, MSGL_DEBUG))
        bd_set_debug_mask(0);

    /* open device */
    char *device_tmp = mp_get_user_path(NULL, s->global, device);
    BLURAY *bd = bd_open(device_tmp, NULL);
    talloc_free(device_tmp);
    if (!bd) {
        MP_ERR(s, "Couldn't open Blu-ray device: %s\n", device);
        ret = STREAM_UNSUPPORTED;
        goto err;
    }
    b->bd = bd;

    if (!check_disc_info(s)) {
        ret = STREAM_UNSUPPORTED;
        goto err;
    }

    b->use_nav = b->cfg_title == BLURAY_MENU_TITLE;
    if (b->use_nav) {
        mp_mutex_init(&b->nav_lock);
        mp_mutex_init(&b->bd_lock);
        b->duration = -1;
        b->pool = mp_image_pool_new(b);
    }

    /* check for available titles on disc */
    b->num_titles = bd_get_titles(bd, TITLES_RELEVANT, 0);
    if (!b->num_titles) {
        MP_ERR(s, "Can't find any Blu-ray-compatible title here.\n");
        ret = STREAM_UNSUPPORTED;
        goto err;
    }

    MP_INFO(s, "List of available titles:\n");

    /* parse titles information */
    for (int i = 0; i < b->num_titles; i++) {
        /* the information we're accessing (duration, playlist, angle count)
         * doesn't depend on the angle */
        BLURAY_TITLE_INFO *ti = bd_get_title_info(bd, i, 0);
        if (!ti)
            continue;

        char *time = mp_format_time(ti->duration / 90000, false);
        MP_INFO(s, "idx: %3d duration: %s angles: %2d (playlist: %05d.mpls)\n",
                    i, time, ti->angle_count, ti->playlist);
        talloc_free(time);

        bd_free_title_info(ti);
    }

    // these should be set before any callback
    b->current_angle = -1;
    b->current_title = -1;
    b->current_playitem = -1;
    b->audio_stream_number = 0xff;
    b->published_audio_epoch = -1;
    b->published_audio_stream = -1;
    b->authored_audio_pid = -1;

    // initialize libbluray event queue
    bd_get_event(bd, NULL);

    if (b->use_nav) {
        bd_register_overlay_proc(bd, s, overlay_process);
        if (bd_play(bd)) {
        } else {
            // Authored navigation couldn't start (e.g. BD-J-only disc). Fall
            // back to direct main-title playback instead of failing the open.
            MP_WARN(s, "Couldn't start Blu-ray navigation; "
                       "falling back to direct title playback.\n");
            b->use_nav = false;
            bd_register_overlay_proc(bd, NULL, NULL);
            mp_mutex_destroy(&b->bd_lock);
            mp_mutex_destroy(&b->nav_lock);
            talloc_free(b->pool);
            b->pool = NULL;
            b->cfg_title = BLURAY_DEFAULT_TITLE;
            b->pending_chapter = -1;
            select_initial_title(s, bd_get_main_title(bd));
        }
    } else {
        select_initial_title(s, bd_get_main_title(bd));
    }

    if (!bd_select_angle(bd, b->opts->angle - 1))
        MP_WARN(s, "Couldn't select angle '%d'.\n", b->opts->angle - 1);

    b->current_angle = bd_get_current_angle(bd);

    s->fill_buffer = b->use_nav ? bdnav_stream_fill_buffer
                                : bluray_stream_fill_buffer;
    s->close       = bluray_stream_close;
    s->control     = bluray_stream_control;
    s->priv        = b;
    s->demuxer     = "+disc";

    MP_VERBOSE(s, "Blu-ray successfully opened.\n");

    return STREAM_OK;

err:
    bluray_stream_close(s);
    return ret;
}

static int bluray_stream_open(stream_t *s)
{
    struct bluray_priv_s *b = talloc_zero(s, struct bluray_priv_s);
    s->priv = b;

    bstr title, bdevice, rest = { .len = 0 };
    bstr_split_tok(bstr0(s->path), "/", &title, &bdevice);

    b->cfg_title = BLURAY_DEFAULT_TITLE;

    struct MPOpts *opts = mp_get_config_group(s, s->global, &mp_opt_root);
    int edition_id = opts->edition_id;
    talloc_free(opts);

    if (edition_id >= 0) {
        b->cfg_title = edition_id;
    } else if (bstr_equals0(title, "longest") || bstr_equals0(title, "first")) {
        b->cfg_title = BLURAY_DEFAULT_TITLE;
    } else if (bstr_equals0(title, "menu")) {
        b->cfg_title = BLURAY_MENU_TITLE;
    } else if (bstr_equals0(title, "mpls")) {
        bstr_split_tok(bdevice, "/", &title, &bdevice);
        long long pl = bstrtoll(title, &rest, 10);
        if (rest.len) {
            MP_ERR(s, "number expected: '%.*s'\n", BSTR_P(rest));
            return STREAM_ERROR;
        } else if (pl < 0 || 99999 < pl) {
            MP_ERR(s, "invalid playlist: '%.*s', must be in the range 0-99999\n",
                            BSTR_P(title));
            return STREAM_ERROR;
        }
        b->cfg_playlist = pl;
        b->cfg_title    = BLURAY_PLAYLIST_TITLE;
    } else if (title.len) {
        long long t = bstrtoll(title, &rest, 10);
        if (rest.len) {
            MP_ERR(s, "number expected: '%.*s'\n", BSTR_P(rest));
            return STREAM_ERROR;
        } else if (t < 0 || 99999 < t) {
            MP_ERR(s, "invalid title: '%.*s', must be in the range 0-99999\n",
                            BSTR_P(title));
            return STREAM_ERROR;
        }
        b->cfg_title = t;
    }

    b->cfg_device = bstrto0(b, bdevice);

    return bluray_stream_open_internal(s);
}

const stream_info_t stream_info_bluray = {
    .name = "bd",
    .open = bluray_stream_open,
    .protocols = (const char*const[]){ "bd", "br", "bluray", NULL },
    .stream_origin = STREAM_ORIGIN_UNSAFE,
};

static bool check_bdmv(const char *path)
{
    if (strcasecmp(mp_basename(path), "MovieObject.bdmv"))
        return false;

    FILE *temp = fopen(path, "rb");
    if (!temp)
        return false;

    char data[50];
    bool ret = false;

    if (fread(data, 50, 1, temp) == 1) {
        bstr bdata = {data, 50};
        ret = bstr_startswith0(bdata, "MOBJ0100") || // AVCHD
              bstr_startswith0(bdata, "MOBJ0200") || // Blu-ray
              bstr_startswith0(bdata, "MOBJ0300");   // UHD BD
    }

    fclose(temp);
    return ret;
}

// Destructively remove the current trailing path component.
static void remove_prefix(char *path)
{
    size_t len = strlen(path);
#if HAVE_DOS_PATHS
    const char *seps = "/\\";
#else
    const char *seps = "/";
#endif
    while (len > 0 && !strchr(seps, path[len - 1]))
        len--;
    while (len > 0 && strchr(seps, path[len - 1]))
        len--;
    path[len] = '\0';
}

static int bdmv_dir_stream_open(stream_t *stream)
{
    struct bluray_priv_s *priv = talloc_ptrtype(stream, priv);
    stream->priv = priv;
    struct MPOpts *opts = mp_get_config_group(NULL, stream->global, &mp_opt_root);
    *priv = (struct bluray_priv_s){
        .cfg_title = opts->edition_id >= 0 ? opts->edition_id : BLURAY_DEFAULT_TITLE,
    };
    talloc_free(opts);

    if (!stream->access_references)
        goto unsupported;

    char *path = mp_file_get_path(priv, bstr0(stream->url));
    if (!path)
        goto unsupported;

    // We allow the path to point to a directory containing BDMV/, a
    // directory containing MovieObject.bdmv, or that file itself.
    if (!check_bdmv(path)) {
        // On UNIX, just assume the filename has always this case.
        char *npath = mp_path_join(priv, path, "MovieObject.bdmv");
        if (!check_bdmv(npath)) {
            npath = mp_path_join(priv, path, "BDMV/MovieObject.bdmv");
            if (!check_bdmv(npath))
                goto unsupported;
        }
        path = npath;
    }

    // Go up by 2 levels.
    remove_prefix(path);
    remove_prefix(path);
    priv->cfg_device = path;
    if (strlen(priv->cfg_device) <= 1)
        goto unsupported;

    MP_INFO(stream, "BDMV detected. Redirecting to bluray://\n");
    return bluray_stream_open_internal(stream);

unsupported:
    talloc_free(priv);
    stream->priv = NULL;
    return STREAM_UNSUPPORTED;
}

const stream_info_t stream_info_bdmv_dir = {
    .name = "bdmv/bluray",
    .open = bdmv_dir_stream_open,
    .protocols = (const char*const[]){ "file", "", NULL },
    .stream_origin = STREAM_ORIGIN_UNSAFE,
};
