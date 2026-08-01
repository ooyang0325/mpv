#include <limits.h>
#include <math.h>
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

void mp_pcm_to_dsd_reset(struct mp_pcm_to_dsd_state *state)
{
    memset(state, 0, sizeof(*state));
    for (int channel = 0; channel < MP_NUM_CHANNELS; channel++)
        state->output[channel] = -1.0;
}

uint16_t mp_pcm_to_dsd_encode(struct mp_pcm_to_dsd_state *state,
                              int channel, double sample)
{
    if (channel < 0 || channel >= MP_NUM_CHANNELS)
        return 0x6969;

    // Leave 6 dB of headroom so the second-order modulator stays stable at
    // full-scale PCM. Linear interpolation moves the 176.4/352.8 kHz input
    // smoothly through the sixteen DSD samples carried by each DoP frame.
    sample = isfinite(sample) ? fmax(-0.5, fmin(0.5, sample * 0.5)) : 0.0;
    double previous = state->previous[channel];
    double i1 = state->integrator1[channel];
    double i2 = state->integrator2[channel];
    double output = state->output[channel];
    uint16_t payload = 0;

    for (int bit = 0; bit < 16; bit++) {
        double input = previous + (sample - previous) * (bit + 1) / 16.0;
        i1 += input - output;
        i2 += i1 - output;
        output = i2 >= 0 ? 1.0 : -1.0;
        payload = payload << 1 | (output > 0);
    }

    state->previous[channel] = sample;
    state->integrator1[channel] = i1;
    state->integrator2[channel] = i2;
    state->output[channel] = output;
    return payload;
}
