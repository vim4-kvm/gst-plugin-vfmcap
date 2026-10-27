/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <linux/dma-heap.h>
#include <gst/gst.h>

static int allocated[16], allocated_count, fail_at = -1, heap_fd;
static int fake_open(const char *path, int flags)
{
	(void)path;
	(void)flags;
	heap_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	return heap_fd;
}

static int fake_ioctl(int fd, unsigned long command, ...)
{
	va_list args;
	va_start(args, command);
	struct dma_heap_allocation_data *request = va_arg(args, void *);
	va_end(args);
	g_assert_cmpint(fd, ==, heap_fd);
	g_assert_cmpuint(command, ==, DMA_HEAP_IOCTL_ALLOC);
	if (allocated_count == fail_at) {
		errno = ENOMEM;
		return -1;
	}
	int output = memfd_create("codecmm-pool-test", MFD_CLOEXEC);
	g_assert_cmpint(output, >=, 0);
	g_assert_cmpint(ftruncate(output, request->len), ==, 0);
	request->fd = output;
	allocated[allocated_count++] = output;
	return 0;
}

#define open fake_open
#define ioctl fake_ioctl
#include "../src/codecmm-pool.c"
#undef open
#undef ioctl

typedef struct {
	GstBufferPool *pool;
	GMutex mutex;
	GCond started;
	gboolean entering;
	GstFlowReturn flow;
} Acquisition;

static gpointer acquire_waiting(gpointer data)
{
	Acquisition *job = data;
	g_mutex_lock(&job->mutex);
	job->entering = TRUE;
	g_cond_signal(&job->started);
	g_mutex_unlock(&job->mutex);
	GstBuffer *buffer = NULL;
	job->flow = gst_buffer_pool_acquire_buffer(job->pool, &buffer, NULL);
	g_assert_null(buffer);
	return NULL;
}

static void bounded_flush_and_outstanding_lifetime(void)
{
	allocated_count = 0;
	fail_at = -1;
	GstBufferPool *pool = codecmm_pool_new(12345, 2, NULL);
	g_assert_nonnull(pool);
	g_assert_cmpint(allocated_count, ==, 2);
	GstBuffer *first = NULL, *second = NULL;
	g_assert_cmpint(gst_buffer_pool_acquire_buffer(pool, &first, NULL), ==, GST_FLOW_OK);
	g_assert_cmpint(gst_buffer_pool_acquire_buffer(pool, &second, NULL), ==, GST_FLOW_OK);
	GstMemory *memory = gst_buffer_peek_memory(first, 0);
	g_assert_true(gst_is_dmabuf_memory(memory));
	gsize offset, capacity;
	g_assert_cmpuint(gst_memory_get_sizes(memory, &offset, &capacity), ==, 12345);
	g_assert_cmpuint(offset, ==, 0);
	g_assert_cmpuint(capacity, ==, 16384);
	Acquisition job = { .pool = pool };
	g_mutex_init(&job.mutex);
	g_cond_init(&job.started);
	GThread *worker = g_thread_new("acquire", acquire_waiting, &job);
	g_mutex_lock(&job.mutex);
	while (!job.entering)
		g_cond_wait(&job.started, &job.mutex);
	g_mutex_unlock(&job.mutex);
	gst_buffer_pool_set_flushing(pool, TRUE);
	g_thread_join(worker);
	g_assert_cmpint(job.flow, ==, GST_FLOW_FLUSHING);
	g_assert_cmpint(allocated_count, ==, 2);
	g_cond_clear(&job.started);
	g_mutex_clear(&job.mutex);
	gst_buffer_pool_set_active(pool, FALSE);
	int retained = gst_dmabuf_memory_get_fd(memory);
	gst_object_unref(pool);
	g_assert_cmpint(fcntl(retained, F_GETFD), >=, 0);
	gst_buffer_unref(first);
	gst_buffer_unref(second);
	for (int i = 0; i < allocated_count; i++)
		g_assert_cmpint(fcntl(allocated[i], F_GETFD), ==, -1);
	g_assert_cmpint(fcntl(heap_fd, F_GETFD), ==, -1);
}

static void partial_allocation_failure(void)
{
	allocated_count = 0;
	fail_at = 1;
	GError *error = NULL;
	g_assert_null(codecmm_pool_new(4096, 2, &error));
	g_assert_error(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NO_SPACE_LEFT);
	g_clear_error(&error);
	g_assert_cmpint(fcntl(allocated[0], F_GETFD), ==, -1);
	g_assert_cmpint(fcntl(heap_fd, F_GETFD), ==, -1);
}

int main(int argc, char **argv)
{
	gst_init(&argc, &argv);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/codecmm/bounded-flush-and-lifetime", bounded_flush_and_outstanding_lifetime);
	g_test_add_func("/codecmm/partial-allocation-failure", partial_allocation_failure);
	return g_test_run();
}
