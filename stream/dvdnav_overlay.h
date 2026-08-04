/*
 * Pure helpers to turn a DVD subpicture (SPU) unit plus libdvdnav PCI button
 * information into premultiplied BGRA pixels, including the authored button
 * highlight. This is deliberately free of any libdvdnav/libmpv dependency so
 * the non-trivial decode/recolor/crop logic can be unit tested in isolation
 * (see test/dvdnav_overlay.c).
 *
 * DVD menu highlights are *not* opaque rectangles: the still menu already
 * carries a subpicture (the authored button graphics) whose 2-bit pixels index
 * a 4-entry sub-palette. The selected/activated button is drawn by re-mapping
 * those 4 entries to the button's PCI selection/action color+contrast word,
 * but only inside the button's crop rectangle. Everything else keeps the
 * subpicture's own colors. This mirrors the VLC/Kodi/libdvdnav reference
 * behavior and old MPlayer/mpv spudec, without the fake gray box.
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

#ifndef MP_DVDNAV_OVERLAY_H
#define MP_DVDNAV_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bluray_overlay.h" // for mp_bd_palette_to_bgra() (shared YUV->BGRA)

// A decoded DVD subpicture unit: a paletted (2-bit) bitmap plus its display
// area on the video plane and the 4-entry sub-palette (CLUT indices + 4-bit
// alpha) it references.
struct mp_dvdspu {
    int x, y;          // top-left of the display area, in video-plane pixels
    int w, h;          // display area size
    uint8_t pal[4];    // CLUT index for sub-colors bg/pattern/emph1/emph2
    uint8_t alpha[4];  // 4-bit alpha (0..15) for the same sub-colors
    uint8_t *idx;      // w*h bytes, each 0..3 (malloc'd; free with free())
};

// One authored highlight: a button crop rectangle (video-plane, inclusive) and
// the PCI color/contrast word to apply inside it. The word is packed by the DVD
// spec as [Ci3 Ci2 Ci1 Ci0 A3 A2 A1 A0] from MSB, i.e. for sub-color i the CLUT
// index is bits (16 + 4*i) and the 4-bit alpha is bits (4*i).
struct mp_dvdspu_hl {
    int x1, y1, x2, y2;
    uint32_t color; // btn_coli[color_number-1][selected:0 / activated:1]
};

static inline int mp_dvdspu_hl_color_index(uint32_t word, int sub)
{
    return (word >> (16 + 4 * sub)) & 0xf;
}

static inline int mp_dvdspu_hl_alpha(uint32_t word, int sub)
{
    return (word >> (4 * sub)) & 0xf;
}

// Convert a DVD CLUT entry (0x00YYCbCr, matching STREAM_CTRL_GET_DVD_INFO) and a
// 4-bit alpha into a premultiplied BGRA pixel, reusing the shared (Blu-ray)
// YUV->BGRA conversion so disc overlays look consistent.
static inline uint32_t mp_dvd_clut_to_bgra(uint32_t clut_entry, int alpha4)
{
    struct mp_bd_palette_entry p = {
        .Y  = (clut_entry >> 16) & 0xff,
        .Cb = (clut_entry >> 8) & 0xff,
        .Cr = clut_entry & 0xff,
        .T  = (uint8_t)((alpha4 & 0xf) * 17), // 0..15 -> 0..255
    };
    return mp_bd_palette_to_bgra(&p);
}

// --- SPU RLE bit reader (nibble granularity) -------------------------------

struct mp_dvdspu_reader {
    const uint8_t *data;
    int size;   // total bytes available
    int nib;    // current nibble index (2 per byte)
};

static inline int mp_dvdspu_get_nibble(struct mp_dvdspu_reader *r)
{
    if ((r->nib >> 1) >= r->size) {
        r->nib++;
        return 0; // reading past the end yields transparent background
    }
    int byte = r->data[r->nib >> 1];
    int val = (r->nib & 1) ? (byte & 0x0f) : (byte >> 4);
    r->nib++;
    return val;
}

static inline void mp_dvdspu_align_byte(struct mp_dvdspu_reader *r)
{
    if (r->nib & 1)
        r->nib++;
}

// Read one RLE run: 1..4 nibbles encode a (length<<2 | color) value; length 0
// means "run to the end of the current line".
static inline unsigned mp_dvdspu_get_rle(struct mp_dvdspu_reader *r)
{
    unsigned v = mp_dvdspu_get_nibble(r);
    if (v < 0x4) {
        v = (v << 4) | mp_dvdspu_get_nibble(r);
        if (v < 0x10) {
            v = (v << 4) | mp_dvdspu_get_nibble(r);
            if (v < 0x40)
                v = (v << 4) | mp_dvdspu_get_nibble(r);
        }
    }
    return v;
}

// Decode one interlaced RLE field into idx[]. The field fills lines
// first_line, first_line+2, ... of a w*h index plane.
static inline void mp_dvdspu_decode_field(const uint8_t *data, int size,
                                          int byte_off, int w, int h,
                                          int first_line, uint8_t *idx)
{
    struct mp_dvdspu_reader r = { .data = data, .size = size, .nib = byte_off * 2 };
    int line = first_line;
    while (line < h && (r.nib >> 1) <= size) {
        int x = 0;
        while (x < w) {
            unsigned v = mp_dvdspu_get_rle(&r);
            int len = v >> 2;
            int color = v & 0x3;
            if (len <= 0 || x + len > w)
                len = w - x; // 0 => rest of line; also clamp malformed runs
            memset(idx + (size_t)line * w + x, color, len);
            x += len;
        }
        line += 2;
        mp_dvdspu_align_byte(&r);
    }
}

// Decode a complete DVD subpicture unit into *out. Returns true and allocates
// out->idx on success. buf/len point at the whole SPU (first two bytes are the
// unit size, the next two the offset to the first display control sequence).
static inline bool mp_dvdspu_decode(const uint8_t *buf, int len,
                                    struct mp_dvdspu *out)
{
    memset(out, 0, sizeof(*out));
    if (!buf || len < 4)
        return false;

    int spu_size = (buf[0] << 8) | buf[1];
    if (spu_size > 0 && spu_size < len)
        len = spu_size; // don't read past the authored unit
    int dcsq = (buf[2] << 8) | buf[3];

    int x1 = 0, y1 = 0, x2 = -1, y2 = -1;
    int top_off = -1, bot_off = -1;
    uint8_t pal[4] = {0}, alpha[4] = {0};
    bool have_area = false, have_pixels = false;

    // Walk the display control sequences; keep the first one that actually
    // starts a display with pixel data (that's the visible menu subpicture).
    for (int guard = 0; guard < 64 && dcsq + 4 <= len; guard++) {
        int next = (buf[dcsq + 2] << 8) | buf[dcsq + 3];
        int p = dcsq + 4;
        bool done = false;
        while (p < len && !done) {
            int cmd = buf[p++];
            switch (cmd) {
            case 0x00: // FSTA_DSP (forced start)
            case 0x01: // STA_DSP (start display)
            case 0x02: // STP_DSP (stop display)
                break;
            case 0x03: // SET_COLOR
                if (p + 2 <= len) {
                    pal[3] = buf[p] >> 4;   pal[2] = buf[p] & 0xf;
                    pal[1] = buf[p+1] >> 4; pal[0] = buf[p+1] & 0xf;
                    p += 2;
                }
                break;
            case 0x04: // SET_CONTR (alpha)
                if (p + 2 <= len) {
                    alpha[3] = buf[p] >> 4;   alpha[2] = buf[p] & 0xf;
                    alpha[1] = buf[p+1] >> 4; alpha[0] = buf[p+1] & 0xf;
                    p += 2;
                }
                break;
            case 0x05: // SET_DAREA
                if (p + 6 <= len) {
                    x1 = (buf[p] << 4) | (buf[p+1] >> 4);
                    x2 = ((buf[p+1] & 0xf) << 8) | buf[p+2];
                    y1 = (buf[p+3] << 4) | (buf[p+4] >> 4);
                    y2 = ((buf[p+4] & 0xf) << 8) | buf[p+5];
                    have_area = true;
                    p += 6;
                }
                break;
            case 0x06: // SET_DSPXA (pixel data addresses)
                if (p + 4 <= len) {
                    top_off = (buf[p] << 8) | buf[p+1];
                    bot_off = (buf[p+2] << 8) | buf[p+3];
                    have_pixels = true;
                    p += 4;
                }
                break;
            case 0x07: // CHG_COLCON (per-line color/contrast) - skipped
                if (p + 2 <= len) {
                    int sz = (buf[p] << 8) | buf[p+1];
                    p += sz > 2 ? sz : 2;
                }
                break;
            case 0xff: // end of this control sequence
                done = true;
                break;
            default:
                done = true; // unknown command: stop parsing this sequence
                break;
            }
        }
        if (have_area && have_pixels)
            break;
        if (next <= dcsq || next + 4 > len)
            break;
        dcsq = next;
    }

    if (!have_area || !have_pixels)
        return false;

    int w = x2 - x1 + 1, h = y2 - y1 + 1;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return false;

    uint8_t *idx = calloc((size_t)w * h, 1);
    if (!idx)
        return false;

    if (top_off >= 0)
        mp_dvdspu_decode_field(buf, len, top_off, w, h, 0, idx);
    if (bot_off >= 0)
        mp_dvdspu_decode_field(buf, len, bot_off, w, h, 1, idx);

    out->x = x1;
    out->y = y1;
    out->w = w;
    out->h = h;
    memcpy(out->pal, pal, sizeof(pal));
    memcpy(out->alpha, alpha, sizeof(alpha));
    out->idx = idx;
    return true;
}

// Render a decoded subpicture into a caller-provided BGRA buffer, applying the
// authored highlight (may be NULL) only inside its crop rectangle. out is
// out_stride pixels wide and at least spu->h rows tall.
static inline void mp_dvdspu_render_bgra(const struct mp_dvdspu *spu,
                                         const uint32_t clut[16],
                                         const struct mp_dvdspu_hl *hl,
                                         uint32_t *out, int out_stride)
{
    for (int yy = 0; yy < spu->h; yy++) {
        uint32_t *row = out + (size_t)yy * out_stride;
        int ay = spu->y + yy;
        for (int xx = 0; xx < spu->w; xx++) {
            int i = spu->idx[(size_t)yy * spu->w + xx] & 3;
            int ax = spu->x + xx;
            int ci, a4;
            if (hl && ax >= hl->x1 && ax <= hl->x2 && ay >= hl->y1 && ay <= hl->y2) {
                ci = mp_dvdspu_hl_color_index(hl->color, i);
                a4 = mp_dvdspu_hl_alpha(hl->color, i);
            } else {
                ci = spu->pal[i];
                a4 = spu->alpha[i];
            }
            row[xx] = mp_dvd_clut_to_bgra(clut[ci & 0xf], a4);
        }
    }
}

static inline void mp_dvdspu_free(struct mp_dvdspu *spu)
{
    free(spu->idx);
    spu->idx = NULL;
}

// --- Small state predicates (kept pure so they can be unit tested) ----------

// A DVD menu is considered active only when the VM is in a menu domain (First
// Play, VMGM or VTSM) and the current PCI advertises at least one button.
static inline bool mp_dvd_menu_active(bool in_menu_domain, int btn_ns)
{
    return in_menu_domain && btn_ns > 0;
}

// The nested demuxer must re-sync (drop buffers) whenever the DVD VM crosses a
// domain boundary (e.g. VMGM menu -> title playback), because the elementary
// stream layout/timebase is discontinuous there. The live dvdnav_t is kept.
static inline bool mp_dvd_domain_changed(int old_domain, int new_domain)
{
    return old_domain != new_domain;
}

// Classify a DVDNAV_STILL_FRAME length. Only 0xff is an infinite still (park
// until the user acts); a length of 0 means "no wait" (skip immediately); any
// other value is a timed still in seconds. Returns -1 for infinite, otherwise
// the length itself (0 = none).
static inline int mp_dvd_still_seconds(int length)
{
    return length == 0xff ? -1 : length;
}

// Whether a demuxed private-stream-1 substream should be accumulated as the menu
// subpicture. Only subpicture substreams (0x20..0x3f) qualify, and once the
// active menu channel is known (>= 0) all other channels are ignored so
// aspect-specific/multi-SPU menus render the selected channel only.
static inline bool mp_dvd_spu_wanted(int want_substream, int substream)
{
    if (substream < 0x20 || substream > 0x3f)
        return false;
    return want_substream < 0 || substream == want_substream;
}

#endif // MP_DVDNAV_OVERLAY_H
