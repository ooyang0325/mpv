#pragma once

struct mp_r8brain;

struct mp_r8brain *mp_r8brain_create(double in_rate, double out_rate,
                                     int channels, int max_input);
void mp_r8brain_destroy(struct mp_r8brain *ctx);
void mp_r8brain_reset(struct mp_r8brain *ctx);
int mp_r8brain_process(struct mp_r8brain *ctx, const double *const input[],
                       int samples, const double *output[]);
double mp_r8brain_get_delay(struct mp_r8brain *ctx);
