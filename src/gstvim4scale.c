/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/base/gstbasetransform.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <aml_ge2d.h>
#include "codecmm-pool.h"

typedef struct {
    GstBaseTransform parent;
    GstVideoInfo input, output;
    GstBufferPool *pool;
    aml_ge2d_t engine;
    gboolean opened;
    guint height, width_alignment;
} GstVim4Scale;
typedef GstBaseTransformClass GstVim4ScaleClass;
G_DEFINE_TYPE(GstVim4Scale, gst_vim4_scale, GST_TYPE_BASE_TRANSFORM)
enum { PROP_0, PROP_HEIGHT, PROP_WIDTH_ALIGNMENT };

static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4Scale *self = (GstVim4Scale *)object;
    if (id == PROP_HEIGHT) self->height = g_value_get_uint(value);
    else if (id == PROP_WIDTH_ALIGNMENT) self->width_alignment = g_value_get_uint(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    GstVim4Scale *self = (GstVim4Scale *)object;
    if (id == PROP_HEIGHT) g_value_set_uint(value, self->height);
    else if (id == PROP_WIDTH_ALIGNMENT) g_value_set_uint(value, self->width_alignment);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(
    "video/x-raw,format=(string){NV12,NV21},width=(int)[2,4096],height=(int)[2,2160]"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(
    "video/x-raw,format=(string){NV12,NV21},width=(int)[2,4096],height=(int)[2,2160]"));

static GstCaps *transform_caps(GstBaseTransform *base, GstPadDirection direction,
    GstCaps *caps, GstCaps *filter)
{
    GstCaps *result = gst_caps_copy(caps);
    for (guint i = 0; i < gst_caps_get_size(result); i++) {
        GstStructure *s = gst_caps_get_structure(result, i);
        gst_structure_remove_fields(s, "width", "height", "format", NULL);
    }
    GstCaps *allowed = gst_static_pad_template_get_caps(
        direction == GST_PAD_SINK ? &src_template : &sink_template);
    GstCaps *bounded = gst_caps_intersect_full(result, allowed, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(result);
    gst_caps_unref(allowed);
    if (filter) {
        result = gst_caps_intersect_full(filter, bounded, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(bounded);
        return result;
    }
    return bounded;
}

static void clear_pool(GstVim4Scale *self)
{
    if (self->pool) {
        gst_buffer_pool_set_active(self->pool, FALSE);
        gst_clear_object(&self->pool);
    }
}

static GstCaps *fixate_caps(GstBaseTransform *base, GstPadDirection direction,
    GstCaps *caps, GstCaps *other)
{
    GstVim4Scale *self = (GstVim4Scale *)base;
    other = gst_caps_make_writable(other);
    gint width, height;
    const GstStructure *input = gst_caps_get_structure(caps, 0);
    GstStructure *output = gst_caps_get_structure(other, 0);
    if (direction == GST_PAD_SINK && gst_structure_get_int(input, "width", &width) &&
        gst_structure_get_int(input, "height", &height)) {
        gboolean scaling = self->height && self->height < (guint)height;
        if (scaling) {
            width = ((guint64)width * self->height / height) / self->width_alignment * self->width_alignment;
            height = self->height;
        }
        gst_structure_fixate_field_nearest_int(output, "width", MAX(width, 2));
        gst_structure_fixate_field_nearest_int(output, "height", height);
        gst_structure_fixate_field_string(output, "format", scaling ? "NV12" :
            gst_structure_get_string(input, "format"));
    }
    return gst_caps_fixate(other);
}

static gboolean set_caps(GstBaseTransform *base, GstCaps *in, GstCaps *out)
{
    GstVim4Scale *self = (GstVim4Scale *)base;
    GstVideoInfo input, output;
    if (!gst_video_info_from_caps(&input, in) || !gst_video_info_from_caps(&output, out) ||
        ((input.width | input.height | output.width | output.height) & 1) ||
        output.width > input.width || output.height > input.height)
        return FALSE;
    gboolean native = input.width == output.width && input.height == output.height &&
        GST_VIDEO_INFO_FORMAT(&input) == GST_VIDEO_INFO_FORMAT(&output);
    clear_pool(self);
    gst_base_transform_set_passthrough(base, native);
    if (native) {
        self->input = input;
        self->output = output;
        return TRUE;
    }
    if (GST_VIDEO_INFO_FORMAT(&output) != GST_VIDEO_FORMAT_NV12) return FALSE;
    /* Canvas width describes storage; the blit rectangle describes visible
     * pixels. Encoding consumers receive the actual pitch in VideoMeta. */
    guint stride = (output.width + 63) & ~63u;
    output.stride[0] = output.stride[1] = stride;
    output.offset[0] = 0;
    output.offset[1] = (gsize)stride * output.height;
    output.size = output.offset[1] * 3 / 2;
    GError *error = NULL;
    self->pool = codecmm_pool_new(output.size, 3, &error);
    if (!self->pool) {
        GST_ELEMENT_ERROR(self, RESOURCE, NO_SPACE_LEFT, ("Cannot allocate SDR scaling DMA pool"),
            ("%s", error ? error->message : "unknown error"));
        g_clear_error(&error);
        return FALSE;
    }
    self->input = input;
    self->output = output;
    return TRUE;
}

static gboolean scale_start(GstBaseTransform *base)
{
    GstVim4Scale *self = (GstVim4Scale *)base;
    self->opened = aml_ge2d_init(&self->engine) >= 0;
    if (!self->opened)
        GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ_WRITE, ("Cannot open GE2D SDR scaler"), (NULL));
    return self->opened;
}

static gboolean stop(GstBaseTransform *base)
{
    GstVim4Scale *self = (GstVim4Scale *)base;
    clear_pool(self);
    if (self->opened) aml_ge2d_exit(&self->engine);
    self->opened = FALSE;
    return TRUE;
}

static GstFlowReturn prepare_output(GstBaseTransform *base, GstBuffer *input, GstBuffer **output)
{
    GstVim4Scale *self = (GstVim4Scale *)base;
    if (gst_base_transform_is_passthrough(base)) {
        *output = gst_buffer_ref(input);
        return GST_FLOW_OK;
    }
    GstFlowReturn flow = gst_buffer_pool_acquire_buffer(self->pool, output, NULL);
    if (flow != GST_FLOW_OK) return flow;
    if (!GST_BASE_TRANSFORM_CLASS(gst_vim4_scale_parent_class)->copy_metadata(base, input, *output)) {
        gst_clear_buffer(output);
        return GST_FLOW_ERROR;
    }
    if (!gst_buffer_get_video_meta(*output))
        gst_buffer_add_video_meta_full(*output, GST_VIDEO_FRAME_FLAG_NONE,
            GST_VIDEO_FORMAT_NV12, self->output.width, self->output.height, 2,
            self->output.offset, self->output.stride);
    return GST_FLOW_OK;
}

static gboolean dma_layout(GstBuffer *buffer, const GstVideoInfo *info, buffer_info_t *layout)
{
    if (gst_buffer_n_memory(buffer) != 1) return FALSE;
    GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
    if (!gst_is_dmabuf_memory(memory)) return FALSE;
    gsize offset;
    gsize bytes = gst_memory_get_sizes(memory, &offset, NULL);
    GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
    if (meta && (meta->n_planes != 2 || meta->width != (guint)info->width ||
        meta->height != (guint)info->height || meta->format != GST_VIDEO_INFO_FORMAT(info)))
        return FALSE;
    gint stride = meta ? meta->stride[0] : info->stride[0];
    gint uv_stride = meta ? meta->stride[1] : info->stride[1];
    gsize y = meta ? meta->offset[0] : info->offset[0];
    gsize uv = meta ? meta->offset[1] : info->offset[1];
    if (offset || y || stride < info->width || (stride & 63) || stride != uv_stride ||
        uv != (gsize)stride * info->height || bytes < uv + uv / 2)
        return FALSE;
    *layout = (buffer_info_t){
        .mem_alloc_type = AML_GE2D_MEM_DMABUF, .memtype = GE2D_CANVAS_ALLOC,
        .canvas_w = stride, .canvas_h = info->height,
        .rect = {0, 0, info->width, info->height},
        .format = GST_VIDEO_INFO_FORMAT(info) == GST_VIDEO_FORMAT_NV21 ?
            PIXEL_FORMAT_YCrCb_420_SP : PIXEL_FORMAT_YCbCr_420_SP_NV12,
        .shared_fd = {gst_dmabuf_memory_get_fd(memory), -1, -1, -1},
        .plane_number = 1, .plane_alpha = 255, .layer_mode = LAYER_MODE_NON,
    };
    return TRUE;
}

static GstFlowReturn transform(GstBaseTransform *base, GstBuffer *input, GstBuffer *output)
{
    GstVim4Scale *self = (GstVim4Scale *)base;
    aml_ge2d_info_t *command = &self->engine.ge2dinfo;
    if (!dma_layout(input, &self->input, &command->src_info[0]) ||
        !dma_layout(output, &self->output, &command->dst_info)) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("SDR scaling requires contiguous NV12/NV21 DMA with valid pitch"), (NULL));
        return GST_FLOW_ERROR;
    }
    command->ge2d_op = AML_GE2D_STRETCHBLIT;
    command->blend_mode = BLEND_MODE_NONE;
    if (aml_ge2d_process(command) < 0) {
        GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("GE2D SDR scaling failed"), (NULL));
        return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
}

static gboolean propose_allocation(GstBaseTransform *base, GstQuery *decide, GstQuery *query)
{
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    return TRUE;
}

static gboolean transform_meta(GstBaseTransform *base, GstBuffer *out, GstMeta *meta, GstBuffer *in)
{
    /* Input geometry must never replace the scaled buffer's own layout. */
    if (meta->info->api == GST_VIDEO_META_API_TYPE) return FALSE;
    return GST_BASE_TRANSFORM_CLASS(gst_vim4_scale_parent_class)->transform_meta(base, out, meta, in);
}

static gboolean sink_event(GstBaseTransform *base, GstEvent *event)
{
    GstVim4Scale *self = (GstVim4Scale *)base;
    if (self->pool && (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START ||
        GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP))
        gst_buffer_pool_set_flushing(self->pool, GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START);
    return GST_BASE_TRANSFORM_CLASS(gst_vim4_scale_parent_class)->sink_event(base, event);
}

static GstStateChangeReturn change_state(GstElement *element, GstStateChange transition)
{
    GstVim4Scale *self = (GstVim4Scale *)element;
    /* A downstream encoder may retain all slots. Wake the streaming thread
     * before BaseTransform waits for it during shutdown. */
    if (transition == GST_STATE_CHANGE_PAUSED_TO_READY && self->pool)
        gst_buffer_pool_set_flushing(self->pool, TRUE);
    return GST_ELEMENT_CLASS(gst_vim4_scale_parent_class)->change_state(element, transition);
}

static void gst_vim4_scale_class_init(GstVim4ScaleClass *klass)
{
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *base = GST_BASE_TRANSFORM_CLASS(klass);
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    g_object_class_install_property(object, PROP_HEIGHT, g_param_spec_uint("height", "Output height",
        "Maximum output height; zero preserves native dimensions", 0, 2160, 0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));
    g_object_class_install_property(object, PROP_WIDTH_ALIGNMENT, g_param_spec_uint("width-alignment", "Width alignment",
        "Visible width alignment when downscaling (2 for H.264, 16 for vendor HEVC)", 2, 16, 2,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));
    gst_element_class_set_static_metadata(element, "VIM4 SDR DMA scaler", "Filter/Video/Scaler",
        "GE2D downscaling with independent visible width and DMA row pitch", "VIM4 KVM");
    gst_element_class_add_static_pad_template(element, &sink_template);
    gst_element_class_add_static_pad_template(element, &src_template);
    base->transform_caps = transform_caps;
    base->fixate_caps = fixate_caps;
    base->set_caps = set_caps;
    base->start = scale_start;
    base->stop = stop;
    base->prepare_output_buffer = prepare_output;
    base->transform = transform;
    base->propose_allocation = propose_allocation;
    base->transform_meta = transform_meta;
    base->sink_event = sink_event;
    element->change_state = change_state;
}

static void gst_vim4_scale_init(GstVim4Scale *self)
{
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), FALSE);
    self->width_alignment = 2;
}
