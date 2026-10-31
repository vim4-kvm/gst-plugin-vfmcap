/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the real source's startup rollback without video/GPU hardware. */
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
int fake_open(const char *path, int flags, ...);
int fake_close(int fd);
int fake_ioctl(int fd, unsigned long request, ...);
int fake_fstat(int fd, struct stat *st);
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define fstat fake_fstat
#include "../src/gstvim4amlysrc.c"
#undef open
#undef close
#undef ioctl
#undef fstat
#include <stdarg.h>

static guint calls, fail_at, descriptors, contexts;
static gsize expected_output_bytes = 640 * 480 * 3;
static gboolean opened[200];
int fake_open(const char *path, int flags, ...)
{
    (void)path; (void)flags;
    opened[100] = TRUE; descriptors++; return 100;
}
int fake_close(int fd)
{
    if (fd < 100) return close(fd); /* The source owns a real eventfd for flush. */
    g_assert_cmpint(fd, >=, 100);
    g_assert_true(opened[fd]); opened[fd] = FALSE; descriptors--; return 0;
}
int fake_fstat(int fd, struct stat *st)
{
    g_assert_true(opened[fd]); st->st_size = 1024 * 1024; return 0;
}
int fake_ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request); void *arg = va_arg(args, void *); va_end(args);
    g_assert_true(opened[fd]);
    if (++calls == fail_at) { errno = EIO; return -1; }
    switch (request) {
    case VIDIOC_S_FMT: {
        struct v4l2_pix_format *fmt = &((struct v4l2_format *)arg)->fmt.pix;
        g_assert_cmpuint(fmt->pixelformat, ==, V4L2_PIX_FMT_AMLY);
        fmt->width = 640; fmt->height = 480; fmt->bytesperline = 1600;
        fmt->sizeimage = 1600 * 480;
        fmt->colorspace = V4L2_COLORSPACE_BT2020;
        fmt->xfer_func = V4L2_XFER_FUNC_SMPTE2084;
        fmt->ycbcr_enc = V4L2_YCBCR_ENC_BT2020;
        fmt->quantization = V4L2_QUANTIZATION_LIM_RANGE;
        return 0;
    }
    case VIDIOC_G_PARM: {
        struct v4l2_streamparm *p = arg;
        p->parm.capture.timeperframe = (struct v4l2_fract){1, 60}; return 0;
    }
    case VIDIOC_EXPBUF: {
        struct v4l2_exportbuffer *e = arg;
        e->fd = 101 + e->index;
        if (opened[e->fd]) g_error("export fd %d already open at failure %u call %u", e->fd, fail_at, calls);
        opened[e->fd] = TRUE; descriptors++; return 0;
    }
    case VIDIOC_REQBUFS: case VIDIOC_QBUF: case VIDIOC_STREAMON: case VIDIOC_STREAMOFF:
        return 0;
    default: g_assert_not_reached();
    }
}
AmlyGpu *amly_gpu_new(GError **error) { (void)error; contexts++; return (AmlyGpu *)1; }
void amly_gpu_free(AmlyGpu *gpu) { if (gpu) contexts--; }
gboolean amly_gpu_convert_padded(AmlyGpu *g, int in, gsize insz, int out, gsize outsz,
    guint iw, guint ih, guint w, guint h, guint stride, guint pitch, guint rows, GError **error)
{
    (void)g; (void)in; (void)insz; (void)out; (void)outsz;
    (void)iw; (void)ih; (void)w; (void)h; (void)stride; (void)pitch; (void)rows; (void)error;
    g_assert_not_reached();
}
GstBufferPool *codecmm_pool_new(gsize size, guint buffers, GError **error)
{
    (void)error; g_assert_cmpuint(size, ==, expected_output_bytes);
    g_assert_cmpuint(buffers, ==, 3); return gst_buffer_pool_new();
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    for (fail_at = 0; fail_at <= 12; fail_at++) {
        calls = 0;
        GstVim4AmlySrc *source = g_object_new(gst_vim4_amly_src_get_type(), NULL);
        gst_object_ref_sink(source);
        gboolean ok = gst_element_set_state(GST_ELEMENT(source), GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE;
        g_assert_cmpint(ok, ==, fail_at == 0);
        if (ok) {
            g_assert_cmpuint(source->output.stride[0], ==, 1280);
            g_assert_cmpuint(source->output.offset[1], ==, 1280 * 480);
            g_assert_true(unlock(GST_BASE_SRC(source)));
            g_assert_true(unlock_stop(GST_BASE_SRC(source)));
        }
        gst_element_set_state(GST_ELEMENT(source), GST_STATE_NULL);
        gst_object_unref(source);
        if (descriptors) g_error("failure %u leaves %u descriptors after %u ioctls", fail_at, descriptors, calls);
        g_assert_cmpuint(contexts, ==, 0);
    }
    const guint geometry[][3] = {{320,240,1}, {624,450,1}, {642,480,0}, {639,480,0}, {640,479,0}};
    fail_at = 0;
    for (guint i = 0; i < G_N_ELEMENTS(geometry); i++) {
        calls = 0;
        expected_output_bytes = GST_ROUND_UP_64(geometry[i][0] * 2) *
            GST_ROUND_UP_16(geometry[i][1]) * 3 / 2;
        GstVim4AmlySrc *source = g_object_new(gst_vim4_amly_src_get_type(),
            "width", geometry[i][0], "height", geometry[i][1], NULL);
        gst_object_ref_sink(source);
        g_assert_cmpint(gst_element_set_state(GST_ELEMENT(source), GST_STATE_PAUSED) !=
            GST_STATE_CHANGE_FAILURE, ==, geometry[i][2]);
        gst_element_set_state(GST_ELEMENT(source), GST_STATE_NULL);
        gst_object_unref(source);
        g_assert_cmpuint(descriptors, ==, 0);
        g_assert_cmpuint(contexts, ==, 0);
    }
    return 0;
}
