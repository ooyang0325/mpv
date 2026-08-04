/*
 * Regression tests for the Blu-ray HDMV overlay pixel logic
 * (stream/bluray_overlay.h): YCrCbT palette -> premultiplied BGRA conversion
 * and RLE region decoding. These are the non-trivial pure routines that turn
 * libbluray overlay data into what mpv renders.
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

#include "test_utils.h"
#include "stream/bluray_overlay.h"

static uint32_t conv(uint8_t y, uint8_t cr, uint8_t cb, uint8_t t)
{
    struct mp_bd_palette_entry p = { .Y = y, .Cr = cr, .Cb = cb, .T = t };
    return mp_bd_palette_to_bgra(&p);
}

static void test_palette(void)
{
    // Fully transparent entries are always zero (nothing premultiplied).
    assert_int_equal(conv(235, 128, 128, 0), 0);
    assert_int_equal(conv(0, 0, 0, 0), 0);

    // Neutral chroma (Cr=Cb=128) => grayscale, so the result is deterministic
    // regardless of the exact matrix coefficients. R==G==B, alpha in the top
    // byte, and the color is premultiplied by alpha.
    uint32_t white = conv(235, 128, 128, 255);
    uint8_t a = white >> 24, r = white >> 16, g = white >> 8, b = white;
    assert_int_equal(a, 255);
    assert_int_equal(r, 234); // 235 * 255 >> 8
    assert_int_equal(g, 234);
    assert_int_equal(b, 234);

    // Half alpha halves the premultiplied color and sets alpha to 128.
    uint32_t half = conv(235, 128, 128, 128);
    assert_int_equal((uint8_t)(half >> 24), 128);
    assert_int_equal((uint8_t)(half >> 16), 117); // 235 * 128 >> 8
    assert_int_equal((uint8_t)(half >> 8), 117);
    assert_int_equal((uint8_t)half, 117);

    // Black opaque is opaque but zero color.
    uint32_t black = conv(0, 128, 128, 255);
    assert_int_equal((uint8_t)(black >> 24), 255);
    assert_int_equal((uint8_t)(black >> 16), 0);
}

static void test_rle_decode(void)
{
    struct mp_bd_palette_entry pal[256] = {0};
    pal[0] = (struct mp_bd_palette_entry){0, 128, 128, 0};     // transparent
    pal[1] = (struct mp_bd_palette_entry){235, 128, 128, 255}; // opaque white
    uint32_t white = conv(235, 128, 128, 255);

    // 3x2 region drawn into a 4-wide buffer (stride 4 pixels) so we also verify
    // the region is written at the right stride and the padding is untouched.
    const int stride = 4;
    uint32_t buf[4 * 2];
    for (int i = 0; i < 4 * 2; i++)
        buf[i] = 0xDEADBEEF;

    // Each row: one white pixel then two transparent pixels.
    struct mp_bd_rle_elem rle[] = {
        {1, 1}, {2, 0}, // row 0
        {1, 1}, {2, 0}, // row 1
    };
    mp_bd_decode_rle(buf, stride, 3, 2, pal, rle);

    for (int y = 0; y < 2; y++) {
        assert_int_equal(buf[y * stride + 0], white);
        assert_int_equal(buf[y * stride + 1], 0);
        assert_int_equal(buf[y * stride + 2], 0);
        assert_int_equal(buf[y * stride + 3], 0xDEADBEEF); // padding untouched
    }
}

static void test_rle_overlong_run(void)
{
    struct mp_bd_palette_entry pal[256] = {0};
    pal[1] = (struct mp_bd_palette_entry){235, 128, 128, 255};
    uint32_t white = conv(235, 128, 128, 255);

    uint32_t buf[3];
    for (int i = 0; i < 3; i++)
        buf[i] = 0;

    // A run longer than the region width must be clamped, not overflow.
    struct mp_bd_rle_elem rle[] = { {99, 1} };
    mp_bd_decode_rle(buf, 3, 3, 1, pal, rle);
    assert_int_equal(buf[0], white);
    assert_int_equal(buf[1], white);
    assert_int_equal(buf[2], white);
}

int main(void)
{
    test_palette();
    test_rle_decode();
    test_rle_overlong_run();
    return 0;
}
