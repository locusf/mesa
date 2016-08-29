/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "isl/isl.h"
#include "intel_resolve_map.h" /* needed for enum gen6_hiz_op */
#include "intel_bufmgr.h" /* needed for drm_intel_bo */

struct brw_context;
struct brw_wm_prog_key;

#ifdef __cplusplus
extern "C" {
#endif

struct brw_blorp_surf
{
   const struct isl_surf *surf;
   drm_intel_bo *bo;
   uint32_t offset;

   const struct isl_surf *aux_surf;
   drm_intel_bo *aux_bo;
   uint32_t aux_offset;
   enum isl_aux_usage aux_usage;

   union isl_color_value clear_color;
};

void
brw_blorp_blit(struct brw_context *brw,
               const struct brw_blorp_surf *src_surf,
               unsigned src_level, unsigned src_layer,
               enum isl_format src_format, int src_swizzle,
               const struct brw_blorp_surf *dst_surf,
               unsigned dst_level, unsigned dst_layer,
               enum isl_format dst_format,
               float src_x0, float src_y0,
               float src_x1, float src_y1,
               float dst_x0, float dst_y0,
               float dst_x1, float dst_y1,
               uint32_t filter, bool mirror_x, bool mirror_y);

void
blorp_fast_clear(struct brw_context *brw,
                 const struct brw_blorp_surf *surf,
                 uint32_t level, uint32_t layer,
                 uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);

void
blorp_clear(struct brw_context *brw,
            const struct brw_blorp_surf *surf,
            uint32_t level, uint32_t layer,
            uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
            enum isl_format format, union isl_color_value clear_color,
            bool color_write_disable[4]);

void
brw_blorp_ccs_resolve(struct brw_context *brw, struct brw_blorp_surf *surf,
                      enum isl_format format);

void
blorp_gen6_hiz_op(struct brw_context *brw, struct brw_blorp_surf *surf,
                  unsigned level, unsigned layer, enum gen6_hiz_op op);

#ifdef __cplusplus
} /* end extern "C" */
#endif /* __cplusplus */
