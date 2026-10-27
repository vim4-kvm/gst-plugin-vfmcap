/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * vfmcapsrc: zero-copy source for the vfm_cap V4L2 device.
 *
 * vfm_cap hands out frames as DMA-buf fds pointing straight at the
 * vdin capture buffers. Each output GstBuffer wraps one such fd; the
 * V4L2 buffer is a flow-control token only and goes back to the queue
 * right after the fd is taken. The fd keeps the frame alive until the
 * GstBuffer is freed.
 *
 *   gst-launch-1.0 vfmcapsrc ! video/x-raw,format=NV21 ! amlvenc ! ...
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>

#include "vfmcap-uapi.h"
#include "vuy24.h"
#include "amly.h"
#include "vdin1.h"
#include "codecmm-pool.h"
#ifdef HAVE_AMLY_GPU
#include "amly-gpu.h"
#endif

/* vfm_cap extension: VDIN's packed Cr-Cb-Y output with no_swap_en=1. */
#define V4L2_PIX_FMT_AML_VUY24 v4l2_fourcc('V', 'U', 'Y', '3')
#define V4L2_PIX_FMT_AMLY v4l2_fourcc('A', 'M', 'L', 'Y')

GST_DEBUG_CATEGORY_STATIC(vfmcapsrc_debug);
#define GST_CAT_DEFAULT vfmcapsrc_debug

#define DEFAULT_DEVICE "/dev/video_cap"
#define NUM_TOKENS 4
#define POLL_TIMEOUT_MS 200

#define GST_TYPE_VFMCAP_SRC (gst_vfmcap_src_get_type())
G_DECLARE_FINAL_TYPE(GstVfmCapSrc, gst_vfmcap_src, GST, VFMCAP_SRC, GstPushSrc)

struct _GstVfmCapSrc {
	GstPushSrc parent;

	gchar *device;
	gint fd;
	gboolean streaming;
	gboolean flushing;
	gboolean timing;
	gboolean repack_vuy24;
	gboolean repack_amly;
	gboolean gpu_conversion;
#ifdef HAVE_AMLY_GPU
	AmlyGpu *gpu;
#endif
	guint input_size;
	guint input_stride;
	guint input_width, input_height;
	guint output_height;

	GstAllocator *allocator;
	GstBufferPool *repack_pool;
	GThreadPool *repack_worker;
	GstVideoInfo info;
	gboolean info_valid;
	guint64 frames;
};

typedef struct {
	uint16_t *y, *uv;
	const uint8_t *src;
	gsize width, height, input_stride, output_stride;
	GMutex mutex;
	GCond complete;
	gboolean done;
} AmlyJob;

static void amly_worker(gpointer data, gpointer unused)
{
	AmlyJob *job = data;
	amly_to_p010(job->y, job->uv, job->src, job->width, job->height,
		job->input_stride, job->output_stride);
	g_mutex_lock(&job->mutex);
	job->done = TRUE;
	g_cond_signal(&job->complete);
	g_mutex_unlock(&job->mutex);
}

static void repack_amly(GstVfmCapSrc *self, uint8_t *output, const uint8_t *input)
{
	gsize width = GST_VIDEO_INFO_WIDTH(&self->info);
	gsize height = GST_VIDEO_INFO_HEIGHT(&self->info);
	gsize stride = self->info.stride[0] / 2;
	/* Split on a pair of rows so chroma averaging never crosses workers.
	 * Both halves finish before releasing the immutable DMA mapping. */
	gsize split = self->repack_worker && height >= 1080 ? (height / 2) & ~(gsize)1 : 0;
	AmlyJob job = { .y = (uint16_t *)output,
		.uv = (uint16_t *)(output + self->info.offset[1]), .src = input,
		.width = width, .height = split, .input_stride = self->input_stride,
		.output_stride = stride };
	if (split) {
		g_mutex_init(&job.mutex);
		g_cond_init(&job.complete);
		if (!g_thread_pool_push(self->repack_worker, &job, NULL))
			split = 0;
	}
	amly_to_p010(job.y + split * stride, job.uv + split / 2 * stride,
		input + split * self->input_stride, width, height - split,
		self->input_stride, stride);
	if (job.height) {
		if (split) {
			g_mutex_lock(&job.mutex);
			while (!job.done)
				g_cond_wait(&job.complete, &job.mutex);
			g_mutex_unlock(&job.mutex);
		}
		g_cond_clear(&job.complete);
		g_mutex_clear(&job.mutex);
	}
}

enum {
	PROP_0,
	PROP_DEVICE,
	PROP_GPU_CONVERSION,
	PROP_HEIGHT,
};

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
	GST_PAD_SRC, GST_PAD_ALWAYS,
	GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE("{ NV21, NV12, UYVY, v308, P010_10LE }")));

G_DEFINE_TYPE(GstVfmCapSrc, gst_vfmcap_src, GST_TYPE_PUSH_SRC);

static void clear_repack(GstVfmCapSrc *self)
{
	GST_OBJECT_LOCK(self);
	GstBufferPool *pool = g_steal_pointer(&self->repack_pool);
	GST_OBJECT_UNLOCK(self);
	if (pool) {
		gst_buffer_pool_set_flushing(pool, TRUE);
		gst_buffer_pool_set_active(pool, FALSE);
		gst_object_unref(pool);
	}
#ifdef HAVE_AMLY_GPU
	amly_gpu_free(self->gpu);
	self->gpu = NULL;
#endif
}

static GstVideoFormat
fourcc_to_format(guint32 fourcc)
{
	switch (fourcc) {
	case V4L2_PIX_FMT_NV21:
		return GST_VIDEO_FORMAT_NV21;
	case V4L2_PIX_FMT_NV12:
		return GST_VIDEO_FORMAT_NV12;
	case V4L2_PIX_FMT_UYVY:
		return GST_VIDEO_FORMAT_UYVY;
	case V4L2_PIX_FMT_AML_VUY24:
		return GST_VIDEO_FORMAT_v308;
	case V4L2_PIX_FMT_AMLY:
		return GST_VIDEO_FORMAT_P010_10LE;
	default:
		return GST_VIDEO_FORMAT_UNKNOWN;
	}
}

/* Read the format vfm_cap currently publishes into self->info. */
static gboolean
query_format(GstVfmCapSrc *self)
{
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	struct v4l2_pix_format_mplane *mp = &f.fmt.pix_mp;
	GstVideoFormat vfmt;
	GstVideoInfo old = self->info;
	guint old_size = self->input_size, old_stride = self->input_stride;
	guint old_width = self->input_width, old_height = self->input_height;
	gboolean old_amly = self->repack_amly, old_vuy24 = self->repack_vuy24;

	if (ioctl(self->fd, VIDIOC_G_FMT, &f) < 0) {
		GST_WARNING_OBJECT(self, "G_FMT: %s", g_strerror(errno));
		return FALSE;
	}
	vfmt = fourcc_to_format(mp->pixelformat);
	if (vfmt == GST_VIDEO_FORMAT_UNKNOWN) {
		GST_ELEMENT_ERROR(self, STREAM, FORMAT,
			("Unsupported vfm_cap format %" GST_FOURCC_FORMAT,
			 GST_FOURCC_ARGS(mp->pixelformat)),
			("Configure vdin for 8-bit NV21/NV12/UYVY/YUV444 output"));
		return FALSE;
	}

	gst_video_info_set_format(&self->info, vfmt, mp->width, mp->height);
	self->repack_vuy24 = mp->pixelformat == V4L2_PIX_FMT_AML_VUY24;
	self->repack_amly = mp->pixelformat == V4L2_PIX_FMT_AMLY;
	self->input_size = mp->plane_fmt[0].sizeimage;
	self->input_stride = mp->plane_fmt[0].bytesperline;
	self->input_width = mp->width;
	self->input_height = mp->height;
	gboolean scaled = self->output_height && self->output_height < mp->height;
	if (scaled) {
		if (!self->repack_amly || self->output_height % 2) {
			GST_ELEMENT_ERROR(self, STREAM, FORMAT,
				("GPU scaling requires packed ten-bit input and an even height"), (NULL));
			return FALSE;
		}
		guint width = (guint64)mp->width * self->output_height / mp->height & ~1u;
		gst_video_info_set_format(&self->info, vfmt, MAX(width, 2u), self->output_height);
	}
	if (self->repack_amly && (mp->width % 2 || mp->height % 2 ||
	    self->input_stride % 16 ||
	    self->input_stride < (((guint64)mp->width * 5 / 2 + 15) & ~(guint64)15) ||
	    self->input_size < (guint64)self->input_stride * mp->height)) {
		GST_ELEMENT_ERROR(self, STREAM, FORMAT,
			("Invalid packed 10-bit frame geometry"), (NULL));
		return FALSE;
	}
	if (self->repack_vuy24 &&
	    (mp->plane_fmt[0].bytesperline < (guint64)mp->width * 3 ||
	     mp->plane_fmt[0].sizeimage <
	     (guint64)mp->plane_fmt[0].bytesperline * mp->height)) {
		GST_ELEMENT_ERROR(self, STREAM, FORMAT,
			("Invalid VUY24 stride or buffer size"), (NULL));
		return FALSE;
	}
	/* vdin writes planes back to back with stride == width */
	if (!self->repack_amly)
		self->info.stride[0] = mp->plane_fmt[0].bytesperline;
	if (!self->repack_amly && GST_VIDEO_INFO_N_PLANES(&self->info) == 2) {
		self->info.stride[1] = self->info.stride[0];
		self->info.offset[1] = (gsize)self->info.stride[0] * mp->height;
	}
	if (!self->repack_amly)
		self->info.size = mp->plane_fmt[0].sizeimage;
	else {
		gsize pitch = ((gsize)self->info.width * 2 + 63) & ~(gsize)63;
		self->info.stride[0] = self->info.stride[1] = pitch;
		self->info.offset[1] = pitch * self->info.height;
		self->info.size = pitch * self->info.height * 3 / 2;
	}
	if (mp->colorspace == V4L2_COLORSPACE_REC709)
		gst_video_colorimetry_from_string(&self->info.colorimetry, "bt709");
	if (mp->colorspace == V4L2_COLORSPACE_BT2020 && mp->xfer_func != V4L2_XFER_FUNC_SMPTE2084) {
		GST_ELEMENT_ERROR(self, STREAM, FORMAT,
			("Native BT.2020 capture currently requires PQ transfer"), (NULL));
		return FALSE;
	}
	if (mp->colorspace == V4L2_COLORSPACE_BT2020 && mp->xfer_func == V4L2_XFER_FUNC_SMPTE2084) {
		/* Resolve the transfer enum in the runtime: vendor GStreamer has an
		 * extra enum entry that changes the numeric value of SMPTE2084. */
		gst_video_colorimetry_from_string(&self->info.colorimetry, "bt2100-pq");
		self->info.colorimetry.range = mp->quantization == V4L2_QUANTIZATION_FULL_RANGE ?
			GST_VIDEO_COLOR_RANGE_0_255 : GST_VIDEO_COLOR_RANGE_16_235;
	}
	/* get_caps/negotiate can query repeatedly before the first frame. Keep the
	 * same preallocated CMA slots unless geometry or colorimetry really changed. */
	if (self->info_valid && old_size == self->input_size && old_stride == self->input_stride &&
	    old_width == self->input_width && old_height == self->input_height &&
	    old_amly == self->repack_amly && old_vuy24 == self->repack_vuy24 &&
	    gst_video_info_is_equal(&old, &self->info) && old.size == self->info.size &&
	    (!(self->repack_amly || self->repack_vuy24) || self->repack_pool))
		return TRUE;
	clear_repack(self);
#ifdef HAVE_AMLY_GPU
	if (self->repack_amly && self->gpu_conversion) {
		GError *error = NULL;
		self->gpu = amly_gpu_new(&error);
		if (self->gpu) {
			gsize pitch = self->info.stride[0];
			/* The low-delay SDK retires the submitted input on each completed
			 * encode call. Two slots overlap GPU N+1 with encoder N. */
			GstBufferPool *pool = codecmm_pool_new(self->info.size, 2, &error);
			if (pool) {
				GST_OBJECT_LOCK(self);
				self->repack_pool = pool;
				gst_buffer_pool_set_flushing(pool, self->flushing);
				GST_OBJECT_UNLOCK(self);
				GST_INFO_OBJECT(self, "AMLY GPU conversion: codecmm P010 pitch=%zu pool=2", pitch);
			} else {
				amly_gpu_free(self->gpu);
				self->gpu = NULL;
			}
		}
		if (!self->gpu)
			GST_WARNING_OBJECT(self, "AMLY GPU unavailable, using CPU conversion: %s",
				error ? error->message : "initialization failed");
		g_clear_error(&error);
	}
#endif
	if (scaled && !self->repack_pool) {
		GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
			("Packed ten-bit scaling requires the Mali GPU and contiguous output buffers"), (NULL));
		return FALSE;
	}
	if (self->repack_vuy24 || self->repack_amly) {
		GstStructure *config;
		if (self->repack_pool)
			goto pool_ready;
		if (self->repack_amly && !self->repack_worker)
			self->repack_worker = g_thread_pool_new(amly_worker, NULL, 1, FALSE, NULL);
		GstBufferPool *pool = gst_buffer_pool_new();
		config = gst_buffer_pool_get_config(pool);
		/* Reuse touched pages instead of allocating/zeroing 25 MB per frame.
		 * No maximum: downstream holding a buffer must not deadlock stop. */
		gst_buffer_pool_config_set_params(config, NULL, self->info.size, 2, 0);
		if (!gst_buffer_pool_set_config(pool, config) ||
		    !gst_buffer_pool_set_active(pool, TRUE)) {
			gst_object_unref(pool);
			return FALSE;
		}
		GST_OBJECT_LOCK(self);
		self->repack_pool = pool;
		gst_buffer_pool_set_flushing(pool, self->flushing);
		GST_OBJECT_UNLOCK(self);
	}
pool_ready:
	self->info_valid = TRUE;
	GST_INFO_OBJECT(self, "format %s %ux%u size %u",
		gst_video_format_to_string(vfmt), mp->width, mp->height,
		mp->plane_fmt[0].sizeimage);
	return TRUE;
}

static gboolean
queue_token(GstVfmCapSrc *self, guint index)
{
	struct v4l2_plane plane = { 0 };
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.index = index,
		.length = 1,
		.m.planes = &plane,
	};

	if (ioctl(self->fd, VIDIOC_QBUF, &buf) < 0) {
		GST_ERROR_OBJECT(self, "QBUF %u: %s", index, g_strerror(errno));
		return FALSE;
	}
	return TRUE;
}

static gboolean
stream_on(GstVfmCapSrc *self)
{
	struct v4l2_requestbuffers req = {
		.count = NUM_TOKENS,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	};
	struct v4l2_event_subscription sub = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
	};
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	guint i;

	if (ioctl(self->fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
		GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS, (NULL),
			("REQBUFS: %s", g_strerror(errno)));
		return FALSE;
	}
	for (i = 0; i < req.count; i++)
		if (!queue_token(self, i))
			return FALSE;
	ioctl(self->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
	if (ioctl(self->fd, VIDIOC_STREAMON, &type) < 0) {
		GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS, (NULL),
			("STREAMON: %s", g_strerror(errno)));
		return FALSE;
	}
	self->streaming = TRUE;
	return TRUE;
}

static void
stream_off(GstVfmCapSrc *self)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct v4l2_requestbuffers req = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	};

	if (!self->streaming)
		return;
	ioctl(self->fd, VIDIOC_STREAMOFF, &type);
	ioctl(self->fd, VIDIOC_REQBUFS, &req);
	self->streaming = FALSE;
}

/* Drain pending V4L2 events; returns TRUE if the source changed. */
static gboolean
drain_events(GstVfmCapSrc *self)
{
	struct v4l2_event ev;
	gboolean changed = FALSE;

	while (ioctl(self->fd, VIDIOC_DQEVENT, &ev) == 0)
		if (ev.type == V4L2_EVENT_SOURCE_CHANGE)
			changed = TRUE;
	return changed;
}

/* Re-read the device format; if it differs, restart with new caps. */
static GstFlowReturn
check_renegotiate(GstVfmCapSrc *self)
{
	GstVideoInfo old = self->info;

	if (!query_format(self))
		return GST_FLOW_ERROR;
	if (gst_video_info_is_equal(&old, &self->info) &&
	    old.size == self->info.size)
		return GST_FLOW_OK;

	GST_INFO_OBJECT(self, "source changed, renegotiating");
	stream_off(self);
	if (!gst_base_src_negotiate(GST_BASE_SRC(self)))
		return GST_FLOW_NOT_NEGOTIATED;
	if (!stream_on(self))
		return GST_FLOW_ERROR;
	return GST_FLOW_OK;
}

static gboolean
gst_vfmcap_src_start(GstBaseSrc *bsrc)
{
	GstVfmCapSrc *self = GST_VFMCAP_SRC(bsrc);

	self->fd = open(self->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (self->fd < 0) {
		GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ_WRITE,
			("Could not open %s", self->device),
			("%s", g_strerror(errno)));
		return FALSE;
	}
	self->allocator = gst_dmabuf_allocator_new();
	self->info_valid = FALSE;
	self->flushing = FALSE;
	self->frames = 0;
	return TRUE;
}

static gboolean
gst_vfmcap_src_stop(GstBaseSrc *bsrc)
{
	GstVfmCapSrc *self = GST_VFMCAP_SRC(bsrc);

	stream_off(self);
	if (self->fd >= 0)
		close(self->fd);
	self->fd = -1;
	gst_clear_object(&self->allocator);
	clear_repack(self);
	return TRUE;
}

static GstCaps *
gst_vfmcap_src_get_caps(GstBaseSrc *bsrc, GstCaps *filter)
{
	GstVfmCapSrc *self = GST_VFMCAP_SRC(bsrc);
	GstCaps *caps;

	if (self->fd >= 0 && (self->info_valid || query_format(self)))
		caps = gst_video_info_to_caps(&self->info);
	else
		caps = gst_pad_get_pad_template_caps(GST_BASE_SRC_PAD(bsrc));

	if (filter) {
		GstCaps *tmp = gst_caps_intersect_full(filter, caps,
						       GST_CAPS_INTERSECT_FIRST);
		gst_caps_unref(caps);
		caps = tmp;
	}
	return caps;
}

static GstCaps *
gst_vfmcap_src_fixate(GstBaseSrc *bsrc, GstCaps *caps)
{
	GstStructure *s;

	caps = gst_caps_make_writable(caps);
	s = gst_caps_get_structure(caps, 0);
	gst_structure_fixate_field_nearest_fraction(s, "framerate", 60, 1);
	return GST_BASE_SRC_CLASS(gst_vfmcap_src_parent_class)->fixate(bsrc, caps);
}

static gboolean
gst_vfmcap_src_negotiate(GstBaseSrc *bsrc)
{
	GstVfmCapSrc *self = GST_VFMCAP_SRC(bsrc);
	GstCaps *caps;
	gboolean ret;

	if (!query_format(self))
		return FALSE;
	caps = gst_video_info_to_caps(&self->info);
	/* vdin follows the HDMI source, the rate is not known up front */
	gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION, 0, 1, NULL);
	ret = gst_base_src_set_caps(bsrc, caps);
	gst_caps_unref(caps);
	return ret;
}

static gboolean
gst_vfmcap_src_unlock(GstBaseSrc *bsrc)
{
	GstVfmCapSrc *self = GST_VFMCAP_SRC(bsrc);
	GST_OBJECT_LOCK(self);
	self->flushing = TRUE;
	GstBufferPool *pool = self->repack_pool ? gst_object_ref(self->repack_pool) : NULL;
	GST_OBJECT_UNLOCK(self);
	if (pool) {
		gst_buffer_pool_set_flushing(pool, TRUE);
		gst_object_unref(pool);
	}
	return TRUE;
}

static gboolean
gst_vfmcap_src_unlock_stop(GstBaseSrc *bsrc)
{
	GstVfmCapSrc *self = GST_VFMCAP_SRC(bsrc);
	GST_OBJECT_LOCK(self);
	self->flushing = FALSE;
	GstBufferPool *pool = self->repack_pool ? gst_object_ref(self->repack_pool) : NULL;
	GST_OBJECT_UNLOCK(self);
	if (pool) {
		gst_buffer_pool_set_flushing(pool, FALSE);
		gst_object_unref(pool);
	}
	return TRUE;
}

static GstFlowReturn
gst_vfmcap_src_create(GstPushSrc *psrc, GstBuffer **outbuf)
{
	GstVfmCapSrc *self = GST_VFMCAP_SRC(psrc);
	struct v4l2_plane plane;
	struct v4l2_buffer buf;
	struct vfm_cap_dmabuf_req req;
	struct pollfd pfd = { .fd = self->fd, .events = POLLIN | POLLPRI };
	GstBuffer *gbuf;
	GstMemory *mem;
	GstClock *clock;

	if (!self->streaming && !stream_on(self))
		return GST_FLOW_ERROR;

	for (;;) {
		gboolean recheck = FALSE;
		int r;

		if (self->flushing)
			return GST_FLOW_FLUSHING;
		r = poll(&pfd, 1, POLL_TIMEOUT_MS);
		if (r < 0 && errno != EINTR) {
			GST_ELEMENT_ERROR(self, RESOURCE, READ, (NULL),
				("poll: %s", g_strerror(errno)));
			return GST_FLOW_ERROR;
		}
		if (r <= 0)
			continue;

		if (pfd.revents & POLLPRI)
			recheck = drain_events(self);
		if (recheck) {
			GstFlowReturn ret = check_renegotiate(self);

			if (ret != GST_FLOW_OK)
				return ret;
		}
		if (!(pfd.revents & POLLIN))
			continue;

		memset(&plane, 0, sizeof(plane));
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.length = 1;
		buf.m.planes = &plane;
		if (ioctl(self->fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN)
				continue;
			GST_ELEMENT_ERROR(self, RESOURCE, READ, (NULL),
				("DQBUF: %s", g_strerror(errno)));
			return GST_FLOW_ERROR;
		}
		if (self->timing && (buf.timestamp.tv_sec || buf.timestamp.tv_usec)) {
			struct timespec now;
			/* vfm_cap timestamps delivery with ktime_get_ns(), not exposure.
			 * CLOCK_MONOTONIC measures how long this token waited for dequeue. */
			if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
				gint64 elapsed = (gint64)now.tv_sec * G_GINT64_CONSTANT(1000000000) + now.tv_nsec
					- (gint64)buf.timestamp.tv_sec * G_GINT64_CONSTANT(1000000000)
					- (gint64)buf.timestamp.tv_usec * 1000;
				if (elapsed >= 0)
					GST_INFO_OBJECT(self, "VIM4 timing delivery_to_dequeue_ms=%.3f sequence=%u",
						(double)elapsed / 1000000.0, buf.sequence);
			}
		}

		memset(&req, 0, sizeof(req));
		req.index = buf.index;
		r = ioctl(self->fd, VFM_CAP_IOC_GET_DMABUF, &req);
		/* the fd holds the frame; the token can go back right away */
		if (!queue_token(self, buf.index)) {
			if (r == 0)
				close(req.fd);
			return GST_FLOW_ERROR;
		}
		if (r < 0 || req.fd < 0) {
			GST_DEBUG_OBJECT(self, "no dmabuf for token %u", buf.index);
			continue;
		}
		if (req.size != self->input_size) {
			GstFlowReturn ret;

			/* format changed without (or ahead of) SOURCE_CHANGE */
			GST_DEBUG_OBJECT(self, "frame size %u, expected %zu",
				req.size, (gsize)self->input_size);
			close(req.fd);
			ret = check_renegotiate(self);
			if (ret != GST_FLOW_OK)
				return ret;
			continue;
		}
		break;
	}

	mem = gst_dmabuf_allocator_alloc(self->allocator, req.fd, req.size);
	gst_memory_resize(mem, 0, self->input_size);
	gbuf = gst_buffer_new();
	gst_buffer_append_memory(gbuf, mem);
#ifdef HAVE_AMLY_GPU
	if (self->gpu) {
		GstBuffer *converted = NULL;
		GstFlowReturn acquired = gst_buffer_pool_acquire_buffer(self->repack_pool, &converted, NULL);
		if (acquired != GST_FLOW_OK) {
			gst_buffer_unref(gbuf);
			return acquired;
		}
		GstMemory *output = gst_buffer_peek_memory(converted, 0);
		gsize capacity, offset;
		gst_memory_get_sizes(output, &offset, &capacity);
		GError *error = NULL;
		gboolean ok = amly_gpu_convert_scaled(self->gpu, req.fd, req.size,
			gst_dmabuf_memory_get_fd(output), capacity,
			self->input_width, self->input_height,
			self->info.width, self->info.height, self->input_stride,
			self->info.stride[0], &error);
		/* GPU imports must be gone before dropping the captured frame fd. */
		gst_buffer_unref(gbuf);
		if (!ok) {
			gst_buffer_unref(converted);
			GST_ELEMENT_ERROR(self, RESOURCE, READ, ("AMLY GPU conversion failed"),
				("%s", error ? error->message : "unknown GPU error"));
			g_clear_error(&error);
			return GST_FLOW_ERROR;
		}
		gbuf = converted;
	} else
#endif
	if (self->repack_vuy24 || self->repack_amly) {
		gint64 begin = self->timing ? g_get_monotonic_time() : 0;
		GstMapInfo input = GST_MAP_INFO_INIT, output = GST_MAP_INFO_INIT;
		GstBuffer *converted = NULL;
		GstFlowReturn acquired = gst_buffer_pool_acquire_buffer(self->repack_pool, &converted, NULL);
		if (acquired != GST_FLOW_OK) {
			gst_buffer_unref(gbuf);
			return acquired;
		}
		struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
		gboolean synced = ioctl(req.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
		gboolean ok = acquired == GST_FLOW_OK && synced && gst_buffer_map(gbuf, &input, GST_MAP_READ) &&
			converted && gst_buffer_map(converted, &output, GST_MAP_WRITE);
		gint64 mapped = self->timing ? g_get_monotonic_time() : 0;

		if (ok && self->repack_amly) {
			repack_amly(self, output.data, input.data);
		} else if (ok) {
			gsize row_bytes = GST_VIDEO_INFO_WIDTH(&self->info) * 3;
			gsize height = GST_VIDEO_INFO_HEIGHT(&self->info);
			gsize stride = self->info.stride[0];
			/* Initialize only padding, avoiding an extra full-frame write. */
			for (gsize y = 0; y < height; y++)
				memset(output.data + y * stride + row_bytes, 0, stride - row_bytes);
			memset(output.data + height * stride, 0, output.size - height * stride);
			/* The driver's read-only cacheable mapping is synchronized above.
			 * Fuse the copy and channel reorder into one memory pass. */
			vuy24_to_v308(output.data, input.data,
				GST_VIDEO_INFO_WIDTH(&self->info),
				GST_VIDEO_INFO_HEIGHT(&self->info), self->info.stride[0]);
		}
		if (self->timing)
			GST_INFO_OBJECT(self, "VIM4 packed timing map_sync_ms=%.3f convert_ms=%.3f",
				(double)(mapped - begin) / 1000.0,
				(double)(g_get_monotonic_time() - mapped) / 1000.0);
		if (output.data)
			gst_buffer_unmap(converted, &output);
		if (input.data)
			gst_buffer_unmap(gbuf, &input);
		if (synced) {
			sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
			if (ioctl(req.fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
				ok = FALSE;
		}
		gst_buffer_unref(gbuf);
		if (!ok) {
			if (converted)
				gst_buffer_unref(converted);
			GST_ELEMENT_ERROR(self, RESOURCE, READ,
				("Cannot map or synchronize packed capture frame"), (NULL));
			return GST_FLOW_ERROR;
		}
		gbuf = converted;
	}
	gst_buffer_add_video_meta_full(gbuf, GST_VIDEO_FRAME_FLAG_NONE,
		GST_VIDEO_INFO_FORMAT(&self->info),
		GST_VIDEO_INFO_WIDTH(&self->info),
		GST_VIDEO_INFO_HEIGHT(&self->info),
		GST_VIDEO_INFO_N_PLANES(&self->info),
		self->info.offset, self->info.stride);

	clock = gst_element_get_clock(GST_ELEMENT(self));
	if (clock) {
		GstClockTime now = gst_clock_get_time(clock);
		GstClockTime base = gst_element_get_base_time(GST_ELEMENT(self));

		GST_BUFFER_PTS(gbuf) = now > base ? now - base : 0;
		gst_object_unref(clock);
	}
	GST_BUFFER_OFFSET(gbuf) = self->frames++;
	*outbuf = gbuf;
	return GST_FLOW_OK;
}

static void
gst_vfmcap_src_set_property(GObject *obj, guint id, const GValue *v,
			    GParamSpec *pspec)
{
	GstVfmCapSrc *self = GST_VFMCAP_SRC(obj);

	switch (id) {
	case PROP_DEVICE:
		g_free(self->device);
		self->device = g_value_dup_string(v);
		break;
	case PROP_GPU_CONVERSION:
		self->gpu_conversion = g_value_get_boolean(v);
		break;
	case PROP_HEIGHT:
		self->output_height = g_value_get_uint(v);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
	}
}

static void
gst_vfmcap_src_get_property(GObject *obj, guint id, GValue *v,
			    GParamSpec *pspec)
{
	GstVfmCapSrc *self = GST_VFMCAP_SRC(obj);

	switch (id) {
	case PROP_DEVICE:
		g_value_set_string(v, self->device);
		break;
	case PROP_GPU_CONVERSION:
		g_value_set_boolean(v, self->gpu_conversion);
		break;
	case PROP_HEIGHT:
		g_value_set_uint(v, self->output_height);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
	}
}

static void
gst_vfmcap_src_finalize(GObject *obj)
{
	GstVfmCapSrc *self = GST_VFMCAP_SRC(obj);
	if (self->repack_worker)
		g_thread_pool_free(self->repack_worker, FALSE, TRUE);
	g_free(self->device);
	G_OBJECT_CLASS(gst_vfmcap_src_parent_class)->finalize(obj);
}

static void
gst_vfmcap_src_class_init(GstVfmCapSrcClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
	GstBaseSrcClass *bsrc_class = GST_BASE_SRC_CLASS(klass);
	GstPushSrcClass *psrc_class = GST_PUSH_SRC_CLASS(klass);

	gobject_class->set_property = gst_vfmcap_src_set_property;
	gobject_class->get_property = gst_vfmcap_src_get_property;
	gobject_class->finalize = gst_vfmcap_src_finalize;

	g_object_class_install_property(gobject_class, PROP_DEVICE,
		g_param_spec_string("device", "Device", "vfm_cap device path",
			DEFAULT_DEVICE,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
			GST_PARAM_MUTABLE_READY));
	g_object_class_install_property(gobject_class, PROP_GPU_CONVERSION,
		g_param_spec_boolean("gpu-conversion", "GPU conversion",
			"Use Mali compute and contiguous DMA for packed 10-bit input when available",
			TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));
	g_object_class_install_property(gobject_class, PROP_HEIGHT,
		g_param_spec_uint("height", "Output height",
			"Maximum packed ten-bit output height, zero keeps native size; scaling requires Mali GPU",
			0, 2160, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));

	gst_element_class_set_static_metadata(element_class,
		"VFM capture source", "Source/Video",
		"Zero-copy DMA-buf frames from the vfm_cap V4L2 device",
		"vim4-kvm");
	gst_element_class_add_static_pad_template(element_class, &src_template);

	bsrc_class->start = gst_vfmcap_src_start;
	bsrc_class->stop = gst_vfmcap_src_stop;
	bsrc_class->get_caps = gst_vfmcap_src_get_caps;
	bsrc_class->fixate = gst_vfmcap_src_fixate;
	bsrc_class->negotiate = gst_vfmcap_src_negotiate;
	bsrc_class->unlock = gst_vfmcap_src_unlock;
	bsrc_class->unlock_stop = gst_vfmcap_src_unlock_stop;
	psrc_class->create = gst_vfmcap_src_create;
}

static void
gst_vfmcap_src_init(GstVfmCapSrc *self)
{
	self->device = g_strdup(DEFAULT_DEVICE);
	self->fd = -1;
	self->gpu_conversion = TRUE;
	self->timing = g_strcmp0(g_getenv("VIM4_VIDEO_TIMING"), "1") == 0;
	gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
	gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
	gst_base_src_set_do_timestamp(GST_BASE_SRC(self), FALSE);
}

static gboolean
plugin_init(GstPlugin *plugin)
{
	GST_DEBUG_CATEGORY_INIT(vfmcapsrc_debug, "vfmcapsrc", 0,
				"vfm_cap source");
	return gst_element_register(plugin, "vfmcapsrc", GST_RANK_NONE,
				    GST_TYPE_VFMCAP_SRC) &&
		gst_element_register(plugin, "vim4vdin1src", GST_RANK_NONE,
				     gst_vim4_vdin1_src_get_type());
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, vfmcap,
	"vfm_cap zero-copy capture", plugin_init, VERSION, "LGPL",
	PACKAGE, "https://github.com/vim4-kvm")
