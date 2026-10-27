/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "codecmm-pool.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <gst/allocators/gstdmabuf.h>

typedef struct {
	GstBufferPool parent;
	GstAllocator *allocator;
	int heap_fd;
	gsize bytes;
} CodecmmPool;
typedef GstBufferPoolClass CodecmmPoolClass;
G_DEFINE_TYPE(CodecmmPool, codecmm_pool, GST_TYPE_BUFFER_POOL)

static GstFlowReturn allocate(GstBufferPool *pool, GstBuffer **buffer,
	GstBufferPoolAcquireParams *params)
{
	CodecmmPool *self = (CodecmmPool *)pool;
	struct dma_heap_allocation_data request = {
		.len = (self->bytes + 4095) & ~(gsize)4095,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	if (ioctl(self->heap_fd, DMA_HEAP_IOCTL_ALLOC, &request) < 0)
		return GST_FLOW_ERROR;
	GstMemory *memory = gst_dmabuf_allocator_alloc(self->allocator, request.fd, request.len);
	if (!memory) {
		close(request.fd);
		return GST_FLOW_ERROR;
	}
	/* Keep allocation capacity for Vulkan import, but expose only video bytes. */
	gst_memory_resize(memory, 0, self->bytes);
	*buffer = gst_buffer_new();
	gst_buffer_append_memory(*buffer, memory);
	return GST_FLOW_OK;
}

static void finalize(GObject *object)
{
	CodecmmPool *self = (CodecmmPool *)object;
	if (self->heap_fd >= 0)
		close(self->heap_fd);
	gst_clear_object(&self->allocator);
	G_OBJECT_CLASS(codecmm_pool_parent_class)->finalize(object);
}

static gboolean start(GstBufferPool *pool)
{
	GstBufferPoolClass *parent = GST_BUFFER_POOL_CLASS(codecmm_pool_parent_class);
	if (parent->start(pool))
		return TRUE;
	/* The base start leaves earlier allocations queued on partial failure,
	 * but set_active(FALSE) does not stop a pool that never became active. */
	parent->stop(pool);
	return FALSE;
}

static void codecmm_pool_class_init(CodecmmPoolClass *klass)
{
	klass->start = start;
	klass->alloc_buffer = allocate;
	G_OBJECT_CLASS(klass)->finalize = finalize;
}

static void codecmm_pool_init(CodecmmPool *self)
{
	self->heap_fd = -1;
	self->allocator = gst_dmabuf_allocator_new();
}

GstBufferPool *codecmm_pool_new(gsize bytes, guint buffers, GError **error)
{
	CodecmmPool *self = g_object_new(codecmm_pool_get_type(), NULL);
	GstBufferPool *pool = GST_BUFFER_POOL(self);
	gst_object_ref_sink(pool);
	self->bytes = bytes;
	self->heap_fd = open("/dev/dma_heap/heap-codecmm", O_RDONLY | O_CLOEXEC);
	if (!bytes || bytes > G_MAXUINT - 4095 || !buffers || self->heap_fd < 0)
		goto failed;
	GstStructure *config = gst_buffer_pool_get_config(pool);
	/* Allocate all slots before opening the encoder's large CMA workspace.
	 * Growing lazily after it opens can leave enough free bytes but no extent. */
	gst_buffer_pool_config_set_params(config, NULL, bytes, buffers, buffers);
	if (!gst_buffer_pool_set_config(pool, config) || !gst_buffer_pool_set_active(pool, TRUE))
		goto failed;
	return pool;
failed:
	g_set_error(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NO_SPACE_LEFT,
		"Cannot allocate %u contiguous codecmm buffers of %zu bytes", buffers, bytes);
	gst_buffer_pool_set_active(pool, FALSE);
	gst_object_unref(pool);
	return NULL;
}
