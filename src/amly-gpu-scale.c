/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "amly-gpu-scale.h"
#include "amly-to-texture-spirv.h"
#include "texture-to-p010-spirv.h"

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
} Texture;

struct AmlyGpuScaler {
    VkDevice device;
    guint width, height;
    Texture textures[2];
    VkSampler sampler;
    VkDescriptorPool pool;
    VkDescriptorSetLayout descriptors[2];
    VkDescriptorSet sets[2];
    VkPipelineLayout layouts[2];
    VkPipeline pipelines[2];
};

typedef struct {
    guint32 width, height, input_width, input_height, input_stride, output_stride_words;
} ScaleParams;

static gboolean check(VkResult result, const char *operation, GError **error)
{
    if (result == VK_SUCCESS)
        return TRUE;
    g_set_error(error, g_quark_from_static_string("amly-gpu-error"), result,
        "%s failed (Vulkan result %d)", operation, result);
    return FALSE;
}

static gboolean create_texture(AmlyGpuScaler *s, VkPhysicalDevice physical,
    Texture *texture, VkFormat format, guint width, GError **error)
{
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & needed) != needed) {
        g_set_error_literal(error, g_quark_from_static_string("amly-gpu-error"), 0,
            "Mali lacks 16-bit float storage textures with linear filtering");
        return FALSE;
    }
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {width, s->height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (!check(vkCreateImage(s->device, &image, NULL, &texture->image), "vkCreateImage", error))
        return FALSE;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(s->device, texture->image, &requirements);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    guint type;
    for (type = 0; type < memory.memoryTypeCount; type++)
        if ((requirements.memoryTypeBits & (1u << type)) &&
            (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            break;
    if (type == memory.memoryTypeCount) {
        g_set_error_literal(error, g_quark_from_static_string("amly-gpu-error"), 0,
            "No device-local memory for HDR scaling textures");
        return FALSE;
    }
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = type,
    };
    if (!check(vkAllocateMemory(s->device, &allocation, NULL, &texture->memory),
            "vkAllocateMemory(texture)", error) ||
        !check(vkBindImageMemory(s->device, texture->image, texture->memory, 0),
            "vkBindImageMemory", error))
        return FALSE;
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    return check(vkCreateImageView(s->device, &view, NULL, &texture->view), "vkCreateImageView", error);
}

static gboolean create_pipeline(AmlyGpuScaler *s, guint index, const guint32 *code,
    gsize code_size, GError **error)
{
    VkDescriptorSetLayoutBinding bindings[3] = {0};
    for (guint i = 0; i < 3; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].descriptorType = index == 0 ?
            (i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) :
            (i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    }
    VkDescriptorSetLayoutCreateInfo descriptors = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings,
    };
    if (!check(vkCreateDescriptorSetLayout(s->device, &descriptors, NULL, &s->descriptors[index]),
            "vkCreateDescriptorSetLayout(scaler)", error))
        return FALSE;
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ScaleParams)};
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &s->descriptors[index],
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    if (!check(vkCreatePipelineLayout(s->device, &layout, NULL, &s->layouts[index]),
            "vkCreatePipelineLayout(scaler)", error))
        return FALSE;
    VkShaderModuleCreateInfo module = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code_size, .pCode = code,
    };
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!check(vkCreateShaderModule(s->device, &module, NULL, &shader), "vkCreateShaderModule(scaler)", error))
        return FALSE;
    VkComputePipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"},
        .layout = s->layouts[index],
    };
    VkResult result = vkCreateComputePipelines(s->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &s->pipelines[index]);
    vkDestroyShaderModule(s->device, shader, NULL);
    return check(result, "vkCreateComputePipelines(scaler)", error);
}

AmlyGpuScaler *amly_gpu_scaler_new(VkPhysicalDevice physical, VkDevice device,
    guint width, guint height, GError **error)
{
    AmlyGpuScaler *s = g_new0(AmlyGpuScaler, 1);
    s->device = device;
    s->width = width;
    s->height = height;
    if (g_getenv("VIM4_VIDEO_TIMING")) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical, &properties);
        g_printerr("amly-gpu scaler=%ux%u subtexel_precision_bits=%u\n", width, height,
            properties.limits.subTexelPrecisionBits);
    }
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    if (!features.shaderStorageImageExtendedFormats) {
        g_set_error_literal(error, g_quark_from_static_string("amly-gpu-error"), 0,
            "HDR scaling requires extended storage-image formats");
        goto fail;
    }
    if (!create_texture(s, physical, &s->textures[0], VK_FORMAT_R16_SFLOAT, width, error) ||
        !create_texture(s, physical, &s->textures[1], VK_FORMAT_R16G16_SFLOAT, width / 2, error))
        goto fail;
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (!check(vkCreateSampler(device, &sampler, NULL, &s->sampler), "vkCreateSampler", error) ||
        !create_pipeline(s, 0, amly_to_texture_spirv, sizeof(amly_to_texture_spirv), error) ||
        !create_pipeline(s, 1, texture_to_p010_spirv, sizeof(texture_to_p010_spirv), error))
        goto fail;
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
    };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = G_N_ELEMENTS(sizes), .pPoolSizes = sizes,
    };
    if (!check(vkCreateDescriptorPool(device, &pool, NULL, &s->pool), "vkCreateDescriptorPool(scaler)", error))
        goto fail;
    VkDescriptorSetAllocateInfo sets = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = s->pool, .descriptorSetCount = 2, .pSetLayouts = s->descriptors,
    };
    if (!check(vkAllocateDescriptorSets(device, &sets, s->sets), "vkAllocateDescriptorSets(scaler)", error))
        goto fail;
    return s;
fail:
    amly_gpu_scaler_free(s);
    return NULL;
}

void amly_gpu_scaler_record(AmlyGpuScaler *s, VkCommandBuffer command,
    VkBuffer input, VkDeviceSize input_range, VkBuffer output, VkDeviceSize output_range,
    guint width, guint height, guint input_stride, guint output_stride_words)
{
    VkDescriptorBufferInfo buffers[] = {{input, 0, input_range}, {output, 0, output_range}};
    VkDescriptorImageInfo images[] = {
        {s->sampler, s->textures[0].view, VK_IMAGE_LAYOUT_GENERAL},
        {s->sampler, s->textures[1].view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet writes[6] = {0};
    for (guint pass = 0; pass < 2; pass++) {
        for (guint binding = 0; binding < 3; binding++) {
            VkWriteDescriptorSet *write = &writes[pass * 3 + binding];
            write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write->dstSet = s->sets[pass];
            write->dstBinding = binding;
            write->descriptorCount = 1;
            if ((!pass && !binding) || (pass && binding == 2)) {
                write->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write->pBufferInfo = &buffers[pass];
            } else {
                write->descriptorType = pass ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write->pImageInfo = &images[pass ? binding : binding - 1];
            }
        }
    }
    vkUpdateDescriptorSets(s->device, G_N_ELEMENTS(writes), writes, 0, NULL);
    VkImageMemoryBarrier barriers[2] = {0};
    for (guint i = 0; i < 2; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        // Every preceding job has completed. Discard its texture contents.
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].image = s->textures[i].image;
        barriers[i].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, 2, barriers);
    ScaleParams params = {width, height, s->width, s->height, input_stride, output_stride_words};
    for (guint pass = 0; pass < 2; pass++) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipelines[pass]);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, s->layouts[pass],
            0, 1, &s->sets[pass], 0, NULL);
        vkCmdPushConstants(command, s->layouts[pass], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdDispatch(command, ((pass ? width : s->width) / 2 + 63) / 64,
            pass ? height / 2 : s->height, 1);
        if (!pass) {
            for (guint i = 0; i < 2; i++) {
                barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            }
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, barriers);
        }
    }
}

void amly_gpu_scaler_free(AmlyGpuScaler *s)
{
    if (!s)
        return;
    if (s->pool) vkDestroyDescriptorPool(s->device, s->pool, NULL);
    for (guint i = 0; i < 2; i++) {
        if (s->pipelines[i]) vkDestroyPipeline(s->device, s->pipelines[i], NULL);
        if (s->layouts[i]) vkDestroyPipelineLayout(s->device, s->layouts[i], NULL);
        if (s->descriptors[i]) vkDestroyDescriptorSetLayout(s->device, s->descriptors[i], NULL);
        if (s->textures[i].view) vkDestroyImageView(s->device, s->textures[i].view, NULL);
        if (s->textures[i].image) vkDestroyImage(s->device, s->textures[i].image, NULL);
        if (s->textures[i].memory) vkFreeMemory(s->device, s->textures[i].memory, NULL);
    }
    if (s->sampler) vkDestroySampler(s->device, s->sampler, NULL);
    g_free(s);
}
