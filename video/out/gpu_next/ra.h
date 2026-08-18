#pragma once

#include <libplacebo/gpu.h>                // for pl_gpu, pl_tex, pl_fmt
#include <libplacebo/log.h>                // for pl_log
#include <libplacebo/renderer.h>           // for pl_renderer
#include "stdbool.h"                       // for bool

/* Forward declarations from mpv */
struct mp_image;
struct mp_log;

/* Public RA handle exposed to higher layers (minimal surface). */
struct ra_next {
    pl_gpu gpu;
    struct mp_log *log;
};

/* Upload an mp_image into a pl_frame suitable for pl_render_image.
 * Caller must call ra_cleanup_pl_frame() to free any textures created.
 * Returns true on success, false on failure. */
bool ra_upload_mp_image(struct ra_next *ra, struct pl_frame *out_frame,
                        const struct mp_image *img, pl_tex textures[4]);

/* Cleanup any textures/resources attached to a pl_frame created by upload. */
void ra_cleanup_pl_frame(struct ra_next *ra, struct pl_frame *frame);

/* --- New Rendering Wrappers --- */
bool ra_next_render_image(struct ra_next *ra, const struct pl_frame *src,
                     struct pl_frame *target, const struct pl_render_params *params);

/* Create the pl-specific RA implementation. */
struct ra_next *ra_pl_create(pl_gpu gpu, struct mp_log *log, pl_log log_pl);

/* Destroys the pl-specific RA implementation. */
void ra_pl_destroy(struct ra_next **rap);

/* Reset the RA (flush caches etc). */
void ra_pl_reset(struct ra_next *ra);
