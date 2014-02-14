/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *      Jerome Glisse
 *      Corbin Simpson <MostAwesomeDude@gmail.com>
 */

#include "pipe/p_screen.h"
#include "util/u_format.h"
#include "util/u_math.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"

#include "r600.h"
#include "radeonsi_pipe.h"

static void r600_buffer_destroy(struct pipe_screen *screen,
				struct pipe_resource *buf)
{
	struct r600_resource *rbuffer = r600_resource(buf);

	pb_reference(&rbuffer->buf, NULL);
	FREE(rbuffer);
}

static void *r600_buffer_transfer_map(struct pipe_context *ctx,
                                      struct pipe_resource *resource,
                                      unsigned level,
                                      unsigned usage,
                                      const struct pipe_box *box,
                                      struct pipe_transfer **ptransfer)
{
	struct r600_context *rctx = (struct r600_context*)ctx;
	struct pipe_transfer *transfer;
        struct r600_resource *rbuffer = r600_resource(resource);
        uint8_t *data;

	data = rctx->b.ws->buffer_map(rbuffer->cs_buf, rctx->b.rings.gfx.cs, usage);
        if (!data) {
		return NULL;
        }

	transfer = util_slab_alloc(&rctx->pool_transfers);
	transfer->resource = resource;
	transfer->level = level;
	transfer->usage = usage;
	transfer->box = *box;
	transfer->stride = 0;
	transfer->layer_stride = 0;
        *ptransfer = transfer;

	return (uint8_t*)data + transfer->box.x;
}

static void r600_buffer_transfer_unmap(struct pipe_context *ctx,
					struct pipe_transfer *transfer)
{
	struct r600_context *rctx = (struct r600_context*)ctx;
	util_slab_free(&rctx->pool_transfers, transfer);
}

static void r600_buffer_transfer_flush_region(struct pipe_context *pipe,
						struct pipe_transfer *transfer,
						const struct pipe_box *box)
{
}

static const struct u_resource_vtbl r600_buffer_vtbl =
{
	u_default_resource_get_handle,		/* get_handle */
	r600_buffer_destroy,			/* resource_destroy */
	r600_buffer_transfer_map,		/* transfer_map */
	r600_buffer_transfer_flush_region,	/* transfer_flush_region */
	r600_buffer_transfer_unmap,		/* transfer_unmap */
	NULL	/* transfer_inline_write */
};

struct pipe_resource *si_buffer_create(struct pipe_screen *screen,
				       const struct pipe_resource *templ)
{
	struct r600_screen *rscreen = (struct r600_screen*)screen;
	struct r600_resource *rbuffer;
	/* XXX We probably want a different alignment for buffers and textures. */
	unsigned alignment = 4096;

	rbuffer = MALLOC_STRUCT(r600_resource);

	rbuffer->b.b = *templ;
	pipe_reference_init(&rbuffer->b.b.reference, 1);
	rbuffer->b.b.screen = screen;
	rbuffer->b.vtbl = &r600_buffer_vtbl;
	util_range_init(&rbuffer->valid_buffer_range);

	if (!r600_init_resource(&rscreen->b, rbuffer, templ->width0, alignment, TRUE, templ->usage)) {
		FREE(rbuffer);
		return NULL;
	}
	return &rbuffer->b.b;
}

void r600_upload_index_buffer(struct r600_context *rctx,
			      struct pipe_index_buffer *ib, unsigned count)
{
	u_upload_data(rctx->uploader, 0, count * ib->index_size,
		      ib->user_buffer, &ib->offset, &ib->buffer);
}

void r600_upload_const_buffer(struct r600_context *rctx, struct r600_resource **rbuffer,
			const uint8_t *ptr, unsigned size,
			uint32_t *const_offset)
{
	if (R600_BIG_ENDIAN) {
		uint32_t *tmpPtr;
		unsigned i;

		if (!(tmpPtr = malloc(size))) {
			R600_ERR("Failed to allocate BE swap buffer.\n");
			return;
		}

		for (i = 0; i < size / 4; ++i) {
			tmpPtr[i] = util_bswap32(((uint32_t *)ptr)[i]);
		}

		u_upload_data(rctx->uploader, 0, size, tmpPtr, const_offset,
				(struct pipe_resource**)rbuffer);

		free(tmpPtr);
	} else {
		u_upload_data(rctx->uploader, 0, size, ptr, const_offset,
					(struct pipe_resource**)rbuffer);
	}
}
