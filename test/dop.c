#include <string.h>
#include <math.h>

#include "audio/dop.h"
#include "audio/format.h"
#include "test_utils.h"

static uint8_t reverse_bits(uint8_t value)
{
    value = (value >> 4) | (value << 4);
    value = ((value & 0xcc) >> 2) | ((value & 0x33) << 2);
    return ((value & 0xaa) >> 1) | ((value & 0x55) << 1);
}

static void test_layout(bool planar, bool lsb_first)
{
    static const uint8_t interleaved[] = {
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    };
    static const uint8_t planar_data[] = {
        0x12, 0x56, 0x9a, 0xde, 0x34, 0x78, 0xbc, 0xf0,
    };
    static const uint32_t expected[] = {
        0x00051256, 0x00053478, 0x00fa9ade, 0x00fabcf0,
    };
    uint8_t source[sizeof(interleaved)];
    memcpy(source, planar ? planar_data : interleaved, sizeof(source));
    if (lsb_first) {
        for (size_t n = 0; n < sizeof(source); n++)
            source[n] = reverse_bits(source[n]);
    }

    struct mp_dop_state state = {0};
    uint32_t output[4] = {0};
    assert_int_equal(mp_dop_output_frames(&state, sizeof(source), 2), 2);
    assert_int_equal(mp_dop_pack(&state, output, source, sizeof(source), 2,
                                 planar, lsb_first), 2);
    assert_memcmp(output, expected, sizeof(expected));
}

static void test_split_packets(void)
{
    static const uint8_t first[] = {0x12, 0x34};
    static const uint8_t rest[] = {0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    static const uint32_t expected[] = {
        0x00051256, 0x00053478, 0x00fa9ade, 0x00fabcf0,
    };
    struct mp_dop_state state = {0};
    uint32_t output[4] = {0};

    assert_int_equal(mp_dop_pack(&state, NULL, first, sizeof(first), 2,
                                 false, false), 0);
    assert_int_equal(state.num_pending, 1);
    assert_int_equal(mp_dop_pack(&state, output, rest, sizeof(rest), 2,
                                 false, false), 2);
    assert_memcmp(output, expected, sizeof(expected));

    mp_dop_reset(&state);
    assert_int_equal(state.num_pending, 0);
    assert_int_equal(mp_dop_output_frames(&state, 3, 2), -1);
}

static void test_float_carrier(void)
{
    const uint32_t words[] = {0x00051256, 0x00fa9ade};
    for (int n = 0; n < MP_ARRAY_SIZE(words); n++) {
        double scaled = mp_dop_word_to_float(words[n]) * 2147483648.0;
        int32_t physical = llrint(scaled);
        assert_int_equal((uint32_t)physical, words[n] << 8);
    }
}

static void test_pcm_to_dsd(void)
{
    struct mp_pcm_to_dsd_state state;
    mp_pcm_to_dsd_reset(&state);

    assert_int_equal(mp_pcm_to_dsd_encode(&state, 0, 0), 0xd333);
    assert_int_equal(mp_pcm_to_dsd_encode(&state, 0, 0), 0x3333);

    int positive = 0;
    int negative = 0;
    for (int n = 0; n < 4096; n++) {
        positive += __builtin_popcount(
            mp_pcm_to_dsd_encode(&state, 0, 1.0));
        negative += __builtin_popcount(
            mp_pcm_to_dsd_encode(&state, 1, -1.0));
    }
    // Full-scale PCM is deliberately limited to +/-0.5 DSD amplitude.
    assert_true(positive > 4096 * 11 && positive < 4096 * 13);
    assert_true(negative > 4096 * 3 && negative < 4096 * 5);

    mp_pcm_to_dsd_reset(&state);
    assert_int_equal(mp_pcm_to_dsd_encode(&state, 0, 0), 0xd333);
    assert_int_equal(mp_pcm_to_dsd_encode(&state, -1, 0), 0x6969);
}

int main(void)
{
    assert_int_equal(af_fmt_to_bytes(AF_FORMAT_S_DOP), 4);
    assert_true(af_fmt_is_spdif(AF_FORMAT_S_DOP));
    assert_false(af_fmt_is_pcm(AF_FORMAT_S_DOP));
    test_layout(false, false);
    test_layout(true, false);
    test_layout(false, true);
    test_layout(true, true);
    test_split_packets();
    test_float_carrier();
    test_pcm_to_dsd();
    return 0;
}
