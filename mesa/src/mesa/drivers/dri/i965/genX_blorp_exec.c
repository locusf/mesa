/*
 * Copyright © 2011 Intel Corporation
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

#include <assert.h>

#include "intel_batchbuffer.h"
#include "intel_mipmap_tree.h"

#include "brw_context.h"
#include "brw_state.h"

#include "blorp_priv.h"

#include "genxml/gen_macros.h"

static void *
blorp_emit_dwords(struct brw_context *brw, unsigned n)
{
   intel_batchbuffer_begin(brw, n, RENDER_RING);
   uint32_t *map = brw->batch.map_next;
   brw->batch.map_next += n;
   intel_batchbuffer_advance(brw);
   return map;
}

struct blorp_address {
   drm_intel_bo *buffer;
   uint32_t read_domains;
   uint32_t write_domain;
   uint32_t offset;
};

static uint64_t
blorp_emit_reloc(struct brw_context *brw, void *location,
                 struct blorp_address address, uint32_t delta)
{
   uint32_t offset = (char *)location - (char *)brw->batch.map;
   if (brw->gen >= 8) {
      return intel_batchbuffer_reloc64(brw, address.buffer, offset,
                                       address.read_domains,
                                       address.write_domain,
                                       address.offset + delta);
   } else {
      return intel_batchbuffer_reloc(brw, address.buffer, offset,
                                     address.read_domains,
                                     address.write_domain,
                                     address.offset + delta);
   }
}

#define __gen_address_type struct blorp_address
#define __gen_user_data struct brw_context

static uint64_t
__gen_combine_address(struct brw_context *brw, void *location,
                      struct blorp_address address, uint32_t delta)
{
   if (address.buffer == NULL) {
      return address.offset + delta;
   } else {
      return blorp_emit_reloc(brw, location, address, delta);
   }
}

#include "genxml/genX_pack.h"

#define _blorp_cmd_length(cmd) cmd ## _length
#define _blorp_cmd_length_bias(cmd) cmd ## _length_bias
#define _blorp_cmd_header(cmd) cmd ## _header
#define _blorp_cmd_pack(cmd) cmd ## _pack

#define blorp_emit(brw, cmd, name)                                \
   for (struct cmd name = { _blorp_cmd_header(cmd) },             \
        *_dst = blorp_emit_dwords(brw, _blorp_cmd_length(cmd));   \
        __builtin_expect(_dst != NULL, 1);                        \
        _blorp_cmd_pack(cmd)(brw, (void *)_dst, &name),           \
        _dst = NULL)

#define blorp_emitn(batch, cmd, n) ({                    \
      uint32_t *_dw = blorp_emit_dwords(batch, n);       \
      struct cmd template = {                            \
         _blorp_cmd_header(cmd),                         \
         .DWordLength = n - _blorp_cmd_length_bias(cmd), \
      };                                                 \
      _blorp_cmd_pack(cmd)(batch, _dw, &template);       \
      _dw + 1; /* Array starts at dw[1] */               \
   })

/* Once vertex fetcher has written full VUE entries with complete
 * header the space requirement is as follows per vertex (in bytes):
 *
 *     Header    Position    Program constants
 *   +--------+------------+-------------------+
 *   |   16   |     16     |      n x 16       |
 *   +--------+------------+-------------------+
 *
 * where 'n' stands for number of varying inputs expressed as vec4s.
 *
 * The URB size is in turn expressed in 64 bytes (512 bits).
 */
static inline unsigned
gen7_blorp_get_vs_entry_size(const struct brw_blorp_params *params)
{
    const unsigned num_varyings =
       params->wm_prog_data ? params->wm_prog_data->num_varying_inputs : 0;
    const unsigned total_needed = 16 + 16 + num_varyings * 16;

   return DIV_ROUND_UP(total_needed, 64);
}

/* 3DSTATE_URB
 * 3DSTATE_URB_VS
 * 3DSTATE_URB_HS
 * 3DSTATE_URB_DS
 * 3DSTATE_URB_GS
 *
 * Assign the entire URB to the VS. Even though the VS disabled, URB space
 * is still needed because the clipper loads the VUE's from the URB. From
 * the Sandybridge PRM, Volume 2, Part 1, Section 3DSTATE,
 * Dword 1.15:0 "VS Number of URB Entries":
 *     This field is always used (even if VS Function Enable is DISABLED).
 *
 * The warning below appears in the PRM (Section 3DSTATE_URB), but we can
 * safely ignore it because this batch contains only one draw call.
 *     Because of URB corruption caused by allocating a previous GS unit
 *     URB entry to the VS unit, software is required to send a “GS NULL
 *     Fence” (Send URB fence with VS URB size == 1 and GS URB size == 0)
 *     plus a dummy DRAW call before any case where VS will be taking over
 *     GS URB space.
 *
 * If the 3DSTATE_URB_VS is emitted, than the others must be also.
 * From the Ivybridge PRM, Volume 2 Part 1, section 1.7.1 3DSTATE_URB_VS:
 *
 *     3DSTATE_URB_HS, 3DSTATE_URB_DS, and 3DSTATE_URB_GS must also be
 *     programmed in order for the programming of this state to be
 *     valid.
 */
static void
emit_urb_config(struct brw_context *brw,
                const struct brw_blorp_params *params)
{
#if GEN_GEN >= 7
   const unsigned vs_entry_size = gen7_blorp_get_vs_entry_size(params);

   if (!(brw->ctx.NewDriverState & (BRW_NEW_CONTEXT | BRW_NEW_URB_SIZE)) &&
       brw->urb.vsize >= vs_entry_size)
      return;

   brw->ctx.NewDriverState |= BRW_NEW_URB_SIZE;

   gen7_upload_urb(brw, vs_entry_size, false, false);
#else
   blorp_emit(brw, GENX(3DSTATE_URB), urb) {
      urb.VSNumberofURBEntries = brw->urb.max_vs_entries;
   }
#endif
}

static void
blorp_emit_vertex_data(struct brw_context *brw,
                       const struct brw_blorp_params *params,
                       struct blorp_address *addr,
                       uint32_t *size)
{
   const float vertices[] = {
      /* v0 */ (float)params->x0, (float)params->y1,
      /* v1 */ (float)params->x1, (float)params->y1,
      /* v2 */ (float)params->x0, (float)params->y0,
   };

   uint32_t offset;
   void *data = brw_state_batch(brw, AUB_TRACE_VERTEX_BUFFER,
                                sizeof(vertices), 32, &offset);
   memcpy(data, vertices, sizeof(vertices));

   *addr = (struct blorp_address) {
      .buffer = brw->batch.bo,
      .read_domains = I915_GEM_DOMAIN_VERTEX,
      .write_domain = 0,
      .offset = offset,
   };
   *size = sizeof(vertices);
}

static void
blorp_emit_input_varying_data(struct brw_context *brw,
                              const struct brw_blorp_params *params,
                              struct blorp_address *addr,
                              uint32_t *size)
{
   const unsigned vec4_size_in_bytes = 4 * sizeof(float);
   const unsigned max_num_varyings =
      DIV_ROUND_UP(sizeof(params->wm_inputs), vec4_size_in_bytes);
   const unsigned num_varyings = params->wm_prog_data->num_varying_inputs;

   *size = num_varyings * vec4_size_in_bytes;

   const float *const inputs_src = (const float *)&params->wm_inputs;
   uint32_t offset;
   float *inputs = brw_state_batch(brw, AUB_TRACE_VERTEX_BUFFER,
                                   *size, 32, &offset);

   /* Walk over the attribute slots, determine if the attribute is used by
    * the program and when necessary copy the values from the input storage to
    * the vertex data buffer.
    */
   for (unsigned i = 0; i < max_num_varyings; i++) {
      const gl_varying_slot attr = VARYING_SLOT_VAR0 + i;

      if (!(params->wm_prog_data->inputs_read & BITFIELD64_BIT(attr)))
         continue;

      memcpy(inputs, inputs_src + i * 4, vec4_size_in_bytes);

      inputs += 4;
   }

   *addr = (struct blorp_address) {
      .buffer = brw->batch.bo,
      .read_domains = I915_GEM_DOMAIN_VERTEX,
      .write_domain = 0,
      .offset = offset,
   };
}

static void
blorp_emit_vertex_buffers(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   struct GENX(VERTEX_BUFFER_STATE) vb[2];
   memset(vb, 0, sizeof(vb));

   unsigned num_buffers = 1;

#if GEN_GEN == 9
   uint32_t mocs = (2 << 1); /* SKL_MOCS_WB */
#elif GEN_GEN == 8
   uint32_t mocs = 0x78; /* BDW_MOCS_WB */
#elif GEN_GEN == 7
   uint32_t mocs = 1; /* GEN7_MOCS_L3 */
#else
   uint32_t mocs = 0;
#endif

   uint32_t size;
   blorp_emit_vertex_data(brw, params, &vb[0].BufferStartingAddress, &size);
   vb[0].VertexBufferIndex = 0;
   vb[0].BufferPitch = 2 * sizeof(float);
   vb[0].VertexBufferMOCS = mocs;
#if GEN_GEN >= 7
   vb[0].AddressModifyEnable = true;
#endif
#if GEN_GEN >= 8
   vb[0].BufferSize = size;
#else
   vb[0].BufferAccessType = VERTEXDATA;
   vb[0].EndAddress = vb[0].BufferStartingAddress;
   vb[0].EndAddress.offset += size - 1;
#endif

   if (params->wm_prog_data && params->wm_prog_data->num_varying_inputs) {
      blorp_emit_input_varying_data(brw, params,
                                    &vb[1].BufferStartingAddress, &size);
      vb[1].VertexBufferIndex = 1;
      vb[1].BufferPitch = 0;
      vb[1].VertexBufferMOCS = mocs;
#if GEN_GEN >= 7
      vb[1].AddressModifyEnable = true;
#endif
#if GEN_GEN >= 8
      vb[1].BufferSize = size;
#else
      vb[1].BufferAccessType = INSTANCEDATA;
      vb[1].EndAddress = vb[1].BufferStartingAddress;
      vb[1].EndAddress.offset += size - 1;
#endif
      num_buffers++;
   }

   const unsigned num_dwords =
      1 + GENX(VERTEX_BUFFER_STATE_length) * num_buffers;
   uint32_t *dw = blorp_emitn(brw, GENX(3DSTATE_VERTEX_BUFFERS), num_dwords);

   for (unsigned i = 0; i < num_buffers; i++) {
      GENX(VERTEX_BUFFER_STATE_pack)(brw, dw, &vb[i]);
      dw += GENX(VERTEX_BUFFER_STATE_length);
   }
}

static void
blorp_emit_vertex_elements(struct brw_context *brw,
                           const struct brw_blorp_params *params)
{
   const unsigned num_varyings =
      params->wm_prog_data ? params->wm_prog_data->num_varying_inputs : 0;
   const unsigned num_elements = 2 + num_varyings;

   struct GENX(VERTEX_ELEMENT_STATE) ve[num_elements];
   memset(ve, 0, num_elements * sizeof(*ve));

   /* Setup VBO for the rectangle primitive..
    *
    * A rectangle primitive (3DPRIM_RECTLIST) consists of only three
    * vertices. The vertices reside in screen space with DirectX
    * coordinates (that is, (0, 0) is the upper left corner).
    *
    *   v2 ------ implied
    *    |        |
    *    |        |
    *   v0 ----- v1
    *
    * Since the VS is disabled, the clipper loads each VUE directly from
    * the URB. This is controlled by the 3DSTATE_VERTEX_BUFFERS and
    * 3DSTATE_VERTEX_ELEMENTS packets below. The VUE contents are as follows:
    *   dw0: Reserved, MBZ.
    *   dw1: Render Target Array Index. The HiZ op does not use indexed
    *        vertices, so set the dword to 0.
    *   dw2: Viewport Index. The HiZ op disables viewport mapping and
    *        scissoring, so set the dword to 0.
    *   dw3: Point Width: The HiZ op does not emit the POINTLIST primitive,
    *        so set the dword to 0.
    *   dw4: Vertex Position X.
    *   dw5: Vertex Position Y.
    *   dw6: Vertex Position Z.
    *   dw7: Vertex Position W.
    *
    *   dw8: Flat vertex input 0
    *   dw9: Flat vertex input 1
    *   ...
    *   dwn: Flat vertex input n - 8
    *
    * For details, see the Sandybridge PRM, Volume 2, Part 1, Section 1.5.1
    * "Vertex URB Entry (VUE) Formats".
    *
    * Only vertex position X and Y are going to be variable, Z is fixed to
    * zero and W to one. Header words dw0-3 are all zero. There is no need to
    * include the fixed values in the vertex buffer. Vertex fetcher can be
    * instructed to fill vertex elements with constant values of one and zero
    * instead of reading them from the buffer.
    * Flat inputs are program constants that are not interpolated. Moreover
    * their values will be the same between vertices.
    *
    * See the vertex element setup below.
    */
   ve[0].VertexBufferIndex = 0;
   ve[0].Valid = true;
   ve[0].SourceElementFormat = ISL_FORMAT_R32G32B32A32_FLOAT;
   ve[0].SourceElementOffset = 0;
   ve[0].Component0Control = VFCOMP_STORE_0;
   ve[0].Component1Control = VFCOMP_STORE_0;
   ve[0].Component2Control = VFCOMP_STORE_0;
   ve[0].Component3Control = VFCOMP_STORE_0;

   ve[1].VertexBufferIndex = 0;
   ve[1].Valid = true;
   ve[1].SourceElementFormat = ISL_FORMAT_R32G32_FLOAT;
   ve[1].SourceElementOffset = 0;
   ve[1].Component0Control = VFCOMP_STORE_SRC;
   ve[1].Component1Control = VFCOMP_STORE_SRC;
   ve[1].Component2Control = VFCOMP_STORE_0;
   ve[1].Component3Control = VFCOMP_STORE_1_FP;

   for (unsigned i = 0; i < num_varyings; ++i) {
      ve[i + 2].VertexBufferIndex = 1;
      ve[i + 2].Valid = true;
      ve[i + 2].SourceElementFormat = ISL_FORMAT_R32G32B32A32_FLOAT;
      ve[i + 2].SourceElementOffset = i * 4 * sizeof(float);
      ve[i + 2].Component0Control = VFCOMP_STORE_SRC;
      ve[i + 2].Component1Control = VFCOMP_STORE_SRC;
      ve[i + 2].Component2Control = VFCOMP_STORE_SRC;
      ve[i + 2].Component3Control = VFCOMP_STORE_SRC;
   }

   const unsigned num_dwords =
      1 + GENX(VERTEX_ELEMENT_STATE_length) * num_elements;
   uint32_t *dw = blorp_emitn(brw, GENX(3DSTATE_VERTEX_ELEMENTS), num_dwords);

   for (unsigned i = 0; i < num_elements; i++) {
      GENX(VERTEX_ELEMENT_STATE_pack)(brw, dw, &ve[i]);
      dw += GENX(VERTEX_ELEMENT_STATE_length);
   }

#if GEN_GEN >= 8
   blorp_emit(brw, GENX(3DSTATE_VF_SGVS), sgvs);

   for (unsigned i = 0; i < num_elements; i++) {
      blorp_emit(brw, GENX(3DSTATE_VF_INSTANCING), vf) {
         vf.VertexElementIndex = i;
         vf.InstancingEnable = false;
      }
   }

   blorp_emit(brw, GENX(3DSTATE_VF_TOPOLOGY), topo) {
      topo.PrimitiveTopologyType = _3DPRIM_RECTLIST;
   }
#endif
}

static void
blorp_emit_sf_config(struct brw_context *brw,
                     const struct brw_blorp_params *params)
{
   const struct brw_blorp_prog_data *prog_data = params->wm_prog_data;

   /* 3DSTATE_SF
    *
    * Disable ViewportTransformEnable (dw2.1)
    *
    * From the SandyBridge PRM, Volume 2, Part 1, Section 1.3, "3D
    * Primitives Overview":
    *     RECTLIST: Viewport Mapping must be DISABLED (as is typical with the
    *     use of screen- space coordinates).
    *
    * A solid rectangle must be rendered, so set FrontFaceFillMode (dw2.4:3)
    * and BackFaceFillMode (dw2.5:6) to SOLID(0).
    *
    * From the Sandy Bridge PRM, Volume 2, Part 1, Section
    * 6.4.1.1 3DSTATE_SF, Field FrontFaceFillMode:
    *     SOLID: Any triangle or rectangle object found to be front-facing
    *     is rendered as a solid object. This setting is required when
    *     (rendering rectangle (RECTLIST) objects.
    */

#if GEN_GEN >= 8

   blorp_emit(brw, GENX(3DSTATE_SF), sf);

   blorp_emit(brw, GENX(3DSTATE_RASTER), raster) {
      raster.CullMode = CULLMODE_NONE;
   }

   blorp_emit(brw, GENX(3DSTATE_SBE), sbe) {
      sbe.VertexURBEntryReadOffset = BRW_SF_URB_ENTRY_READ_OFFSET;
      sbe.NumberofSFOutputAttributes = prog_data->num_varying_inputs;
      sbe.VertexURBEntryReadLength = brw_blorp_get_urb_length(prog_data);
      sbe.ForceVertexURBEntryReadLength = true;
      sbe.ForceVertexURBEntryReadOffset = true;
      sbe.ConstantInterpolationEnable = prog_data->flat_inputs;

#if GEN_GEN >= 9
      for (unsigned i = 0; i < 32; i++)
         sbe.AttributeActiveComponentFormat[i] = ACF_XYZW;
#endif
   }

#elif GEN_GEN >= 7

   blorp_emit(brw, GENX(3DSTATE_SF), sf) {
      sf.FrontFaceFillMode = FILL_MODE_SOLID;
      sf.BackFaceFillMode = FILL_MODE_SOLID;

      sf.MultisampleRasterizationMode = params->dst.surf.samples > 1 ?
         MSRASTMODE_ON_PATTERN : MSRASTMODE_OFF_PIXEL;

#if GEN_GEN == 7
      sf.DepthBufferSurfaceFormat = params->depth_format;
#endif
   }

   blorp_emit(brw, GENX(3DSTATE_SBE), sbe) {
      sbe.VertexURBEntryReadOffset = BRW_SF_URB_ENTRY_READ_OFFSET;
      if (prog_data) {
         sbe.NumberofSFOutputAttributes = prog_data->num_varying_inputs;
         sbe.VertexURBEntryReadLength = brw_blorp_get_urb_length(prog_data);
         sbe.ConstantInterpolationEnable = prog_data->flat_inputs;
      } else {
         sbe.NumberofSFOutputAttributes = 0;
         sbe.VertexURBEntryReadLength = 1;
      }
   }

#else /* GEN_GEN <= 6 */

   blorp_emit(brw, GENX(3DSTATE_SF), sf) {
      sf.FrontFaceFillMode = FILL_MODE_SOLID;
      sf.BackFaceFillMode = FILL_MODE_SOLID;

      sf.MultisampleRasterizationMode = params->dst.surf.samples > 1 ?
         MSRASTMODE_ON_PATTERN : MSRASTMODE_OFF_PIXEL;

      sf.VertexURBEntryReadOffset = BRW_SF_URB_ENTRY_READ_OFFSET;
      if (prog_data) {
         sf.NumberofSFOutputAttributes = prog_data->num_varying_inputs;
         sf.VertexURBEntryReadLength = brw_blorp_get_urb_length(prog_data);
         sf.ConstantInterpolationEnable = prog_data->flat_inputs;
      } else {
         sf.NumberofSFOutputAttributes = 0;
         sf.VertexURBEntryReadLength = 1;
      }
   }

#endif /* GEN_GEN */
}

static void
blorp_emit_ps_config(struct brw_context *brw,
                     const struct brw_blorp_params *params)
{
   const struct brw_blorp_prog_data *prog_data = params->wm_prog_data;

   /* Even when thread dispatch is disabled, max threads (dw5.25:31) must be
    * nonzero to prevent the GPU from hanging.  While the documentation doesn't
    * mention this explicitly, it notes that the valid range for the field is
    * [1,39] = [2,40] threads, which excludes zero.
    *
    * To be safe (and to minimize extraneous code) we go ahead and fully
    * configure the WM state whether or not there is a WM program.
    */

#if GEN_GEN >= 8

   blorp_emit(brw, GENX(3DSTATE_WM), wm);

   blorp_emit(brw, GENX(3DSTATE_PS), ps) {
      if (params->src.bo) {
         ps.SamplerCount = 1; /* Up to 4 samplers */
         ps.BindingTableEntryCount = 2;
      } else {
         ps.BindingTableEntryCount = 1;
      }

      ps.DispatchGRFStartRegisterForConstantSetupData0 =
         prog_data->first_curbe_grf_0;
      ps.DispatchGRFStartRegisterForConstantSetupData2 =
         prog_data->first_curbe_grf_2;

      ps._8PixelDispatchEnable = prog_data->dispatch_8;
      ps._16PixelDispatchEnable = prog_data->dispatch_16;

      ps.KernelStartPointer0 = params->wm_prog_kernel;
      ps.KernelStartPointer2 =
         params->wm_prog_kernel + prog_data->ksp_offset_2;

      /* 3DSTATE_PS expects the number of threads per PSD, which is always 64;
       * it implicitly scales for different GT levels (which have some # of
       * PSDs).
       *
       * In Gen8 the format is U8-2 whereas in Gen9 it is U8-1.
       */
      if (GEN_GEN >= 9)
         ps.MaximumNumberofThreadsPerPSD = 64 - 1;
      else
         ps.MaximumNumberofThreadsPerPSD = 64 - 2;

      switch (params->fast_clear_op) {
#if GEN_GEN >= 9
      case (1 << 6): /* GEN7_PS_RENDER_TARGET_RESOLVE_ENABLE */
         ps.RenderTargetResolveType = RESOLVE_PARTIAL;
         break;
      case (3 << 6): /* GEN9_PS_RENDER_TARGET_RESOLVE_FULL */
         ps.RenderTargetResolveType = RESOLVE_FULL;
         break;
#else
      case (1 << 6): /* GEN7_PS_RENDER_TARGET_RESOLVE_ENABLE */
         ps.RenderTargetResolveEnable = true;
         break;
#endif
      case (1 << 8): /* GEN7_PS_RENDER_TARGET_FAST_CLEAR_ENABLE */
         ps.RenderTargetFastClearEnable = true;
         break;
      }
   }

   blorp_emit(brw, GENX(3DSTATE_PS_EXTRA), psx) {
      psx.PixelShaderValid = true;

      if (params->src.bo)
         psx.PixelShaderKillsPixel = true;

      psx.AttributeEnable = prog_data->num_varying_inputs > 0;

      if (prog_data && prog_data->persample_msaa_dispatch)
         psx.PixelShaderIsPerSample = true;
   }

#elif GEN_GEN >= 7

   blorp_emit(brw, GENX(3DSTATE_WM), wm) {
      switch (params->hiz_op) {
      case GEN6_HIZ_OP_DEPTH_CLEAR:
         wm.DepthBufferClear = true;
         break;
      case GEN6_HIZ_OP_DEPTH_RESOLVE:
         wm.DepthBufferResolveEnable = true;
         break;
      case GEN6_HIZ_OP_HIZ_RESOLVE:
         wm.HierarchicalDepthBufferResolveEnable = true;
         break;
      case GEN6_HIZ_OP_NONE:
         break;
      default:
         unreachable("not reached");
      }

      if (prog_data)
         wm.ThreadDispatchEnable = true;

      if (params->src.bo)
         wm.PixelShaderKillPixel = true;

      if (params->dst.surf.samples > 1) {
         wm.MultisampleRasterizationMode = MSRASTMODE_ON_PATTERN;
         wm.MultisampleDispatchMode =
            (prog_data && prog_data->persample_msaa_dispatch) ?
            MSDISPMODE_PERSAMPLE : MSDISPMODE_PERPIXEL;
      } else {
         wm.MultisampleRasterizationMode = MSRASTMODE_OFF_PIXEL;
         wm.MultisampleDispatchMode = MSDISPMODE_PERSAMPLE;
      }
   }

   blorp_emit(brw, GENX(3DSTATE_PS), ps) {
      ps.MaximumNumberofThreads = brw->max_wm_threads - 1;

#if GEN_IS_HASWELL
      ps.SampleMask = 1;
#endif

      if (prog_data) {
         ps.DispatchGRFStartRegisterforConstantSetupData0 =
            prog_data->first_curbe_grf_0;
         ps.DispatchGRFStartRegisterforConstantSetupData2 =
            prog_data->first_curbe_grf_2;

         ps.KernelStartPointer0 = params->wm_prog_kernel;
         ps.KernelStartPointer2 =
            params->wm_prog_kernel + prog_data->ksp_offset_2;

         ps._8PixelDispatchEnable = prog_data->dispatch_8;
         ps._16PixelDispatchEnable = prog_data->dispatch_16;

         ps.AttributeEnable = prog_data->num_varying_inputs > 0;
      } else {
         /* Gen7 hardware gets angry if we don't enable at least one dispatch
          * mode, so just enable 16-pixel dispatch if we don't have a program.
          */
         ps._16PixelDispatchEnable = true;
      }

      if (params->src.bo)
         ps.SamplerCount = 1; /* Up to 4 samplers */

      switch (params->fast_clear_op) {
      case (1 << 6): /* GEN7_PS_RENDER_TARGET_RESOLVE_ENABLE */
         ps.RenderTargetResolveEnable = true;
         break;
      case (1 << 8): /* GEN7_PS_RENDER_TARGET_FAST_CLEAR_ENABLE */
         ps.RenderTargetFastClearEnable = true;
         break;
      }
   }

#else /* GEN_GEN <= 6 */

   blorp_emit(brw, GENX(3DSTATE_WM), wm) {
      wm.MaximumNumberofThreads = brw->max_wm_threads - 1;

      switch (params->hiz_op) {
      case GEN6_HIZ_OP_DEPTH_CLEAR:
         wm.DepthBufferClear = true;
         break;
      case GEN6_HIZ_OP_DEPTH_RESOLVE:
         wm.DepthBufferResolveEnable = true;
         break;
      case GEN6_HIZ_OP_HIZ_RESOLVE:
         wm.HierarchicalDepthBufferResolveEnable = true;
         break;
      case GEN6_HIZ_OP_NONE:
         break;
      default:
         unreachable("not reached");
      }

      if (prog_data) {
         wm.ThreadDispatchEnable = true;

         wm.DispatchGRFStartRegisterforConstantSetupData0 =
            prog_data->first_curbe_grf_0;
         wm.DispatchGRFStartRegisterforConstantSetupData2 =
            prog_data->first_curbe_grf_2;

         wm.KernelStartPointer0 = params->wm_prog_kernel;
         wm.KernelStartPointer2 =
            params->wm_prog_kernel + prog_data->ksp_offset_2;

         wm._8PixelDispatchEnable = prog_data->dispatch_8;
         wm._16PixelDispatchEnable = prog_data->dispatch_16;

         wm.NumberofSFOutputAttributes = prog_data->num_varying_inputs;
      }

      if (params->src.bo) {
         wm.SamplerCount = 1; /* Up to 4 samplers */
         wm.PixelShaderKillPixel = true; /* TODO: temporarily smash on */
      }

      if (params->dst.surf.samples > 1) {
         wm.MultisampleRasterizationMode = MSRASTMODE_ON_PATTERN;
         wm.MultisampleDispatchMode =
            (prog_data && prog_data->persample_msaa_dispatch) ?
            MSDISPMODE_PERSAMPLE : MSDISPMODE_PERPIXEL;
      } else {
         wm.MultisampleRasterizationMode = MSRASTMODE_OFF_PIXEL;
         wm.MultisampleDispatchMode = MSDISPMODE_PERSAMPLE;
      }
   }

#endif /* GEN_GEN */
}


static void
blorp_emit_depth_stencil_config(struct brw_context *brw,
                                const struct brw_blorp_params *params)
{
   brw_emit_depth_stall_flushes(brw);

#if GEN_GEN >= 7
   const uint32_t mocs = 1; /* GEN7_MOCS_L3 */
#else
   const uint32_t mocs = 0;
#endif

   blorp_emit(brw, GENX(3DSTATE_DEPTH_BUFFER), db) {
      switch (params->depth.surf.dim) {
      case ISL_SURF_DIM_1D:
         db.SurfaceType = SURFTYPE_1D;
         break;
      case ISL_SURF_DIM_2D:
         db.SurfaceType = SURFTYPE_2D;
         break;
      case ISL_SURF_DIM_3D:
         db.SurfaceType = SURFTYPE_3D;
         break;
      }

      db.SurfaceFormat = params->depth_format;

#if GEN_GEN >= 7
      db.DepthWriteEnable = true;
#endif

#if GEN_GEN <= 6
      db.TiledSurface = true;
      db.TileWalk = TILEWALK_YMAJOR;
      db.MIPMapLayoutMode = MIPLAYOUT_BELOW;
      db.SeparateStencilBufferEnable = true;
#endif

      db.HierarchicalDepthBufferEnable = true;

      db.Width = params->depth.surf.logical_level0_px.width - 1;
      db.Height = params->depth.surf.logical_level0_px.height - 1;
      db.RenderTargetViewExtent = db.Depth =
         MAX2(params->depth.surf.logical_level0_px.depth,
              params->depth.surf.logical_level0_px.array_len) - 1;

      db.LOD = params->depth.view.base_level;
      db.MinimumArrayElement = params->depth.view.base_array_layer;

      db.SurfacePitch = params->depth.surf.row_pitch - 1;
      db.SurfaceBaseAddress = (struct blorp_address) {
         .buffer = params->depth.bo,
         .read_domains = I915_GEM_DOMAIN_RENDER,
         .write_domain = I915_GEM_DOMAIN_RENDER,
         .offset = params->depth.offset,
      };
      db.DepthBufferMOCS = mocs;
   }

   blorp_emit(brw, GENX(3DSTATE_HIER_DEPTH_BUFFER), hiz) {
      hiz.SurfacePitch = params->depth.aux_surf.row_pitch - 1;
      hiz.SurfaceBaseAddress = (struct blorp_address) {
         .buffer = params->depth.aux_bo,
         .read_domains = I915_GEM_DOMAIN_RENDER,
         .write_domain = I915_GEM_DOMAIN_RENDER,
         .offset = params->depth.aux_offset,
      };
      hiz.HierarchicalDepthBufferMOCS = mocs;
   }

   blorp_emit(brw, GENX(3DSTATE_STENCIL_BUFFER), sb);
}

static uint32_t
blorp_emit_blend_state(struct brw_context *brw,
                       const struct brw_blorp_params *params)
{
   struct GENX(BLEND_STATE) blend;
   memset(&blend, 0, sizeof(blend));

   for (unsigned i = 0; i < params->num_draw_buffers; ++i) {
      blend.Entry[i].PreBlendColorClampEnable = true;
      blend.Entry[i].PostBlendColorClampEnable = true;
      blend.Entry[i].ColorClampRange = COLORCLAMP_RTFORMAT;

      blend.Entry[i].WriteDisableRed = params->color_write_disable[0];
      blend.Entry[i].WriteDisableGreen = params->color_write_disable[1];
      blend.Entry[i].WriteDisableBlue = params->color_write_disable[2];
      blend.Entry[i].WriteDisableAlpha = params->color_write_disable[3];
   }

   uint32_t offset;
   void *state = brw_state_batch(brw, AUB_TRACE_BLEND_STATE,
                                 GENX(BLEND_STATE_length) * 4, 64, &offset);
   GENX(BLEND_STATE_pack)(NULL, state, &blend);

#if GEN_GEN >= 7
   blorp_emit(brw, GENX(3DSTATE_BLEND_STATE_POINTERS), sp) {
      sp.BlendStatePointer = offset;
#if GEN_GEN >= 8
      sp.BlendStatePointerValid = true;
#endif
   }
#endif

#if GEN_GEN >= 8
   blorp_emit(brw, GENX(3DSTATE_PS_BLEND), ps_blend) {
      ps_blend.HasWriteableRT = true;
   }
#endif

   return offset;
}

static uint32_t
blorp_emit_color_calc_state(struct brw_context *brw,
                            const struct brw_blorp_params *params)
{
   uint32_t offset;
   void *state = brw_state_batch(brw, AUB_TRACE_CC_STATE,
                                 GENX(COLOR_CALC_STATE_length) * 4, 64, &offset);
   memset(state, 0, GENX(COLOR_CALC_STATE_length) * 4);

#if GEN_GEN >= 7
   blorp_emit(brw, GENX(3DSTATE_CC_STATE_POINTERS), sp) {
      sp.ColorCalcStatePointer = offset;
#if GEN_GEN >= 8
      sp.ColorCalcStatePointerValid = true;
#endif
   }
#endif

   return offset;
}

static uint32_t
blorp_emit_depth_stencil_state(struct brw_context *brw,
                               const struct brw_blorp_params *params)
{
#if GEN_GEN >= 8

   /* On gen8+, DEPTH_STENCIL state is simply an instruction */
   blorp_emit(brw, GENX(3DSTATE_WM_DEPTH_STENCIL), ds);
   return 0;

#else /* GEN_GEN <= 7 */

   /* See the following sections of the Sandy Bridge PRM, Volume 1, Part2:
    *   - 7.5.3.1 Depth Buffer Clear
    *   - 7.5.3.2 Depth Buffer Resolve
    *   - 7.5.3.3 Hierarchical Depth Buffer Resolve
    */
   struct GENX(DEPTH_STENCIL_STATE) ds = {
      .DepthBufferWriteEnable = true,
   };

   if (params->hiz_op == GEN6_HIZ_OP_DEPTH_RESOLVE) {
      ds.DepthTestEnable = true;
      ds.DepthTestFunction = COMPAREFUNCTION_NEVER;
   }

   uint32_t offset;
   void *state = brw_state_batch(brw, AUB_TRACE_DEPTH_STENCIL_STATE,
                                 GENX(DEPTH_STENCIL_STATE_length) * 4, 64,
                                 &offset);
   GENX(DEPTH_STENCIL_STATE_pack)(NULL, state, &ds);

#if GEN_GEN >= 7
   blorp_emit(brw, GENX(3DSTATE_DEPTH_STENCIL_STATE_POINTERS), sp) {
      sp.PointertoDEPTH_STENCIL_STATE = offset;
   }
#endif

   return offset;

#endif /* GEN_GEN */
}

static void
blorp_emit_surface_states(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   uint32_t bind_offset;
   uint32_t *bind =
      brw_state_batch(brw, AUB_TRACE_BINDING_TABLE,
                      sizeof(uint32_t) * BRW_BLORP_NUM_BINDING_TABLE_ENTRIES,
                      32, /* alignment */ &bind_offset);

   bind[BRW_BLORP_RENDERBUFFER_BINDING_TABLE_INDEX] =
      brw_blorp_emit_surface_state(brw, &params->dst,
                                   I915_GEM_DOMAIN_RENDER,
                                   I915_GEM_DOMAIN_RENDER, true);
   if (params->src.bo) {
      bind[BRW_BLORP_TEXTURE_BINDING_TABLE_INDEX] =
         brw_blorp_emit_surface_state(brw, &params->src,
                                      I915_GEM_DOMAIN_SAMPLER, 0, false);
   }

#if GEN_GEN >= 7
   blorp_emit(brw, GENX(3DSTATE_BINDING_TABLE_POINTERS_PS), bt) {
      bt.PointertoPSBindingTable = bind_offset;
   }
#else
   blorp_emit(brw, GENX(3DSTATE_BINDING_TABLE_POINTERS), bt) {
      bt.PSBindingTableChange = true;
      bt.PointertoPSBindingTable = bind_offset;
   }
#endif
}

static void
blorp_emit_sampler_state(struct brw_context *brw,
                         const struct brw_blorp_params *params)
{
   struct GENX(SAMPLER_STATE) sampler = {
      .MipModeFilter = MIPFILTER_NONE,
      .MagModeFilter = MAPFILTER_LINEAR,
      .MinModeFilter = MAPFILTER_LINEAR,
      .MinLOD = 0,
      .MaxLOD = 0,
      .TCXAddressControlMode = TCM_CLAMP,
      .TCYAddressControlMode = TCM_CLAMP,
      .TCZAddressControlMode = TCM_CLAMP,
      .MaximumAnisotropy = RATIO21,
      .RAddressMinFilterRoundingEnable = true,
      .RAddressMagFilterRoundingEnable = true,
      .VAddressMinFilterRoundingEnable = true,
      .VAddressMagFilterRoundingEnable = true,
      .UAddressMinFilterRoundingEnable = true,
      .UAddressMagFilterRoundingEnable = true,
      .NonnormalizedCoordinateEnable = true,
   };

   uint32_t offset;
   void *state = brw_state_batch(brw, AUB_TRACE_SAMPLER_STATE,
                                 GENX(SAMPLER_STATE_length) * 4, 32, &offset);
   GENX(SAMPLER_STATE_pack)(NULL, state, &sampler);

#if GEN_GEN >= 7
   blorp_emit(brw, GENX(3DSTATE_SAMPLER_STATE_POINTERS_PS), ssp) {
      ssp.PointertoPSSamplerState = offset;
   }
#else
   blorp_emit(brw, GENX(3DSTATE_SAMPLER_STATE_POINTERS), ssp) {
      ssp.VSSamplerStateChange = true;
      ssp.GSSamplerStateChange = true;
      ssp.PSSamplerStateChange = true;
      ssp.PointertoPSSamplerState = offset;
   }
#endif
}

/* 3DSTATE_VIEWPORT_STATE_POINTERS */
static void
blorp_emit_viewport_state(struct brw_context *brw,
                          const struct brw_blorp_params *params)
{
   uint32_t cc_vp_offset;

   void *state = brw_state_batch(brw, AUB_TRACE_CC_VP_STATE,
                                 GENX(CC_VIEWPORT_length) * 4, 32,
                                 &cc_vp_offset);

   GENX(CC_VIEWPORT_pack)(brw, state,
      &(struct GENX(CC_VIEWPORT)) {
         .MinimumDepth = 0.0,
         .MaximumDepth = 1.0,
      });

#if GEN_GEN >= 7
   blorp_emit(brw, GENX(3DSTATE_VIEWPORT_STATE_POINTERS_CC), vsp) {
      vsp.CCViewportPointer = cc_vp_offset;
   }
#else
   blorp_emit(brw, GENX(3DSTATE_VIEWPORT_STATE_POINTERS), vsp) {
      vsp.CCViewportStateChange = true;
      vsp.PointertoCC_VIEWPORT = cc_vp_offset;
   }
#endif
}


/**
 * \brief Execute a blit or render pass operation.
 *
 * To execute the operation, this function manually constructs and emits a
 * batch to draw a rectangle primitive. The batchbuffer is flushed before
 * constructing and after emitting the batch.
 *
 * This function alters no GL state.
 */
void
genX(blorp_exec)(struct brw_context *brw,
                 const struct brw_blorp_params *params)
{
   uint32_t blend_state_offset = 0;
   uint32_t color_calc_state_offset = 0;
   uint32_t depth_stencil_state_offset;

#if GEN_GEN == 6
   /* Emit workaround flushes when we switch from drawing to blorping. */
   brw_emit_post_sync_nonzero_flush(brw);
#endif

   brw_upload_state_base_address(brw);

#if GEN_GEN >= 8
   gen7_l3_state.emit(brw);
#endif

   blorp_emit_vertex_buffers(brw, params);
   blorp_emit_vertex_elements(brw, params);

   emit_urb_config(brw, params);

   if (params->wm_prog_data) {
      blend_state_offset = blorp_emit_blend_state(brw, params);
      color_calc_state_offset = blorp_emit_color_calc_state(brw, params);
   }
   depth_stencil_state_offset = blorp_emit_depth_stencil_state(brw, params);

#if GEN_GEN <= 6
   /* 3DSTATE_CC_STATE_POINTERS
    *
    * The pointer offsets are relative to
    * CMD_STATE_BASE_ADDRESS.DynamicStateBaseAddress.
    *
    * The HiZ op doesn't use BLEND_STATE or COLOR_CALC_STATE.
    *
    * The dynamic state emit helpers emit their own STATE_POINTERS packets on
    * gen7+.  However, on gen6 and earlier, they're all lumpped together in
    * one CC_STATE_POINTERS packet so we have to emit that here.
    */
   blorp_emit(brw, GENX(3DSTATE_CC_STATE_POINTERS), cc) {
      cc.BLEND_STATEChange = true;
      cc.COLOR_CALC_STATEChange = true;
      cc.DEPTH_STENCIL_STATEChange = true;
      cc.PointertoBLEND_STATE = blend_state_offset;
      cc.PointertoCOLOR_CALC_STATE = color_calc_state_offset;
      cc.PointertoDEPTH_STENCIL_STATE = depth_stencil_state_offset;
   }
#else
   (void)blend_state_offset;
   (void)color_calc_state_offset;
   (void)depth_stencil_state_offset;
#endif

   blorp_emit(brw, GENX(3DSTATE_CONSTANT_VS), vs);
#if GEN_GEN >= 7
   blorp_emit(brw, GENX(3DSTATE_CONSTANT_HS), hs);
   blorp_emit(brw, GENX(3DSTATE_CONSTANT_DS), DS);
#endif
   blorp_emit(brw, GENX(3DSTATE_CONSTANT_GS), gs);
   blorp_emit(brw, GENX(3DSTATE_CONSTANT_PS), ps);

   if (brw->use_resource_streamer)
      gen7_disable_hw_binding_tables(brw);

   if (params->wm_prog_data)
      blorp_emit_surface_states(brw, params);

   if (params->src.bo)
      blorp_emit_sampler_state(brw, params);

#if GEN_GEN >= 8
   gen8_emit_3dstate_multisample(brw, params->dst.surf.samples);
#else
   gen6_emit_3dstate_multisample(brw, params->dst.surf.samples);
#endif

   blorp_emit(brw, GENX(3DSTATE_SAMPLE_MASK), mask) {
      mask.SampleMask = (1 << params->dst.surf.samples) - 1;
   }

   /* From the BSpec, 3D Pipeline > Geometry > Vertex Shader > State,
    * 3DSTATE_VS, Dword 5.0 "VS Function Enable":
    *
    *   [DevSNB] A pipeline flush must be programmed prior to a
    *   3DSTATE_VS command that causes the VS Function Enable to
    *   toggle. Pipeline flush can be executed by sending a PIPE_CONTROL
    *   command with CS stall bit set and a post sync operation.
    *
    * We've already done one at the start of the BLORP operation.
    */
   blorp_emit(brw, GENX(3DSTATE_VS), vs);
#if GEN_GEN >= 7
   blorp_emit(brw, GENX(3DSTATE_HS), hs);
   blorp_emit(brw, GENX(3DSTATE_TE), te);
   blorp_emit(brw, GENX(3DSTATE_DS), DS);
   blorp_emit(brw, GENX(3DSTATE_STREAMOUT), so);
#endif
   blorp_emit(brw, GENX(3DSTATE_GS), gs);

   blorp_emit(brw, GENX(3DSTATE_CLIP), clip) {
      clip.PerspectiveDivideDisable = true;
   }

   blorp_emit_sf_config(brw, params);
   blorp_emit_ps_config(brw, params);

   blorp_emit_viewport_state(brw, params);

   if (params->depth.bo) {
      blorp_emit_depth_stencil_config(brw, params);
   } else {
      brw_emit_depth_stall_flushes(brw);

      blorp_emit(brw, GENX(3DSTATE_DEPTH_BUFFER), db) {
         db.SurfaceType = SURFTYPE_NULL;
         db.SurfaceFormat = D32_FLOAT;
      }
      blorp_emit(brw, GENX(3DSTATE_HIER_DEPTH_BUFFER), hiz);
      blorp_emit(brw, GENX(3DSTATE_STENCIL_BUFFER), sb);
   }

   /* 3DSTATE_CLEAR_PARAMS
    *
    * From the Sandybridge PRM, Volume 2, Part 1, Section 3DSTATE_CLEAR_PARAMS:
    *   [DevSNB] 3DSTATE_CLEAR_PARAMS packet must follow the DEPTH_BUFFER_STATE
    *   packet when HiZ is enabled and the DEPTH_BUFFER_STATE changes.
    */
   blorp_emit(brw, GENX(3DSTATE_CLEAR_PARAMS), clear) {
      clear.DepthClearValueValid = true;
      clear.DepthClearValue = params->depth.clear_color.u32[0];
   }

   blorp_emit(brw, GENX(3DSTATE_DRAWING_RECTANGLE), rect) {
      rect.ClippedDrawingRectangleXMax = MAX2(params->x1, params->x0) - 1;
      rect.ClippedDrawingRectangleYMax = MAX2(params->y1, params->y0) - 1;
   }

   blorp_emit(brw, GENX(3DPRIMITIVE), prim) {
      prim.VertexAccessType = SEQUENTIAL;
      prim.PrimitiveTopologyType = _3DPRIM_RECTLIST;
      prim.VertexCountPerInstance = 3;
      prim.InstanceCount = params->num_layers;
   }
}
