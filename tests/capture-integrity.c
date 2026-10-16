/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exclusive board test with capture-pattern.swift on the physical 4K input.
 * Usage: capture-integrity /dev/video_cap FRAME_COUNT [last-frame.raw]
 * Measures source -> appsink, including full-frame repacking. No encoding.
 */
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t marker(const guint8 *data, gint stride, guint y)
{
	uint32_t word = 0;
	for (guint bit = 0; bit < 32; bit++) {
		guint x = 90 + bit * 235 / 2;
		word = (word << 1) | (data[y * stride + x * 3] > 180);
	}
	return word;
}

static gboolean same_color(const guint8 *a, const guint8 *b)
{
	for (guint component = 0; component < 3; component++)
		if (abs((int)a[component] - b[component]) > 3)
			return FALSE;
	return TRUE;
}

int main(int argc, char **argv)
{
	gst_init(&argc, &argv);
	if (argc < 3 || argc > 4)
		return 2;
	guint count = atoi(argv[2]);
	if (count < 2)
		return 2;
	GError *error = NULL;
	GstElement *pipeline = gst_parse_launch(
		"vfmcapsrc name=source ! video/x-raw,format=v308 ! "
		"appsink name=sink sync=false max-buffers=2 drop=false", &error);
	if (error) {
		g_printerr("%s\n", error->message);
		g_error_free(error);
		if (pipeline) gst_object_unref(pipeline);
		return 1;
	}
	GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
	GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
	g_object_set(source, "device", argv[1], "num-buffers", (gint)count + 120, NULL);
	gst_object_unref(source);
	gst_element_set_state(pipeline, GST_STATE_PLAYING);
	guint frames = 0, invalid = 0, torn = 0, repeated = 0, skipped = 0, color_bad = 0;
	guint previous = 0;
	gint64 first = 0, last = 0, max_gap = 0;
	gboolean failed = FALSE;
	/* Exclude startup, first-touch faults and initial queue fill from cadence. */
	for (guint warmup = 0; warmup < 120; warmup++) {
		GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
		if (!sample) break;
		gst_sample_unref(sample);
	}
	for (; frames < count; frames++) {
		GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
		if (!sample) {
			g_printerr("No frame at %u\n", frames);
			failed = TRUE;
			break;
		}
		gint64 now = g_get_monotonic_time();
		if (!frames) first = now;
		else max_gap = MAX(max_gap, now - last);
		last = now;
		GstVideoInfo info;
		GstVideoFrame frame;
		if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
		    info.width != 3840 || info.height != 2160 ||
		    GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_v308 ||
		    !gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
			g_printerr("Invalid frame format/map\n");
			gst_sample_unref(sample);
			failed = TRUE;
			break;
		}
		const guint8 *data = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
		gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
		uint32_t word = marker(data, stride, 170);
		if (word != marker(data, stride, 1080) || word != marker(data, stride, 1990)) torn++;
		if ((word >> 24) != 0xa5 ||
		    (((word >> 16) ^ (word >> 8) ^ 0xa5) & 255) != (word & 255)) invalid++;
		guint id = (word >> 8) & 65535;
		if (frames) {
			guint step = (id - previous) & 65535;
			if (!step) repeated++;
			else if (step > 1) skipped += step - 1;
			if (step != 1)
				g_print("frame_transition index=%u previous=%u current=%u step=%u\n",
					frames, previous, id, step);
		}
		previous = id;
		const guint8 *red = data + 300 * stride + 240 * 3;
		const guint8 *cyan = data + 300 * stride + 1680 * 3;
		gboolean colors_ok = abs((int)red[2] - cyan[2]) > 180;
		for (guint x = 200; x < 216; x++)
			colors_ok &= same_color(data + 760 * stride + x * 3, x % 2 ? cyan : red);
		for (guint y = 1400; y < 1416; y++)
			colors_ok &= same_color(data + y * stride + 1000 * 3, y % 2 ? red : cyan);
		if (!colors_ok) color_bad++;
		if (frames + 1 == count && argc == 4) {
			FILE *output = fopen(argv[3], "wb");
			if (!output) failed = TRUE;
			else {
				for (guint y = 0; y < 2160; y++)
					if (fwrite(data + y * stride, 1, 11520, output) != 11520) failed = TRUE;
				if (fclose(output)) failed = TRUE;
			}
		}
		gst_video_frame_unmap(&frame);
		gst_sample_unref(sample);
	}
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(sink);
	gst_object_unref(pipeline);
	g_print("frames=%u seconds=%.6f fps=%.3f max_gap_ms=%.3f invalid=%u torn=%u repeated=%u skipped=%u color_bad=%u\n",
		frames, (last-first)/1e6, frames > 1 ? (frames-1)*1e6/(last-first) : 0,
		max_gap/1000.0, invalid, torn, repeated, skipped, color_bad);
	return failed || frames != count || invalid || torn || color_bad || repeated || skipped ? 1 : 0;
}
