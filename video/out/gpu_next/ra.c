#include "video/out/gpu_next/ra.h"
#include <libplacebo/renderer.h>           // for pl_frame, pl_plane, pl_ren...
#include <libplacebo/utils/upload.h>       // for pl_plane_data, pl_upload_p...
#include <stdint.h>                        // for uint64_t
#include <stdlib.h>                        // for NULL, abs
#include <string.h>                        // for memset
#include "common/common.h"                 // for MPSWAP, MPMAX
#include "common/msg.h"                    // for mp_msg, MSGL_DEBUG, MSGL_ERR
#include "libplacebo/colorspace.h"         // for pl_bit_encoding, pl_bit_en...
#include "libplacebo/gpu.h"                // for pl_tex, pl_fmt_type, pl_fi...
#include "libplacebo/log.h"                // for pl_log
#include "ta/ta_talloc.h"                  // for talloc_free, talloc_zero
#include "video/img_format.h"              // for mp_imgfmt_comp_desc, mp_im...
#include "video/mp_image.h"                // for mp_image, mp_image_params

/**
 * @brief Private state for the libplacebo rendering abstraction.
 *
 * This struct contains all the internal state for the `ra_pl` implementation.
 * The public `ra_next` struct must be the first member to allow for safe casting
 * from a `ra_next*` to a `ra_priv*`.
 */
struct ra_priv {
    struct ra_next pub;     // Public interface, must be the first member.
    pl_renderer renderer;   // The core libplacebo renderer instance.
};

/* --- New Abstraction Implementations --- */

bool ra_next_render_image(struct ra_next *ra, const struct pl_frame *src,
                     struct pl_frame *target, const struct pl_render_params *params)
{
    struct ra_priv *p = (struct ra_priv *)ra;
    if (!p->renderer) return false;
    return pl_render_image(p->renderer, src, target, params);
}

/**
 * @brief Public wrapper to clean up GPU resources associated with a pl_frame.
 * @param ra The rendering abstraction context.
 * @param frame The pl_frame whose textures should be destroyed.
 */
void ra_cleanup_pl_frame(struct ra_next *ra, struct pl_frame *frame)
{
    if (!frame)
        return;
    // Iterate over each plane and destroy its associated texture.
    for (int i = 0; i < frame->num_planes; i++)
        pl_tex_destroy(ra->gpu, &frame->planes[i].texture);
}

/**
 * @brief Translates an mpv image format enum into a libplacebo plane description.
 * This is a key translation layer between mpv's and libplacebo's data structures.
 * @param out_data The output array of `pl_plane_data` to be populated.
 * @param out_bits The output bit encoding information.
 * @param imgfmt The input mpv image format enum.
 * @param use_uint Whether to use unsigned integer formats.
 * @return The number of planes on success, 0 on failure.
 */
static int plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                                  struct pl_bit_encoding *out_bits,
                                  enum mp_imgfmt imgfmt, bool use_uint)
{
    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(imgfmt);
    if (!desc.num_planes || !(desc.flags & MP_IMGFLAG_HAS_COMPS))
        return 0;

    // Filter out unsupported or non-CPU formats.
    if (desc.flags & MP_IMGFLAG_HWACCEL || !(desc.flags & MP_IMGFLAG_NE) ||
        desc.flags & MP_IMGFLAG_PAL ||
        ((desc.flags & MP_IMGFLAG_TYPE_FLOAT) && (desc.flags & MP_IMGFLAG_YUV)))
        return 0;

    bool has_bits = false;
    bool any_padded = false;

    // Iterate through each plane of the mpv image format.
    for (int p = 0; p < desc.num_planes; p++) {
        struct pl_plane_data *data = &out_data[p];
        struct mp_imgfmt_comp_desc sorted[MP_NUM_COMPONENTS];
        int num_comps = 0;
        if (desc.bpp[p] % 8)
            return 0; // Require byte-aligned pixels.

        // Collect and sort all components belonging to the current plane.
        for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
            if (desc.comps[c].plane != p)
                continue;

            data->component_map[num_comps] = c;
            sorted[num_comps] = desc.comps[c];
            num_comps++;

            // Simple insertion sort to order components by memory offset.
            for (int i = num_comps - 1; i > 0; i--) {
                if (sorted[i].offset >= sorted[i - 1].offset)
                    break;
                MPSWAP(struct mp_imgfmt_comp_desc, sorted[i], sorted[i - 1]);
                MPSWAP(int, data->component_map[i], data->component_map[i - 1]);
            }
        }

        // Calculate component sizes, padding, and bit encoding for libplacebo.
        uint64_t total_bits = 0;
        memset(data->component_size, 0, sizeof(data->component_size));
        for (int c = 0; c < num_comps; c++) {
            data->component_size[c] = sorted[c].size;
            data->component_pad[c] = sorted[c].offset - total_bits;
            total_bits += data->component_pad[c] + data->component_size[c];
            any_padded |= sorted[c].pad;

            if (!out_bits || data->component_map[c] == 3) // PL_CHANNEL_A
                continue;

            struct pl_bit_encoding bits = {
                .sample_depth = data->component_size[c],
                .color_depth = sorted[c].size - abs(sorted[c].pad),
                .bit_shift = MPMAX(sorted[c].pad, 0),
            };

            // Ensure all components have the same bit encoding.
            if (!has_bits) {
                *out_bits = bits;
                has_bits = true;
            } else if (!pl_bit_encoding_equal(out_bits, &bits)) {
                *out_bits = (struct pl_bit_encoding){0};
                out_bits = NULL;
            }
        }

        data->pixel_stride = desc.bpp[p] / 8;
        data->type = (desc.flags & MP_IMGFLAG_TYPE_FLOAT)
                            ? PL_FMT_FLOAT
                            : (use_uint ? PL_FMT_UINT : PL_FMT_UNORM);
    }

    if (any_padded && !out_bits)
        return 0;

    return desc.num_planes;
}

/**
 * Uploads an `mp_image` to a `pl_frame`.
 * @param ra The rendering abstraction context.
 * @param out_frame The output `pl_frame`.
 * @param img The input `mp_image`.
 * @return True on success, false on failure.
 */
bool ra_upload_mp_image(struct ra_next *ra, struct pl_frame *out_frame,
                        const struct mp_image *img)
{
    if (!img || !out_frame)
        return false;

    // Initialize the frame with color space and crop metadata. Assign the fields this
    // function owns rather than replacing the struct: a compound literal here silently
    // cleared everything the caller had already set, and rotation -- which the VO
    // advertises via VO_CAP_ROTATE90 and libplacebo reads back off the frame -- went 3 -> 0,
    // so rotated video rendered upright on the software-decode path.
    out_frame->color = img->params.color;
    out_frame->repr = img->params.repr;
    out_frame->crop = (struct pl_rect2df){
        .x0 = 0, .y0 = 0,
        .x1 = img->w, .y1 = img->h,
    };

    // Convert the mpv format to libplacebo's plane data description.
    struct pl_plane_data data[4] = {0};
    int planes = plane_data_from_imgfmt(data, &out_frame->repr.bits, img->imgfmt, false);
    if (!planes) {
        mp_msg(ra->log, MSGL_ERR, "Failed to describe image format '%s'\n",
               mp_imgfmt_to_name(img->imgfmt));
        return false;
    }

    out_frame->num_planes = planes;

    // Upload each plane's pixel data to a new GPU texture.
    for (int n = 0; n < planes; n++) {
        data[n].width = mp_image_plane_w((struct mp_image *)img, n);
        data[n].height = mp_image_plane_h((struct mp_image *)img, n);
        data[n].row_stride = img->stride[n];
        data[n].pixels = img->planes[n];

        // Let libplacebo handle the texture creation and data upload.
        if (!pl_upload_plane(ra->gpu, &out_frame->planes[n],
                             &out_frame->planes[n].texture, &data[n]))
        {
            mp_msg(ra->log, MSGL_ERR, "Failed to upload mp_image plane %d\n", n);
            goto error;
        }
    }

    return true;

error:
    // Clean up any successfully created textures if one fails.
    ra_cleanup_pl_frame(ra, out_frame);
    return false;
}

/**
 * @brief Resets the renderer by flushing internal caches.
 * @param ra The rendering abstraction context.
 */
void ra_pl_reset(struct ra_next *ra)
{
    struct ra_priv *p = (struct ra_priv *)ra;
    if (p && p->renderer)
        pl_renderer_flush_cache(p->renderer);
}

/**
 * @brief Destroys the rendering abstraction context and all associated resources.
 * This is the corrected version that fixes the valgrind crash.
 * @param rap A pointer to the rendering abstraction context pointer. The pointer
 *            will be set to NULL after destruction to prevent use-after-free.
 */
void ra_pl_destroy(struct ra_next **rap)
{
    if (!rap || !*rap)
        return;

    // Correctly dereference the double pointer to get the struct pointer.
    struct ra_priv *p = (struct ra_priv *)*rap;

    // Destroy the renderer (if any)
    if (p->renderer) {
        mp_msg(p->pub.log, MSGL_DEBUG, "ra_pl_destroy: destroying renderer %p\n",
               (void*)p->renderer);
        pl_renderer_destroy(&p->renderer);
    }

    // Finally free the ra_priv block itself
    mp_msg(p->pub.log, MSGL_DEBUG, "ra_pl_destroy: freeing ra_priv %p\n", (void*)p);
    talloc_free(p);

    // Nullify the original pointer to prevent dangling pointers.
    *rap = NULL;
}

/**
 * @brief Creates a new libplacebo-based rendering abstraction context.
 * @param gpu The libplacebo GPU object to use.
 * @param log The parent mpv logging context.
 * @param log_pl The libplacebo logging context.
 * @return A new ra_next context on success, or NULL on failure.
 */
struct ra_next *ra_pl_create(pl_gpu gpu, struct mp_log *log, pl_log log_pl)
{
    // Allocate the private implementation struct.
    struct ra_priv *p = talloc_zero(NULL, struct ra_priv);
    struct ra_next *ra = &p->pub;

    // Initialize public members.
    ra->gpu = gpu;
    ra->log = mp_log_new(p, log, "ra-pl"); // Create a sub-logger for this module.
    // Create renderer (needed by the higher-level pl_render_image calls)
    p->renderer = pl_renderer_create(log_pl, gpu);
    if (!p->renderer) {
        talloc_free(p);
        return NULL;
    }

    return ra;
}
