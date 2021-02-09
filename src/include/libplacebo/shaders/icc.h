/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBPLACEBO_SHADERS_ICC_H_
#define LIBPLACEBO_SHADERS_ICC_H_

// Functions for generating and applying ICC-derived 3DLUTs

#include <libplacebo/colorspace.h>
#include <libplacebo/shaders.h>

// ICC profiles

struct pl_icc_params {
    // The rendering intent to use when computing the color transformation. A
    // recommended value is PL_INTENT_RELATIVE_COLORIMETRIC for color-accurate
    // video reproduction, or PL_INTENT_PERCEPTUAL for profiles containing
    // meaningful perceptual mapping tables.
    enum pl_rendering_intent intent;

    // The size of the 3DLUT to generate. If left as NULL, these individually
    // default to 64, which is the recommended default for all three.
    size_t size_r, size_g, size_b;
};

extern const struct pl_icc_params pl_icc_default_params;

struct pl_icc_color_space {
    // The nominal, closest approximation representation of the color profile,
    // as permitted by `pl_color_space` enums. This will be used as a fallback
    // in the event that an ICC profile is absent, or that parsing the ICC
    // profile fails. This is also that will be returned for the corresponding
    // field in `pl_icc_result` when the ICC profile is in use.
    struct pl_color_space color;

    // The ICC profile itself. (Optional)
    struct pl_icc_profile profile;
};

struct pl_icc_result {
    // The source color space. This is the color space that the colors should
    // actually be in at the point in time that they're ingested by the 3DLUT.
    // This may differ from the `pl_color_space color` specified in the
    // `pl_icc_color_space`. Users should make sure to apply
    // `pl_shader_color_map` in order to get the colors into this format before
    // applying `pl_icc_apply`.
    //
    // Note: `pl_shader_color_map` is a no-op when the source and destination
    // color spaces are the same, so this can safely be used without disturbing
    // the colors in the event that an ICC profile is actually in use.
    struct pl_color_space src_color;

    // The destination color space. This is the color space that the colors
    // will (nominally) be in at the time they exit the 3DLUT.
    struct pl_color_space dst_color;
};

// Updates/generates a 3DLUT based on ICC profiles. Returns success. If true,
// `out` will be updated to a struct describing the color space chosen for the
// input and output of the 3DLUT. (See `pl_icc_color_space`) If `params` is
// NULL, it defaults to &pl_icc_default_params.
//
// Note: This function must always be called before `pl_icc_apply`, on the
// same `pl_shader` object, The only reason it's separate from `pl_icc_apply`
// is to give users a chance to adapt the input colors to the color space
// chosen by the ICC profile before applying it.
bool pl_icc_update(struct pl_shader *sh,
                   const struct pl_icc_color_space *src,
                   const struct pl_icc_color_space *dst,
                   struct pl_shader_obj **icc,
                   struct pl_icc_result *out,
                   const struct pl_icc_params *params);

// Actually applies a 3DLUT as generated by `pl_icc_update`. The reason this is
// separated from `pl_icc_update` is so that the user has the chance to
// correctly map the colors into the specified `src_color` space. This should
// be called only on the `pl_shader_obj` previously updated by `pl_icc_update`,
// and only when that function returned true.
void pl_icc_apply(struct pl_shader *sh, struct pl_shader_obj **icc);

// Backwards compatibility aliases
#define pl_3dlut_params pl_icc_params
#define pl_3dlut_default_params pl_icc_default_params
#define pl_3dlut_profile pl_icc_color_space
#define pl_3dlut_result pl_icc_result

static PL_DEPRECATED inline bool pl_3dlut_update(struct pl_shader *sh,
                                                 const struct pl_icc_color_space *src,
                                                 const struct pl_icc_color_space *dst,
                                                 struct pl_shader_obj **lut3d,
                                                 struct pl_icc_result *out,
                                                 const struct pl_icc_params *params)
{
    return pl_icc_update(sh, src, dst, lut3d, out, params);
}

static PL_DEPRECATED inline void pl_3dlut_apply(struct pl_shader *sh,
                                                struct pl_shader_obj **lut3d)
{
    return pl_icc_apply(sh, lut3d);
}

#endif // LIBPLACEBO_SHADERS_ICC_H_