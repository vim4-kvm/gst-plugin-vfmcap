/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <glib.h>
#include <vulkan/vulkan.h>

typedef struct AmlyGpuScaler AmlyGpuScaler;

/* Owns two fixed, GPU-private ten-bit textures. Call only with the preceding
 * dispatch retired; the parent converter owns submission and DMA lifetimes. */
AmlyGpuScaler *amly_gpu_scaler_new(VkPhysicalDevice physical, VkDevice device,
    guint width, guint height, GError **error);
void amly_gpu_scaler_free(AmlyGpuScaler *scaler);
void amly_gpu_scaler_record(AmlyGpuScaler *scaler, VkCommandBuffer command,
    VkBuffer input, VkDeviceSize input_range, VkBuffer output, VkDeviceSize output_range,
    guint width, guint height, guint input_stride, guint output_stride_words,
    guint output_luma_rows);
