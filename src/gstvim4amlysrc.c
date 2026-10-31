/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/base/gstpushsrc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <linux/amlogic-vdin.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "amly-gpu.h"
#include "codecmm-pool.h"

/* A single caller owns the capture queue and Vulkan context. A frame is
 * requeued only after the GPU fence completes; downstream owns a distinct
 * bounded P010 allocation. No pixel mapping or CPU fallback is provided.
 */
typedef struct {
    GstPushSrc parent;
    gchar *device;
    int fd, wake, dma[4];
    gsize capacity[4];
    gboolean streaming;
    AmlyGpu *gpu;
    GstBufferPool *pool;
    GstVideoInfo output;
    guint width, height, stride;
    guint output_width, output_height;
} GstVim4AmlySrc;
typedef GstPushSrcClass GstVim4AmlySrcClass;
G_DEFINE_TYPE(GstVim4AmlySrc, gst_vim4_amly_src, GST_TYPE_PUSH_SRC)
enum { PROP_0, PROP_DEVICE, PROP_WIDTH, PROP_HEIGHT };
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(
    "video/x-raw,format=P010_10LE,width=[2,3840],height=[2,2160],"
    "framerate=[1/1,120/1],colorimetry=bt2100-pq"));

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do { ret = ioctl(fd, request, arg); } while (ret < 0 && errno == EINTR);
    return ret;
}

static GstCaps *get_caps(GstBaseSrc *base, GstCaps *filter)
{
    GstVim4AmlySrc *self = (GstVim4AmlySrc *)base;
    GstCaps *caps = self->output.width ? gst_video_info_to_caps(&self->output) :
        gst_static_pad_template_get_caps(&src_template);
    if (filter) {
        GstCaps *filtered = gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(caps);
        caps = filtered;
    }
    return caps;
}

static gboolean stop(GstBaseSrc *base)
{
    GstVim4AmlySrc *self = (GstVim4AmlySrc *)base;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (self->streaming) xioctl(self->fd, VIDIOC_STREAMOFF, &type);
    self->streaming = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(self->dma); i++) {
        if (self->dma[i] >= 0) close(self->dma[i]);
        self->dma[i] = -1;
    }
    if (self->fd >= 0) close(self->fd);
    self->fd = -1;
    if (self->pool) {
        gst_buffer_pool_set_active(self->pool, FALSE);
        gst_clear_object(&self->pool);
    }
    amly_gpu_free(self->gpu);
    self->gpu = NULL;
    gst_video_info_init(&self->output);
    return TRUE;
}

static gboolean start(GstBaseSrc *base)
{
    GstVim4AmlySrc *self = (GstVim4AmlySrc *)base;
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    struct v4l2_requestbuffers req = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP, .count = 4};
    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    GError *error = NULL;
    if (self->wake < 0) goto failed;
    self->fd = open(self->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (self->fd < 0) goto failed;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_AMLY;
    if (xioctl(self->fd, VIDIOC_S_FMT, &fmt) < 0 ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_AMLY ||
        fmt.fmt.pix.colorspace != V4L2_COLORSPACE_BT2020 ||
        fmt.fmt.pix.xfer_func != V4L2_XFER_FUNC_SMPTE2084 ||
        fmt.fmt.pix.ycbcr_enc != V4L2_YCBCR_ENC_BT2020 ||
        fmt.fmt.pix.quantization != V4L2_QUANTIZATION_LIM_RANGE)
        goto failed;
    self->width = fmt.fmt.pix.width;
    self->height = fmt.fmt.pix.height;
    self->stride = fmt.fmt.pix.bytesperline;
    if (!self->width || !self->height || (self->width & 1) || (self->height & 1) ||
        self->width > 3840 || self->height > 2160 ||
        self->stride < self->width * 5 / 2 || self->stride % 32 ||
        xioctl(self->fd, VIDIOC_G_PARM, &parm) < 0 ||
        !parm.parm.capture.timeperframe.numerator ||
        !parm.parm.capture.timeperframe.denominator)
        goto failed;
    guint width = self->output_width ? self->output_width : self->width;
    guint height = self->output_height ? self->output_height : self->height;
    if (width > self->width || height > self->height || (width & 1) || (height & 1))
        goto failed;
    gst_video_info_set_format(&self->output, GST_VIDEO_FORMAT_P010_10LE, width, height);
    /* Wave5 raw storage is 32 pixels by 16 rows. Keep visible geometry
     * separate; the shader writes UV after the padded luma allocation.
     */
    self->output.stride[0] = self->output.stride[1] = GST_ROUND_UP_64(width * 2);
    self->output.offset[1] = self->output.stride[0] * GST_ROUND_UP_16(height);
    self->output.size = self->output.offset[1] * 3 / 2;
    self->output.fps_n = parm.parm.capture.timeperframe.denominator;
    self->output.fps_d = parm.parm.capture.timeperframe.numerator;
    gst_video_colorimetry_from_string(&self->output.colorimetry, "bt2100-pq");
    self->gpu = amly_gpu_new(&error);
    if (!self->gpu) goto failed;
    self->pool = codecmm_pool_new(self->output.size, 3, &error);
    if (!self->pool || xioctl(self->fd, VIDIOC_REQBUFS, &req) < 0 || req.count != 4)
        goto failed;
    for (guint i = 0; i < 4; i++) {
        struct v4l2_exportbuffer exp = {.type = req.type, .index = i,
            .flags = O_CLOEXEC | O_RDWR};
        struct v4l2_buffer buf = {.type = req.type, .memory = req.memory, .index = i};
        struct stat st;
        if (xioctl(self->fd, VIDIOC_EXPBUF, &exp) < 0) goto failed;
        self->dma[i] = exp.fd;
        if (fstat(exp.fd, &st) || st.st_size < fmt.fmt.pix.sizeimage) goto failed;
        self->capacity[i] = st.st_size;
        if (xioctl(self->fd, VIDIOC_QBUF, &buf) < 0) goto failed;
    }
    if (xioctl(self->fd, VIDIOC_STREAMON, &req.type) < 0) goto failed;
    self->streaming = TRUE;
    GstCaps *caps = gst_video_info_to_caps(&self->output);
    gboolean ok = gst_base_src_set_caps(base, caps);
    gst_caps_unref(caps);
    if (ok) return TRUE;
failed:
    GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ, ("Native AMLY DMA capture unavailable"),
        ("%s", error ? error->message : g_strerror(errno)));
    g_clear_error(&error);
    stop(base);
    return FALSE;
}

static GstFlowReturn create(GstPushSrc *src, GstBuffer **output)
{
    GstVim4AmlySrc *self = (GstVim4AmlySrc *)src;
    GstFlowReturn flow = gst_buffer_pool_acquire_buffer(self->pool, output, NULL);
    if (flow != GST_FLOW_OK) return flow;
    struct pollfd pollfd[2] = {{self->fd, POLLIN, 0}, {self->wake, POLLIN, 0}};
    int ret;
    do { ret = poll(pollfd, 2, 1000); } while (ret < 0 && errno == EINTR);
    if (pollfd[1].revents) { flow = GST_FLOW_FLUSHING; goto failed; }
    if (ret <= 0 || !(pollfd[0].revents & POLLIN)) goto error;
    struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP};
    if (xioctl(self->fd, VIDIOC_DQBUF, &buf) < 0 || buf.index >= 4) goto error;
    GError *gpu_error = NULL;
    GstMemory *memory = gst_buffer_peek_memory(*output, 0);
    gboolean ok = !(buf.flags & V4L2_BUF_FLAG_ERROR) &&
        buf.bytesused >= self->stride * self->height &&
        amly_gpu_convert_padded(self->gpu, self->dma[buf.index], self->capacity[buf.index],
            gst_dmabuf_memory_get_fd(memory), memory->maxsize, self->width,
            self->height, self->output.width, self->output.height,
            self->stride, self->output.stride[0], GST_ROUND_UP_16(self->output.height),
            &gpu_error);
    /* A submitted failure is drained by amly_gpu_convert before returning. */
    if (xioctl(self->fd, VIDIOC_QBUF, &buf) < 0 || !ok) {
        GST_ELEMENT_ERROR(self, STREAM, FAILED, ("AMLY GPU conversion failed"),
            ("%s", gpu_error ? gpu_error->message : g_strerror(errno)));
        g_clear_error(&gpu_error);
        goto error;
    }
    GstVideoMeta *meta = gst_buffer_add_video_meta_full(*output, GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_FORMAT_P010_10LE, self->output.width, self->output.height, 2,
        self->output.offset, self->output.stride);
    GstVideoAlignment alignment = {0};
    alignment.padding_right = GST_ROUND_UP_32(self->output.width) - self->output.width;
    alignment.padding_bottom = GST_ROUND_UP_16(self->output.height) - self->output.height;
    if (!meta || !gst_video_meta_set_alignment(meta, alignment)) goto error;
    GstClock *clock = gst_element_get_clock(GST_ELEMENT(self));
    if (clock) {
        GST_BUFFER_PTS(*output) = gst_clock_get_time(clock) -
            gst_element_get_base_time(GST_ELEMENT(self));
        gst_object_unref(clock);
    }
    GST_BUFFER_OFFSET(*output) = buf.sequence;
    GST_BUFFER_DURATION(*output) = gst_util_uint64_scale(GST_SECOND,
        self->output.fps_d, self->output.fps_n);
    return GST_FLOW_OK;
error:
    flow = GST_FLOW_ERROR;
failed:
    gst_clear_buffer(output);
    return flow;
}

static gboolean unlock(GstBaseSrc *base)
{
    GstVim4AmlySrc *self = (GstVim4AmlySrc *)base;
    eventfd_write(self->wake, 1);
    if (self->pool) gst_buffer_pool_set_flushing(self->pool, TRUE);
    return TRUE;
}
static gboolean unlock_stop(GstBaseSrc *base)
{
    GstVim4AmlySrc *self = (GstVim4AmlySrc *)base;
    eventfd_t value;
    while (eventfd_read(self->wake, &value) == 0) {}
    if (self->pool) gst_buffer_pool_set_flushing(self->pool, FALSE);
    return TRUE;
}
static void set_property(GObject *obj, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4AmlySrc *self = (GstVim4AmlySrc *)obj;
    if (id == PROP_DEVICE) { g_free(self->device); self->device = g_value_dup_string(value); }
    else if (id == PROP_WIDTH) self->output_width = g_value_get_uint(value);
    else if (id == PROP_HEIGHT) self->output_height = g_value_get_uint(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, spec);
}
static void get_property(GObject *obj, guint id, GValue *value, GParamSpec *spec)
{
    if (id == PROP_DEVICE) g_value_set_string(value, ((GstVim4AmlySrc *)obj)->device);
    else if (id == PROP_WIDTH) g_value_set_uint(value, ((GstVim4AmlySrc *)obj)->output_width);
    else if (id == PROP_HEIGHT) g_value_set_uint(value, ((GstVim4AmlySrc *)obj)->output_height);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, spec);
}
static void finalize(GObject *obj)
{
    GstVim4AmlySrc *self = (GstVim4AmlySrc *)obj;
    stop(GST_BASE_SRC(obj));
    if (self->wake >= 0) close(self->wake);
    g_free(self->device);
    G_OBJECT_CLASS(gst_vim4_amly_src_parent_class)->finalize(obj);
}
static void gst_vim4_amly_src_class_init(GstVim4AmlySrcClass *klass)
{
    GObjectClass *obj = G_OBJECT_CLASS(klass);
    GstBaseSrcClass *base = GST_BASE_SRC_CLASS(klass);
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    obj->set_property = set_property; obj->get_property = get_property; obj->finalize = finalize;
    g_object_class_install_property(obj, PROP_DEVICE, g_param_spec_string("device", "Device",
        "VDIN capture node", "/dev/video0", G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY));
    g_object_class_install_property(obj, PROP_WIDTH, g_param_spec_uint("width", "Width",
        "GPU output width; zero preserves native width", 0, 3840, 0,
        G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY));
    g_object_class_install_property(obj, PROP_HEIGHT, g_param_spec_uint("height", "Height",
        "GPU output height; zero preserves native height", 0, 2160, 0,
        G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY));
    gst_element_class_add_static_pad_template(element, &src_template);
    gst_element_class_set_static_metadata(element, "Native AMLY HDR source", "Source/Video",
        "Direct VDIN DMA and Vulkan P010 conversion", "vim4-kvm");
    base->start = start; base->stop = stop; base->unlock = unlock; base->unlock_stop = unlock_stop;
    base->get_caps = get_caps;
    GST_PUSH_SRC_CLASS(klass)->create = create;
}
static void gst_vim4_amly_src_init(GstVim4AmlySrc *self)
{
    self->fd = -1; self->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    for (guint i = 0; i < 4; i++) self->dma[i] = -1;
    self->device = g_strdup("/dev/video0");
    gst_video_info_init(&self->output);
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
}
