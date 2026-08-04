/*
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

#include "config.h"

#if !HAVE_GPL
#error GPL only
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#ifdef __linux__
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#endif

#include <dvdnav/dvdnav.h>
#include <libavutil/common.h>
#include <libavutil/intreadwrite.h>

#include "osdep/io.h"
#include "osdep/threads.h"

#include "mpv_talloc.h"
#include "common/common.h"
#include "options/options.h"
#include "common/msg.h"
#include "input/input.h"
#include "misc/thread_tools.h"
#include "options/m_config.h"
#include "options/path.h"
#include "osdep/timer.h"
#include "stream.h"
#include "discnav.h"
#include "dvdnav_overlay.h"
#include "sub/osd.h"
#include "video/mp_image.h"
#include "demux/demux.h"
#include "video/out/vo.h"

#define TITLE_MENU -1
#define TITLE_LONGEST -2

// Poll interval while parked on a still frame, to avoid busy-spinning (ns).
#define DVD_STILL_POLL_NS (20 * 1000 * 1000)

struct priv {
    dvdnav_t *dvdnav;                   // handle to libdvdnav stuff
    char *filename;                     // path
    unsigned int duration;              // in milliseconds
    int mousex, mousey;
    int title;
    uint32_t spu_clut[16];
    bool spu_clut_valid;
    bool had_initial_vts;

    int dvd_speed;

    int track;
    char *device;

    // Disc menu navigation (menu mode only). Direct title playback leaves all
    // of the following untouched. The live dvdnav_t is never recreated across
    // First Play/VMGM/VTS/cell/hop transitions.
    bool use_nav;

    // Read-thread-only decode state (touched only inside fill_buffer).
    uint8_t *spu_accum;      // in-progress subpicture unit (talloc)
    int spu_accum_len;
    int spu_want;            // target size of the unit being assembled
    int spu_sub;            // substream id of the unit being assembled
    int spu_stream;          // active menu subpicture substream (0x20+phys, -1 any)
    struct mp_dvdspu spu;    // last fully decoded subpicture (spu.idx malloc'd)
    bool spu_valid;
    bool overlay_dirty;      // overlay needs to be rebuilt/published
    bool skip_still;         // a user action asked to leave the current still
    bool on_still;           // currently parked on a STILL_FRAME (arm skip scope)
    bool activated;          // draw the selected button in its "action" colors
    int cur_domain;          // tracked DVD domain, for demux re-sync detection
    int video_w, video_h;    // authored overlay-plane (video) resolution

    // Shared with the player thread; protected by nav_lock.
    mp_mutex nav_lock;
    struct sub_bitmaps *pending_overlay; // owned; handed to the player on demand
    int overlay_change_id;
    int overlay_w, overlay_h;
    bool overlay_visible;
    bool menu_active;
    bool mouse_over_button;
    uint32_t uo_mask;
    int still_length;        // 0: none, -1: infinite, >0: seconds
    bool reset_pending;      // nested demuxer should re-sync
    bool wait_pending;       // parked at a DVDNAV_WAIT sync point
    bool wait_release;       // player signalled its pipeline has drained
    struct mp_nav_cmd *cmd_queue;
    int num_cmds;

    struct dvd_opts *opts;
};

struct dvd_opts {
    int angle;
    int speed;
    char *device;
};

#define OPT_BASE_STRUCT struct dvd_opts

const struct m_sub_options dvd_conf = {
    .opts = (const struct m_option[]){
        {"device", OPT_STRING(device), .flags = M_OPT_FILE},
        {"speed", OPT_INT(speed)},
        {"angle", OPT_INT(angle), M_RANGE(1, 99)},
        {0}
    },
    .size = sizeof(struct dvd_opts),
    .defaults = &(const struct dvd_opts){
        .angle = 1,
    },
};

#define DNE(e) [e] = # e
static const char *const mp_dvdnav_events[] = {
    DNE(DVDNAV_BLOCK_OK),
    DNE(DVDNAV_NOP),
    DNE(DVDNAV_STILL_FRAME),
    DNE(DVDNAV_SPU_STREAM_CHANGE),
    DNE(DVDNAV_AUDIO_STREAM_CHANGE),
    DNE(DVDNAV_VTS_CHANGE),
    DNE(DVDNAV_CELL_CHANGE),
    DNE(DVDNAV_NAV_PACKET),
    DNE(DVDNAV_STOP),
    DNE(DVDNAV_HIGHLIGHT),
    DNE(DVDNAV_SPU_CLUT_CHANGE),
    DNE(DVDNAV_HOP_CHANNEL),
    DNE(DVDNAV_WAIT),
};

#define LOOKUP_NAME(array, i) \
    (((i) >= 0 && (i) < MP_ARRAY_SIZE(array)) ? array[(i)] : "?")

static void dvd_set_speed(stream_t *stream, char *device, unsigned speed)
{
#if defined(__linux__) && defined(SG_IO) && defined(GPCMD_SET_STREAMING)
    int fd;
    unsigned char buffer[28];
    unsigned char cmd[12];
    struct sg_io_hdr sghdr;
    struct stat st;

    memset(&st, 0, sizeof(st));

    if (stat(device, &st) == -1) return;

    if (!S_ISBLK(st.st_mode)) return; /* not a block device */

    switch (speed) {
    case 0: /* don't touch speed setting */
        return;
    case -1: /* restore default value */
        MP_INFO(stream, "Restoring DVD speed... ");
        break;
    default: /* limit to <speed> KB/s */
        // speed < 100 is multiple of DVD single speed (1350KB/s)
        if (speed < 100)
            speed *= 1350;
        MP_INFO(stream, "Limiting DVD speed to %dKB/s... ", speed);
        break;
    }

    memset(&sghdr, 0, sizeof(sghdr));
    sghdr.interface_id = 'S';
    sghdr.timeout = 5000;
    sghdr.dxfer_direction = SG_DXFER_TO_DEV;
    sghdr.dxfer_len = sizeof(buffer);
    sghdr.dxferp = buffer;
    sghdr.cmd_len = sizeof(cmd);
    sghdr.cmdp = cmd;

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = GPCMD_SET_STREAMING;
    cmd[10] = sizeof(buffer);

    memset(buffer, 0, sizeof(buffer));
    /* first sector 0, last sector 0xffffffff */
    AV_WB32(buffer + 8, 0xffffffff);
    if (speed == -1)
        buffer[0] = 4; /* restore default */
    else {
        /* <speed> kilobyte */
        AV_WB32(buffer + 12, speed);
        AV_WB32(buffer + 20, speed);
    }
    /* 1 second */
    AV_WB16(buffer + 18, 1000);
    AV_WB16(buffer + 26, 1000);

    fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) {
        MP_INFO(stream, "Couldn't open DVD device for writing, changing DVD speed needs write access.\n");
        return;
    }

    if (ioctl(fd, SG_IO, &sghdr) < 0)
        MP_INFO(stream, "failed\n");
    else
        MP_INFO(stream, "successful\n");

    close(fd);
#endif
}

// Check if this is likely to be an .ifo or similar file.
static int dvd_probe(const char *path, const char *ext, const char *sig)
{
    if (!bstr_case_endswith(bstr0(path), bstr0(ext)))
        return false;

    FILE *temp = fopen(path, "rb");
    if (!temp)
        return false;

    bool r = false;

    char data[50];

    mp_assert(strlen(sig) <= sizeof(data));

    if (fread(data, 50, 1, temp) == 1) {
        if (memcmp(data, sig, strlen(sig)) == 0)
            r = true;
    }

    fclose(temp);
    return r;
}

/**
 * \brief mp_dvdnav_lang_from_aid() returns the language corresponding to audio id 'aid'
 * \param stream: - stream pointer
 * \param sid: physical subtitle id
 * \return 0 on error, otherwise language id
 */
static int mp_dvdnav_lang_from_aid(stream_t *stream, int aid)
{
    uint8_t lg;
    uint16_t lang;
    struct priv *priv = stream->priv;

    if (aid < 0)
        return 0;
    lg = dvdnav_get_audio_logical_stream(priv->dvdnav, aid & 0x7);
    if (lg == 0xff)
        return 0;
    lang = dvdnav_audio_stream_to_lang(priv->dvdnav, lg);
    if (lang == 0xffff)
        return 0;
    return lang;
}

/**
 * \brief mp_dvdnav_lang_from_sid() returns the language corresponding to subtitle id 'sid'
 * \param stream: - stream pointer
 * \param sid: physical subtitle id
 * \return 0 on error, otherwise language id
 */
static int mp_dvdnav_lang_from_sid(stream_t *stream, int sid)
{
    uint8_t k;
    uint16_t lang;
    struct priv *priv = stream->priv;
    if (sid < 0)
        return 0;
    for (k = 0; k < 32; k++)
        if (dvdnav_get_spu_logical_stream(priv->dvdnav, k) == sid)
            break;
    if (k == 32)
        return 0;
    lang = dvdnav_spu_stream_to_lang(priv->dvdnav, k);
    if (lang == 0xffff)
        return 0;
    return lang;
}

/**
 * \brief mp_dvdnav_number_of_subs() returns the count of available subtitles
 * \param stream: - stream pointer
 * \return 0 on error, something meaningful otherwise
 */
static int mp_dvdnav_number_of_subs(stream_t *stream)
{
    struct priv *priv = stream->priv;
    uint8_t lg, k, n = 0;

    for (k = 0; k < 32; k++) {
        lg = dvdnav_get_spu_logical_stream(priv->dvdnav, k);
        if (lg == 0xff)
            continue;
        if (lg >= n)
            n = lg + 1;
    }
    return n;
}

// ---- Disc menu navigation (menu mode only) -------------------------------

static void dvd_reset_spu(struct priv *p)
{
    p->spu_accum_len = 0;
    p->spu_want = 0;
    p->spu_sub = -1;
    mp_dvdspu_free(&p->spu);
    p->spu_valid = false;
    p->overlay_dirty = true;
}

// Accumulate subpicture (SPU) fragments demuxed out of the VOB into a whole
// unit and decode it. A PES with a PTS starts a new unit; continuation packets
// carry no PTS. Runs on the demuxer read thread only.
static void dvd_spu_accumulate(stream_t *s, int substream, bool has_pts,
                               const uint8_t *data, int len)
{
    struct priv *p = s->priv;
    // Only accumulate the active menu subpicture channel (blocker: ignore other
    // 0x20..0x3f substreams so aspect-specific/multi-SPU menus stay correct).
    if (!mp_dvd_spu_wanted(p->spu_stream, substream))
        return;

    if (has_pts) {
        p->spu_accum_len = 0;
        if (len < 2)
            return;
        int size = (data[0] << 8) | data[1];
        if (size < 4)
            return;
        p->spu_want = MPMIN(size, 128 * 1024);
        p->spu_sub = substream;
    } else {
        if (p->spu_accum_len == 0 || substream != p->spu_sub)
            return; // no unit start seen yet, or a different substream
    }

    int room = p->spu_want - p->spu_accum_len;
    int n = MPMIN(len, room);
    if (n <= 0)
        return;
    MP_TARRAY_GROW(p, p->spu_accum, p->spu_accum_len + n);
    memcpy(p->spu_accum + p->spu_accum_len, data, n);
    p->spu_accum_len += n;

    if (p->spu_accum_len >= p->spu_want) {
        struct mp_dvdspu decoded;
        if (mp_dvdspu_decode(p->spu_accum, p->spu_want, &decoded)) {
            mp_dvdspu_free(&p->spu);
            p->spu = decoded;
            p->spu_valid = true;
            p->overlay_dirty = true;
        }
        p->spu_accum_len = 0;
    }
}

// Peek at a DVD pack (2048-byte MPEG-2 program stream sector) and feed any
// subpicture payload to the SPU assembler. Read-only: the block still flows to
// the nested demuxer unchanged.
static void dvd_sniff_spu(stream_t *s, const uint8_t *buf)
{
    if (!(buf[0] == 0 && buf[1] == 0 && buf[2] == 1 && buf[3] == 0xBA))
        return; // not a pack header
    int pos = 14 + (buf[13] & 7); // pack header + stuffing
    while (pos + 6 <= 2048) {
        if (!(buf[pos] == 0 && buf[pos + 1] == 0 && buf[pos + 2] == 1))
            break;
        int sid = buf[pos + 3];
        int plen = (buf[pos + 4] << 8) | buf[pos + 5];
        if (sid == 0xBD && pos + 9 <= 2048 && (buf[pos + 6] & 0xC0) == 0x80) {
            bool has_pts = (buf[pos + 7] & 0x80) != 0;
            int hdrlen = buf[pos + 8];
            int data = pos + 9 + hdrlen;
            int end = pos + 6 + plen;
            if (end > 2048)
                end = 2048;
            if (data < end) {
                int substream = buf[data];
                dvd_spu_accumulate(s, substream, has_pts,
                                   buf + data + 1, end - (data + 1));
            }
        }
        if (plen <= 0)
            break;
        pos += 6 + plen;
    }
}

// Build a single-part BGRA overlay from the last decoded subpicture, recoloring
// the currently selected/activated button inside its PCI crop rectangle.
static struct sub_bitmaps *dvd_build_overlay(stream_t *s, pci_t *pci)
{
    struct priv *p = s->priv;
    if (!p->spu_valid || !p->spu.idx || !p->spu_clut_valid)
        return NULL;
    struct mp_dvdspu *spu = &p->spu;

    struct mp_dvdspu_hl hl = {0};
    bool have_hl = false;
    int32_t button = 0;
    int btn_ns = pci->hli.hl_gi.btn_ns & 0x3f;
    dvdnav_get_current_highlight(p->dvdnav, &button);
    if (button > 0 && button <= btn_ns && button <= 36) {
        btni_t *b = &pci->hli.btnit[button - 1];
        hl.x1 = FFMIN(b->x_start, b->x_end);
        hl.x2 = FFMAX(b->x_start, b->x_end);
        hl.y1 = FFMIN(b->y_start, b->y_end);
        hl.y2 = FFMAX(b->y_start, b->y_end);
        int coln = b->btn_coln;
        if (coln >= 1 && coln <= 3) {
            hl.color = pci->hli.btn_colit.btn_coli[coln - 1][p->activated ? 1 : 0];
            have_hl = true;
        }
    }

    struct mp_image *packed = mp_image_alloc(IMGFMT_BGRA, spu->w, spu->h);
    if (!packed)
        return NULL;
    mp_image_clear(packed, 0, 0, spu->w, spu->h);
    mp_dvdspu_render_bgra(spu, p->spu_clut, have_hl ? &hl : NULL,
                          (uint32_t *)packed->planes[0], packed->stride[0] / 4);

    struct sub_bitmaps *res = talloc_zero(NULL, struct sub_bitmaps);
    res->format = SUBBITMAP_BGRA;
    res->packed = talloc_steal(res, packed);
    res->packed_w = spu->w;
    res->packed_h = spu->h;
    res->parts = talloc_array(res, struct sub_bitmap, 1);
    res->parts[0] = (struct sub_bitmap){
        .bitmap = packed->planes[0],
        .stride = packed->stride[0],
        .w = spu->w, .h = spu->h,
        .dw = spu->w, .dh = spu->h,
        .x = spu->x, .y = spu->y,
        .src_x = 0, .src_y = 0,
    };
    res->num_parts = 1;
    return res;
}

static bool dvd_in_menu_domain(dvdnav_t *nav)
{
    return dvdnav_is_domain_fp(nav) > 0 || dvdnav_is_domain_vmgm(nav) > 0 ||
           dvdnav_is_domain_vtsm(nav) > 0;
}

// Rebuild the overlay/state snapshot and publish it to the player side. Bitmaps
// are built outside the lock; the change id, bitmaps, visibility and authored
// size are stored together under nav_lock so the consumer stays consistent.
static void dvd_publish_overlay(stream_t *s)
{
    struct priv *p = s->priv;
    dvdnav_t *nav = p->dvdnav;

    bool menu_domain = dvd_in_menu_domain(nav);
    pci_t *pci = dvdnav_get_current_nav_pci(nav);
    int btn_ns = (pci && menu_domain) ? (pci->hli.hl_gi.btn_ns & 0x3f) : 0;

    struct sub_bitmaps *imgs = NULL;
    bool over_button = false;
    uint32_t uo = 0;
    if (menu_domain && btn_ns > 0) {
        imgs = dvd_build_overlay(s, pci);
        int32_t button = 0;
        dvdnav_get_current_highlight(nav, &button);
        over_button = button > 0;
        // Surface the authored UOP restrictions the player may care about.
        user_ops_t ops = pci->pci_gi.vobu_uop_ctl;
        if (ops.button_select_or_activate) uo |= MP_NAV_UO_BUTTON;
        if (ops.title_menu_call || ops.root_menu_call) uo |= MP_NAV_UO_MENU;
        if (ops.resume) uo |= MP_NAV_UO_RESUME;
    }

    uint32_t w = p->video_w > 0 ? p->video_w : 720;
    uint32_t h = p->video_h > 0 ? p->video_h : 480;

    mp_mutex_lock(&p->nav_lock);
    int id = ++p->overlay_change_id;
    if (imgs)
        imgs->change_id = id;
    talloc_free(p->pending_overlay);
    p->pending_overlay = imgs;
    p->overlay_visible = imgs != NULL;
    p->menu_active = mp_dvd_menu_active(menu_domain, btn_ns);
    p->mouse_over_button = over_button;
    p->uo_mask = uo;
    p->overlay_w = w;
    p->overlay_h = h;
    mp_mutex_unlock(&p->nav_lock);
}

// Apply a single navigation command. Runs on the demuxer read thread so that
// the dvdnav_t VM is only ever touched from one thread.
static void dvd_apply_nav_command(stream_t *s, struct mp_nav_cmd *cmd)
{
    struct priv *p = s->priv;
    dvdnav_t *nav = p->dvdnav;
    pci_t *pci = dvdnav_get_current_nav_pci(nav);
    if (!pci)
        return;
    user_ops_t uo = pci->pci_gi.vobu_uop_ctl;

    switch (cmd->action) {
    case MP_NAV_ACTION_UP:
        if (!uo.button_select_or_activate)
            dvdnav_upper_button_select(nav, pci);
        p->overlay_dirty = true;
        break;
    case MP_NAV_ACTION_DOWN:
        if (!uo.button_select_or_activate)
            dvdnav_lower_button_select(nav, pci);
        p->overlay_dirty = true;
        break;
    case MP_NAV_ACTION_LEFT:
        if (!uo.button_select_or_activate)
            dvdnav_left_button_select(nav, pci);
        p->overlay_dirty = true;
        break;
    case MP_NAV_ACTION_RIGHT:
        if (!uo.button_select_or_activate)
            dvdnav_right_button_select(nav, pci);
        p->overlay_dirty = true;
        break;
    case MP_NAV_ACTION_SELECT:
        // Only leave the authored still if the button was actually activated,
        // and only arm the skip when we are currently parked on a still (so a
        // success on a motion menu can't leak into a later unrelated still).
        if (!uo.button_select_or_activate &&
            dvdnav_button_activate(nav, pci) == DVDNAV_STATUS_OK) {
            p->activated = true;
            if (p->on_still)
                p->skip_still = true;
        }
        p->overlay_dirty = true;
        break;
    case MP_NAV_ACTION_MENU: {
        dvdnav_status_t st = DVDNAV_STATUS_ERR;
        if (!uo.root_menu_call)
            st = dvdnav_menu_call(nav, DVD_MENU_Root);
        if (st != DVDNAV_STATUS_OK && !uo.title_menu_call)
            st = dvdnav_menu_call(nav, DVD_MENU_Title);
        if (st == DVDNAV_STATUS_OK && p->on_still)
            p->skip_still = true;
        p->overlay_dirty = true;
        break;
    }
    case MP_NAV_ACTION_POPUP:
        // DVD has no popup menu; treat it like opening the root menu.
        if (!uo.root_menu_call &&
            dvdnav_menu_call(nav, DVD_MENU_Root) == DVDNAV_STATUS_OK && p->on_still)
            p->skip_still = true;
        p->overlay_dirty = true;
        break;
    case MP_NAV_ACTION_MOUSE_MOVE: {
        p->mousex = cmd->x;
        p->mousey = cmd->y;
        dvdnav_status_t st = dvdnav_mouse_select(nav, pci, cmd->x, cmd->y);
        mp_mutex_lock(&p->nav_lock);
        p->mouse_over_button = st == DVDNAV_STATUS_OK;
        mp_mutex_unlock(&p->nav_lock);
        p->overlay_dirty = true;
        break;
    }
    case MP_NAV_ACTION_MOUSE_CLICK:
        p->mousex = cmd->x;
        p->mousey = cmd->y;
        // A click outside any button must not skip the authored still:
        // dvdnav_mouse_activate() only returns OK when it hit a button.
        if (!uo.button_select_or_activate &&
            dvdnav_mouse_activate(nav, pci, cmd->x, cmd->y) == DVDNAV_STATUS_OK) {
            p->activated = true;
            if (p->on_still)
                p->skip_still = true;
        }
        p->overlay_dirty = true;
        break;
    case MP_NAV_ACTION_RESUME:
        // Leave the menu / resume playback where possible.
        if (!uo.resume &&
            dvdnav_menu_call(nav, DVD_MENU_Escape) == DVDNAV_STATUS_OK && p->on_still)
            p->skip_still = true;
        p->overlay_dirty = true;
        break;
    default:
        break;
    }
}

// Drain and apply all queued navigation commands (demuxer read thread).
static void dvd_drain_nav_commands(stream_t *s)
{
    struct priv *p = s->priv;
    mp_mutex_lock(&p->nav_lock);
    struct mp_nav_cmd *queue = p->cmd_queue;
    int num = p->num_cmds;
    p->cmd_queue = NULL;
    p->num_cmds = 0;
    mp_mutex_unlock(&p->nav_lock);

    for (int i = 0; i < num; i++)
        dvd_apply_nav_command(s, &queue[i]);
    talloc_free(queue);
}

static void dvd_update_video_res(struct priv *p)
{
    uint32_t w = 0, h = 0;
    if (dvdnav_get_video_resolution(p->dvdnav, &w, &h) == DVDNAV_STATUS_OK &&
        w > 0 && h > 0)
    {
        p->video_w = w;
        p->video_h = h;
    }
}

// Menu-mode read loop: keeps one live dvdnav_t, applies queued input, decodes
// the menu subpicture/highlight, and parks on stills without busy-spinning.
static int dvd_nav_fill_buffer(stream_t *s, void *buf, int max_len)
{
    struct priv *p = s->priv;
    dvdnav_t *nav = p->dvdnav;
    int64_t still_start = 0;

    for (;;) {
        dvd_drain_nav_commands(s);
        if (p->overlay_dirty) {
            dvd_publish_overlay(s);
            p->overlay_dirty = false;
        }

        // DVDNAV_WAIT drain handshake. On a WAIT we return 0 (below) instead of
        // parking this read thread, so the slave lavf flushes its buffered
        // packets and the player can present and drain them. We are only called
        // again here once demux_disc has cleared the slave's transient EOF,
        // which happens after the player signalled its pipeline drained. Perform
        // the skip on this thread (the VM is only ever touched here) and fall
        // through to read the post-WAIT block.
        mp_mutex_lock(&p->nav_lock);
        bool wait_pending = p->wait_pending;
        bool wait_release = p->wait_release;
        mp_mutex_unlock(&p->nav_lock);
        if (wait_pending) {
            if (!wait_release)
                return 0; // still draining (normally not reached: slave is EOF)
            dvdnav_wait_skip(nav);
            mp_mutex_lock(&p->nav_lock);
            p->wait_pending = false;
            p->wait_release = false;
            mp_mutex_unlock(&p->nav_lock);
            still_start = 0;
        }

        int len = -1, event = DVDNAV_NOP;
        if (dvdnav_get_next_block(nav, buf, &event, &len) != DVDNAV_STATUS_OK) {
            MP_ERR(s, "Error getting next block from DVD %d (%s)\n",
                   event, dvdnav_err_to_string(nav));
            return 0;
        }
        if (event != DVDNAV_BLOCK_OK) {
            const char *name = LOOKUP_NAME(mp_dvdnav_events, event);
            MP_TRACE(s, "DVDNAV: event %s (%d).\n", name, event);
        }

        // Track still-parked scope: any non-still event means we are no longer
        // waiting on a still, so a pending skip must not carry over to a later
        // unrelated still.
        if (event != DVDNAV_STILL_FRAME) {
            p->on_still = false;
            p->skip_still = false;
        }

        switch (event) {
        case DVDNAV_BLOCK_OK:
            dvd_sniff_spu(s, buf);
            if (p->overlay_dirty) {
                dvd_publish_overlay(s);
                p->overlay_dirty = false;
            }
            return len;
        case DVDNAV_STOP:
            return 0;
        case DVDNAV_NAV_PACKET:
            // A new PCI (buttons/UOP) is now current: refresh the overlay. The
            // nav pack itself is a private stream the demuxer ignores; don't
            // return it.
            p->overlay_dirty = true;
            still_start = 0;
            break;
        case DVDNAV_STILL_FRAME: {
            dvdnav_still_event_t *ev = (dvdnav_still_event_t *)buf;
            int length = ev->length; // 0xff => infinite; 0 => no wait
            bool infinite = length == 0xff;
            mp_mutex_lock(&p->nav_lock);
            p->still_length = mp_dvd_still_seconds(length); // 0 none, -1 inf, >0 s
            mp_mutex_unlock(&p->nav_lock);
            if (p->overlay_dirty) {
                dvd_publish_overlay(s);
                p->overlay_dirty = false;
            }

            // A zero-length still must not park: skip it immediately. A user
            // action (activate/menu/resume that succeeded) also ends the wait.
            if (length == 0 || p->skip_still) {
                p->skip_still = false;
                p->on_still = false;
                dvdnav_still_skip(nav);
                mp_mutex_lock(&p->nav_lock);
                p->still_length = 0;
                mp_mutex_unlock(&p->nav_lock);
                still_start = 0;
                break;
            }
            if (!infinite) { // timed still (length > 0): wait it out, then skip
                if (!still_start)
                    still_start = mp_time_ns();
                if (mp_time_ns() - still_start >= (int64_t)length * 1000000000) {
                    dvdnav_still_skip(nav);
                    p->on_still = false;
                    mp_mutex_lock(&p->nav_lock);
                    p->still_length = 0;
                    mp_mutex_unlock(&p->nav_lock);
                    still_start = 0;
                    break;
                }
            }
            // Infinite (0xff) or a not-yet-elapsed timed still: we are parked on
            // this still now, so user actions may arm a skip for it. Poll without
            // busy-spinning until the time elapses or the user acts.
            p->on_still = true;
            if (s->cancel && mp_cancel_test(s->cancel))
                return 0;
            mp_sleep_ns(DVD_STILL_POLL_NS);
            break;
        }
        case DVDNAV_WAIT: {
            // libdvdnav pipeline sync point: it will not advance until the
            // player has presented everything queued up to this boundary. Do
            // NOT block the read thread here -- that would trap libavformat
            // mid-read and keep its already-parsed packets out of the player
            // cache, starving the VO (the "white screen / demux parked in
            // fill_buffer" failure). Instead publish any pending highlight, flag
            // the wait, and return 0 so the slave lavf flushes its buffered
            // packets. demux_disc keeps the demuxer alive (not EOF) while the
            // player drains, services menu input via STREAM_CTRL_GET_NAV_WAIT,
            // and clears the slave's transient EOF once the player releases us;
            // the actual dvdnav_wait_skip() then runs at the top of this loop.
            // The single live dvdnav_t is preserved and never seeked/reopened.
            if (p->overlay_dirty) {
                dvd_publish_overlay(s);
                p->overlay_dirty = false;
            }
            mp_mutex_lock(&p->nav_lock);
            p->wait_pending = true;
            p->wait_release = false;
            mp_mutex_unlock(&p->nav_lock);
            still_start = 0;
            return 0;
        }
        case DVDNAV_HIGHLIGHT:
            p->activated = false;
            p->overlay_dirty = true;
            still_start = 0;
            break;
        case DVDNAV_SPU_CLUT_CHANGE:
            memcpy(p->spu_clut, buf, 16 * sizeof(uint32_t));
            p->spu_clut_valid = true;
            p->overlay_dirty = true;
            break;
        case DVDNAV_SPU_STREAM_CHANGE: {
            // Record the active physical subpicture channel now (the event
            // payload aliases the block buffer and is only valid until the next
            // dvdnav_get_next_block()). Resolve the aspect-correct physical
            // stream via the API, falling back to the event's wide channel.
            dvdnav_spu_stream_change_event_t *ev =
                (dvdnav_spu_stream_change_event_t *)buf;
            int phys = dvdnav_get_active_spu_stream(nav);
            if (phys < 0)
                phys = ev->physical_wide;
            p->spu_stream = (phys >= 0 && phys <= 31) ? 0x20 + phys : -1;
            dvd_reset_spu(p); // stale half-built unit belongs to the old channel
            break;
        }
        case DVDNAV_AUDIO_STREAM_CHANGE:
            break;
        case DVDNAV_VTS_CHANGE: {
            dvd_update_video_res(p);
            p->cur_domain = dvdnav_is_domain_vts(nav) > 0 ? 3 :
                            dvdnav_is_domain_vtsm(nav) > 0 ? 2 :
                            dvdnav_is_domain_vmgm(nav) > 0 ? 1 : 0;
            dvd_reset_spu(p);
            p->spu_stream = -1; // unknown until the next SPU_STREAM_CHANGE
            p->activated = false;
            if (!p->had_initial_vts) {
                // dvdnav emits an initial VTS change before any data; don't ask
                // the nested demuxer to re-sync before it has even started.
                p->had_initial_vts = true;
            } else {
                // Any VTS boundary is an elementary-stream discontinuity.
                mp_mutex_lock(&p->nav_lock);
                p->reset_pending = true;
                mp_mutex_unlock(&p->nav_lock);
            }
            dvd_publish_overlay(s);
            p->overlay_dirty = false;
            still_start = 0;
            break;
        }
        case DVDNAV_CELL_CHANGE: {
            dvdnav_cell_change_event_t *ev = (dvdnav_cell_change_event_t *)buf;
            if (ev->pgc_length)
                p->duration = ev->pgc_length / 90;
            int new_domain = dvdnav_is_domain_vts(nav) > 0 ? 3 :
                             dvdnav_is_domain_vtsm(nav) > 0 ? 2 :
                             dvdnav_is_domain_vmgm(nav) > 0 ? 1 : 0;
            if (mp_dvd_domain_changed(p->cur_domain, new_domain)) {
                p->cur_domain = new_domain;
                dvd_reset_spu(p);
                p->activated = false;
                mp_mutex_lock(&p->nav_lock);
                p->reset_pending = true;
                mp_mutex_unlock(&p->nav_lock);
            }
            p->overlay_dirty = true;
            still_start = 0;
            break;
        }
        case DVDNAV_HOP_CHANNEL:
            dvd_reset_spu(p);
            p->spu_stream = -1; // resolved again on the next SPU_STREAM_CHANGE
            mp_mutex_lock(&p->nav_lock);
            p->reset_pending = true;
            mp_mutex_unlock(&p->nav_lock);
            still_start = 0;
            break;
        default:
            break;
        }
    }
    return 0;
}

static int fill_buffer(stream_t *s, void *buf, int max_len)
{
    struct priv *priv = s->priv;
    dvdnav_t *dvdnav = priv->dvdnav;

    if (max_len < 2048) {
        MP_FATAL(s, "Short read size. Data corruption will follow. Please "
                    "provide a patch.\n");
        return -1;
    }

    if (priv->use_nav)
        return dvd_nav_fill_buffer(s, buf, max_len);

    while (1) {
        int len = -1;
        int event = DVDNAV_NOP;
        if (dvdnav_get_next_block(dvdnav, buf, &event, &len) != DVDNAV_STATUS_OK)
        {
            MP_ERR(s, "Error getting next block from DVD %d (%s)\n",
                   event, dvdnav_err_to_string(dvdnav));
            return 0;
        }
        if (event != DVDNAV_BLOCK_OK) {
            const char *name = LOOKUP_NAME(mp_dvdnav_events, event);
            MP_TRACE(s, "DVDNAV: event %s (%d).\n", name, event);
        }
        switch (event) {
        case DVDNAV_BLOCK_OK:
            return len;
        case DVDNAV_STOP:
            return 0;
        case DVDNAV_NAV_PACKET: {
            pci_t *pnavpci = dvdnav_get_current_nav_pci(dvdnav);
            uint32_t start_pts = pnavpci->pci_gi.vobu_s_ptm;
            MP_TRACE(s, "start pts = %"PRIu32"\n", start_pts);
            break;
        }
        case DVDNAV_STILL_FRAME:
            dvdnav_still_skip(dvdnav);
            return 0;
        case DVDNAV_WAIT:
            dvdnav_wait_skip(dvdnav);
            return 0;
        case DVDNAV_HIGHLIGHT:
            break;
        case DVDNAV_VTS_CHANGE: {
            int tit = 0, part = 0;
            dvdnav_vts_change_event_t *vts_event =
                (dvdnav_vts_change_event_t *)s->buffer;
            MP_INFO(s, "DVDNAV, switched to title: %d\n",
                   vts_event->new_vtsN);
            if (!priv->had_initial_vts) {
                // dvdnav sends an initial VTS change before any data; don't
                // cause a blocking wait for the player, because the player in
                // turn can't initialize the demuxer without data.
                priv->had_initial_vts = true;
                break;
            }
            if (dvdnav_current_title_info(dvdnav, &tit, &part) == DVDNAV_STATUS_OK)
            {
                MP_VERBOSE(s, "DVDNAV, NEW TITLE %d\n", tit);
                if (priv->title > 0 && tit != priv->title)
                    MP_WARN(s, "Requested title not found\n");
            }
            break;
        }
        case DVDNAV_CELL_CHANGE: {
            dvdnav_cell_change_event_t *ev =  (dvdnav_cell_change_event_t *)buf;

            if (ev->pgc_length)
                priv->duration = ev->pgc_length / 90;

            break;
        }
        case DVDNAV_SPU_CLUT_CHANGE: {
            memcpy(priv->spu_clut, buf, 16 * sizeof(uint32_t));
            priv->spu_clut_valid = true;
            break;
        }
        }
    }
    return 0;
}

static int control(stream_t *stream, int cmd, void *arg)
{
    struct priv *priv = stream->priv;
    dvdnav_t *dvdnav = priv->dvdnav;
    int tit, part;

    switch (cmd) {
    case STREAM_CTRL_GET_NUM_CHAPTERS: {
        if (dvdnav_current_title_info(dvdnav, &tit, &part) != DVDNAV_STATUS_OK)
            break;
        if (dvdnav_get_number_of_parts(dvdnav, tit, &part) != DVDNAV_STATUS_OK)
            break;
        if (!part)
            break;
        *(unsigned int *)arg = part;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_CHAPTER_TIME: {
        double *ch = arg;
        int chapter = *ch;
        if (dvdnav_current_title_info(dvdnav, &tit, &part) != DVDNAV_STATUS_OK)
            break;
        uint64_t *parts = NULL, duration = 0;
        int n = dvdnav_describe_title_chapters(dvdnav, tit, &parts, &duration);
        if (!parts)
            break;
        if (chapter < 0 || chapter + 1 > n) {
            free(parts);
            break;
        }
        *ch = chapter > 0 ? parts[chapter - 1] / 90000.0 : 0;
        free(parts);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TIME_LENGTH: {
        if (priv->duration) {
            *(double *)arg = (double)priv->duration / 1000.0;
            return STREAM_OK;
        }
        break;
    }
    case STREAM_CTRL_GET_ASPECT_RATIO: {
        uint8_t ar = dvdnav_get_video_aspect(dvdnav);
        *(double *)arg = !ar ? 4.0 / 3.0 : 16.0 / 9.0;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_CURRENT_TIME: {
        double tm;
        tm = dvdnav_get_current_time(dvdnav) / 90000.0f;
        if (tm != -1) {
            *(double *)arg = tm;
            return STREAM_OK;
        }
        break;
    }
    case STREAM_CTRL_GET_NUM_TITLES: {
        int32_t num_titles = 0;
        if (dvdnav_get_number_of_titles(dvdnav, &num_titles) != DVDNAV_STATUS_OK)
            break;
        *((unsigned int*)arg)= num_titles;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_TITLE_LENGTH: {
        int t = *(double *)arg;
        int32_t num_titles = 0;
        if (dvdnav_get_number_of_titles(dvdnav, &num_titles) != DVDNAV_STATUS_OK)
            break;
        if (t < 0 || t >= num_titles)
            break;
        uint64_t duration = 0;
        uint64_t *parts = NULL;
        dvdnav_describe_title_chapters(dvdnav, t + 1, &parts, &duration);
        if (!parts)
            break;
        free(parts);
        *(double *)arg = duration / 90000.0;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_CURRENT_TITLE: {
        if (dvdnav_current_title_info(dvdnav, &tit, &part) != DVDNAV_STATUS_OK)
            break;
        *((unsigned int *) arg) = tit - 1;
        return STREAM_OK;
    }
    case STREAM_CTRL_SET_CURRENT_TITLE: {
        int title = *((unsigned int *) arg);
        if (dvdnav_title_play(priv->dvdnav, title + 1) != DVDNAV_STATUS_OK)
            break;
        stream_drop_buffers(stream);
        return STREAM_OK;
    }
    case STREAM_CTRL_SEEK_TO_TIME: {
        double *args = arg;
        double d = args[0]; // absolute target timestamp
        int flags = args[1]; // from SEEK_* flags (demux.h)
        if (flags & SEEK_HR)
            d -= 10; // fudge offset; it's a hack, because fuck libdvd*
        int64_t tm = (int64_t)(d * 90000);
        if (tm < 0)
            tm = 0;
        if (priv->duration && tm >= (int64_t)priv->duration * 90)
            tm = (int64_t)priv->duration * 90 - 1;
        uint32_t pos, len;
        if (dvdnav_get_position(dvdnav, &pos, &len) != DVDNAV_STATUS_OK)
            break;
        MP_VERBOSE(stream, "seek to PTS %f (%"PRId64")\n", d, tm);
        if (dvdnav_time_search(dvdnav, tm) != DVDNAV_STATUS_OK)
            break;
        stream_drop_buffers(stream);
        d = dvdnav_get_current_time(dvdnav) / 90000.0f;
        MP_VERBOSE(stream, "landed at: %f\n", d);
        if (dvdnav_get_position(dvdnav, &pos, &len) == DVDNAV_STATUS_OK)
            MP_VERBOSE(stream, "block: %lu\n", (unsigned long)pos);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NUM_ANGLES: {
        uint32_t curr, angles;
        if (dvdnav_get_angle_info(dvdnav, &curr, &angles) != DVDNAV_STATUS_OK)
            break;
        *(int *)arg = angles;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_ANGLE: {
        uint32_t curr, angles;
        if (dvdnav_get_angle_info(dvdnav, &curr, &angles) != DVDNAV_STATUS_OK)
            break;
        *(int *)arg = curr;
        return STREAM_OK;
    }
    case STREAM_CTRL_SET_ANGLE: {
        uint32_t curr, angles;
        int new_angle = *(int *)arg;
        if (dvdnav_get_angle_info(dvdnav, &curr, &angles) != DVDNAV_STATUS_OK)
            break;
        if (new_angle > angles || new_angle < 1)
            break;
        if (dvdnav_angle_change(dvdnav, new_angle) != DVDNAV_STATUS_OK)
            break;
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_LANG: {
        struct stream_lang_req *req = arg;
        int lang = 0;
        switch (req->type) {
        case STREAM_AUDIO:
            lang = mp_dvdnav_lang_from_aid(stream, req->id);
            break;
        case STREAM_SUB:
            lang = mp_dvdnav_lang_from_sid(stream, req->id);
            break;
        }
        if (!lang)
            break;
        snprintf(req->name, sizeof(req->name), "%c%c", lang >> 8, lang);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_DVD_INFO: {
        struct stream_dvd_info_req *req = arg;
        memset(req, 0, sizeof(*req));
        req->num_subs = mp_dvdnav_number_of_subs(stream);
        static_assert(sizeof(uint32_t) == sizeof(unsigned int), "");
        memcpy(req->palette, priv->spu_clut, sizeof(req->palette));
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_DISC_NAME: {
        const char *volume = NULL;
        if (dvdnav_get_title_string(dvdnav, &volume) != DVDNAV_STATUS_OK)
            break;
        if (!volume || !volume[0])
            break;
        *(char**)arg = talloc_strdup(NULL, volume);
        return STREAM_OK;
    }
    case STREAM_CTRL_NAV_CMD: {
        if (!priv->use_nav)
            return STREAM_UNSUPPORTED;
        struct mp_nav_cmd *in = arg;
        mp_mutex_lock(&priv->nav_lock);
        MP_TARRAY_APPEND(priv, priv->cmd_queue, priv->num_cmds, *in);
        mp_mutex_unlock(&priv->nav_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NAV_STATE: {
        if (!priv->use_nav)
            return STREAM_UNSUPPORTED;
        struct mp_nav_state_info *out = arg;
        mp_mutex_lock(&priv->nav_lock);
        *out = (struct mp_nav_state_info){
            .menu_active = priv->menu_active,
            .popup_available = false, // DVD has no Blu-ray-style popup menu
            .mouse_over_button = priv->mouse_over_button,
            .overlay_visible = priv->overlay_visible,
            .wait_pending = priv->wait_pending,
            .still_seconds = priv->still_length,
            .uo_mask = priv->uo_mask,
            .overlay_w = priv->overlay_w,
            .overlay_h = priv->overlay_h,
            .overlay_change_id = priv->overlay_change_id,
        };
        mp_mutex_unlock(&priv->nav_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NAV_OVERLAY: {
        if (!priv->use_nav)
            return STREAM_UNSUPPORTED;
        struct mp_nav_overlay *out = arg;
        mp_mutex_lock(&priv->nav_lock);
        out->imgs = priv->pending_overlay; // transfer ownership
        priv->pending_overlay = NULL;
        out->change_id = priv->overlay_change_id;
        out->w = priv->overlay_w;
        out->h = priv->overlay_h;
        mp_mutex_unlock(&priv->nav_lock);
        return STREAM_OK;
    }
    case STREAM_CTRL_GET_NAV_RESET: {
        if (!priv->use_nav)
            return STREAM_UNSUPPORTED;
        mp_mutex_lock(&priv->nav_lock);
        bool pending = priv->reset_pending;
        priv->reset_pending = false;
        mp_mutex_unlock(&priv->nav_lock);
        return pending ? STREAM_OK : STREAM_UNSUPPORTED;
    }
    case STREAM_CTRL_NAV_WAIT_DONE: {
        // The player reports that its pipeline has drained to the DVDNAV_WAIT
        // boundary. Just flag the release; the actual dvdnav_wait_skip() runs on
        // the demux read thread (top of the fill_buffer loop) so the VM is only
        // ever touched from one thread. Called from the player thread while the
        // demuxer is kept alive by demux_disc's WAIT handshake.
        if (!priv->use_nav)
            return STREAM_UNSUPPORTED;
        mp_mutex_lock(&priv->nav_lock);
        bool waiting = priv->wait_pending;
        if (waiting)
            priv->wait_release = true;
        mp_mutex_unlock(&priv->nav_lock);
        return waiting ? STREAM_OK : STREAM_UNSUPPORTED;
    }
    case STREAM_CTRL_GET_NAV_WAIT: {
        // Polled by demux_disc on the demux read thread while a DVDNAV_WAIT is
        // outstanding. Returns the wait phase (0 none / 1 waiting / 2 released)
        // and -- so a long WAIT menu still reacts to input -- applies any queued
        // navigation commands and republishes a dirty highlight overlay here, on
        // the same thread that owns the VM. Safe because the VM is only ever
        // touched from the demux read thread (fill_buffer or this poll), never
        // concurrently; the player only queues commands and reads state.
        if (!priv->use_nav)
            return STREAM_UNSUPPORTED;
        mp_mutex_lock(&priv->nav_lock);
        bool waiting = priv->wait_pending;
        bool released = priv->wait_release;
        mp_mutex_unlock(&priv->nav_lock);
        if (waiting) {
            dvd_drain_nav_commands(stream);
            if (priv->overlay_dirty) {
                dvd_publish_overlay(stream);
                priv->overlay_dirty = false;
            }
        }
        if (arg)
            *(int *)arg = mp_nav_wait_phase(waiting, released);
        return STREAM_OK;
    }
    }

    return STREAM_UNSUPPORTED;
}

static void stream_dvdnav_close(stream_t *s)
{
    struct priv *priv = s->priv;
    if (priv->use_nav) {
        talloc_free(priv->pending_overlay);
        priv->pending_overlay = NULL;
        talloc_free(priv->cmd_queue);
        priv->cmd_queue = NULL;
        priv->num_cmds = 0;
        mp_dvdspu_free(&priv->spu);
        mp_mutex_destroy(&priv->nav_lock);
        priv->use_nav = false;
    }
    if (priv->dvdnav)
        dvdnav_close(priv->dvdnav);
    priv->dvdnav = NULL;
    if (priv->dvd_speed)
        dvd_set_speed(s, priv->filename, -1);
}

static struct priv *new_dvdnav_stream(stream_t *stream, char *filename)
{
    struct priv *priv = stream->priv;
    const char *title_str;

    if (!filename)
        return NULL;

    if (!(priv->filename = mp_get_user_path(priv, stream->global, filename)))
        return NULL;

    priv->dvd_speed = priv->opts->speed;
    dvd_set_speed(stream, priv->filename, priv->dvd_speed);

    if (dvdnav_open(&(priv->dvdnav), priv->filename) != DVDNAV_STATUS_OK)
        return NULL;

    if (!priv->dvdnav)
        return NULL;

    dvdnav_set_readahead_flag(priv->dvdnav, 1);
    if (dvdnav_set_PGC_positioning_flag(priv->dvdnav, 1) != DVDNAV_STATUS_OK)
        MP_ERR(stream, "stream_dvdnav, failed to set PGC positioning\n");
    /* report the title?! */
    dvdnav_get_title_string(priv->dvdnav, &title_str);

    return priv;
}

static int open_s_internal(stream_t *stream)
{
    struct priv *priv, *p;
    priv = p = stream->priv;
    char *filename;
    int ret = 0;

    p->opts = mp_get_config_group(stream, stream->global, &dvd_conf);

    if (p->device && p->device[0])
        filename = p->device;
    else if (p->opts->device && p->opts->device[0])
        filename = p->opts->device;
    else
        filename = DEFAULT_OPTICAL_DEVICE;
    if (!new_dvdnav_stream(stream, filename)) {
        MP_ERR(stream, "Couldn't open DVD device: %s\n",
                filename);
        ret = STREAM_ERROR;
        goto err;
    }

    if (p->track == TITLE_LONGEST) { // longest
        dvdnav_t *dvdnav = priv->dvdnav;
        uint64_t best_length = 0;
        int best_title = -1;
        int32_t num_titles;
        if (dvdnav_get_number_of_titles(dvdnav, &num_titles) == DVDNAV_STATUS_OK) {
            MP_VERBOSE(stream, "List of available titles:\n");
            for (int n = 1; n <= num_titles; n++) {
                uint64_t *parts = NULL, duration = 0;
                dvdnav_describe_title_chapters(dvdnav, n, &parts, &duration);
                if (parts) {
                    if (duration > best_length) {
                        best_length = duration;
                        best_title = n;
                    }
                    if (duration > 90000) { // arbitrarily ignore <1s titles
                        char *time = mp_format_time(duration / 90000, false);
                        MP_VERBOSE(stream, "title: %3d duration: %s\n",
                                   n - 1, time);
                        talloc_free(time);
                    }
                    free(parts);
                }
            }
        }
        p->track = best_title - 1;
        MP_INFO(stream, "Selecting title %d.\n", p->track);
    }

    if (p->track >= 0) {
        priv->title = p->track;
        if (dvdnav_title_play(priv->dvdnav, p->track + 1) != DVDNAV_STATUS_OK) {
            MP_FATAL(stream, "dvdnav_stream, couldn't select title %d, error '%s'\n",
                   p->track, dvdnav_err_to_string(priv->dvdnav));
            ret = STREAM_UNSUPPORTED;
            goto err;
        }
    } else {
        // Menu mode: keep one live dvdnav_t and let the disc's First Play
        // program run (typically into the VMGM/root menu). The VM is never
        // recreated across menu/title transitions.
        priv->use_nav = true;
        priv->cur_domain = -1;
        priv->spu_sub = -1;
        priv->spu_stream = -1;
        mp_mutex_init(&priv->nav_lock);
        dvd_update_video_res(priv);
        MP_VERBOSE(stream, "DVD menu navigation enabled\n");
    }
    if (p->opts->angle > 1)
        dvdnav_angle_change(priv->dvdnav, p->opts->angle);

    stream->fill_buffer = fill_buffer;
    stream->control = control;
    stream->close = stream_dvdnav_close;
    stream->demuxer = "+disc";
    stream->lavf_type = "mpeg";

    return STREAM_OK;

err:
    stream_dvdnav_close(stream);
    return ret;
}

static int open_s(stream_t *stream)
{
    struct priv *priv = talloc_zero(stream, struct priv);
    stream->priv = priv;

    bstr title, bdevice;
    bstr_split_tok(bstr0(stream->path), "/", &title, &bdevice);

    priv->track = TITLE_LONGEST;

    struct MPOpts *opts = mp_get_config_group(stream, stream->global, &mp_opt_root);
    int edition_id = opts->edition_id;
    talloc_free(opts);

    if (edition_id >= 0) {
        priv->track = edition_id;
    } else if (bstr_equals0(title, "longest") || bstr_equals0(title, "first")) {
        priv->track = TITLE_LONGEST;
    } else if (bstr_equals0(title, "menu")) {
        priv->track = TITLE_MENU;
    } else if (title.len) {
        bstr rest;
        priv->track = bstrtoll(title, &rest, 10);
        if (rest.len) {
            MP_ERR(stream, "number expected: '%.*s'\n", BSTR_P(rest));
            return STREAM_ERROR;
        }
    }

    priv->device = bstrto0(priv, bdevice);

    return open_s_internal(stream);
}

const stream_info_t stream_info_dvdnav = {
    .name = "dvdnav",
    .open = open_s,
    .protocols = (const char*const[]){ "dvd", "dvdnav", NULL },
    .stream_origin = STREAM_ORIGIN_UNSAFE,
};

static bool check_ifo(const char *path)
{
    if (strcasecmp(mp_basename(path), "video_ts.ifo"))
        return false;

    return dvd_probe(path, ".ifo", "DVDVIDEO-VMG");
}

static int ifo_dvdnav_stream_open(stream_t *stream)
{
    struct priv *priv = talloc_zero(stream, struct priv);
    stream->priv = priv;

    if (!stream->access_references)
        goto unsupported;

    struct MPOpts *opts = mp_get_config_group(NULL, stream->global, &mp_opt_root);
    priv->track = opts->edition_id >= 0 ? opts->edition_id : TITLE_LONGEST;
    talloc_free(opts);

    char *path = mp_file_get_path(priv, bstr0(stream->url));
    if (!path)
        goto unsupported;

    // We allow the path to point to a directory containing VIDEO_TS/, a
    // directory containing VIDEO_TS.IFO, or that file itself.
    if (!check_ifo(path)) {
        // On UNIX, just assume the filename is always uppercase.
        char *npath = mp_path_join(priv, path, "VIDEO_TS.IFO");
        if (!check_ifo(npath)) {
            npath = mp_path_join(priv, path, "VIDEO_TS/VIDEO_TS.IFO");
            if (!check_ifo(npath))
                goto unsupported;
        }
        path = npath;
    }

    priv->device = bstrto0(priv, mp_dirname(path));

    MP_INFO(stream, ".IFO detected. Redirecting to dvd://\n");
    return open_s_internal(stream);

unsupported:
    talloc_free(priv);
    stream->priv = NULL;
    return STREAM_UNSUPPORTED;
}

const stream_info_t stream_info_ifo_dvdnav = {
    .name = "ifo_dvdnav",
    .open = ifo_dvdnav_stream_open,
    .protocols = (const char*const[]){ "file", "", NULL },
    .stream_origin = STREAM_ORIGIN_UNSAFE,
};
