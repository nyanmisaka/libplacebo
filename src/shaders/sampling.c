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
 * License along with libplacebo. If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include "shaders.h"

const struct pl_deband_params pl_deband_default_params = {
    .iterations = 1,
    .threshold  = 4.0,
    .radius     = 16.0,
    .grain      = 6.0,
};

void pl_shader_deband(struct pl_shader *sh, const struct ra_tex *ra_tex,
                      const struct pl_deband_params *params)
{
    if (!sh_require(sh, PL_SHADER_SIG_NONE, ra_tex->params.w, ra_tex->params.h))
        return;

    GLSL("vec4 color;\n");
    GLSL("// pl_shader_deband\n");
    GLSL("{\n");

    ident_t tex, pos, pt;
    tex = sh_bind(sh, ra_tex, "deband", NULL, &pos, NULL, &pt);
    if (!tex)
        return;

    GLSL("vec2 pos = %s;\n", pos);

    // Initialize the PRNG. This is friendly for wide usage and returns in
    // a very pleasant-looking distribution across frames even if the difference
    // between input coordinates is very small. Shamelessly stolen from some
    // GLSL tricks forum post years from a decade ago.
    ident_t random = sh_fresh(sh, "random"), permute = sh_fresh(sh, "permute");
    GLSLH("float %s(float x) {                          \n"
          "    x = (34.0 * x + 1.0) * x;                \n"
          "    return x - floor(x * 1.0/289.0) * 289.0; \n" // mod 289
          "}                                            \n"
          "float %s(inout float state) {                \n"
          "    state = %s(state);                       \n"
          "    return fract(state * 1.0/41.0);          \n"
          "}\n", permute, random, permute);

    ident_t seed = sh_var(sh, (struct pl_shader_var) {
        .var  = ra_var_float("seed"),
        .data = &params->seed,
    });

    GLSL("vec3 _m = vec3(pos, %s) + vec3(1.0);         \n"
         "float prng = %s(%s(%s(_m.x) + _m.y) + _m.z); \n"
         "vec4 avg, diff;                              \n"
         "color = texture(%s, pos);                    \n",
         seed, permute, permute, permute, tex);

    // Helper function: Compute a stochastic approximation of the avg color
    // around a pixel, given a specified radius
    ident_t average = sh_fresh(sh, "average");
    GLSLH("vec4 %s(vec2 pos, float range, inout float prng) {   \n"
          // Compute a random angle and distance
          "    float dist = %s(prng) * range;                   \n"
          "    float dir  = %s(prng) * %f;                      \n"
          "    vec2 o = dist * vec2(cos(dir), sin(dir));        \n"
          // Sample at quarter-turn intervals around the source pixel
          "    vec4 sum = vec4(0.0);                            \n"
          "    sum += texture(%s, pos + %s * vec2( o.x,  o.y)); \n"
          "    sum += texture(%s, pos + %s * vec2(-o.x,  o.y)); \n"
          "    sum += texture(%s, pos + %s * vec2(-o.x, -o.y)); \n"
          "    sum += texture(%s, pos + %s * vec2( o.x, -o.y)); \n"
          // Return the (normalized) average
          "    return 0.25 * sum;                               \n"
          "}\n", average, random, random, M_PI * 2,
          tex, pt, tex, pt, tex, pt, tex, pt);

    // For each iteration, compute the average at a given distance and
    // pick it instead of the color if the difference is below the threshold.
    for (int i = 1; i <= params->iterations; i++) {
        GLSL("avg = %s(pos, %f, prng);                              \n"
             "diff = abs(color - avg);                              \n"
             "color = mix(avg, color, greaterThan(diff, vec4(%f))); \n",
             average, i * params->radius, params->threshold / (1000 * i));
    }

    // Add some random noise to smooth out residual differences
    if (params->grain > 0) {
        GLSL("vec3 noise = vec3(%s(prng), %s(prng), %s(prng)); \n"
             "color.rgb += %f * (noise - vec3(0.5));           \n",
             random, random, random, params->grain / 1000.0);
    }

    GLSL("}\n");
}

static bool filter_compat(const struct pl_filter *filter, float inv_scale,
                          int lut_entries,
                          const struct pl_sample_polar_params *params)
{
    if (!filter)
        return false;
    if (filter->params.lut_entries != lut_entries)
        return false;
    if (fabs(filter->params.filter_scale - inv_scale) > 1e-3)
        return false;

    return pl_filter_config_eq(&filter->params.config, &params->filter);
}

// Subroutine for computing and adding an individual texel contribution
// If planar is false, samples directly
// If planar is true, takes the pixel from inX[idx] where X is the component and
// `idx` must be defined by the caller
static void polar_sample(struct pl_shader *sh, const struct pl_filter *filter,
                         ident_t tex, ident_t lut, ident_t lut_pos,
                         int x, int y, int comps, bool planar)
{
    // Since we can't know the subpixel position in advance, assume a
    // worst case scenario
    int yy = y > 0 ? y-1 : y;
    int xx = x > 0 ? x-1 : x;
    float dmax = sqrt(xx*xx + yy*yy);
    // Skip samples definitely outside the radius
    if (dmax >= filter->radius_cutoff)
        return;

    GLSL("d = length(vec2(%d.0, %d.0) - fcoord);\n", x, y);
    // Check for samples that might be skippable
    bool maybe_skippable = dmax >= filter->radius_cutoff - M_SQRT2;
    if (maybe_skippable)
        GLSL("if (d < %f) {\n", filter->radius_cutoff);

    // Get the weight for this pixel
    GLSL("w = texture(%s, %s(d * 1.0/%f)).r; \n"
         "wsum += w;                        \n",
         lut, lut_pos, filter->radius);

    if (planar) {
        for (int n = 0; n < comps; n++)
            GLSL("color[%d] += w * in%d[idx];\n", n, n);
    } else {
        GLSL("in0 = texture(%s, base + pt * vec2(%d.0, %d.0)); \n"
             "color += vec4(w) * in0);                         \n",
             tex, x, y);
    }

    if (maybe_skippable)
        GLSL("}\n");
}

bool pl_shader_sample_polar(struct pl_shader *sh, const struct pl_sample_src *src,
                            const struct pl_sample_polar_params *params)
{
    if (!params->filter.polar) {
        PL_ERR(sh, "Trying to use polar sampling with a non-polar filter?");
        return false;
    }

    const struct ra *ra = sh->ra;
    const struct ra_tex *tex = src->tex;
    assert(ra && tex);

    int comps = PL_DEF(src->components, tex->params.format->num_components);
    float src_w = pl_rect_w(src->rect), src_h = pl_rect_h(src->rect);
    src_w = PL_DEF(src_w, tex->params.w);
    src_h = PL_DEF(src_h, tex->params.h);

    int out_w = PL_DEF(src->new_w, src_w),
        out_h = PL_DEF(src->new_h, src_h);
    float ratio_x = out_w / src_w,
          ratio_y = out_h / src_h;

    if (!sh_require(sh, PL_SHADER_SIG_NONE, out_w, out_h))
        return false;
    if (!sh_require_obj(sh, params->lut, PL_SHADER_OBJ_LUT))
        return false;

    struct pl_shader_obj *lut = *params->lut;
    int lut_entries = PL_DEF(params->lut_entries, 64);
    float inv_scale = 1.0 / PL_MIN(ratio_x, ratio_y);
    inv_scale = PL_MAX(inv_scale, 1.0);

    if (ra->limits.max_tex_1d_dim < lut_entries) {
        PL_ERR(sh, "LUT of size %d exceeds the max 1D texture dimension (%d)",
               lut_entries, ra->limits.max_tex_1d_dim);
        return false;
    }

    if (!lut->tex || !filter_compat(lut->filter, inv_scale, lut_entries, params))
    {
        const struct ra_fmt *fmt = ra_find_fmt(ra, RA_FMT_FLOAT, 1, 32, true,
                                               RA_FMT_CAP_SAMPLEABLE |
                                               RA_FMT_CAP_LINEAR);
        if (!fmt) {
            PL_WARN(sh, "Found no matching texture format for polar LUT");
            return false;
        }

        PL_INFO(sh, "Recreating polar filter LUT");
        pl_filter_free(&lut->filter);
        lut->filter = pl_filter_generate(sh->ctx, &(struct pl_filter_params) {
            .config         = params->filter,
            .lut_entries    = lut_entries,
            .filter_scale   = inv_scale,
            .cutoff         = PL_DEF(params->cutoff, 0.001),
        });

        if (!lut->filter) {
            // This should never happen, but just in case ..
            PL_ERR(sh, "Failed initializing polar filter!");
            return false;
        }

        lut->tex = ra_tex_create(ra, &(struct ra_tex_params) {
            .w              = lut_entries,
            .format         = fmt,
            .sampleable     = true,
            .sample_mode    = RA_TEX_SAMPLE_LINEAR,
            .address_mode   = RA_TEX_ADDRESS_CLAMP,
            .initial_data   = lut->filter->weights,
        });

        if (!lut->tex) {
            PL_ERR(sh, "Failed creating polar LUT texture!");
            return false;
        }
    }

    assert(lut->filter && lut->tex);
    ident_t lut_tex = sh_desc(sh, (struct pl_shader_desc) {
        .desc = {
            .name = "polar_lut",
            .type = RA_DESC_SAMPLED_TEX,
        },
        .object = lut->tex,
    });

    struct pl_rect2df rect = {
        .x0 = src->rect.x0,
        .y0 = src->rect.y0,
        .x1 = src->rect.x0 + src_w,
        .y1 = src->rect.x0 + src_h,
    };

    ident_t pos, size, pt;
    ident_t src_tex = sh_bind(sh, tex, "src_tex", &rect, &pos, &size, &pt);
    ident_t lut_pos = sh_lut_pos(sh, lut_entries);

    GLSL("// pl_shader_sample_polar          \n"
         "vec4 color = vec4(0.0);            \n"
         "{                                  \n"
         "vec2 pos = %s, size = %s, pt = %s; \n"
         "float w, d, wsum = 0.0;            \n"
         "int idx;                           \n"
         "vec4 c;                            \n",
         pos, size, pt);

    int bound   = ceil(lut->filter->radius_cutoff);
    int offset  = bound - 1; // padding top/left
    int padding = offset + bound; // total padding

    // For performance we want to load at least as many pixels horizontally as
    // there are threads in a warp, as well as enough to take advantage of
    // shmem parallelism. However, on the other hand, to hide latency we want
    // to avoid making the kernel too large. A good size overall is 256
    // threads, which allows at least 8 to run in parallel assuming good VGPR
    // distribution. A good trade-off for the horizontal row size is 32, which
    // is the warp size on nvidia. Going up to 64 (AMD's wavefront size)
    // is not worth it even on AMD hardware.
    const int bw = 32, bh = 256 / bw;

    // We need to sample everything from base_min to base_max, so make sure
    // we have enough room in shmem
    int iw = (int) ceil(bw / ratio_x) + padding + 1,
        ih = (int) ceil(bh / ratio_y) + padding + 1;

    int shmem_req = iw * ih * comps * sizeof(float);
    if (sh_try_compute(sh, bw, bh, false, shmem_req)) {
        // Compute shader kernel
        GLSL("vec2 wpos = %s_map(gl_WorkGroupID * gl_WorkGroupSize);        \n"
             "vec2 wbase = wpos - pt * fract(wpos * size - vec2(0.5));      \n"
             "vec2 fcoord = fract(pos * size - vec2(0.5));                  \n"
             "vec2 base = pos - pt * fcoord;                                \n"
             "ivec2 rel = ivec2(round((base - wbase) * size));              \n",
             pos);

        // Load all relevant texels into shmem
        GLSL("for (int y = int(gl_LocalInvocationID.y); y < %d; y += %d) {  \n"
             "for (int x = int(gl_LocalInvocationID.x); x < %d; x += %d) {  \n"
             "c = texture(%s, wbase + pt * vec2(x - %d, y - %d));           \n",
             iw, bh, iw, bw, src_tex, offset, offset);

        for (int c = 0; c < comps; c++) {
            GLSLH("shared float in%d[%d];   \n", c, ih * iw);
            GLSL("in%d[%d * y + x] = c[%d]; \n", c, iw, c);
        }

        GLSL("}}                    \n"
             "groupMemoryBarrier(); \n"
             "barrier();            \n");

        // Dispatch the actual samples
        for (int y = 1 - bound; y <= bound; y++) {
            for (int x = 1 - bound; x <= bound; x++) {
                GLSL("idx = %d * rel.y + rel.x + %d;\n",
                     iw, iw * (y + offset) + x + offset);
                polar_sample(sh, lut->filter, src_tex, lut_tex, lut_pos, x, y,
                             comps, true);
            }
        }
    } else {
        // Fragment shader sampling
        abort(); // TODO
    }

    GLSL("color = color / vec4(wsum); \n"
         "}");
    return true;
}
