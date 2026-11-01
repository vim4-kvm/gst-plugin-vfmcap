/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real transform negotiation and rollback; GPU pixels need a board test. */
#include "../src/gstvim4pq2sdr.c"
static guint contexts, submissions;
static gboolean fail_pool, fail_gpu;
AmlyGpu *amly_gpu_new_tonemap(GError **error)
{ contexts++; return (AmlyGpu *)1; }
void amly_gpu_free(AmlyGpu *gpu) { if (gpu) contexts--; }
GstBufferPool *codecmm_pool_new(gsize size, guint buffers, GError **error)
{
    g_assert_cmpuint(buffers, ==, 2); g_assert_cmpuint(size, ==, 640 * 360 * 3 / 2);
    if (fail_pool) { g_set_error_literal(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NO_SPACE_LEFT, "injected"); return NULL; }
    return gst_buffer_pool_new();
}
gboolean amly_gpu_tonemap(AmlyGpu *gpu, int input_fd, gsize input_size,
    int output_fd, gsize output_size, guint width, guint height,
    guint input_stride, guint input_rows, guint output_stride, float white, float peak, GError **error)
{
    submissions++;
    if (fail_gpu) { g_set_error_literal(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED, "injected"); return FALSE; }
    return TRUE;
}
static void negotiation_and_rollback(void)
{
    GstBaseTransform *filter = g_object_new(gst_vim4_pq2sdr_get_type(), NULL);
    gst_object_ref_sink(filter);
    GstCaps *pq = gst_caps_from_string("video/x-raw,format=P010_10LE,width=640,height=360,colorimetry=bt2100-pq");
    GstCaps *sdr = gst_caps_from_string("video/x-raw,format=NV12,width=640,height=360,colorimetry=bt709");
    GstCaps *result = transform_caps(filter, GST_PAD_SINK, pq, NULL);
    g_assert_true(gst_caps_is_equal(result, sdr)); gst_caps_unref(result);
    g_assert_true(set_caps(filter, pq, sdr));
    g_assert_cmpuint(contexts, ==, 1);
    g_assert_false(gst_base_transform_is_passthrough(filter));
    stop(filter); g_assert_cmpuint(contexts, ==, 0);
    fail_pool = TRUE;
    g_assert_false(set_caps(filter, pq, sdr));
    g_assert_cmpuint(contexts, ==, 0);
    fail_pool = FALSE;
    g_assert_true(set_caps(filter, sdr, sdr));
    g_assert_true(gst_base_transform_is_passthrough(filter));
    GstCaps *wrong = gst_caps_copy(pq);
    gst_caps_set_simple(wrong, "colorimetry", G_TYPE_STRING, "bt709", NULL);
    g_assert_false(set_caps(filter, wrong, sdr));
    gst_caps_unref(wrong);
    wrong = gst_caps_copy(sdr);
    gst_caps_set_simple(wrong, "colorimetry", G_TYPE_STRING, "bt2100-pq", NULL);
    g_assert_false(set_caps(filter, wrong, sdr)); gst_caps_unref(wrong);
    g_assert_cmpuint(contexts, ==, 0); g_assert_cmpuint(submissions, ==, 0);
    gst_caps_unref(pq); gst_caps_unref(sdr); gst_object_unref(filter);
}
static void live_curve_preferences(void)
{
    GstVim4Pq2Sdr *filter = g_object_new(gst_vim4_pq2sdr_get_type(), NULL);
    gst_object_ref_sink(filter);
    curve_settings(filter, "HDR_SDR_WHITE=250\nHDR_SDR_PEAK=1500\n");
    g_assert_cmpuint(filter->white, ==, 250); g_assert_cmpuint(filter->peak, ==, 1500);
    curve_settings(filter, "HDR_SDR_WHITE=400\nHDR_SDR_PEAK=200\n");
    g_assert_cmpuint(filter->white, ==, 250); g_assert_cmpuint(filter->peak, ==, 1500);
    g_object_set(filter, "sdr-white", 300u, NULL);
    curve_settings(filter, "HDR_SDR_WHITE=203\nHDR_SDR_PEAK=1200\n");
    g_assert_cmpuint(filter->white, ==, 300); g_assert_cmpuint(filter->peak, ==, 1200);
    gst_object_unref(filter);
}

static void released(gpointer count, GstMiniObject *object)
{
    (void)object;
    g_atomic_int_inc((gint *)count);
}

static void nv12_buffers_return_to_capture(void)
{
    gint returned = 0;
    GstElement *pipeline = gst_pipeline_new(NULL);
    GstElement *source = gst_element_factory_make("appsrc", NULL);
    GstElement *filter = g_object_new(gst_vim4_pq2sdr_get_type(), NULL);
    GstElement *sink = gst_element_factory_make("fakesink", NULL);
    g_assert_nonnull(source); g_assert_nonnull(sink);
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=NV12,width=640,height=360,colorimetry=bt709,framerate=60/1");
    g_object_set(source, "caps", caps, "format", GST_FORMAT_TIME, "max-bytes", (guint64)0, NULL);
    g_object_set(sink, "sync", FALSE, "enable-last-sample", FALSE, NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(pipeline), source, filter, sink, NULL);
    g_assert_true(gst_element_link_many(source, filter, sink, NULL));
    g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
    for (guint i = 0; i < 60; i++) {
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, 640 * 360 * 3 / 2, NULL);
        GST_BUFFER_PTS(buffer) = i * GST_SECOND / 60;
        GST_BUFFER_DURATION(buffer) = GST_SECOND / 60;
        gst_mini_object_weak_ref(GST_MINI_OBJECT(buffer), released, &returned);
        GstFlowReturn flow;
        g_signal_emit_by_name(source, "push-buffer", buffer, &flow);
        gst_buffer_unref(buffer);
        g_assert_cmpint(flow, ==, GST_FLOW_OK);
    }
    GstFlowReturn flow;
    g_signal_emit_by_name(source, "end-of-stream", &flow);
    g_assert_cmpint(flow, ==, GST_FLOW_OK);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    g_assert_nonnull(message);
    g_assert_cmpint(GST_MESSAGE_TYPE(message), ==, GST_MESSAGE_EOS);
    gst_message_unref(message); gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_assert_cmpint(g_atomic_int_get(&returned), ==, 60);
    g_assert_cmpuint(contexts, ==, 0); g_assert_cmpuint(submissions, ==, 0);
}
int main(int argc, char **argv)
{
    gst_init(&argc, &argv); g_test_init(&argc, &argv, NULL);
    g_test_add_func("/pq2sdr/negotiation-and-rollback", negotiation_and_rollback);
    g_test_add_func("/pq2sdr/live-curve-preferences", live_curve_preferences);
    g_test_add_func("/pq2sdr/nv12-buffers-return-to-capture", nv12_buffers_return_to_capture);
    return g_test_run();
}
