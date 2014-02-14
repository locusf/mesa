/*
 Copyright (C) Intel Corp.  2006.  All Rights Reserved.
 Intel funded Tungsten Graphics (http://www.tungstengraphics.com) to
 develop this 3D driver.
 
 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:
 
 The above copyright notice and this permission notice (including the
 next paragraph) shall be included in all copies or substantial
 portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 
 **********************************************************************/
 /*
  * Authors:
  *   Keith Whitwell <keith@tungstengraphics.com>
  */
                   

#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "intel_mipmap_tree.h"

#include "main/macros.h"
#include "main/samplerobj.h"


/* Samplers aren't strictly wm state from the hardware's perspective,
 * but that is the only situation in which we use them in this driver.
 */



uint32_t
translate_wrap_mode(GLenum wrap, bool using_nearest)
{
   switch( wrap ) {
   case GL_REPEAT: 
      return BRW_TEXCOORDMODE_WRAP;
   case GL_CLAMP:
      /* GL_CLAMP is the weird mode where coordinates are clamped to
       * [0.0, 1.0], so linear filtering of coordinates outside of
       * [0.0, 1.0] give you half edge texel value and half border
       * color.  The fragment shader will clamp the coordinates, and
       * we set clamp_border here, which gets the result desired.  We
       * just use clamp(_to_edge) for nearest, because for nearest
       * clamping to 1.0 gives border color instead of the desired
       * edge texels.
       */
      if (using_nearest)
	 return BRW_TEXCOORDMODE_CLAMP;
      else
	 return BRW_TEXCOORDMODE_CLAMP_BORDER;
   case GL_CLAMP_TO_EDGE: 
      return BRW_TEXCOORDMODE_CLAMP;
   case GL_CLAMP_TO_BORDER: 
      return BRW_TEXCOORDMODE_CLAMP_BORDER;
   case GL_MIRRORED_REPEAT: 
      return BRW_TEXCOORDMODE_MIRROR;
   case GL_MIRROR_CLAMP_TO_EDGE:
      return BRW_TEXCOORDMODE_MIRROR_ONCE;
   default: 
      return BRW_TEXCOORDMODE_WRAP;
   }
}

/**
 * Upload SAMPLER_BORDER_COLOR_STATE.
 */
void
upload_default_color(struct brw_context *brw,
                     struct gl_sampler_object *sampler,
                     int unit,
                     uint32_t *sdc_offset)
{
   struct gl_context *ctx = &brw->ctx;
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   struct gl_texture_object *texObj = texUnit->_Current;
   struct gl_texture_image *firstImage = texObj->Image[0][texObj->BaseLevel];
   float color[4];

   switch (firstImage->_BaseFormat) {
   case GL_DEPTH_COMPONENT:
      /* GL specs that border color for depth textures is taken from the
       * R channel, while the hardware uses A.  Spam R into all the
       * channels for safety.
       */
      color[0] = sampler->BorderColor.f[0];
      color[1] = sampler->BorderColor.f[0];
      color[2] = sampler->BorderColor.f[0];
      color[3] = sampler->BorderColor.f[0];
      break;
   case GL_ALPHA:
      color[0] = 0.0;
      color[1] = 0.0;
      color[2] = 0.0;
      color[3] = sampler->BorderColor.f[3];
      break;
   case GL_INTENSITY:
      color[0] = sampler->BorderColor.f[0];
      color[1] = sampler->BorderColor.f[0];
      color[2] = sampler->BorderColor.f[0];
      color[3] = sampler->BorderColor.f[0];
      break;
   case GL_LUMINANCE:
      color[0] = sampler->BorderColor.f[0];
      color[1] = sampler->BorderColor.f[0];
      color[2] = sampler->BorderColor.f[0];
      color[3] = 1.0;
      break;
   case GL_LUMINANCE_ALPHA:
      color[0] = sampler->BorderColor.f[0];
      color[1] = sampler->BorderColor.f[0];
      color[2] = sampler->BorderColor.f[0];
      color[3] = sampler->BorderColor.f[3];
      break;
   default:
      color[0] = sampler->BorderColor.f[0];
      color[1] = sampler->BorderColor.f[1];
      color[2] = sampler->BorderColor.f[2];
      color[3] = sampler->BorderColor.f[3];
      break;
   }

   /* In some cases we use an RGBA surface format for GL RGB textures,
    * where we've initialized the A channel to 1.0.  We also have to set
    * the border color alpha to 1.0 in that case.
    */
   if (firstImage->_BaseFormat == GL_RGB)
      color[3] = 1.0;

   if (brw->gen == 5 || brw->gen == 6) {
      struct gen5_sampler_default_color *sdc;

      sdc = brw_state_batch(brw, AUB_TRACE_SAMPLER_DEFAULT_COLOR,
			    sizeof(*sdc), 32, sdc_offset);

      memset(sdc, 0, sizeof(*sdc));

      UNCLAMPED_FLOAT_TO_UBYTE(sdc->ub[0], color[0]);
      UNCLAMPED_FLOAT_TO_UBYTE(sdc->ub[1], color[1]);
      UNCLAMPED_FLOAT_TO_UBYTE(sdc->ub[2], color[2]);
      UNCLAMPED_FLOAT_TO_UBYTE(sdc->ub[3], color[3]);

      UNCLAMPED_FLOAT_TO_USHORT(sdc->us[0], color[0]);
      UNCLAMPED_FLOAT_TO_USHORT(sdc->us[1], color[1]);
      UNCLAMPED_FLOAT_TO_USHORT(sdc->us[2], color[2]);
      UNCLAMPED_FLOAT_TO_USHORT(sdc->us[3], color[3]);

      UNCLAMPED_FLOAT_TO_SHORT(sdc->s[0], color[0]);
      UNCLAMPED_FLOAT_TO_SHORT(sdc->s[1], color[1]);
      UNCLAMPED_FLOAT_TO_SHORT(sdc->s[2], color[2]);
      UNCLAMPED_FLOAT_TO_SHORT(sdc->s[3], color[3]);

      sdc->hf[0] = _mesa_float_to_half(color[0]);
      sdc->hf[1] = _mesa_float_to_half(color[1]);
      sdc->hf[2] = _mesa_float_to_half(color[2]);
      sdc->hf[3] = _mesa_float_to_half(color[3]);

      sdc->b[0] = sdc->s[0] >> 8;
      sdc->b[1] = sdc->s[1] >> 8;
      sdc->b[2] = sdc->s[2] >> 8;
      sdc->b[3] = sdc->s[3] >> 8;

      sdc->f[0] = color[0];
      sdc->f[1] = color[1];
      sdc->f[2] = color[2];
      sdc->f[3] = color[3];
   } else {
      struct brw_sampler_default_color *sdc;

      sdc = brw_state_batch(brw, AUB_TRACE_SAMPLER_DEFAULT_COLOR,
			    sizeof(*sdc), 32, sdc_offset);

      COPY_4V(sdc->color, color);
   }
}

/**
 * Sets the sampler state for a single unit based off of the sampler key
 * entry.
 */
static void brw_update_sampler_state(struct brw_context *brw,
				     int unit,
                                     int ss_index,
                                     struct brw_sampler_state *sampler,
                                     uint32_t sampler_state_table_offset,
                                     uint32_t *sdc_offset)
{
   struct gl_context *ctx = &brw->ctx;
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   struct gl_texture_object *texObj = texUnit->_Current;
   struct gl_sampler_object *gl_sampler = _mesa_get_samplerobj(ctx, unit);
   bool using_nearest = false;

   /* These don't use samplers at all. */
   if (texObj->Target == GL_TEXTURE_BUFFER)
      return;

   switch (gl_sampler->MinFilter) {
   case GL_NEAREST:
      sampler->ss0.min_filter = BRW_MAPFILTER_NEAREST;
      sampler->ss0.mip_filter = BRW_MIPFILTER_NONE;
      using_nearest = true;
      break;
   case GL_LINEAR:
      sampler->ss0.min_filter = BRW_MAPFILTER_LINEAR;
      sampler->ss0.mip_filter = BRW_MIPFILTER_NONE;
      break;
   case GL_NEAREST_MIPMAP_NEAREST:
      sampler->ss0.min_filter = BRW_MAPFILTER_NEAREST;
      sampler->ss0.mip_filter = BRW_MIPFILTER_NEAREST;
      break;
   case GL_LINEAR_MIPMAP_NEAREST:
      sampler->ss0.min_filter = BRW_MAPFILTER_LINEAR;
      sampler->ss0.mip_filter = BRW_MIPFILTER_NEAREST;
      break;
   case GL_NEAREST_MIPMAP_LINEAR:
      sampler->ss0.min_filter = BRW_MAPFILTER_NEAREST;
      sampler->ss0.mip_filter = BRW_MIPFILTER_LINEAR;
      break;
   case GL_LINEAR_MIPMAP_LINEAR:
      sampler->ss0.min_filter = BRW_MAPFILTER_LINEAR;
      sampler->ss0.mip_filter = BRW_MIPFILTER_LINEAR;
      break;
   default:
      break;
   }

   /* Set Anisotropy: 
    */
   if (gl_sampler->MaxAnisotropy > 1.0) {
      sampler->ss0.min_filter = BRW_MAPFILTER_ANISOTROPIC; 
      sampler->ss0.mag_filter = BRW_MAPFILTER_ANISOTROPIC;

      if (gl_sampler->MaxAnisotropy > 2.0) {
	 sampler->ss3.max_aniso = MIN2((gl_sampler->MaxAnisotropy - 2) / 2,
				       BRW_ANISORATIO_16);
      }
   }
   else {
      switch (gl_sampler->MagFilter) {
      case GL_NEAREST:
	 sampler->ss0.mag_filter = BRW_MAPFILTER_NEAREST;
	 using_nearest = true;
	 break;
      case GL_LINEAR:
	 sampler->ss0.mag_filter = BRW_MAPFILTER_LINEAR;
	 break;
      default:
	 break;
      }  
   }

   sampler->ss1.r_wrap_mode = translate_wrap_mode(gl_sampler->WrapR,
						  using_nearest);
   sampler->ss1.s_wrap_mode = translate_wrap_mode(gl_sampler->WrapS,
						  using_nearest);
   sampler->ss1.t_wrap_mode = translate_wrap_mode(gl_sampler->WrapT,
						  using_nearest);

   if (brw->gen >= 6 &&
       sampler->ss0.min_filter != sampler->ss0.mag_filter)
	sampler->ss0.min_mag_neq = 1;

   /* Cube-maps on 965 and later must use the same wrap mode for all 3
    * coordinate dimensions.  Futher, only CUBE and CLAMP are valid.
    */
   if (texObj->Target == GL_TEXTURE_CUBE_MAP ||
       texObj->Target == GL_TEXTURE_CUBE_MAP_ARRAY) {
      if ((ctx->Texture.CubeMapSeamless || gl_sampler->CubeMapSeamless) &&
	  (gl_sampler->MinFilter != GL_NEAREST ||
	   gl_sampler->MagFilter != GL_NEAREST)) {
	 sampler->ss1.r_wrap_mode = BRW_TEXCOORDMODE_CUBE;
	 sampler->ss1.s_wrap_mode = BRW_TEXCOORDMODE_CUBE;
	 sampler->ss1.t_wrap_mode = BRW_TEXCOORDMODE_CUBE;
      } else {
	 sampler->ss1.r_wrap_mode = BRW_TEXCOORDMODE_CLAMP;
	 sampler->ss1.s_wrap_mode = BRW_TEXCOORDMODE_CLAMP;
	 sampler->ss1.t_wrap_mode = BRW_TEXCOORDMODE_CLAMP;
      }
   } else if (texObj->Target == GL_TEXTURE_1D) {
      /* There's a bug in 1D texture sampling - it actually pays
       * attention to the wrap_t value, though it should not.
       * Override the wrap_t value here to GL_REPEAT to keep
       * any nonexistent border pixels from floating in.
       */
      sampler->ss1.t_wrap_mode = BRW_TEXCOORDMODE_WRAP;
   }


   /* Set shadow function: 
    */
   if (gl_sampler->CompareMode == GL_COMPARE_R_TO_TEXTURE_ARB) {
      /* Shadowing is "enabled" by emitting a particular sampler
       * message (sample_c).  So need to recompile WM program when
       * shadow comparison is enabled on each/any texture unit.
       */
      sampler->ss0.shadow_function =
	 intel_translate_shadow_compare_func(gl_sampler->CompareFunc);
   }

   /* Set LOD bias: 
    */
   sampler->ss0.lod_bias = S_FIXED(CLAMP(texUnit->LodBias +
					 gl_sampler->LodBias, -16, 15), 6);

   sampler->ss0.lod_preclamp = 1; /* OpenGL mode */
   sampler->ss0.default_color_mode = 0; /* OpenGL/DX10 mode */

   sampler->ss0.base_level = U_FIXED(0, 1);

   sampler->ss1.max_lod = U_FIXED(CLAMP(gl_sampler->MaxLod, 0, 13), 6);
   sampler->ss1.min_lod = U_FIXED(CLAMP(gl_sampler->MinLod, 0, 13), 6);

   /* On Gen6+, the sampler can handle non-normalized texture
    * rectangle coordinates natively
    */
   if (brw->gen >= 6 && texObj->Target == GL_TEXTURE_RECTANGLE) {
      sampler->ss3.non_normalized_coord = 1;
   }

   upload_default_color(brw, gl_sampler, unit, sdc_offset);

   if (brw->gen >= 6) {
      sampler->ss2.default_color_pointer = *sdc_offset >> 5;
   } else {
      /* reloc */
      sampler->ss2.default_color_pointer = (brw->batch.bo->offset +
					    *sdc_offset) >> 5;

      drm_intel_bo_emit_reloc(brw->batch.bo,
			      sampler_state_table_offset +
			      ss_index * sizeof(struct brw_sampler_state) +
			      offsetof(struct brw_sampler_state, ss2),
			      brw->batch.bo, *sdc_offset,
			      I915_GEM_DOMAIN_SAMPLER, 0);
   }

   if (sampler->ss0.min_filter != BRW_MAPFILTER_NEAREST)
      sampler->ss3.address_round |= BRW_ADDRESS_ROUNDING_ENABLE_U_MIN |
                                    BRW_ADDRESS_ROUNDING_ENABLE_V_MIN |
                                    BRW_ADDRESS_ROUNDING_ENABLE_R_MIN;
   if (sampler->ss0.mag_filter != BRW_MAPFILTER_NEAREST)
      sampler->ss3.address_round |= BRW_ADDRESS_ROUNDING_ENABLE_U_MAG |
                                    BRW_ADDRESS_ROUNDING_ENABLE_V_MAG |
                                    BRW_ADDRESS_ROUNDING_ENABLE_R_MAG;
}


static void
brw_upload_sampler_state_table(struct brw_context *brw,
                               struct gl_program *prog,
                               uint32_t sampler_count,
                               uint32_t *sst_offset,
                               uint32_t *sdc_offset)
{
   struct gl_context *ctx = &brw->ctx;
   struct brw_sampler_state *samplers;

   GLbitfield SamplersUsed = prog->SamplersUsed;

   if (sampler_count == 0)
      return;

   samplers = brw_state_batch(brw, AUB_TRACE_SAMPLER_STATE,
			      sampler_count * sizeof(*samplers),
			      32, sst_offset);
   memset(samplers, 0, sampler_count * sizeof(*samplers));

   for (unsigned s = 0; s < sampler_count; s++) {
      if (SamplersUsed & (1 << s)) {
         const unsigned unit = prog->SamplerUnits[s];
         if (ctx->Texture.Unit[unit]._ReallyEnabled)
            brw_update_sampler_state(brw, unit, s, &samplers[s],
                                     *sst_offset, &sdc_offset[s]);
      }
   }

   brw->state.dirty.cache |= CACHE_NEW_SAMPLER;
}

static void
brw_upload_fs_samplers(struct brw_context *brw)
{
   /* BRW_NEW_FRAGMENT_PROGRAM */
   struct gl_program *fs = (struct gl_program *) brw->fragment_program;
   brw->vtbl.upload_sampler_state_table(brw, fs,
                                        brw->wm.base.sampler_count,
                                        &brw->wm.base.sampler_offset,
                                        brw->wm.base.sdc_offset);
}

const struct brw_tracked_state brw_fs_samplers = {
   .dirty = {
      .mesa = _NEW_TEXTURE,
      .brw = BRW_NEW_BATCH |
             BRW_NEW_FRAGMENT_PROGRAM,
      .cache = 0
   },
   .emit = brw_upload_fs_samplers,
};

static void
brw_upload_vs_samplers(struct brw_context *brw)
{
   struct brw_stage_state *stage_state = &brw->vs.base;

   /* BRW_NEW_VERTEX_PROGRAM */
   struct gl_program *vs = (struct gl_program *) brw->vertex_program;
   brw->vtbl.upload_sampler_state_table(brw, vs,
                                        stage_state->sampler_count,
                                        &stage_state->sampler_offset,
                                        stage_state->sdc_offset);
}


const struct brw_tracked_state brw_vs_samplers = {
   .dirty = {
      .mesa = _NEW_TEXTURE,
      .brw = BRW_NEW_BATCH |
             BRW_NEW_VERTEX_PROGRAM,
      .cache = 0
   },
   .emit = brw_upload_vs_samplers,
};


static void
brw_upload_gs_samplers(struct brw_context *brw)
{
   struct brw_stage_state *stage_state = &brw->gs.base;

   /* BRW_NEW_GEOMETRY_PROGRAM */
   struct gl_program *gs = (struct gl_program *) brw->geometry_program;
   if (!gs)
      return;

   brw->vtbl.upload_sampler_state_table(brw, gs,
                                        stage_state->sampler_count,
                                        &stage_state->sampler_offset,
                                        stage_state->sdc_offset);
}


const struct brw_tracked_state brw_gs_samplers = {
   .dirty = {
      .mesa = _NEW_TEXTURE,
      .brw = BRW_NEW_BATCH |
             BRW_NEW_GEOMETRY_PROGRAM,
      .cache = 0
   },
   .emit = brw_upload_gs_samplers,
};


void
gen4_init_vtable_sampler_functions(struct brw_context *brw)
{
   brw->vtbl.upload_sampler_state_table = brw_upload_sampler_state_table;
}
