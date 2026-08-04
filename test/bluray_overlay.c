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

static void test_rle_end_of_line(void)
{
    struct mp_bd_palette_entry pal[256] = {0};
    pal[1] = (struct mp_bd_palette_entry){235, 128, 128, 255}; // opaque white
    uint32_t white = conv(235, 128, 128, 255);

    // A zero-length run terminates the current line. Pixels drawn before it are
    // kept; the rest of the line is left to the caller's pre-cleared buffer.
    uint32_t buf[4];
    for (int i = 0; i < 4; i++)
        buf[i] = 0;
    struct mp_bd_rle_elem rle[] = { {2, 1}, {0, 0} };
    mp_bd_decode_rle(buf, 4, 4, 1, pal, rle);
    assert_int_equal(buf[0], white);
    assert_int_equal(buf[1], white);
    assert_int_equal(buf[2], 0);
    assert_int_equal(buf[3], 0);
}

// Regression for the "gray box" artifact: libbluray terminates every line with
// a (len=0, color=0) end-of-line marker after runs that already sum to the full
// width. The marker must advance to the next line, not be expanded as a run of
// palette[0]. palette[0] here is deliberately OPAQUE so the buggy decoder (which
// filled whole lines with it) is caught: those opaque lines are what blended
// into gray rectangles behind the menu text.
static void test_rle_multiline_eol(void)
{
    struct mp_bd_palette_entry pal[256] = {0};
    pal[0] = (struct mp_bd_palette_entry){16, 128, 128, 255};  // OPAQUE (bug filler)
    pal[1] = (struct mp_bd_palette_entry){0, 128, 128, 0};     // transparent
    pal[2] = (struct mp_bd_palette_entry){235, 128, 128, 255}; // opaque white
    uint32_t white = conv(235, 128, 128, 255);

    // 3x3 region. Each line's runs sum to exactly w=3, then a (0,0) EOL marker,
    // exactly as libbluray emits.
    struct mp_bd_rle_elem rle[] = {
        {3, 1}, {0, 0},                 // line 0: transparent
        {1, 1}, {1, 2}, {1, 1}, {0, 0}, // line 1: transp, white, transp
        {3, 1}, {0, 0},                 // line 2: transparent
    };
    uint32_t buf[9];
    for (int i = 0; i < 9; i++)
        buf[i] = 0xDEADBEEF;
    mp_bd_decode_rle(buf, 3, 3, 3, pal, rle);

    const uint32_t expect[9] = {
        0,     0,     0,
        0,     white, 0,
        0,     0,     0,
    };
    for (int i = 0; i < 9; i++)
        assert_int_equal(buf[i], expect[i]);
}

static void test_rle_empty_line(void)
{
    struct mp_bd_palette_entry pal[256] = {0};
    pal[1] = (struct mp_bd_palette_entry){235, 128, 128, 255};
    uint32_t white = conv(235, 128, 128, 255);
    struct mp_bd_rle_elem rle[] = {
        {0, 0}, // empty line
        {2, 1},
    };
    uint32_t buf[4] = {0};
    mp_bd_decode_rle(buf, 2, 2, 2, pal, rle);
    assert_int_equal(buf[0], 0);
    assert_int_equal(buf[1], 0);
    assert_int_equal(buf[2], white);
    assert_int_equal(buf[3], white);
}

int main(void)
{
    test_palette();
    test_rle_decode();
    test_rle_overlong_run();
    test_rle_end_of_line();
    test_rle_multiline_eol();
    test_rle_empty_line();
    return 0;
}
