#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <CDSPResampler.h>

extern "C" {
#include "audio/r8brain.h"
}

struct mp_r8brain {
    double in_rate;
    double out_rate;
    int channels;
    int max_input;
    int64_t total_input;
    int64_t total_output;
    std::vector<std::unique_ptr<r8b::CDSPResampler24>> resamplers;
    std::vector<double> silence;
};

extern "C" struct mp_r8brain *mp_r8brain_create(double in_rate,
                                                  double out_rate,
                                                  int channels,
                                                  int max_input)
{
    if (in_rate <= 0 || out_rate <= 0 || channels < 1 || max_input < 1)
        return nullptr;

    try {
        auto ctx = std::make_unique<mp_r8brain>();
        ctx->in_rate = in_rate;
        ctx->out_rate = out_rate;
        ctx->channels = channels;
        ctx->max_input = max_input;
        ctx->silence.resize(max_input);
        for (int n = 0; n < channels; n++) {
            ctx->resamplers.emplace_back(
                std::make_unique<r8b::CDSPResampler24>(
                    in_rate, out_rate, max_input));
        }
        return ctx.release();
    } catch (...) {
        return nullptr;
    }
}

extern "C" void mp_r8brain_destroy(struct mp_r8brain *ctx)
{
    delete ctx;
}

extern "C" void mp_r8brain_reset(struct mp_r8brain *ctx)
{
    if (!ctx)
        return;
    for (auto &resampler : ctx->resamplers)
        resampler->clear();
    ctx->total_input = 0;
    ctx->total_output = 0;
}

extern "C" int mp_r8brain_process(struct mp_r8brain *ctx,
                                    const double *const input[],
                                    int samples, const double *output[])
{
    if (!ctx || samples < 0 || samples > ctx->max_input || !output)
        return -1;

    bool flushing = !input;
    int64_t expected = (int64_t)std::ceil(
        ctx->total_input * ctx->out_rate / ctx->in_rate);
    if (flushing && ctx->total_output >= expected)
        return 0;

    if (flushing)
        samples = ctx->max_input;

    int produced = -1;
    for (int channel = 0; channel < ctx->channels; channel++) {
        double *out = nullptr;
        double *in = flushing
            ? ctx->silence.data()
            : const_cast<double *>(input[channel]);
        int count = ctx->resamplers[channel]->process(in, samples, out);
        if (produced >= 0 && count != produced)
            return -1;
        produced = count;
        output[channel] = out;
    }

    if (!flushing)
        ctx->total_input += samples;
    if (flushing)
        produced = std::min<int64_t>(produced, expected - ctx->total_output);
    ctx->total_output += produced;
    return produced;
}

extern "C" double mp_r8brain_get_delay(struct mp_r8brain *ctx)
{
    if (!ctx)
        return 0;
    return ctx->total_input / ctx->in_rate -
           ctx->total_output / ctx->out_rate;
}
