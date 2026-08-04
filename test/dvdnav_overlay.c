/*
 * Regression tests for the pure DVD subpicture/highlight logic
 * (stream/dvdnav_overlay.h): SPU control-sequence + RLE decode, the DVD CLUT ->
 * premultiplied BGRA conversion, and the authored button highlight crop/recolor
 * that turns a selected/activated button's PCI color word into the pixels mpv
 * renders inside the button's crop rectangle.
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
#include "stream/discnav.h"
#include "stream/dvdnav_overlay.h"

// A minimal but complete SPU unit: a 4x2 display area at (0,0). Top field
// (line 0) is [pattern, pattern, bg, bg]; bottom field (line 1) is four emph1
// pixels. Sub-palette maps colors 0..3 to CLUT indices 0,1,2,3, all opaque.
static const uint8_t spu_data[] = {
    0x00, 0x1e,             // [0..1] total size = 30
    0x00, 0x06,             // [2..3] first DCSQ at offset 6
    0x98,                   // [4] top field RLE: run(len2,col1), run(len2,col0)
    0x12,                   // [5] bottom field RLE: run(len4,col2)
    0x00, 0x00,             // [6..7] DCSQ date
    0x00, 0x06,             // [8..9] next DCSQ = 6 (self -> terminates)
    0x01,                   // [10] STA_DSP
    0x03, 0x32, 0x10,       // [11..13] SET_COLOR -> pal = {0,1,2,3}
    0x04, 0xff, 0xff,       // [14..16] SET_CONTR -> alpha = {15,15,15,15}
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x01, // [17..23] DAREA 0,0 - 3,1
    0x06, 0x00, 0x04, 0x00, 0x05,             // [24..28] DSPXA top=4 bot=5
    0xff,                   // [29] end of control sequence
};

static uint32_t clut[16];

static void init_clut(void)
{
    // Distinct luma per index so conversions differ; neutral chroma keeps the
    // values deterministic regardless of the exact matrix coefficients.
    for (int i = 0; i < 16; i++)
        clut[i] = ((uint32_t)(i * 16) << 16) | (128 << 8) | 128; // Y, Cb, Cr
}

static void test_color_word(void)
{
    // btn_coli word: [Ci3 Ci2 Ci1 Ci0 A3 A2 A1 A0] from MSB.
    uint32_t word = 0x1234ABCDu;
    assert_int_equal(mp_dvdspu_hl_color_index(word, 0), 0x4); // Ci0
    assert_int_equal(mp_dvdspu_hl_color_index(word, 1), 0x3); // Ci1
    assert_int_equal(mp_dvdspu_hl_color_index(word, 2), 0x2); // Ci2
    assert_int_equal(mp_dvdspu_hl_color_index(word, 3), 0x1); // Ci3
    assert_int_equal(mp_dvdspu_hl_alpha(word, 0), 0xD);       // A0
    assert_int_equal(mp_dvdspu_hl_alpha(word, 1), 0xC);       // A1
    assert_int_equal(mp_dvdspu_hl_alpha(word, 2), 0xB);       // A2
    assert_int_equal(mp_dvdspu_hl_alpha(word, 3), 0xA);       // A3
}

static void test_clut_bgra(void)
{
    // Alpha 0 is fully transparent -> zero pixel.
    assert_int_equal(mp_dvd_clut_to_bgra(clut[5], 0), 0);
    // Alpha 15 -> ~opaque; top byte is 15*17 == 255.
    uint32_t px = mp_dvd_clut_to_bgra(clut[5], 15);
    assert_int_equal((uint8_t)(px >> 24), 255);
}

static void test_decode(void)
{
    struct mp_dvdspu spu;
    assert_true(mp_dvdspu_decode(spu_data, sizeof(spu_data), &spu));

    assert_int_equal(spu.x, 0);
    assert_int_equal(spu.y, 0);
    assert_int_equal(spu.w, 4);
    assert_int_equal(spu.h, 2);

    assert_int_equal(spu.pal[0], 0);
    assert_int_equal(spu.pal[1], 1);
    assert_int_equal(spu.pal[2], 2);
    assert_int_equal(spu.pal[3], 3);
    for (int i = 0; i < 4; i++)
        assert_int_equal(spu.alpha[i], 15);

    static const uint8_t expect[2][4] = {
        {1, 1, 0, 0}, // top field
        {2, 2, 2, 2}, // bottom field
    };
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++)
            assert_int_equal(spu.idx[y * 4 + x], expect[y][x]);
    }
    mp_dvdspu_free(&spu);
}

static void test_render_highlight(void)
{
    struct mp_dvdspu spu;
    assert_true(mp_dvdspu_decode(spu_data, sizeof(spu_data), &spu));

    uint32_t out[2 * 4];

    // Base render (no highlight): every pixel uses the SPU sub-palette.
    memset(out, 0xAB, sizeof(out));
    mp_dvdspu_render_bgra(&spu, clut, NULL, out, 4);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            int i = spu.idx[y * 4 + x];
            uint32_t want = mp_dvd_clut_to_bgra(clut[spu.pal[i]], spu.alpha[i]);
            assert_int_equal(out[y * 4 + x], want);
        }
    }

    // Highlight only the left half (columns 0..1). Inside the crop the button
    // color word is used; outside it the base sub-palette must be untouched.
    struct mp_dvdspu_hl hl = { .x1 = 0, .y1 = 0, .x2 = 1, .y2 = 1,
                               .color = 0x77770F00u };
    // color word: all four CLUT indices = 7; alphas A0=0,A1=0,A2=15,A3=0, so
    // only sub-color 2 (the bottom-field pixels) is opaque inside the crop.
    memset(out, 0, sizeof(out));
    mp_dvdspu_render_bgra(&spu, clut, &hl, out, 4);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            int i = spu.idx[y * 4 + x];
            uint32_t want;
            if (x <= 1) {
                int ci = mp_dvdspu_hl_color_index(hl.color, i);
                int a4 = mp_dvdspu_hl_alpha(hl.color, i);
                want = mp_dvd_clut_to_bgra(clut[ci], a4);
            } else {
                want = mp_dvd_clut_to_bgra(clut[spu.pal[i]], spu.alpha[i]);
            }
            assert_int_equal(out[y * 4 + x], want);
        }
    }
    // Spot check: inside the crop, sub-color 1 (line 0) has alpha 0 while
    // sub-color 2 (line 1) has alpha 15.
    assert_int_equal(out[0 * 4 + 0], 0);      // line0 col0: idx1 -> alpha 0
    assert_true(out[1 * 4 + 0] != 0);         // line1 col0: idx2 -> alpha 15
    // Outside the crop the base sub-palette is used (idx0 -> pal[0], alpha 15).
    assert_int_equal(out[0 * 4 + 2],
                     mp_dvd_clut_to_bgra(clut[spu.pal[0]], spu.alpha[0]));

    mp_dvdspu_free(&spu);
}

static void test_menu_state(void)
{
    // Menu is active only in a menu domain with at least one button.
    assert_true(mp_dvd_menu_active(true, 3));
    assert_false(mp_dvd_menu_active(true, 0));
    assert_false(mp_dvd_menu_active(false, 3));
    // A demux re-sync is due exactly when the DVD domain changes.
    assert_true(mp_dvd_domain_changed(2, 4));
    assert_false(mp_dvd_domain_changed(4, 4));
}

static void test_still_seconds(void)
{
    // Only 0xff is an infinite still; 0 means no wait; others are timed.
    assert_int_equal(mp_dvd_still_seconds(0xff), -1);
    assert_int_equal(mp_dvd_still_seconds(0), 0);
    assert_int_equal(mp_dvd_still_seconds(1), 1);
    assert_int_equal(mp_dvd_still_seconds(10), 10);
}

static void test_spu_wanted(void)
{
    // Non-subpicture substreams are never accumulated.
    assert_false(mp_dvd_spu_wanted(-1, 0x1f));
    assert_false(mp_dvd_spu_wanted(-1, 0x40));
    // Unknown channel (-1) accepts any subpicture substream.
    assert_true(mp_dvd_spu_wanted(-1, 0x20));
    assert_true(mp_dvd_spu_wanted(-1, 0x3f));
    // A known channel accepts only that channel.
    assert_true(mp_dvd_spu_wanted(0x21, 0x21));
    assert_false(mp_dvd_spu_wanted(0x21, 0x20));
    assert_false(mp_dvd_spu_wanted(0x21, 0x22));
}

static void test_wait_drained(void)
{
    // The DVDNAV_WAIT release requires the nested demux queues empty AND every
    // present audio/video output to have presented everything.
    // Nothing drained while demux still has data.
    assert_false(mp_nav_wait_drained(false, true, true, true, true));
    // Never release when nothing is playing (no outputs).
    assert_false(mp_nav_wait_drained(true, false, false, false, false));
    // Video-only: released once the video output drained.
    assert_true(mp_nav_wait_drained(true, false, false, true, true));
    assert_false(mp_nav_wait_drained(true, false, false, true, false));
    // Audio-only.
    assert_true(mp_nav_wait_drained(true, true, true, false, false));
    assert_false(mp_nav_wait_drained(true, true, false, false, false));
    // Both present: both must have drained.
    assert_true(mp_nav_wait_drained(true, true, true, true, true));
    assert_false(mp_nav_wait_drained(true, true, true, true, false));
    assert_false(mp_nav_wait_drained(true, true, false, true, true));
}

static void test_wait_phase(void)
{
    // No wait outstanding -> phase 0 regardless of the release flag.
    assert_int_equal(0, mp_nav_wait_phase(false, false));
    assert_int_equal(0, mp_nav_wait_phase(false, true));
    // Waiting, not yet released by the player -> phase 1 (keep demuxer alive).
    assert_int_equal(1, mp_nav_wait_phase(true, false));
    // Waiting and released -> phase 2 (stream may wait_skip and resume).
    assert_int_equal(2, mp_nav_wait_phase(true, true));
}

static void test_nav_hold(void)
{
    // Held while a button menu is on screen (still, motion or WAIT-parked): the
    // player suppresses the cache pause here.
    assert_true(mp_nav_hold(true, 0));
    // Held on any authored still even without buttons (infinite or timed).
    assert_true(mp_nav_hold(false, -1));
    assert_true(mp_nav_hold(false, 5));
    // Not held while content plays (no menu, no still).
    assert_false(mp_nav_hold(false, 0));
}

static void test_nav_audio_idle(void)
{
    // A genuinely silent hold idles the audio output: a button menu, an
    // indefinite or timed still, or a WAIT with no program audio.
    assert_true(mp_nav_audio_idle(true, 0, false, false));   // silent button menu
    assert_true(mp_nav_audio_idle(false, -1, false, false)); // indefinite still
    assert_true(mp_nav_audio_idle(false, 5, false, false));  // timed still
    assert_true(mp_nav_audio_idle(false, 0, true, false));   // silent WAIT
    // A hold that carries authored in-band audio (motion menu / still with
    // background music) must keep playing -- do NOT idle.
    assert_false(mp_nav_audio_idle(true, 0, false, true));   // menu + program audio
    assert_false(mp_nav_audio_idle(false, -1, false, true)); // still + program audio
    assert_false(mp_nav_audio_idle(false, 0, true, true));   // WAIT + program audio
    // Not held at all -> never idle, regardless of audio.
    assert_false(mp_nav_audio_idle(false, 0, false, false));
    assert_false(mp_nav_audio_idle(false, 0, false, true));
}

static void test_demux_drained(void)
{
    // Threaded: the demuxer reports underrun once its queues run dry.
    assert_true(mp_nav_demux_drained(true, 4096));
    // Unthreaded (--demuxer-thread=no): underrun is forced false, so fall back
    // to the thread-independent empty-forward-queue signal.
    assert_true(mp_nav_demux_drained(false, 0));
    assert_false(mp_nav_demux_drained(false, 4096));
    // Both signals agree at the drain boundary.
    assert_true(mp_nav_demux_drained(true, 0));
}

int main(void)
{
    init_clut();
    test_color_word();
    test_clut_bgra();
    test_decode();
    test_render_highlight();
    test_menu_state();
    test_still_seconds();
    test_spu_wanted();
    test_wait_drained();
    test_wait_phase();
    test_nav_hold();
    test_nav_audio_idle();
    test_demux_drained();
    return 0;
}
