#ifndef MPV_AUDIO_DOP_H
#define MPV_AUDIO_DOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/chmap.h"

struct mp_dop_state {
    uint8_t pending[MP_NUM_CHANNELS][4];
    int num_pending;
};

struct mp_pcm_to_dsd_state {
    double previous[MP_NUM_CHANNELS];
    double integrator1[MP_NUM_CHANNELS];
    double integrator2[MP_NUM_CHANNELS];
    double output[MP_NUM_CHANNELS];
};

void mp_dop_reset(struct mp_dop_state *state);
int mp_dop_output_frames(const struct mp_dop_state *state, size_t src_size,
                         int channels);
int mp_dop_pack(struct mp_dop_state *state, uint32_t *dst,
                const uint8_t *src, size_t src_size, int channels,
                bool planar, bool lsb_first);
float mp_dop_word_to_float(uint32_t word);

void mp_pcm_to_dsd_reset(struct mp_pcm_to_dsd_state *state);
uint16_t mp_pcm_to_dsd_encode(struct mp_pcm_to_dsd_state *state,
                              int channel, double sample);

#endif
