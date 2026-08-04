#include <stdint.h>
#include <string.h>

#include "audio/aframe.h"
#include "audio/bd_sfx.h"
#include "audio/chmap.h"
#include "audio/format.h"
#include "test_utils.h"

// Overlaying decoded PCM effects is only safe on an ordinary, mixable PCM
// route. Passthrough (IEC61937), DoP and any bit-exact AO carrier must be
// rejected.
static void test_route_predicate(void)
{
    // Ordinary decoded PCM: safe.
    assert_true(mp_bd_sfx_route_allows_mix(AF_FORMAT_FLOAT, false));
    assert_true(mp_bd_sfx_route_allows_mix(AF_FORMAT_S16, false));
    assert_true(mp_bd_sfx_route_allows_mix(AF_FORMAT_S32, false));

    // Same PCM formats, but the AO carries them bit-for-bit (PCM-to-DSD or a
    // non-mixable exclusive carrier): must be suppressed.
    assert_false(mp_bd_sfx_route_allows_mix(AF_FORMAT_FLOAT, true));
    assert_false(mp_bd_sfx_route_allows_mix(AF_FORMAT_S16, true));

    // Encoded passthrough and DoP are not PCM: always suppressed.
    assert_false(mp_bd_sfx_route_allows_mix(AF_FORMAT_S_AC3, false));
    assert_false(mp_bd_sfx_route_allows_mix(AF_FORMAT_S_EAC3, false));
    assert_false(mp_bd_sfx_route_allows_mix(AF_FORMAT_S_DTSHD, false));
    assert_false(mp_bd_sfx_route_allows_mix(AF_FORMAT_S_TRUEHD, false));
    assert_false(mp_bd_sfx_route_allows_mix(AF_FORMAT_S_DOP, false));

    // PCM layouts the mixer does not implement are rejected rather than
    // retaining a pending clip forever.
    assert_false(mp_bd_sfx_route_allows_mix(AF_FORMAT_U8, false));
    assert_false(mp_bd_sfx_route_allows_mix(AF_FORMAT_DOUBLE, false));
    assert_false(mp_bd_sfx_route_allows_mix(AF_FORMAT_FLOATP, false));
}

// Float mix domain: additive overlay, headroom attenuation, and saturation.
static void test_mix_f32(void)
{
    // Headroom gain scales the effect down before it is added.
    float a[1] = {0.0f};
    float s1[1] = {1.0f};
    mp_bd_sfx_mix_f32(a, s1, 1, 0.5f);
    assert_float_equal(a[0], 0.5f, 1e-6);

    // Additive on top of existing program audio.
    float b[1] = {0.25f};
    float s2[1] = {0.5f};
    mp_bd_sfx_mix_f32(b, s2, 1, 0.5f);
    assert_float_equal(b[0], 0.5f, 1e-6);

    // Saturation, both rails.
    float hi[1] = {0.8f};
    float shi[1] = {1.0f};
    mp_bd_sfx_mix_f32(hi, shi, 1, 0.5f); // 0.8 + 0.5 = 1.3 -> clip
    assert_float_equal(hi[0], 1.0f, 1e-6);

    float lo[1] = {-0.8f};
    float slo[1] = {-1.0f};
    mp_bd_sfx_mix_f32(lo, slo, 1, 0.5f); // -1.3 -> clip
    assert_float_equal(lo[0], -1.0f, 1e-6);
}

// 16-bit: conversion to/from float, rounding and saturation.
static void test_mix_s16(void)
{
    // Full-scale effect at unity saturates to the positive max (not wrap).
    int16_t a[1] = {0};
    float s1[1] = {1.0f};
    mp_bd_sfx_mix_s16(a, s1, 1, 1.0f);
    assert_int_equal(a[0], 32767);

    // Half scale.
    int16_t b[1] = {0};
    float s2[1] = {0.5f};
    mp_bd_sfx_mix_s16(b, s2, 1, 1.0f);
    assert_int_equal(b[0], 16384);

    // Headroom halves the effect.
    int16_t c[1] = {0};
    float s3[1] = {1.0f};
    mp_bd_sfx_mix_s16(c, s3, 1, 0.5f);
    assert_int_equal(c[0], 16384);

    // Additive on top of existing signal (0.25 + 0.25 = 0.5).
    int16_t d[1] = {8192};
    float s4[1] = {0.25f};
    mp_bd_sfx_mix_s16(d, s4, 1, 1.0f);
    assert_int_equal(d[0], 16384);

    // Negative saturation.
    int16_t e[1] = {-16384};
    float s5[1] = {-1.0f};
    mp_bd_sfx_mix_s16(e, s5, 1, 1.0f); // -0.5 - 1.0 -> clip
    assert_int_equal(e[0], -32768);
}

// 32-bit: same behaviour at the wider scale.
static void test_mix_s32(void)
{
    int32_t a[1] = {0};
    float s1[1] = {1.0f};
    mp_bd_sfx_mix_s32(a, s1, 1, 1.0f); // clips to INT32_MAX
    assert_int_equal(a[0], INT32_MAX);

    int32_t b[1] = {0};
    float s2[1] = {0.5f};
    mp_bd_sfx_mix_s32(b, s2, 1, 1.0f);
    assert_int_equal(b[0], 1073741824);

    int32_t c[1] = {0};
    float s3[1] = {-1.0f};
    mp_bd_sfx_mix_s32(c, s3, 1, 1.0f);
    assert_int_equal(c[0], INT32_MIN);
}

// The pending queue is bounded and rejects invalid clips.
static void test_queue_bounds(void)
{
    struct mp_bd_sfx *sfx = mp_bd_sfx_create(NULL, NULL);
    assert_int_equal(mp_bd_sfx_num_pending(sfx), 0);

    // 2 frames, stereo interleaved.
    static const int16_t clip[4] = {1, 2, 3, 4};

    // Invalid inputs are rejected and retain nothing.
    assert_false(mp_bd_sfx_add(sfx, NULL, 2, 2));
    assert_false(mp_bd_sfx_add(sfx, clip, 0, 2));
    assert_false(mp_bd_sfx_add(sfx, clip, 2, 0));
    assert_false(mp_bd_sfx_add(sfx, clip, 2, 3)); // only mono/stereo authored
    assert_int_equal(mp_bd_sfx_num_pending(sfx), 0);

    // Fill exactly to the cap.
    for (int i = 0; i < MP_BD_SFX_MAX_PENDING; i++)
        assert_true(mp_bd_sfx_add(sfx, clip, 2, 2));
    assert_int_equal(mp_bd_sfx_num_pending(sfx), MP_BD_SFX_MAX_PENDING);

    // Further clips are dropped: the queue never grows past the cap.
    assert_false(mp_bd_sfx_add(sfx, clip, 2, 2));
    assert_false(mp_bd_sfx_add(sfx, clip, 2, 2));
    assert_int_equal(mp_bd_sfx_num_pending(sfx), MP_BD_SFX_MAX_PENDING);

    // Flush clears everything and the mixer is reusable afterwards.
    mp_bd_sfx_flush(sfx);
    assert_int_equal(mp_bd_sfx_num_pending(sfx), 0);
    assert_true(mp_bd_sfx_add(sfx, clip, 2, 1)); // mono also accepted
    assert_int_equal(mp_bd_sfx_num_pending(sfx), 1);

    talloc_free(sfx);
}

// Build a silent packed float aframe at 48 kHz stereo (the format a menu with
// no program audio would hand us for the silence-drain path).
static struct mp_aframe *make_silence(int samples)
{
    struct mp_aframe *af = mp_aframe_create();
    struct mp_chmap chmap;
    mp_chmap_from_str(&chmap, bstr0("stereo"));
    mp_aframe_set_format(af, AF_FORMAT_FLOAT);
    mp_aframe_set_rate(af, MP_BD_SFX_SRC_RATE);
    mp_aframe_set_chmap(af, &chmap);
    assert_true(mp_aframe_alloc_data(af, samples));
    mp_aframe_set_silence(af, 0, samples);
    return af;
}

static bool frame_is_silent(struct mp_aframe *af)
{
    int n = mp_aframe_get_size(af) * mp_aframe_get_channels(af);
    float *d = (float *)mp_aframe_get_data_ro(af)[0];
    for (int i = 0; i < n; i++) {
        if (d[i] != 0.0f)
            return false;
    }
    return true;
}

// A pending effect is drained onto synthesized silence: mp_bd_sfx_mix() turns a
// silent frame into the effect, and the mixer reports output only until the clip
// is exhausted (so the caller stops generating silence).
static void test_silence_drain(void)
{
    struct mp_bd_sfx *sfx = mp_bd_sfx_create(NULL, NULL);

    // 100-frame stereo clip at half scale (S16 16384 -> 0.5 float).
    int16_t clip[200];
    for (int i = 0; i < 200; i++)
        clip[i] = 16384;
    assert_true(mp_bd_sfx_add(sfx, clip, 100, 2));
    assert_true(mp_bd_sfx_has_output(sfx));

    // First silent chunk longer than the clip: the clip surfaces, and the tail
    // past the clip stays silent. 48 kHz in and out means a 1:1 convert, so the
    // effect is 0.5 * headroom.
    struct mp_aframe *a = make_silence(160);
    assert_true(frame_is_silent(a)); // silent before the mix
    mp_bd_sfx_mix(sfx, a, false);
    float *d = (float *)mp_aframe_get_data_ro(a)[0];
    // Within the clip (frame 0): both channels ~= 0.5 * 0.5 headroom = 0.25.
    assert_float_equal(d[0], 0.5f * MP_BD_SFX_HEADROOM, 0.02);
    assert_float_equal(d[1], 0.5f * MP_BD_SFX_HEADROOM, 0.02);
    // Past the clip (frame 120 -> sample index 240): silent tail.
    assert_float_equal(d[240], 0.0f, 1e-6);
    assert_float_equal(d[241], 0.0f, 1e-6);
    // The 100-frame clip was fully consumed by the 160-frame chunk.
    assert_false(mp_bd_sfx_has_output(sfx));
    talloc_free(a);

    // With nothing pending, a further silent frame stays untouched (no
    // continuous silence/output beyond the clip).
    struct mp_aframe *b = make_silence(64);
    mp_bd_sfx_mix(sfx, b, false);
    assert_true(frame_is_silent(b));
    talloc_free(b);

    talloc_free(sfx);
}

// A pending effect is dropped, and silence stays silent, on a bit-exact route.
static void test_silence_suppressed_bit_exact(void)
{
    struct mp_bd_sfx *sfx = mp_bd_sfx_create(NULL, NULL);
    int16_t clip[64];
    for (int i = 0; i < 64; i++)
        clip[i] = 30000;
    assert_true(mp_bd_sfx_add(sfx, clip, 32, 2));
    assert_true(mp_bd_sfx_has_output(sfx));

    struct mp_aframe *a = make_silence(64);
    mp_bd_sfx_mix(sfx, a, true); // bit-exact carrier: must suppress + flush
    assert_true(frame_is_silent(a));
    assert_false(mp_bd_sfx_has_output(sfx));
    talloc_free(a);

    talloc_free(sfx);
}

int main(void)
{
    test_route_predicate();
    test_mix_f32();
    test_mix_s16();
    test_mix_s32();
    test_queue_bounds();
    test_silence_drain();
    test_silence_suppressed_bit_exact();
    return 0;
}
