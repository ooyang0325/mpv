/*
 * Runtime loader for liborender (the Omniphony spatial render engine).
 *
 * mpv used to link liborender at build time (DT_NEEDED), which welded every
 * release to the engine version it was built against. This module dlopens it
 * instead, walking a search order (see orender_dl.c) and performing the ABI
 * handshake from common/orender_abi.h: the library's orender_version_major()
 * must equal the ORENDER_ABI_MAJOR mpv was compiled against; optional features
 * are gated on symbol presence, never on the minor version. A missing or
 * incompatible library degrades to mpv's native decode path — it is never
 * fatal.
 *
 * This file is part of mpv. mpv is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 */

#ifndef MP_ORENDER_DL_H
#define MP_ORENDER_DL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/orender_abi.h"

struct mpv_global;
struct mp_log;

/* Function table over the loaded library. Field types mirror the prototypes in
 * common/orender_abi.h exactly; names drop the `orender_` prefix.
 *
 * REQUIRED symbols make or break the load: a candidate library missing any of
 * them (or failing the major check) is rejected. OPTIONAL symbols are resolved
 * individually and replaced by no-op/neutral stubs when absent, so call sites
 * stay unconditional and an older engine simply lacks the newer features. */
struct orender_dl {
    /* required — decoder core */
    struct OrenderRenderer *(*create)(const struct OrenderConfig *cfg);
    void (*destroy)(struct OrenderRenderer *r);
    int (*process)(struct OrenderRenderer *r, const uint8_t *pkt,
                   uintptr_t pkt_len, int64_t pts_us, float *out,
                   uintptr_t out_cap_samples, uintptr_t *out_frames,
                   uint32_t *out_channels, int64_t *out_pts_us);
    void (*reset)(struct OrenderRenderer *r);
    uint32_t (*channel_count)(const struct OrenderRenderer *r);
    uint32_t (*channel_layout)(const struct OrenderRenderer *r,
                               uint8_t *out_labels, uint32_t cap);
    int (*channel_mapping)(const struct OrenderRenderer *r);
    int (*channel_mode)(const struct OrenderRenderer *r);
    void (*set_channel_mode)(struct OrenderRenderer *r, int mode);
    int (*object_count)(const struct OrenderRenderer *r);
    int (*dialnorm_db)(const struct OrenderRenderer *r);
    uint32_t (*bed_layout)(const struct OrenderRenderer *r,
                           uint8_t *out_labels, uint32_t cap);

    /* optional — stubbed when missing */
    /* Resolved from orender_has_objects (ABI minor >= 6), falling back to
     * its deprecated alias orender_is_spatial on older engines. Live fact:
     * may flip mid-stream, never latch it. */
    int (*has_objects)(const struct OrenderRenderer *r);
    int (*set_option)(struct OrenderRenderer *r, const char *key,
                      const char *value);
    const char *(*build_id)(void);
    void (*overlay_set_rendering)(int rendering);
    void (*overlay_clear)(void);
    uintptr_t (*overlay_ass)(uint32_t res_x, uint32_t res_y, uint8_t *out,
                             uintptr_t cap);
    uintptr_t (*overlay_heatmap_bgra)(uint32_t res_x, uint32_t res_y,
                                      uint8_t *out, uintptr_t cap,
                                      int32_t *geom);
    void (*overlay_set_enabled)(int enabled);
    int (*overlay_toggle)(void);
    int (*overlay_toggle_labels)(void);
    int (*overlay_toggle_objects)(void);
    int (*overlay_toggle_trails)(void);
    int (*overlay_toggle_heatmap)(void);
    uint32_t (*overlay_cycle_heatmap_colormap)(void);
    uint32_t (*overlay_adjust_heatmap_bands)(int32_t delta);

    uint32_t abi_major, abi_minor;  // reported by the loaded library
    const char *path;               // candidate that won (for logging)
    bool have_overlay;              // full overlay symbol group present
    bool have_set_option;
};

/* Load liborender once per process (thread-safe; both success and failure are
 * cached — the search does not re-run). Returns NULL when no compatible
 * library was found; the reason is available via orender_dl_error(). Only the
 * first call logs the candidate walk, through its `log`. */
const struct orender_dl *orender_dl_get(struct mpv_global *global,
                                        struct mp_log *log);

/* All-stubs table for hosts that want unconditional call sites even when the
 * load failed: create() returns NULL (the caller's existing no-engine path),
 * queries return their neutral values, overlay calls are no-ops. */
const struct orender_dl *orender_dl_stubs(void);

/* Human-readable reason of a failed load ("" before the first orender_dl_get
 * or after a successful one). Static storage. */
const char *orender_dl_error(void);

#endif
