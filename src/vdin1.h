/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <gst/gst.h>

GType gst_vim4_vdin1_src_get_type(void);
GstCaps *vim4_vdin1_caps(const gchar *input_info, guint output_height);
