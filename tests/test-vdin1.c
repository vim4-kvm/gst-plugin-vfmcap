/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/video/video.h>
#include "vdin1.h"

static void geometry(void)
{
    GstCaps *caps = vim4_vdin1_caps("Hactive: 3840\nVactive: 2160\nFrame Rate: 5998\n", 1080);
    GstVideoInfo info;
    g_assert_nonnull(caps);
    g_assert_true(gst_video_info_from_caps(&info, caps));
    g_assert_cmpint(info.width, ==, 1920);
    g_assert_cmpint(info.height, ==, 1080);
    g_assert_cmpint(info.fps_n, ==, 60);
    g_assert_cmpint(GST_VIDEO_INFO_FORMAT(&info), ==, GST_VIDEO_FORMAT_NV21);
    gst_caps_unref(caps);
    caps = vim4_vdin1_caps("Hactive: 1280\nVactive: 720\nFrame Rate: 5000\n", 2160);
    g_assert_true(gst_video_info_from_caps(&info, caps));
    g_assert_cmpint(info.width, ==, 1280);
    g_assert_cmpint(info.height, ==, 720);
    g_assert_cmpint(info.fps_n, ==, 50);
    gst_caps_unref(caps);
    g_assert_null(vim4_vdin1_caps("Hactive: 3840\nVactive: 2160\nFrame Rate: 0\n", 0));
    g_assert_null(vim4_vdin1_caps("Hactive: 3840\nFrame Rate: 6000\n", 0));
}

static void dma_allocation(void)
{
    GstElement *capture = g_object_new(gst_vim4_vdin1_src_get_type(), NULL);
    GstElement *source = gst_bin_get_by_name(GST_BIN(capture), "capture");
    g_assert_nonnull(source);
    GstPad *pad = gst_element_get_static_pad(source, "src");
    GstCaps *caps = vim4_vdin1_caps("Hactive: 3840\nVactive: 2160\nFrame Rate: 6000\n", 0);
    GstQuery *query = gst_query_new_allocation(caps, TRUE);
    g_assert_true(gst_pad_peer_query(pad, query));
    g_assert_true(gst_query_find_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL));
    guint bytes, minimum, maximum;
    gst_query_parse_nth_allocation_pool(query, 0, NULL, &bytes, &minimum, &maximum);
    g_assert_cmpuint(bytes, >=, 3840 * 2160 * 3 / 2);
    /* v4l2src adds four slots. Leave room for the 200 MiB encoder workspace
     * and reallocating VDIN0: the 4K raw pool must stay below 90 MiB. */
    g_assert_cmpuint(minimum + 4, >=, 6);
    g_assert_cmpuint((guint64)(minimum + 4) * bytes, <=, 90 * 1024 * 1024);
    g_assert_cmpuint(maximum, >=, minimum);
    gst_query_unref(query);
    gst_caps_unref(caps);
    gst_object_unref(pad);
    gst_object_unref(source);
    /* video71 must never silently capture display writeback on stock kernels. */
    g_unsetenv("VIM4_SECONDARY_HDMI_CAPTURE");
    g_assert_cmpint(gst_element_set_state(capture, GST_STATE_READY), ==, GST_STATE_CHANGE_FAILURE);
    gst_element_set_state(capture, GST_STATE_NULL);
    gst_object_unref(capture);
}

static void variable_cadence(void)
{
    for (guint rate = 4800; rate <= 12000; rate += 100) {
        gchar *text = g_strdup_printf("Hactive: 1920\nVactive: 1080\nFrame Rate: %u\nCapture Frame Rate: 12000\n", rate);
        GstCaps *caps = vim4_vdin1_caps(text, 0);
        GstVideoInfo info;
        g_assert_nonnull(caps);
        g_assert_true(gst_video_info_from_caps(&info, caps));
        g_assert_cmpint(info.fps_n, ==, 120);
        gst_caps_unref(caps);
        g_free(text);
    }
    g_assert_null(vim4_vdin1_caps("Hactive: 1920\nVactive: 1080\nFrame Rate: 4800\nCapture Frame Rate: 0\n", 0));
    g_assert_null(vim4_vdin1_caps("Hactive: 1920\nVactive: 1080\nFrame Rate: 0\nCapture Frame Rate: 12000\n", 0));
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/vdin1/geometry", geometry);
    g_test_add_func("/vdin1/dma-allocation", dma_allocation);
    g_test_add_func("/vdin1/variable-cadence", variable_cadence);
    return g_test_run();
}
