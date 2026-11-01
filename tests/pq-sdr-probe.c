/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Offline shader arithmetic only. Host-visible buffers deliberately do not
 * stand in for board DMA imports, cache ownership, or real GPU performance. */
#include <vulkan/vulkan.h>
#include <glib.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define VK_CHECK(expression) do { VkResult vk_status = (expression); if (vk_status != VK_SUCCESS) g_error("%s: %d", #expression, vk_status); } while (0)
typedef struct {
    guint32 width, height, input_stride, output_stride_words, input_width,
        input_height, output_luma_rows, input_luma_rows;
    float white, peak, shoulder;
} Params;
typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; VkDeviceSize capacity; } Buffer;
static Buffer allocate(VkPhysicalDevice physical, VkDevice device, gsize bytes)
{
    Buffer result = {0};
    VkBufferCreateInfo create = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=bytes, .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VK_CHECK(vkCreateBuffer(device, &create, NULL, &result.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    guint type = 0;
    for (; type < properties.memoryTypeCount; type++)
        if ((requirements.memoryTypeBits & (1u << type)) &&
            (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) break;
    g_assert_cmpuint(type, <, properties.memoryTypeCount);
    VkMemoryAllocateInfo allocation = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=requirements.size, .memoryTypeIndex=type};
    VK_CHECK(vkAllocateMemory(device, &allocation, NULL, &result.memory));
    VK_CHECK(vkBindBufferMemory(device, result.buffer, result.memory, 0));
    VK_CHECK(vkMapMemory(device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped));
    result.capacity = requirements.size;
    return result;
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 11);
    Params params = {.width=atoi(argv[2]), .height=atoi(argv[3]), .input_stride=atoi(argv[4]),
        .input_luma_rows=atoi(argv[5]), .output_stride_words=atoi(argv[6])/4,
        .white=atof(argv[7]), .peak=atof(argv[8])};
    params.input_width=params.width; params.input_height=params.height; params.output_luma_rows=params.height;
    double knee=pow((.9+.055)/1.055,2.4), span=params.peak/params.white-knee;
    double slope=1.055/(2.4*pow((.9+.055)/1.055,1.4)), lo=0, hi=1;
    while (.1*hi/log1p(hi*span)<slope) hi*=2;
    for (guint i=0; i<64; i++) {
        double mid=(lo+hi)*.5;
        if (.1*mid/log1p(mid*span)<slope) lo=mid; else hi=mid;
    }
    params.shoulder=(lo+hi)*.5;
    gchar *shader, *input; gsize shader_bytes, input_bytes;
    g_assert_true(g_file_get_contents(argv[1], &shader, &shader_bytes, NULL));
    g_assert_true(g_file_get_contents(argv[9], &input, &input_bytes, NULL));
    gsize output_bytes=(gsize)params.output_stride_words*4*params.height*3/2;
    VkApplicationInfo application = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance_info = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&application};
    VkInstance instance; VK_CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    guint count=0; VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL)); g_assert_cmpuint(count, >, 0);
    VkPhysicalDevice *devices=g_new(VkPhysicalDevice, count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice physical=devices[0]; g_free(devices);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    g_printerr("arithmetic device=%s (no DMA or performance qualification)\n", properties.deviceName);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families=g_new(VkQueueFamilyProperties, count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    guint family=0;
    while (family<count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
    g_assert_cmpuint(family, <, count); g_free(families);
    float priority=1;
    VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info};
    VkDevice device; VK_CHECK(vkCreateDevice(physical,&device_info,NULL,&device));
    VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
    Buffer buffers[]={allocate(physical,device,input_bytes),allocate(physical,device,output_bytes)};
    memcpy(buffers[0].mapped,input,input_bytes); memset(buffers[1].mapped,0xa5,output_bytes);
    VkMappedMemoryRange ranges[2];
    for (guint i=0;i<2;i++) ranges[i]=(VkMappedMemoryRange){.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=buffers[i].memory,.size=VK_WHOLE_SIZE};
    VK_CHECK(vkFlushMappedMemoryRanges(device,2,ranges));
    VkDescriptorSetLayoutBinding bindings[2];
    for (guint i=0;i<2;i++) bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo set_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VkDescriptorSetLayout set_layout; VK_CHECK(vkCreateDescriptorSetLayout(device,&set_info,NULL,&set_layout));
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=sizeof(params)};
    VkPipelineLayoutCreateInfo layout_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    VkPipelineLayout layout; VK_CHECK(vkCreatePipelineLayout(device,&layout_info,NULL,&layout));
    VkShaderModuleCreateInfo shader_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=shader_bytes,.pCode=(guint32 *)shader};
    VkShaderModule module; VK_CHECK(vkCreateShaderModule(device,&shader_info,NULL,&module));
    VkComputePipelineCreateInfo pipeline_info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=layout};
    VkPipeline pipeline; VK_CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_info,NULL,&pipeline));
    VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&pool_size};
    VkDescriptorPool pool; VK_CHECK(vkCreateDescriptorPool(device,&pool_info,NULL,&pool));
    VkDescriptorSetAllocateInfo set_allocate={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set; VK_CHECK(vkAllocateDescriptorSets(device,&set_allocate,&set));
    VkDescriptorBufferInfo buffer_info[2]; VkWriteDescriptorSet writes[2];
    for (guint i=0;i<2;i++) {
        buffer_info[i]=(VkDescriptorBufferInfo){buffers[i].buffer,0,i?output_bytes:input_bytes};
        writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer_info[i]};
    }
    vkUpdateDescriptorSets(device,2,writes,0,NULL);
    VkCommandPoolCreateInfo command_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool commands; VK_CHECK(vkCreateCommandPool(device,&command_info,NULL,&commands));
    VkCommandBufferAllocateInfo command_allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command; VK_CHECK(vkAllocateCommandBuffers(device,&command_allocate,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(command,&begin));
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);
    vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);
    vkCmdDispatch(command,((params.width+3)/4+63)/64,params.height/2,1);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    VK_CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    VkFenceCreateInfo fence_info={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; VK_CHECK(vkCreateFence(device,&fence_info,NULL,&fence));
    VK_CHECK(vkQueueSubmit(queue,1,&submit,fence));
    VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ULL));
    VK_CHECK(vkInvalidateMappedMemoryRanges(device,1,ranges+1));
    g_assert_true(g_file_set_contents(argv[10],buffers[1].mapped,output_bytes,NULL));
    for (guint i=0;i<2;i++) { vkUnmapMemory(device,buffers[i].memory); vkDestroyBuffer(device,buffers[i].buffer,NULL); vkFreeMemory(device,buffers[i].memory,NULL); }
    vkDestroyFence(device,fence,NULL); vkDestroyCommandPool(device,commands,NULL); vkDestroyDescriptorPool(device,pool,NULL);
    vkDestroyPipeline(device,pipeline,NULL); vkDestroyShaderModule(device,module,NULL); vkDestroyPipelineLayout(device,layout,NULL);
    vkDestroyDescriptorSetLayout(device,set_layout,NULL); vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
    g_free(input); g_free(shader);
    return 0;
}
