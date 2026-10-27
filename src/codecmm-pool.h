/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <gst/gst.h>

/* Bounded, eagerly allocated physical DMA buffers. Deactivation/flush wakes
 * blocked acquisitions; outstanding buffers retain their own fd ownership. */
GstBufferPool *codecmm_pool_new(gsize bytes, guint buffers, GError **error);
