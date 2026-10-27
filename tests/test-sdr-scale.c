/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-heap.h>
#include <gst/check/gstharness.h>
#include <aml_ge2d.h>

/* Exercise negotiation and DMA metadata through the real transform. Only the
 * physical heap and synchronous GE2D submission are replaced on the host. */
static int heap_open(const char *path, int flags) { return open("/dev/null", flags); }
static int heap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    struct dma_heap_allocation_data *allocation = va_arg(args, void *);
    va_end(args);
    allocation->fd = memfd_create("scale-test", MFD_CLOEXEC);
    return ftruncate(allocation->fd, allocation->len);
}
#define open heap_open
#define ioctl heap_ioctl
#include "../src/codecmm-pool.c"
#undef ioctl
#undef open

static aml_ge2d_info_t submitted;
static guint calls;
int aml_ge2d_init(aml_ge2d_t *engine) { return 0; }
void aml_ge2d_exit(aml_ge2d_t *engine) {}
int aml_ge2d_process(aml_ge2d_info_t *command) { submitted = *command; calls++; return 0; }
#include "../src/gstvim4scale.c"

static GstBuffer *input_buffer(guint width, guint height, guint stride)
{
    int fd = memfd_create("scale-input", MFD_CLOEXEC);
    gsize size = (gsize)stride * height * 3 / 2;
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    GstAllocator *allocator = gst_dmabuf_allocator_new();
    GstBuffer *buffer = gst_buffer_new();
    gst_buffer_append_memory(buffer, gst_dmabuf_allocator_alloc(allocator, fd, size));
    gst_object_unref(allocator);
    gsize offsets[GST_VIDEO_MAX_PLANES] = {0, (gsize)stride * height};
    gint strides[GST_VIDEO_MAX_PLANES] = {stride, stride};
    gst_buffer_add_video_meta_full(buffer, 0, GST_VIDEO_FORMAT_NV21, width, height, 2, offsets, strides);
    GST_BUFFER_PTS(buffer) = 123 * GST_MSECOND;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 60;
    return buffer;
}

static void padded_scaling(void)
{
    for (guint alignment = 2; alignment <= 16; alignment += 14) {
        GstElement *element = g_object_new(gst_vim4_scale_get_type(), "height", 720, "width-alignment", alignment, NULL);
        GstHarness *h = gst_harness_new_with_element(element, "sink", "src");
        gst_harness_set_src_caps_str(h, "video/x-raw,format=NV21,width=3440,height=1440,framerate=60/1,colorimetry=bt709");
        guint expected_width = 1720 / alignment * alignment;
        guint before = calls;
        g_assert_cmpint(gst_harness_push(h, input_buffer(3440, 1440, 3456)), ==, GST_FLOW_OK);
        GstBuffer *output = gst_harness_pull(h);
        GstVideoMeta *meta = gst_buffer_get_video_meta(output);
        g_assert_cmpuint(meta->width, ==, expected_width);
        g_assert_cmpuint(meta->height, ==, 720);
        g_assert_cmpint(meta->stride[0], ==, 1728);
        g_assert_cmpuint(meta->offset[1], ==, 1728 * 720);
        g_assert_cmpuint(GST_BUFFER_PTS(output), ==, 123 * GST_MSECOND);
        g_assert_cmpuint(GST_BUFFER_DURATION(output), ==, GST_SECOND / 60);
        g_assert_cmpuint(calls, ==, before + 1);
        g_assert_cmpuint(submitted.src_info[0].canvas_w, ==, 3456);
        g_assert_cmpuint(submitted.src_info[0].rect.w, ==, 3440);
        g_assert_cmpuint(submitted.dst_info.canvas_w, ==, 1728);
        g_assert_cmpuint(submitted.dst_info.rect.w, ==, expected_width);
        g_assert_cmpint(submitted.src_info[0].format, ==, PIXEL_FORMAT_YCrCb_420_SP);
        g_assert_cmpint(submitted.dst_info.format, ==, PIXEL_FORMAT_YCbCr_420_SP_NV12);
        /* An encoder-held output must outlive teardown and keep its DMA fd. */
        int fd = gst_dmabuf_memory_get_fd(gst_buffer_peek_memory(output, 0));
        gst_harness_teardown(h);
        g_assert_cmpint(fcntl(fd, F_GETFD), >=, 0);
        gst_buffer_unref(output);
        g_assert_cmpint(fcntl(fd, F_GETFD), ==, -1);
    }
}

static void reject_bad_layout(void)
{
    GstElement *element = g_object_new(gst_vim4_scale_get_type(), "height", 720, NULL);
    GstHarness *h = gst_harness_new_with_element(element, "sink", "src");
    gst_harness_set_src_caps_str(h, "video/x-raw,format=NV21,width=3440,height=1440,framerate=60/1");
    GstBuffer *buffer = input_buffer(3440, 1440, 3456);
    gst_buffer_get_video_meta(buffer)->offset[1] -= 16;
    guint before = calls;
    g_assert_cmpint(gst_harness_push(h, buffer), ==, GST_FLOW_ERROR);
    g_assert_cmpuint(calls, ==, before);
    gst_harness_teardown(h);
}

static void native_limit_preserves_input_dma(void)
{
    GstElement *element = g_object_new(gst_vim4_scale_get_type(), "height", 2160, NULL);
    GstHarness *h = gst_harness_new_with_element(element, "sink", "src");
    gst_harness_set_src_caps_str(h, "video/x-raw,format=NV21,width=1920,height=1080,framerate=60/1");
    GstBuffer *input = input_buffer(1920, 1080, 1920);
    GstMemory *memory = gst_buffer_peek_memory(input, 0);
    guint before = calls;
    g_assert_cmpint(gst_harness_push(h, input), ==, GST_FLOW_OK);
    GstBuffer *output = gst_harness_pull(h);
    g_assert_true(gst_buffer_peek_memory(output, 0) == memory);
    g_assert_cmpuint(calls, ==, before);
    g_assert_null(((GstVim4Scale *)element)->pool);
    g_assert_cmpuint(gst_buffer_get_video_meta(output)->width, ==, 1920);
    gst_buffer_unref(output);
    gst_harness_teardown(h);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sdr-scale/padded-width-and-output-lifetime", padded_scaling);
    g_test_add_func("/sdr-scale/reject-bad-layout", reject_bad_layout);
    g_test_add_func("/sdr-scale/native-limit-keeps-input-dma", native_limit_preserves_input_dma);
    return g_test_run();
}
