/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/base/gstbasetransform.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include "amly-gpu.h"
#include "codecmm-pool.h"

/* The raw branch shares its capture owner with Main10. Only this branch
 * changes transfer/primaries. No second VDIN open or CPU pixel conversion. */
typedef struct {
    GstBaseTransform parent;
    GstVideoInfo input, output;
    AmlyGpu *gpu;
    GstBufferPool *pool;
    guint stride, white, peak;
    gboolean explicit_white, explicit_peak;
    gint64 checked_at;
} GstVim4Pq2Sdr;
typedef GstBaseTransformClass GstVim4Pq2SdrClass;
G_DEFINE_TYPE(GstVim4Pq2Sdr, gst_vim4_pq2sdr, GST_TYPE_BASE_TRANSFORM)
enum { PROP_0, PROP_WHITE, PROP_PEAK };

static void curve_settings(GstVim4Pq2Sdr *self, const gchar *text)
{
    guint white = 203, peak = 1000;
    gchar **lines = g_strsplit(text, "\n", -1);
    for (gchar **line = lines; *line; line++) {
        gchar **pair = g_strsplit(*line, "=", 2);
        if (pair[1]) {
            gchar *end;
            guint64 value = g_ascii_strtoull(g_strstrip(pair[1]), &end, 10);
            if (*pair[1] && !*end) {
                if (!strcmp(g_strstrip(pair[0]), "HDR_SDR_WHITE") && value >= 160 && value <= 1000) white = value;
                if (!strcmp(g_strstrip(pair[0]), "HDR_SDR_PEAK") && value >= 161 && value <= 10000) peak = value;
            }
        }
        g_strfreev(pair);
    }
    g_strfreev(lines);
    if (self->explicit_white) white = self->white;
    if (self->explicit_peak) peak = self->peak;
    if (peak > white) { self->white = white; self->peak = peak; }
}
static void refresh_curve(GstVim4Pq2Sdr *self)
{
    gint64 now = g_get_monotonic_time();
    if (now - self->checked_at < G_USEC_PER_SEC) return;
    self->checked_at = now;
    gchar *text = NULL;
    if (g_file_get_contents("/etc/default/vim4-kvm-hdr", &text, NULL, NULL)) curve_settings(self, text);
    g_free(text);
}

static GstCaps *transform_caps(GstBaseTransform *base, GstPadDirection direction,
                               GstCaps *caps, GstCaps *filter)
{
    GstCaps *result = gst_caps_new_empty();
    for (guint i = 0; i < gst_caps_get_size(caps); i++) {
        GstStructure *structure = gst_structure_copy(gst_caps_get_structure(caps, i));
        gst_structure_remove_field(structure, "colorimetry");
        if (direction == GST_PAD_SINK) {
            gst_structure_set(structure, "format", G_TYPE_STRING, "NV12",
                "colorimetry", G_TYPE_STRING, "bt709", NULL);
            gst_caps_append_structure(result, structure);
        } else {
            GstCaps *formats = gst_caps_from_string("video/x-raw,format=(string){NV12,P010_10LE}");
            gst_structure_set_value(structure, "format", gst_structure_get_value(gst_caps_get_structure(formats, 0), "format"));
            gst_caps_unref(formats);
            gst_caps_append_structure(result, structure);
        }
    }
    if (filter) {
        GstCaps *intersection = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(result); result = intersection;
    }
    return result;
}

static gboolean stop(GstBaseTransform *base)
{
    GstVim4Pq2Sdr *self = (GstVim4Pq2Sdr *)base;
    if (self->pool) {
        gst_buffer_pool_set_active(self->pool, FALSE);
        gst_clear_object(&self->pool);
    }
    amly_gpu_free(self->gpu); self->gpu = NULL;
    return TRUE;
}

static gboolean set_caps(GstBaseTransform *base, GstCaps *input, GstCaps *output)
{
    GstVim4Pq2Sdr *self = (GstVim4Pq2Sdr *)base;
    stop(base);
    if (!gst_video_info_from_caps(&self->input, input) ||
        !gst_video_info_from_caps(&self->output, output) ||
        self->input.width != self->output.width || self->input.height != self->output.height ||
        self->input.width < 2 || self->input.width > 4096 || (self->input.width & 1) ||
        self->input.height < 2 || self->input.height > 2160 || (self->input.height & 1) ||
        GST_VIDEO_INFO_FORMAT(&self->output) != GST_VIDEO_FORMAT_NV12) return FALSE;
    if (GST_VIDEO_INFO_FORMAT(&self->input) == GST_VIDEO_FORMAT_NV12) {
        if (self->input.colorimetry.primaries != GST_VIDEO_COLOR_PRIMARIES_BT709 ||
            self->input.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709) return FALSE;
        gst_base_transform_set_passthrough(base, TRUE);
        return TRUE;
    }
    const GstVideoColorimetry *color = &self->input.colorimetry;
    if (GST_VIDEO_INFO_FORMAT(&self->input) != GST_VIDEO_FORMAT_P010_10LE ||
        color->range != GST_VIDEO_COLOR_RANGE_16_235 || color->matrix != GST_VIDEO_COLOR_MATRIX_BT2020 ||
        color->primaries != GST_VIDEO_COLOR_PRIMARIES_BT2020 ||
        gst_video_transfer_function_to_iso(color->transfer) != 16 || self->peak <= self->white)
        return FALSE;
    gst_base_transform_set_passthrough(base, FALSE);
    self->stride = (self->input.width + 63) & ~63u;
    GError *error = NULL;
    self->gpu = amly_gpu_new_tonemap(&error);
    if (self->gpu) self->pool = codecmm_pool_new((gsize)self->stride * self->input.height * 3 / 2, 2, &error);
    if (!self->gpu || !self->pool) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS, ("Cannot start GPU SDR branch"),
            ("%s", error ? error->message : "unknown error"));
        g_clear_error(&error); stop(base); return FALSE;
    }
    return TRUE;
}

static GstFlowReturn prepare(GstBaseTransform *base, GstBuffer *input, GstBuffer **output)
{
    GstVim4Pq2Sdr *self = (GstVim4Pq2Sdr *)base;
    if (gst_base_transform_is_passthrough(base)) { *output = gst_buffer_ref(input); return GST_FLOW_OK; }
    GstFlowReturn result = gst_buffer_pool_acquire_buffer(self->pool, output, NULL);
    if (result != GST_FLOW_OK) return result;
    gsize offsets[GST_VIDEO_MAX_PLANES] = {0, (gsize)self->stride * self->input.height};
    gint strides[GST_VIDEO_MAX_PLANES] = {self->stride, self->stride};
    gst_buffer_add_video_meta_full(*output, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12,
        self->input.width, self->input.height, 2, offsets, strides);
    return GST_FLOW_OK;
}

static gboolean dma(GstBuffer *buffer, int *fd, gsize *capacity)
{
    if (gst_buffer_n_memory(buffer) != 1) return FALSE;
    GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
    gsize offset;
    if (!gst_is_dmabuf_memory(memory)) return FALSE;
    gst_memory_get_sizes(memory, &offset, capacity);
    if (offset) return FALSE;
    *fd = gst_dmabuf_memory_get_fd(memory);
    return TRUE;
}

static GstFlowReturn transform(GstBaseTransform *base, GstBuffer *input, GstBuffer *output)
{
    GstVim4Pq2Sdr *self = (GstVim4Pq2Sdr *)base;
    GstVideoMeta *meta = gst_buffer_get_video_meta(input);
    int source, target;
    gsize source_capacity, target_capacity;
    if (!meta || meta->n_planes != 2 || meta->format != GST_VIDEO_FORMAT_P010_10LE ||
        meta->width != (guint)self->input.width || meta->height != (guint)self->input.height ||
        meta->stride[0] < self->input.width * 2 || meta->stride[0] != meta->stride[1] ||
        meta->offset[0] || meta->offset[1] % meta->stride[0] ||
        meta->offset[1] + (gsize)meta->stride[0] * self->input.height / 2 > gst_buffer_get_size(input) ||
        !dma(input, &source, &source_capacity) || !dma(output, &target, &target_capacity)) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("GPU SDR requires one padded P010 DMA allocation with VideoMeta"), (NULL));
        return GST_FLOW_ERROR;
    }
    GError *error = NULL;
    refresh_curve(self);
    gboolean ok = amly_gpu_tonemap(self->gpu, source, source_capacity, target, target_capacity,
        self->input.width, self->input.height, meta->stride[0], meta->offset[1] / meta->stride[0],
        self->stride, self->white, self->peak, &error);
    if (!ok) {
        GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("GPU SDR conversion failed"), ("%s", error->message));
        g_clear_error(&error); return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
}

static gboolean copy_metadata(GstBaseTransform *base, GstBuffer *input, GstBuffer *output)
{ return gst_buffer_copy_into(output, input, GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_FLAGS, 0, -1); }

static gboolean sink_event(GstBaseTransform *base, GstEvent *event)
{
    GstVim4Pq2Sdr *self = (GstVim4Pq2Sdr *)base;
    if (self->pool && GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) gst_buffer_pool_set_flushing(self->pool, TRUE);
    if (self->pool && GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) gst_buffer_pool_set_flushing(self->pool, FALSE);
    return GST_BASE_TRANSFORM_CLASS(gst_vim4_pq2sdr_parent_class)->sink_event(base, event);
}
static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4Pq2Sdr *self = (GstVim4Pq2Sdr *)object;
    if (id == PROP_WHITE) { self->white = g_value_get_uint(value); self->explicit_white = TRUE; }
    else if (id == PROP_PEAK) { self->peak = g_value_get_uint(value); self->explicit_peak = TRUE; }
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    GstVim4Pq2Sdr *self = (GstVim4Pq2Sdr *)object;
    if (id == PROP_WHITE) g_value_set_uint(value, self->white);
    else if (id == PROP_PEAK) g_value_set_uint(value, self->peak);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void gst_vim4_pq2sdr_class_init(GstVim4Pq2SdrClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->set_property = set_property; object->get_property = get_property;
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=(string){NV12,P010_10LE},width=[2,4096],height=[2,2160]");
    gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    caps = gst_caps_from_string("video/x-raw,format=NV12,width=[2,4096],height=[2,2160],colorimetry=bt709");
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    gst_element_class_set_static_metadata(element, "VIM4 PQ to SDR DMA", "Filter/Video",
        "Vulkan P010 PQ/2020 to NV12 SDR; NV12 passthrough", "vim4-kvm");
    const guint flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY;
    g_object_class_install_property(object, PROP_WHITE, g_param_spec_uint("sdr-white", "SDR white", "SDR white nits", 160, 1000, 203, flags));
    g_object_class_install_property(object, PROP_PEAK, g_param_spec_uint("hdr-peak", "HDR peak", "HDR highlight peak nits", 161, 10000, 1000, flags));
    GstBaseTransformClass *transform_class = GST_BASE_TRANSFORM_CLASS(klass);
    transform_class->transform_caps = transform_caps; transform_class->set_caps = set_caps;
    transform_class->prepare_output_buffer = prepare; transform_class->transform = transform;
    transform_class->copy_metadata = copy_metadata; transform_class->stop = stop;
    transform_class->sink_event = sink_event;
}
static void gst_vim4_pq2sdr_init(GstVim4Pq2Sdr *self)
{ self->white = 203; self->peak = 1000; }
