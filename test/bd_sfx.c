#include <stdint.h>
#include <string.h>

#include "audio/bd_sfx.h"
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

int main(void)
{
    test_route_predicate();
    test_mix_f32();
    test_mix_s16();
    test_mix_s32();
    test_queue_bounds();
    return 0;
}
