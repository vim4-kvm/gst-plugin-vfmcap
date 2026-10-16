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

/* vfm_cap extension: VDIN's packed Cr-Cb-Y output with no_swap_en=1. */
#define V4L2_PIX_FMT_AML_VUY24 v4l2_fourcc('V', 'U', 'Y', '3')

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

	GstAllocator *allocator;
	GstBufferPool *repack_pool;
	GstVideoInfo info;
	gboolean info_valid;
	guint64 frames;
};

enum {
	PROP_0,
	PROP_DEVICE,
};

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
	GST_PAD_SRC, GST_PAD_ALWAYS,
	GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE("{ NV21, NV12, UYVY, v308 }")));

G_DEFINE_TYPE(GstVfmCapSrc, gst_vfmcap_src, GST_TYPE_PUSH_SRC);

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
	if (self->repack_vuy24 &&
	    (mp->plane_fmt[0].bytesperline < (guint64)mp->width * 3 ||
	     mp->plane_fmt[0].sizeimage <
	     (guint64)mp->plane_fmt[0].bytesperline * mp->height)) {
		GST_ELEMENT_ERROR(self, STREAM, FORMAT,
			("Invalid VUY24 stride or buffer size"), (NULL));
		return FALSE;
	}
	/* vdin writes planes back to back with stride == width */
	self->info.stride[0] = mp->plane_fmt[0].bytesperline;
	if (GST_VIDEO_INFO_N_PLANES(&self->info) == 2) {
		self->info.stride[1] = self->info.stride[0];
		self->info.offset[1] = (gsize)self->info.stride[0] * mp->height;
	}
	self->info.size = mp->plane_fmt[0].sizeimage;
	if (self->repack_pool) {
		gst_buffer_pool_set_active(self->repack_pool, FALSE);
		gst_clear_object(&self->repack_pool);
	}
	if (self->repack_vuy24) {
		GstStructure *config;
		self->repack_pool = gst_buffer_pool_new();
		config = gst_buffer_pool_get_config(self->repack_pool);
		/* Reuse touched pages instead of allocating/zeroing 25 MB per frame.
		 * No maximum: downstream holding a buffer must not deadlock stop. */
		gst_buffer_pool_config_set_params(config, NULL, self->info.size, 2, 0);
		if (!gst_buffer_pool_set_config(self->repack_pool, config) ||
		    !gst_buffer_pool_set_active(self->repack_pool, TRUE))
			return FALSE;
	}
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
	if (self->repack_pool) {
		gst_buffer_pool_set_active(self->repack_pool, FALSE);
		gst_clear_object(&self->repack_pool);
	}
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
	GST_VFMCAP_SRC(bsrc)->flushing = TRUE;
	return TRUE;
}

static gboolean
gst_vfmcap_src_unlock_stop(GstBaseSrc *bsrc)
{
	GST_VFMCAP_SRC(bsrc)->flushing = FALSE;
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
		if (req.size != self->info.size) {
			GstFlowReturn ret;

			/* format changed without (or ahead of) SOURCE_CHANGE */
			GST_DEBUG_OBJECT(self, "frame size %u, expected %zu",
				req.size, self->info.size);
			close(req.fd);
			ret = check_renegotiate(self);
			if (ret != GST_FLOW_OK)
				return ret;
			continue;
		}
		break;
	}

	mem = gst_dmabuf_allocator_alloc(self->allocator, req.fd, req.size);
	gst_memory_resize(mem, 0, self->info.size);
	gbuf = gst_buffer_new();
	gst_buffer_append_memory(gbuf, mem);
	if (self->repack_vuy24) {
		GstMapInfo input = GST_MAP_INFO_INIT, output = GST_MAP_INFO_INIT;
		GstBuffer *converted = NULL;
		GstFlowReturn acquired = gst_buffer_pool_acquire_buffer(self->repack_pool, &converted, NULL);
		struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
		gboolean synced = ioctl(req.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
		gboolean ok = acquired == GST_FLOW_OK && synced && gst_buffer_map(gbuf, &input, GST_MAP_READ) &&
			converted && gst_buffer_map(converted, &output, GST_MAP_WRITE);

		if (ok) {
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
				("Cannot map or synchronize VUY24 frame"), (NULL));
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
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
	}
}

static void
gst_vfmcap_src_finalize(GObject *obj)
{
	g_free(GST_VFMCAP_SRC(obj)->device);
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
				    GST_TYPE_VFMCAP_SRC);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, vfmcap,
	"vfm_cap zero-copy capture", plugin_init, VERSION, "LGPL",
	PACKAGE, "https://github.com/vim4-kvm")
