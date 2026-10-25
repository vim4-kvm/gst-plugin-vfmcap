/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdio.h>
#include <gst/video/video.h>
#include "vdin1.h"

typedef struct {
    GstBin parent;
    GstElement *source, *filter;
    guint height;
    gint num_buffers;
} GstVim4Vdin1Src;
typedef struct { GstBinClass parent; } GstVim4Vdin1SrcClass;
G_DEFINE_TYPE(GstVim4Vdin1Src, gst_vim4_vdin1_src, GST_TYPE_BIN)
enum { PROP_0, PROP_HEIGHT, PROP_NUM_BUFFERS };

GstCaps *vim4_vdin1_caps(const gchar *input_info, guint output_height)
{
    guint width = 0, height = 0, rate = 0;
    gchar **lines = g_strsplit(input_info, "\n", -1);
    for (gchar **line = lines; *line; ++line) {
        if (sscanf(*line, " Hactive: %u", &width) == 1) continue;
        if (sscanf(*line, " Vactive: %u", &height) == 1) continue;
        sscanf(*line, " Frame Rate: %u", &rate);
    }
    g_strfreev(lines);
    if (width < 16 || width > 4096 || height < 16 || height > 2160 ||
        rate < 100 || rate > 12000)
        return NULL;
    if (output_height && output_height < height) {
        width = ((guint64)width * output_height / height) & ~1u;
        height = output_height & ~1u;
    }
    if (width < 16 || height < 16 || (width & 1) || (height & 1))
        return NULL;
    return gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV21",
        "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
        "framerate", GST_TYPE_FRACTION, (rate + 50) / 100, 1,
        "colorimetry", G_TYPE_STRING, "bt709", NULL);
}

static GstPadProbeReturn allocation(GstPad *pad, GstPadProbeInfo *probe, gpointer data)
{
    GstQuery *query = GST_PAD_PROBE_INFO_QUERY(probe);
    if (GST_QUERY_TYPE(query) == GST_QUERY_ALLOCATION) {
        GstCaps *caps;
        GstVideoInfo info;
        gst_query_parse_allocation(query, &caps, NULL);
        if (!caps || !gst_video_info_from_caps(&info, caps))
            return GST_PAD_PROBE_OK;
        /* The DMA pool must outlive buffers held by the encoder/JPEG branches.
         * VideoMeta keeps the real VDIN stride and offsets through downstream. */
        gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
        gst_query_add_allocation_pool(query, NULL, info.size, 6, 8);
        return GST_PAD_PROBE_HANDLED;
    }
    return GST_PAD_PROBE_OK;
}

static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4Vdin1Src *self = (GstVim4Vdin1Src *)object;
    if (id == PROP_HEIGHT) self->height = g_value_get_uint(value);
    else if (id == PROP_NUM_BUFFERS) self->num_buffers = g_value_get_int(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    GstVim4Vdin1Src *self = (GstVim4Vdin1Src *)object;
    if (id == PROP_HEIGHT) g_value_set_uint(value, self->height);
    else if (id == PROP_NUM_BUFFERS) g_value_set_int(value, self->num_buffers);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static GstStateChangeReturn change_state(GstElement *element, GstStateChange transition)
{
    GstVim4Vdin1Src *self = (GstVim4Vdin1Src *)element;
    if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
        gchar *input_info = NULL;
        GstCaps *caps = NULL;
        /* video71 is HDMI only with the explicitly enabled native-HDR runtime;
         * stock 5.15 uses this node for display writeback. */
        if (g_strcmp0(g_getenv("VIM4_SECONDARY_HDMI_CAPTURE"), "1") != 0 ||
            !self->source || !self->filter ||
            !g_file_get_contents("/sys/class/hdmirx/hdmirx0/info", &input_info, NULL, NULL) ||
            !(caps = vim4_vdin1_caps(input_info, self->height))) {
            g_free(input_info);
            GST_ELEMENT_ERROR(element, RESOURCE, SETTINGS,
                ("VDIN1 HDMI capture is unavailable"),
                ("Requires the native-HDR runtime, standard v4l2src and a valid HDMI input"));
            return GST_STATE_CHANGE_FAILURE;
        }
        g_free(input_info);
        g_object_set(self->source, "device", "/dev/video71", "io-mode", 4,
            "do-timestamp", TRUE, "num-buffers", self->num_buffers, NULL);
        g_object_set(self->filter, "caps", caps, NULL);
        gst_caps_unref(caps);
    }
    return GST_ELEMENT_CLASS(gst_vim4_vdin1_src_parent_class)->change_state(element, transition);
}

static void gst_vim4_vdin1_src_class_init(GstVim4Vdin1SrcClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    g_object_class_install_property(object, PROP_HEIGHT, g_param_spec_uint("height", "Output height",
        "VDIN1 downscale height; zero keeps the HDMI input dimensions", 0, 2160, 0,
        G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object, PROP_NUM_BUFFERS, g_param_spec_int("num-buffers", "Number of buffers",
        "Capture limit; minus one is unlimited", -1, G_MAXINT, -1,
        G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS));
    element->change_state = change_state;
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=NV21,width=[16,4096],height=[16,2160]");
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    gst_element_class_set_static_metadata(element, "VIM4 secondary HDMI capture", "Source/Video",
        "Independent SDR DMA capture alongside native HDR display", "vim4-kvm");
}

static void gst_vim4_vdin1_src_init(GstVim4Vdin1Src *self)
{
    self->num_buffers = -1;
    self->source = gst_element_factory_make("v4l2src", "capture");
    self->filter = gst_element_factory_make("capsfilter", "capture_format");
    if (!self->source || !self->filter) {
        gst_clear_object(&self->source);
        gst_clear_object(&self->filter);
        return;
    }
    gst_bin_add_many(GST_BIN(self), self->source, self->filter, NULL);
    gst_element_link(self->source, self->filter);
    GstPad *sink = gst_element_get_static_pad(self->filter, "sink");
    gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, allocation, NULL, NULL);
    gst_object_unref(sink);
    GstPad *src = gst_element_get_static_pad(self->filter, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", src));
    gst_object_unref(src);
}
