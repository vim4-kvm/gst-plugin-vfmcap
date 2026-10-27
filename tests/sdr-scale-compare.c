/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

static void sync_buffer(GstBuffer *buffer, guint64 flags)
{
    struct dma_buf_sync sync = {.flags = flags};
    int fd = gst_dmabuf_memory_get_fd(gst_buffer_peek_memory(buffer, 0));
    g_assert_cmpint(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync), ==, 0);
}

/* Exclusive board diagnostic: poisonous row padding must not enter the scaled
 * picture, and NV21 V/U must become NV12 U/V without changing YUV values. */
int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    guint width = 3440, height = 1440, stride = 3456, frames = 240;
    if (argc != 2 || g_strcmp0(argv[1], "--exclusive")) {
        g_printerr("Usage: sdr-scale-compare --exclusive (requires idle capture/encoder)\n");
        return 2;
    }
    gsize size = (gsize)stride * height * 3 / 2;
    int heap = open("/dev/dma_heap/heap-codecmm", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(heap, >=, 0);
    struct dma_heap_allocation_data allocation = {.len = (size + 4095) & ~(gsize)4095, .fd_flags = O_RDWR | O_CLOEXEC};
    g_assert_cmpint(ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation), ==, 0);
    close(heap);
    GstAllocator *allocator = gst_dmabuf_allocator_new();
    GstBuffer *input = gst_buffer_new();
    gst_buffer_append_memory(input, gst_dmabuf_allocator_alloc(allocator, allocation.fd, size));
    gst_object_unref(allocator);
    gsize offsets[GST_VIDEO_MAX_PLANES] = {0, (gsize)stride * height};
    gint strides[GST_VIDEO_MAX_PLANES] = {stride, stride};
    gst_buffer_add_video_meta_full(input, 0, GST_VIDEO_FORMAT_NV21, width, height, 2, offsets, strides);
    sync_buffer(input, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    GstMapInfo map;
    g_assert_true(gst_buffer_map(input, &map, GST_MAP_WRITE));
    memset(map.data, 250, map.size);
    for (guint y = 0; y < height; y++)
        for (guint x = 0; x < width; x++)
            map.data[y * stride + x] = (x < width / 2 ? 40 : 180) + (y < height / 2 ? 0 : 20);
    for (guint y = 0; y < height / 2; y++)
        for (guint x = 0; x < width; x += 2) {
            map.data[offsets[1] + y * stride + x] = 150;
            map.data[offsets[1] + y * stride + x + 1] = 90;
        }
    gst_buffer_unmap(input, &map);
    sync_buffer(input, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    for (guint alignment = 2; alignment <= 16; alignment += 14) {
        char *description = g_strdup_printf("appsrc name=input format=time caps=video/x-raw,format=NV21,width=3440,height=1440,framerate=60/1,colorimetry=bt709 ! vim4scale height=720 width-alignment=%u ! appsink name=output sync=false", alignment);
        GError *error = NULL;
        GstElement *pipeline = gst_parse_launch(description, &error);
        g_free(description);
        g_assert_no_error(error);
        GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
        g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
        gint64 begin = g_get_monotonic_time();
        for (guint frame = 0; frame < frames; frame++) {
            GstBuffer *copy = gst_buffer_copy(input);
            GST_BUFFER_PTS(copy) = frame * GST_SECOND / 60;
            GST_BUFFER_DURATION(copy) = GST_SECOND / 60;
            g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(source), copy), ==, GST_FLOW_OK);
            GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
            g_assert_nonnull(sample);
            GstBuffer *output = gst_sample_get_buffer(sample);
            g_assert_cmpuint(GST_BUFFER_PTS(output), ==, frame * GST_SECOND / 60);
            if (frame == 0 || frame == frames - 1) {
                GstVideoMeta *meta = gst_buffer_get_video_meta(output);
                g_assert_cmpuint(meta->width, ==, 1720 / alignment * alignment);
                g_assert_cmpuint(meta->height, ==, 720);
                sync_buffer(output, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
                g_assert_true(gst_buffer_map(output, &map, GST_MAP_READ));
                for (guint y = 8; y < meta->height; y += 16)
                    for (guint x = 8; x < meta->width; x += 16) {
                        if (ABS((gint)x - (gint)meta->width / 2) < 4 || ABS((gint)y - 360) < 4) continue;
                        guint expected = (x < meta->width / 2 ? 40 : 180) + (y < 360 ? 0 : 20);
                        g_assert_cmpint(ABS((gint)map.data[y * meta->stride[0] + x] - (gint)expected), <=, 2);
                    }
                for (guint y = 8; y < meta->height / 2; y += 16)
                    for (guint x = 8; x < meta->width; x += 16) {
                        gsize p = meta->offset[1] + y * meta->stride[1] + x;
                        g_assert_cmpint(ABS(map.data[p] - 90), <=, 2);
                        g_assert_cmpint(ABS(map.data[p + 1] - 150), <=, 2);
                    }
                gst_buffer_unmap(output, &map);
                sync_buffer(output, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            }
            gst_sample_unref(sample);
        }
        g_print("3440x1440 pitch3456 -> %ux720 pitch1728: pixels/chroma/PTS PASS, %.2f fps (%u frames, GE2D only)\n", 1720 / alignment * alignment, frames * 1e6 / (g_get_monotonic_time() - begin), frames);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(source);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
    }
    gst_buffer_unref(input);
    return 0;
}
