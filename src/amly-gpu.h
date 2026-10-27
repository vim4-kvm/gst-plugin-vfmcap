/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VFMCAP_AMLY_GPU_H
#define VFMCAP_AMLY_GPU_H

#include <glib.h>

typedef struct AmlyGpu AmlyGpu;

/* Requires the real ARM Mali-G52 Vulkan device and DMA-buf import support.
 * ICD selection belongs to the launcher, before the Vulkan loader is used. */
AmlyGpu *amly_gpu_new(GError **error);

/* Convert little-endian full-packed AMLY 4:2:2 to standard high-bit P010.
 * Strides are bytes; sizes are the actual DMA allocation capacities, including
 * alignment padding. Input and output must be distinct DMA allocations.
 * UV follows output_stride_bytes * height. Only visible samples are written.
 *
 * This synchronous, single-caller API borrows both fds only during the call.
 * On return, GPU work has completed and imported resources have been released;
 * the caller may return the capture frame and submit the output to the encoder.
 * A submitted GPU error drains the device and makes this instance unusable. */
gboolean amly_gpu_convert(AmlyGpu *gpu, int input_fd, gsize input_size,
                         int output_fd, gsize output_size, guint width,
                         guint height, guint input_stride,
                         guint output_stride_bytes, GError **error);

void amly_gpu_free(AmlyGpu *gpu);

#endif
