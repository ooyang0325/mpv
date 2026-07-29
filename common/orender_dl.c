/*
 * Runtime loader for liborender — see orender_dl.h for the contract.
 *
 * Search order (first winner is cached for the process lifetime):
 *   1. --ad-orender-library=PATH    (explicit user intent: failure is final,
 *   2. $ORENDER_LIBRARY              no fall-through to later candidates)
 *   3. the Studio-deployed per-user library:
 *        Linux   $XDG_DATA_HOME/omniphony/lib/liborender.so.<major>
 *                (default ~/.local/share)
 *        macOS   ~/Library/Application Support/omniphony/lib/liborender.dylib
 *        Windows %LOCALAPPDATA%\omniphony\lib\orender.dll
 *   4. next to the mpv executable   (the lib bundled in the release zip)
 *   5. the bare platform name via the system loader (ldconfig paths / distro
 *      package). Skipped on Windows: mp_dlopen resolves relative names
 *      against the CWD, which is not a meaningful search location.
 *
 * A candidate is accepted only if it exports the version handshake, reports
 * the ABI major mpv was compiled against, and resolves every required symbol.
 * Rejected candidates (skew, pre-handshake builds) fall through to the next
 * one, so e.g. a stale Studio deployment degrades to the bundled fallback
 * instead of killing spatial audio.
 *
 * This file is part of mpv. mpv is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/decode/ad_orender.h"
#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "misc/path_utils.h"
#include "mpv_talloc.h"
#include "options/m_config.h"
#include "osdep/io.h"
#include "osdep/threads.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/param.h>
#endif

#include "common/orender_dl.h"

#define OR_STR_(x) #x
#define OR_STR(x) OR_STR_(x)

#if defined(_WIN32)
#define ORENDER_LIB_NAME "orender.dll"
#elif defined(__APPLE__)
#define ORENDER_LIB_NAME "liborender.dylib"
#else
#define ORENDER_LIB_NAME "liborender.so." OR_STR(ORENDER_ABI_MAJOR)
#endif

static void lib_close(void *handle)
{
#if defined(_WIN32)
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

/* ── no-op stubs for optional symbols ─────────────────────────────────────── */

static int stub_has_objects(const struct OrenderRenderer *r) { return -1; }
static int stub_set_option(struct OrenderRenderer *r, const char *key,
                           const char *value) { return -1; }
static const char *stub_build_id(void) { return NULL; }
static void stub_overlay_set_rendering(int rendering) {}
static void stub_overlay_clear(void) {}
static uintptr_t stub_overlay_ass(uint32_t res_x, uint32_t res_y, uint8_t *out,
                                  uintptr_t cap) { return 0; }
static uintptr_t stub_overlay_heatmap_bgra(uint32_t res_x, uint32_t res_y,
                                           uint8_t *out, uintptr_t cap,
                                           int32_t *geom) { return 0; }
static void stub_overlay_set_enabled(int enabled) {}
static int stub_overlay_toggle(void) { return 0; }
static int stub_overlay_toggle_flag(void) { return 0; }
static uint32_t stub_overlay_cycle_heatmap_colormap(void) { return 0; }
static uint32_t stub_overlay_adjust_heatmap_bands(int32_t delta) { return 0; }

/* Stub used when the whole load failed and the host still wants unconditional
 * call sites: create() returning NULL routes the caller into its existing
 * no-engine fallback. */
static struct OrenderRenderer *stub_create(const struct OrenderConfig *cfg)
{
    return NULL;
}
static void stub_destroy(struct OrenderRenderer *r) {}
static int stub_process(struct OrenderRenderer *r, const uint8_t *pkt,
                        uintptr_t pkt_len, int64_t pts_us, float *out,
                        uintptr_t out_cap_samples, uintptr_t *out_frames,
                        uint32_t *out_channels, int64_t *out_pts_us)
{
    return -1;
}
static void stub_reset(struct OrenderRenderer *r) {}
static uint32_t stub_channel_count(const struct OrenderRenderer *r) { return 0; }
static uint32_t stub_layout(const struct OrenderRenderer *r,
                            uint8_t *out_labels, uint32_t cap) { return 0; }
static int stub_channel_mapping(const struct OrenderRenderer *r) { return -1; }
static int stub_channel_mode(const struct OrenderRenderer *r) { return -1; }
static void stub_set_channel_mode(struct OrenderRenderer *r, int mode) {}
static int stub_object_count(const struct OrenderRenderer *r) { return -1; }
static int stub_dialnorm_db(const struct OrenderRenderer *r) { return INT32_MIN; }

static const struct orender_dl stubs = {
    .create = stub_create,
    .destroy = stub_destroy,
    .process = stub_process,
    .reset = stub_reset,
    .channel_count = stub_channel_count,
    .channel_layout = stub_layout,
    .channel_mapping = stub_channel_mapping,
    .channel_mode = stub_channel_mode,
    .set_channel_mode = stub_set_channel_mode,
    .object_count = stub_object_count,
    .dialnorm_db = stub_dialnorm_db,
    .bed_layout = stub_layout,
    .has_objects = stub_has_objects,
    .set_option = stub_set_option,
    .build_id = stub_build_id,
    .overlay_set_rendering = stub_overlay_set_rendering,
    .overlay_clear = stub_overlay_clear,
    .overlay_ass = stub_overlay_ass,
    .overlay_heatmap_bgra = stub_overlay_heatmap_bgra,
    .overlay_set_enabled = stub_overlay_set_enabled,
    .overlay_toggle = stub_overlay_toggle,
    .overlay_toggle_labels = stub_overlay_toggle_flag,
    .overlay_toggle_objects = stub_overlay_toggle_flag,
    .overlay_toggle_trails = stub_overlay_toggle_flag,
    .overlay_toggle_heatmap = stub_overlay_toggle_flag,
    .overlay_cycle_heatmap_colormap = stub_overlay_cycle_heatmap_colormap,
    .overlay_adjust_heatmap_bands = stub_overlay_adjust_heatmap_bands,
    .path = "(none)",
};

/* ── loader state (process singleton) ─────────────────────────────────────── */

static mp_static_mutex load_lock = MP_STATIC_MUTEX_INITIALIZER;
static enum { LOAD_UNTRIED, LOAD_OK, LOAD_FAILED } load_state = LOAD_UNTRIED;
static struct orender_dl table;
static char load_error[512];

static void set_error(const char *fmt, ...) MP_PRINTF_ATTRIBUTE(1, 2);
static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(load_error, sizeof(load_error), fmt, ap);
    va_end(ap);
}

static char *get_exe_dir(void *ta)
{
#if defined(_WIN32)
    wchar_t wpath[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wpath, MP_ARRAY_SIZE(wpath));
    if (n == 0 || n >= MP_ARRAY_SIZE(wpath))
        return NULL;
    char *path = mp_to_utf8(ta, wpath);
#elif defined(__APPLE__)
    char buf[MAXPATHLEN];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return NULL;
    char *path = talloc_strdup(ta, buf);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    char *path = talloc_strdup(ta, buf);
#endif
    if (!path)
        return NULL;
    return bstrdup0(ta, mp_dirname(path));
}

/* Where the Studio distribution deploys the engine library for external
 * consumers (see the search-order comment at the top). */
static char *get_studio_lib_path(void *ta)
{
#if defined(_WIN32)
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0])
        return NULL;
    return talloc_asprintf(ta, "%s\\omniphony\\lib\\%s", base, ORENDER_LIB_NAME);
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    return talloc_asprintf(ta, "%s/Library/Application Support/omniphony/lib/%s",
                           home, ORENDER_LIB_NAME);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0])
        return talloc_asprintf(ta, "%s/omniphony/lib/%s", xdg, ORENDER_LIB_NAME);
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    return talloc_asprintf(ta, "%s/.local/share/omniphony/lib/%s",
                           home, ORENDER_LIB_NAME);
#endif
}

/* Try one candidate. On success fills `table` and returns true; on failure
 * logs why (verbose for "not there", warn for "there but unusable"), records
 * the reason, closes the handle, and returns false. */
static bool try_load(struct mp_log *log, const char *path, const char *origin)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *err = dlerror();
        mp_msg(log, MSGL_V, "orender: no library at %s candidate '%s'%s%s\n",
               origin, path, err ? ": " : "", err ? err : "");
        set_error("%s: not loadable (%s)", path, err ? err : "unknown error");
        return false;
    }

    uint32_t (*vmajor)(void) = (uint32_t (*)(void))dlsym(h, "orender_version_major");
    uint32_t (*vminor)(void) = (uint32_t (*)(void))dlsym(h, "orender_version_minor");
    if (!vmajor || !vminor) {
        mp_msg(log, MSGL_WARN, "orender: rejecting '%s' (%s): no version "
               "handshake — liborender predates ABI reporting\n", path, origin);
        set_error("%s: no version handshake", path);
        lib_close(h);
        return false;
    }
    uint32_t abi_major = vmajor(), abi_minor = vminor();
    if (abi_major != ORENDER_ABI_MAJOR) {
        mp_msg(log, MSGL_WARN, "orender: rejecting '%s' (%s): ABI major %u, "
               "this mpv needs %u\n", path, origin, (unsigned)abi_major,
               (unsigned)ORENDER_ABI_MAJOR);
        set_error("%s: ABI major %u incompatible with %u", path,
                  (unsigned)abi_major, (unsigned)ORENDER_ABI_MAJOR);
        lib_close(h);
        return false;
    }

    /* Seed with stubs so every optional slot is callable, then overwrite. */
    struct orender_dl t = stubs;
    t.abi_major = abi_major;
    t.abi_minor = abi_minor;

    const struct { const char *name; size_t offset; } required[] = {
#define REQ(field, sym) {sym, offsetof(struct orender_dl, field)}
        REQ(create,           "orender_create"),
        REQ(destroy,          "orender_destroy"),
        REQ(process,          "orender_process"),
        REQ(reset,            "orender_reset"),
        REQ(channel_count,    "orender_channel_count"),
        REQ(channel_layout,   "orender_channel_layout"),
        REQ(channel_mapping,  "orender_channel_mapping"),
        REQ(channel_mode,     "orender_channel_mode"),
        REQ(set_channel_mode, "orender_set_channel_mode"),
        REQ(object_count,     "orender_object_count"),
        REQ(dialnorm_db,      "orender_dialnorm_db"),
        REQ(bed_layout,       "orender_bed_layout"),
#undef REQ
    };
    for (int i = 0; i < MP_ARRAY_SIZE(required); i++) {
        void *sym = dlsym(h, required[i].name);
        if (!sym) {
            mp_msg(log, MSGL_WARN, "orender: rejecting '%s' (%s): missing "
                   "required symbol %s\n", path, origin, required[i].name);
            set_error("%s: missing required symbol %s", path, required[i].name);
            lib_close(h);
            return false;
        }
        memcpy((char *)&t + required[i].offset, &sym, sizeof(sym));
    }

    /* Optional symbols: resolve individually, keep the stub when absent. An
     * older engine simply lacks the newer features — never a reason to reject. */
    const struct { const char *name; size_t offset; } optional[] = {
#define OPT_SYM(field, sym) {sym, offsetof(struct orender_dl, field)}
        /* Alias first so the preferred name overwrites it when both exist. */
        OPT_SYM(has_objects,           "orender_is_spatial"),
        OPT_SYM(has_objects,           "orender_has_objects"),
        OPT_SYM(set_option,            "orender_set_option"),
        OPT_SYM(build_id,              "orender_build_id"),
        OPT_SYM(overlay_set_rendering, "orender_overlay_set_rendering"),
        OPT_SYM(overlay_clear,         "orender_overlay_clear"),
        OPT_SYM(overlay_ass,           "orender_overlay_ass"),
        OPT_SYM(overlay_heatmap_bgra,  "orender_overlay_heatmap_bgra"),
        OPT_SYM(overlay_set_enabled,   "orender_overlay_set_enabled"),
        OPT_SYM(overlay_toggle,        "orender_overlay_toggle"),
        OPT_SYM(overlay_toggle_labels, "orender_overlay_toggle_labels"),
        OPT_SYM(overlay_toggle_objects, "orender_overlay_toggle_objects"),
        OPT_SYM(overlay_toggle_trails, "orender_overlay_toggle_trails"),
        OPT_SYM(overlay_toggle_heatmap, "orender_overlay_toggle_heatmap"),
        OPT_SYM(overlay_cycle_heatmap_colormap,
                "orender_overlay_cycle_heatmap_colormap"),
        OPT_SYM(overlay_adjust_heatmap_bands,
                "orender_overlay_adjust_heatmap_bands"),
#undef OPT_SYM
    };
    int missing_optional = 0;
    for (int i = 0; i < MP_ARRAY_SIZE(optional); i++) {
        void *sym = dlsym(h, optional[i].name);
        if (sym) {
            memcpy((char *)&t + optional[i].offset, &sym, sizeof(sym));
        } else {
            missing_optional++;
            mp_msg(log, MSGL_V, "orender: '%s' lacks optional symbol %s "
                   "(feature disabled)\n", path, optional[i].name);
        }
    }
    /* The overlay client needs the whole group; the two calls the decoder
     * makes (set_rendering/clear) stay safe through the stubs either way. */
    t.have_overlay = t.overlay_ass != stub_overlay_ass &&
                     t.overlay_set_enabled != stub_overlay_set_enabled &&
                     t.overlay_toggle != stub_overlay_toggle;
    t.have_set_option = t.set_option != stub_set_option;
    t.path = talloc_strdup(NULL, path);   // process singleton; never freed

    table = t;

    const char *build = t.build_id();
    mp_msg(log, MSGL_INFO, "orender: loaded liborender ABI %u.%u%s%s from %s (%s)%s\n",
           (unsigned)abi_major, (unsigned)abi_minor,
           build ? " " : "", build ? build : "",
           t.path, origin,
           missing_optional ? " — older engine, some optional features off" : "");
    return true;
}

const struct orender_dl *orender_dl_get(struct mpv_global *global,
                                        struct mp_log *log)
{
    mp_mutex_lock(&load_lock);
    if (load_state != LOAD_UNTRIED) {
        const struct orender_dl *r = load_state == LOAD_OK ? &table : NULL;
        mp_mutex_unlock(&load_lock);
        return r;
    }

    void *tmp = talloc_new(NULL);
    bool ok = false;

    struct ad_orender_params *opts =
        mp_get_config_group(tmp, global, &ad_orender_conf);

    /* 1-2: explicit user intent — an unusable library here is an error the
     * user must see, not something to silently paper over with a fallback. */
    const char *explicit_path = NULL, *explicit_origin = NULL;
    if (opts->library_path && opts->library_path[0]) {
        explicit_path = opts->library_path;
        explicit_origin = "--ad-orender-library";
    } else {
        const char *env = getenv("ORENDER_LIBRARY");
        if (env && env[0]) {
            explicit_path = env;
            explicit_origin = "$ORENDER_LIBRARY";
        }
    }
    if (explicit_path) {
        ok = try_load(log, explicit_path, explicit_origin);
        if (!ok)
            mp_msg(log, MSGL_ERR, "orender: %s points at an unusable library "
                   "(%s); spatial audio disabled\n", explicit_origin, load_error);
    } else {
        /* 3-5: implicit locations — fall through on any failure. */
        const char *studio = get_studio_lib_path(tmp);
        if (studio)
            ok = try_load(log, studio, "studio install");

        if (!ok) {
            const char *exe_dir = get_exe_dir(tmp);
            if (exe_dir) {
                ok = try_load(log, mp_path_join(tmp, exe_dir, ORENDER_LIB_NAME),
                              "next to mpv");
            }
        }

#if !defined(_WIN32)
        if (!ok)
            ok = try_load(log, ORENDER_LIB_NAME, "system loader");
#endif

        if (!ok)
            mp_msg(log, MSGL_V, "orender: no usable liborender found (last: "
                   "%s); spatial audio unavailable\n", load_error);
    }

    talloc_free(tmp);
    if (ok) {
        load_error[0] = '\0';
        load_state = LOAD_OK;
    } else {
        load_state = LOAD_FAILED;
    }
    const struct orender_dl *r = ok ? &table : NULL;
    mp_mutex_unlock(&load_lock);
    return r;
}

const struct orender_dl *orender_dl_stubs(void)
{
    return &stubs;
}

const char *orender_dl_error(void)
{
    return load_error;
}
