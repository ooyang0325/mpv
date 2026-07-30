#include <limits.h>
#include <string.h>

#include "audio/dop.h"

static uint8_t reverse_bits(uint8_t value)
{
    value = (value >> 4) | (value << 4);
    value = ((value & 0xcc) >> 2) | ((value & 0x33) << 2);
    return ((value & 0xaa) >> 1) | ((value & 0x55) << 1);
}

void mp_dop_reset(struct mp_dop_state *state)
{
    memset(state, 0, sizeof(*state));
}

int mp_dop_output_frames(const struct mp_dop_state *state, size_t src_size,
                         int channels)
{
    if (channels < 1 || channels > MP_NUM_CHANNELS ||
        src_size % channels)
        return -1;

    size_t bytes = state->num_pending + src_size / channels;
    size_t frames = bytes / 4 * 2;
    return frames <= INT_MAX ? frames : -1;
}

int mp_dop_pack(struct mp_dop_state *state, uint32_t *dst,
                const uint8_t *src, size_t src_size, int channels,
                bool planar, bool lsb_first)
{
    int expected = mp_dop_output_frames(state, src_size, channels);
    if (expected < 0 || (expected && !dst))
        return -1;

    size_t samples = src_size / channels;
    int frames = 0;
    for (size_t sample = 0; sample < samples; sample++) {
        for (int channel = 0; channel < channels; channel++) {
            size_t index = planar ? channel * samples + sample
                                  : sample * channels + channel;
            uint8_t value = src[index];
            state->pending[channel][state->num_pending] =
                lsb_first ? reverse_bits(value) : value;
        }

        if (++state->num_pending < 4)
            continue;

        for (int channel = 0; channel < channels; channel++) {
            uint8_t *p = state->pending[channel];
            dst[frames * channels + channel] =
                0x00050000u | (uint32_t)p[0] << 8 | p[1];
        }
        frames++;
        for (int channel = 0; channel < channels; channel++) {
            uint8_t *p = state->pending[channel];
            dst[frames * channels + channel] =
                0x00fa0000u | (uint32_t)p[2] << 8 | p[3];
        }
        frames++;
        state->num_pending = 0;
    }

    return frames == expected ? frames : -1;
}

float mp_dop_word_to_float(uint32_t word)
{
    int32_t sample = word & 0xffffff;
    if (sample & 0x800000)
        sample -= 0x1000000;
    return sample / 8388608.0f;
}
