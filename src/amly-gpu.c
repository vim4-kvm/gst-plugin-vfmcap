/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "amly-gpu.h"
#include "amly-gpu-scale.h"
#include "amly-to-p010-spirv.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define GPU_FENCE_TIMEOUT_NS (10ULL * 1000 * 1000 * 1000)

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
} ImportedBuffer;

typedef struct {
    guint32 width, height, input_stride, output_stride_words;
    guint32 input_width, input_height;
} ConvertParams;

struct AmlyGpu {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkDevice device;
    VkQueue queue;
    guint queue_family;
    VkDescriptorSetLayout descriptor_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    AmlyGpuScaler *scaler;
    guint scale_input_width, scale_input_height;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    gboolean failed;
    gboolean timing;
    guint64 frames;
    double import_us, dispatch_us, wait_us, free_us;
};

static gboolean fail(GError **error, const char *operation, VkResult result)
{
    g_set_error(error, g_quark_from_static_string("amly-gpu-error"), result,
                "%s failed (Vulkan result %d)", operation, result);
    return FALSE;
}

static gboolean invalid(GError **error, const char *message)
{
    g_set_error_literal(error, g_quark_from_static_string("amly-gpu-error"),
                        VK_ERROR_INITIALIZATION_FAILED, message);
    return FALSE;
}

static gboolean has_extension(const VkExtensionProperties *extensions,
                              guint count, const char *name)
{
    for (guint i = 0; i < count; i++)
        if (!strcmp(extensions[i].extensionName, name))
            return TRUE;
    return FALSE;
}

static gboolean select_device(AmlyGpu *gpu, GError **error)
{
    guint count = 0;
    VkResult result = vkEnumeratePhysicalDevices(gpu->instance, &count, NULL);
    if (result != VK_SUCCESS)
        return fail(error, "vkEnumeratePhysicalDevices", result);
    if (!count)
        return invalid(error, "No Vulkan hardware device is available");
    VkPhysicalDevice *devices = g_new(VkPhysicalDevice, count);
    result = vkEnumeratePhysicalDevices(gpu->instance, &count, devices);
    if (result != VK_SUCCESS) {
        g_free(devices);
        return fail(error, "vkEnumeratePhysicalDevices", result);
    }
    for (guint i = 0; i < count; i++) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.vendorID == 0x13b5 &&
            properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
            g_str_has_prefix(properties.deviceName, "Mali-G52")) {
            gpu->physical = devices[i];
            gpu->properties = properties;
            break;
        }
    }
    g_free(devices);
    if (!gpu->physical)
        return invalid(error, "AMLY conversion requires the real ARM Mali-G52 GPU");
    if (gpu->properties.apiVersion < VK_API_VERSION_1_1)
        return invalid(error, "AMLY DMA import requires Vulkan 1.1 or newer");

    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu->physical, &count, NULL);
    VkQueueFamilyProperties *queues = g_new(VkQueueFamilyProperties, count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu->physical, &count, queues);
    gboolean found = FALSE;
    for (guint i = 0; i < count; i++) {
        if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            gpu->queue_family = i;
            found = TRUE;
            break;
        }
    }
    g_free(queues);
    if (!found)
        return invalid(error, "Mali device exposes no compute queue");
    if (gpu->properties.limits.maxComputeWorkGroupInvocations < 64 ||
        gpu->properties.limits.maxComputeWorkGroupSize[0] < 64 ||
        gpu->properties.limits.maxPushConstantsSize < sizeof(ConvertParams))
        return invalid(error, "Mali compute limits cannot run the AMLY shader");
    vkGetPhysicalDeviceMemoryProperties(gpu->physical, &gpu->memory_properties);
    return TRUE;
}

static gboolean create_device(AmlyGpu *gpu, GError **error)
{
    guint count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(gpu->physical, NULL,
                                                          &count, NULL);
    if (result != VK_SUCCESS)
        return fail(error, "vkEnumerateDeviceExtensionProperties", result);
    VkExtensionProperties *properties = g_new(VkExtensionProperties, count);
    result = vkEnumerateDeviceExtensionProperties(gpu->physical, NULL, &count,
                                                  properties);
    if (result != VK_SUCCESS) {
        g_free(properties);
        return fail(error, "vkEnumerateDeviceExtensionProperties", result);
    }
    const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                               VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    gboolean supported = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(extensions); i++)
        supported &= has_extension(properties, count, extensions[i]);
    g_free(properties);
    if (!supported)
        return invalid(error, "Mali driver does not support Vulkan DMA-buf imports");

    VkPhysicalDeviceExternalBufferInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkExternalBufferProperties external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    vkGetPhysicalDeviceExternalBufferProperties(gpu->physical, &external_info,
                                                 &external);
    if (!(external.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        !(external.externalMemoryProperties.compatibleHandleTypes &
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT))
        return invalid(error, "Mali storage buffers cannot import DMA-buf memory");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = gpu->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = G_N_ELEMENTS(extensions),
        .ppEnabledExtensionNames = extensions,
    };
    VkPhysicalDeviceFeatures available, enabled = {0};
    vkGetPhysicalDeviceFeatures(gpu->physical, &available);
    enabled.shaderStorageImageExtendedFormats = available.shaderStorageImageExtendedFormats;
    create.pEnabledFeatures = &enabled;
    result = vkCreateDevice(gpu->physical, &create, NULL, &gpu->device);
    if (result != VK_SUCCESS)
        return fail(error, "vkCreateDevice", result);
    vkGetDeviceQueue(gpu->device, gpu->queue_family, 0, &gpu->queue);
    gpu->get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(gpu->device, "vkGetMemoryFdPropertiesKHR");
    if (!gpu->get_memory_fd_properties)
        return invalid(error, "Mali driver lacks vkGetMemoryFdPropertiesKHR");
    return TRUE;
}

static gboolean create_pipeline(AmlyGpu *gpu, GError **error)
{
    VkDescriptorSetLayoutBinding bindings[2] = {0};
    for (guint i = 0; i < 2; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptor = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    VkResult result = vkCreateDescriptorSetLayout(gpu->device, &descriptor, NULL,
                                                   &gpu->descriptor_layout);
    if (result != VK_SUCCESS)
        return fail(error, "vkCreateDescriptorSetLayout", result);
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size = sizeof(ConvertParams),
    };
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &gpu->descriptor_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    result = vkCreatePipelineLayout(gpu->device, &layout, NULL, &gpu->pipeline_layout);
    if (result != VK_SUCCESS)
        return fail(error, "vkCreatePipelineLayout", result);
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(amly_to_p010_spirv),
        .pCode = amly_to_p010_spirv,
    };
    VkShaderModule shader = VK_NULL_HANDLE;
    result = vkCreateShaderModule(gpu->device, &shader_info, NULL, &shader);
    if (result != VK_SUCCESS)
        return fail(error, "vkCreateShaderModule", result);
    VkComputePipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main",
        },
        .layout = gpu->pipeline_layout,
    };
    result = vkCreateComputePipelines(gpu->device, VK_NULL_HANDLE, 1, &pipeline,
                                       NULL, &gpu->pipeline);
    vkDestroyShaderModule(gpu->device, shader, NULL);
    if (result != VK_SUCCESS)
        return fail(error, "vkCreateComputePipelines", result);

    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    result = vkCreateDescriptorPool(gpu->device, &pool, NULL, &gpu->descriptor_pool);
    if (result != VK_SUCCESS)
        return fail(error, "vkCreateDescriptorPool", result);
    VkDescriptorSetAllocateInfo set = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = gpu->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &gpu->descriptor_layout,
    };
    result = vkAllocateDescriptorSets(gpu->device, &set, &gpu->descriptor_set);
    if (result != VK_SUCCESS)
        return fail(error, "vkAllocateDescriptorSets", result);
    VkCommandPoolCreateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = gpu->queue_family,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    result = vkCreateCommandPool(gpu->device, &commands, NULL, &gpu->command_pool);
    if (result != VK_SUCCESS)
        return fail(error, "vkCreateCommandPool", result);
    VkCommandBufferAllocateInfo command = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = gpu->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    result = vkAllocateCommandBuffers(gpu->device, &command, &gpu->command);
    if (result != VK_SUCCESS)
        return fail(error, "vkAllocateCommandBuffers", result);
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = vkCreateFence(gpu->device, &fence, NULL, &gpu->fence);
    return result == VK_SUCCESS || fail(error, "vkCreateFence", result);
}

AmlyGpu *amly_gpu_new(GError **error)
{
    AmlyGpu *gpu = g_new0(AmlyGpu, 1);
    gpu->timing = g_strcmp0(g_getenv("VIM4_VIDEO_TIMING"), "1") == 0;
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "VIM4 AMLY converter",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkResult result = vkCreateInstance(&instance, NULL, &gpu->instance);
    if (result != VK_SUCCESS) {
        fail(error, "vkCreateInstance", result);
        amly_gpu_free(gpu);
        return NULL;
    }
    if (!select_device(gpu, error) || !create_device(gpu, error) ||
        !create_pipeline(gpu, error)) {
        amly_gpu_free(gpu);
        return NULL;
    }
    return gpu;
}

static void release_import(AmlyGpu *gpu, ImportedBuffer *buffer)
{
    if (buffer->buffer)
        vkDestroyBuffer(gpu->device, buffer->buffer, NULL);
    if (buffer->memory)
        vkFreeMemory(gpu->device, buffer->memory, NULL);
    memset(buffer, 0, sizeof(*buffer));
}

static gboolean import_buffer(AmlyGpu *gpu, int fd, gsize capacity,
                              VkDeviceSize range, ImportedBuffer *buffer,
                              GError **error)
{
    VkExternalMemoryBufferCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkBufferCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external,
        .size = range,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vkCreateBuffer(gpu->device, &create, NULL, &buffer->buffer);
    if (result != VK_SUCCESS)
        return fail(error, "vkCreateBuffer(DMA-buf)", result);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(gpu->device, buffer->buffer, &requirements);
    if (requirements.size > capacity)
        return invalid(error, "DMA allocation is smaller than Vulkan buffer requirements");
    VkMemoryFdPropertiesKHR properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    result = gpu->get_memory_fd_properties(gpu->device,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &properties);
    if (result != VK_SUCCESS)
        return fail(error, "vkGetMemoryFdPropertiesKHR", result);
    guint types = properties.memoryTypeBits & requirements.memoryTypeBits;
    guint type = G_MAXUINT;
    for (guint i = 0; i < gpu->memory_properties.memoryTypeCount; i++) {
        if (types & (1u << i)) {
            if (type == G_MAXUINT)
                type = i;
            if (gpu->memory_properties.memoryTypes[i].propertyFlags &
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                type = i;
                break;
            }
        }
    }
    if (type == G_MAXUINT)
        return invalid(error, "DMA-buf and Vulkan buffer have no compatible memory type");

    int imported_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (imported_fd < 0) {
        g_set_error(error, g_quark_from_static_string("amly-gpu-error"), errno,
                    "Duplicating DMA-buf fd failed: %s", g_strerror(errno));
        return FALSE;
    }
    // Always dedicate imports, including devices requiring DEDICATED_ONLY.
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = buffer->buffer,
    };
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    result = vkAllocateMemory(gpu->device, &allocate, NULL, &buffer->memory);
    if (result != VK_SUCCESS) {
        // Ownership transfers to Vulkan only after successful allocation.
        close(imported_fd);
        return fail(error, "vkAllocateMemory(DMA-buf)", result);
    }
    result = vkBindBufferMemory(gpu->device, buffer->buffer, buffer->memory, 0);
    return result == VK_SUCCESS || fail(error, "vkBindBufferMemory", result);
}

static void buffer_barriers(AmlyGpu *gpu, ImportedBuffer *input,
                             ImportedBuffer *output, gboolean acquire)
{
    VkBufferMemoryBarrier barriers[2] = {0};
    ImportedBuffer *buffers[] = {input, output};
    for (guint i = 0; i < 2; i++) {
        barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[i].srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : gpu->queue_family;
        barriers[i].dstQueueFamilyIndex = acquire ? gpu->queue_family : VK_QUEUE_FAMILY_EXTERNAL;
        barriers[i].buffer = buffers[i]->buffer;
        barriers[i].size = VK_WHOLE_SIZE;
        VkAccessFlags access = i == 0 ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].srcAccessMask = acquire ? 0 : access;
        barriers[i].dstAccessMask = acquire ? access : 0;
    }
    vkCmdPipelineBarrier(gpu->command,
        acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        acquire ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 2, barriers, 0, NULL);
}

static gboolean record_commands(AmlyGpu *gpu, ImportedBuffer *input,
                                 ImportedBuffer *output, VkDeviceSize input_range,
                                 VkDeviceSize output_range, const ConvertParams *params,
                                 GError **error)
{
    VkResult result = vkResetCommandBuffer(gpu->command, 0);
    if (result != VK_SUCCESS)
        return fail(error, "vkResetCommandBuffer", result);
    result = vkResetFences(gpu->device, 1, &gpu->fence);
    if (result != VK_SUCCESS)
        return fail(error, "vkResetFences", result);
    VkDescriptorBufferInfo infos[] = {
        {input->buffer, 0, input_range}, {output->buffer, 0, output_range},
    };
    VkWriteDescriptorSet writes[2] = {0};
    for (guint i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = gpu->descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(gpu->device, 2, writes, 0, NULL);
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkBeginCommandBuffer(gpu->command, &begin);
    if (result != VK_SUCCESS)
        return fail(error, "vkBeginCommandBuffer", result);
    buffer_barriers(gpu, input, output, TRUE);
    if (params->width != params->input_width || params->height != params->input_height) {
        amly_gpu_scaler_record(gpu->scaler, gpu->command, input->buffer, input_range,
            output->buffer, output_range, params->width, params->height,
            params->input_stride, params->output_stride_words);
    } else {
        vkCmdBindPipeline(gpu->command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu->pipeline);
        vkCmdBindDescriptorSets(gpu->command, VK_PIPELINE_BIND_POINT_COMPUTE,
            gpu->pipeline_layout, 0, 1, &gpu->descriptor_set, 0, NULL);
        vkCmdPushConstants(gpu->command, gpu->pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(*params), params);
        vkCmdDispatch(gpu->command, (params->width / 2 + 63) / 64, params->height / 2, 1);
    }
    buffer_barriers(gpu, input, output, FALSE);
    result = vkEndCommandBuffer(gpu->command);
    return result == VK_SUCCESS || fail(error, "vkEndCommandBuffer", result);
}

gboolean amly_gpu_convert(AmlyGpu *gpu, int input_fd, gsize input_size,
                         int output_fd, gsize output_size, guint width,
                         guint height, guint input_stride,
                         guint output_stride_bytes, GError **error)
{
    return amly_gpu_convert_scaled(gpu, input_fd, input_size, output_fd, output_size,
        width, height, width, height, input_stride, output_stride_bytes, error);
}

gboolean amly_gpu_convert_scaled(AmlyGpu *gpu, int input_fd, gsize input_size,
                         int output_fd, gsize output_size,
                         guint input_width, guint input_height,
                         guint width, guint height, guint input_stride,
                         guint output_stride_bytes, GError **error)
{
    if (!gpu || gpu->failed)
        return invalid(error, "AMLY GPU instance is unavailable after a device error");
    if (input_fd < 0 || output_fd < 0 || input_fd == output_fd ||
        !width || !height || (width & 1) || (height & 1) ||
        !input_width || !input_height || (input_width & 1) || (input_height & 1) ||
        width > input_width || height > input_height ||
        (guint64)input_stride < (guint64)input_width * 5 / 2 ||
        (guint64)output_stride_bytes < (guint64)width * 2 ||
        output_stride_bytes % 16)
        return invalid(error, "AMLY conversion requires distinct DMA fds, even geometry and valid pitches");
    guint64 input_range = ((guint64)input_stride * input_height + 3) & ~(guint64)3;
    guint64 output_luma_size = (guint64)output_stride_bytes * height;
    if (input_range > G_MAXUINT32 || output_luma_size > G_MAXUINT32)
        return invalid(error, "AMLY shader byte offsets exceed 32-bit addressing");
    guint64 output_range = output_luma_size * 3 / 2;
    const VkPhysicalDeviceLimits *limits = &gpu->properties.limits;
    if (input_range > G_MAXUINT32 || output_range > G_MAXUINT32 ||
        input_range > input_size || output_range > output_size ||
        input_range > limits->maxStorageBufferRange ||
        output_range > limits->maxStorageBufferRange ||
        ((guint64)width / 2 + 63) / 64 > limits->maxComputeWorkGroupCount[0] ||
        height / 2 > limits->maxComputeWorkGroupCount[1])
        return invalid(error, "AMLY geometry exceeds DMA capacities or Vulkan compute limits");

    ImportedBuffer input = {0}, output = {0};
    gboolean ok = FALSE;
    if (width != input_width || height != input_height) {
        if (gpu->scaler && (gpu->scale_input_width != input_width || gpu->scale_input_height != input_height)) {
            amly_gpu_scaler_free(gpu->scaler);
            gpu->scaler = NULL;
        }
        if (!gpu->scaler) {
            gpu->scaler = amly_gpu_scaler_new(gpu->physical, gpu->device, input_width, input_height, error);
            if (!gpu->scaler)
                return FALSE;
            gpu->scale_input_width = input_width;
            gpu->scale_input_height = input_height;
        }
    }
    gint64 t0 = gpu->timing ? g_get_monotonic_time() : 0;
    gint64 t1 = t0, t2 = t0, t3 = t0;
    if (!import_buffer(gpu, input_fd, input_size, input_range, &input, error) ||
        !import_buffer(gpu, output_fd, output_size, output_range, &output, error))
        goto cleanup;
    if (gpu->timing)
        t1 = g_get_monotonic_time();
    ConvertParams params = {width, height, input_stride, output_stride_bytes / 4,
                            input_width, input_height};
    if (!record_commands(gpu, &input, &output, input_range, output_range,
                           &params, error))
        goto cleanup;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &gpu->command,
    };
    VkResult result = vkQueueSubmit(gpu->queue, 1, &submit, gpu->fence);
    if (result != VK_SUCCESS) {
        gpu->failed = TRUE;
        fail(error, "vkQueueSubmit", result);
        // Submission failure may be a device loss; drain before releasing fds.
        vkDeviceWaitIdle(gpu->device);
        goto cleanup;
    }
    if (gpu->timing)
        t2 = g_get_monotonic_time();
    result = vkWaitForFences(gpu->device, 1, &gpu->fence, VK_TRUE, GPU_FENCE_TIMEOUT_NS);
    if (gpu->timing)
        t3 = g_get_monotonic_time();
    if (result != VK_SUCCESS) {
        gpu->failed = TRUE;
        fail(error, "vkWaitForFences", result);
        // A timeout is not completion: never return an input still used by GPU.
        vkDeviceWaitIdle(gpu->device);
        goto cleanup;
    }
    ok = TRUE;

cleanup:
    release_import(gpu, &input);
    release_import(gpu, &output);
    // r44p0 can defer releasing imported DMA-buf references after the fence.
    // Flush them before the caller returns its capture frame to the driver.
    VkResult idle = vkDeviceWaitIdle(gpu->device);
    if (idle != VK_SUCCESS) {
        gpu->failed = TRUE;
        if (ok)
            fail(error, "vkDeviceWaitIdle(import release)", idle);
        ok = FALSE;
    }
    if (ok && gpu->timing) {
        gint64 t4 = g_get_monotonic_time();
        gpu->import_us += t1 - t0;
        gpu->dispatch_us += t2 - t1;
        gpu->wait_us += t3 - t2;
        gpu->free_us += t4 - t3;
        gpu->frames++;
        if (gpu->frames % 120 == 0)
            g_printerr("amly-gpu frames=%" G_GUINT64_FORMAT
                " import_ms=%.3f dispatch_ms=%.3f wait_ms=%.3f free_idle_ms=%.3f\n",
                gpu->frames, gpu->import_us / gpu->frames / 1000.0,
                gpu->dispatch_us / gpu->frames / 1000.0,
                gpu->wait_us / gpu->frames / 1000.0,
                gpu->free_us / gpu->frames / 1000.0);
    }
    return ok;
}

void amly_gpu_free(AmlyGpu *gpu)
{
    if (!gpu)
        return;
    if (gpu->device) {
        vkDeviceWaitIdle(gpu->device);
        amly_gpu_scaler_free(gpu->scaler);
        if (gpu->fence)
            vkDestroyFence(gpu->device, gpu->fence, NULL);
        if (gpu->command_pool)
            vkDestroyCommandPool(gpu->device, gpu->command_pool, NULL);
        if (gpu->descriptor_pool)
            vkDestroyDescriptorPool(gpu->device, gpu->descriptor_pool, NULL);
        if (gpu->pipeline)
            vkDestroyPipeline(gpu->device, gpu->pipeline, NULL);
        if (gpu->pipeline_layout)
            vkDestroyPipelineLayout(gpu->device, gpu->pipeline_layout, NULL);
        if (gpu->descriptor_layout)
            vkDestroyDescriptorSetLayout(gpu->device, gpu->descriptor_layout, NULL);
        vkDestroyDevice(gpu->device, NULL);
    }
    if (gpu->instance)
        vkDestroyInstance(gpu->instance, NULL);
    g_free(gpu);
}
