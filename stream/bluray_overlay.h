/*
 * Pure helpers to convert libbluray HDMV/PG overlay data into premultiplied
 * BGRA pixels. Kept free of any libbluray dependency so the logic can be unit
 * tested in isolation (see test/bluray_overlay.c).
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

#ifndef MP_BLURAY_OVERLAY_H
#define MP_BLURAY_OVERLAY_H

#include <stdint.h>

// Layout-compatible with libbluray's BD_PG_PALETTE_ENTRY.
struct mp_bd_palette_entry {
    uint8_t Y;
    uint8_t Cr;
    uint8_t Cb;
    uint8_t T; // transparency: 0 transparent, 255 opaque
};

// Layout-compatible with libbluray's BD_PG_RLE_ELEM.
struct mp_bd_rle_elem {
    uint16_t len;
    uint16_t color;
};

#define MPCLAMP_U8(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

// Convert a single YCrCbT palette entry into a premultiplied BGRA pixel, laid
// out for IMGFMT_BGRA (native-endian 0xAARRGGBB, i.e. bytes B,G,R,A in memory).
// Uses the same fast (slightly inaccurate) BT.601-ish coefficients mpv shipped
// historically for disc overlays.
static inline uint32_t mp_bd_palette_to_bgra(const struct mp_bd_palette_entry *p)
{
    if (!p->T)
        return 0;
    const int y = p->Y, cb = (int)p->Cb - 128, cr = (int)p->Cr - 128;
    int r = y + cr + (cr >> 2) + (cr >> 3) + (cr >> 5);
    int g = y - ((cb >> 2) + (cb >> 4) + (cb >> 5))
              - ((cr >> 3) + (cr >> 4) + (cr >> 5));
    int b = y + cb + (cb >> 1) + (cb >> 2) + (cb >> 6);
    // Premultiply by alpha.
    r = MPCLAMP_U8(r) * p->T >> 8;
    g = MPCLAMP_U8(g) * p->T >> 8;
    b = MPCLAMP_U8(b) * p->T >> 8;
    return ((uint32_t)p->T << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | (uint32_t)b;
}

// Decode an RLE-compressed HDMV overlay region into a BGRA plane buffer.
//   dst          top-left of the target region inside a larger BGRA buffer
//   dst_stride   stride of that buffer, in pixels (not bytes)
//   w, h         region size
//   palette      256 palette entries
//   rle          RLE element stream (runs left-to-right, top-to-bottom)
//
// libbluray emits the decoded object as a stream of (len, color) runs where a
// run with len == 0 is an end-of-line marker: the pixel runs of a line sum to
// exactly `w`, followed by a (0, 0) element that terminates the line. The
// end-of-line marker must advance to the next line, NOT be expanded as a run of
// palette[0] (which would fill every other line with opaque colour 0 and show up
// as gray boxes once the overlay is scaled for display).
//
// Rows are written fully (transparent runs included) so the region is
// overwritten. This mirrors libbluray's BD_OVERLAY_DRAW semantics.
static inline void mp_bd_decode_rle(uint32_t *dst, int dst_stride, int w, int h,
                                    const struct mp_bd_palette_entry *palette,
                                    const struct mp_bd_rle_elem *rle)
{
    const struct mp_bd_rle_elem *in = rle;
    int x = 0, y = 0;
    while (y < h) {
        int len = in->len;
        int color = in->color;
        ++in;

        if (len == 0) {
            // End-of-line marker: finish the current line (only if it had
            // content, so a marker following a line already filled to `w` is a
            // harmless no-op).
            if (x > 0) {
                x = 0;
                y++;
            }
            continue;
        }

        if (x + len > w)
            len = w - x; // clamp malformed/overlong runs to the line
        if (len > 0) {
            uint32_t c = mp_bd_palette_to_bgra(&palette[color]);
            uint32_t *out = dst + (size_t)dst_stride * y + x;
            for (int i = 0; i < len; i++)
                out[i] = c;
            x += len;
        }

        if (x >= w) {
            // Line filled by runs without a trailing marker: wrap implicitly.
            x = 0;
            y++;
        }
    }
}

#endif // MP_BLURAY_OVERLAY_H
