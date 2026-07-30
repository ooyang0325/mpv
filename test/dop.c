#include <string.h>

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
    return 0;
}
