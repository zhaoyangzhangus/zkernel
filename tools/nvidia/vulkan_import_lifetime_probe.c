/* 导入 surface 的 CPU/窗口所有者消失后，仅通过 GPU 回读验证生命周期。 */
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <uapi/linux_abi.h>

#define PATTERN UINT32_C(0x5070ac1d)
#define CHECK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { \
    fprintf(stderr,"VULKAN_LIFETIME_FAIL %s result=%d\n",#call,(int)r);goto cleanup;} } while(0)

int vulkan_import_lifetime(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
    uint32_t family,VkBuffer imported,uint64_t address,uint32_t bytes,int *fd) {
    if(!imported || !address || !bytes || bytes%4U || !fd || *fd<0)return 1;
    int status=1;
    VkBuffer readback=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    void *mapped=NULL;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device,&bi,NULL,&readback));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device,readback,&req);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(gpu,&properties);
    uint32_t type=UINT32_MAX;
    VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(uint32_t i=0;i<properties.memoryTypeCount;++i)
        if((req.memoryTypeBits & (1U<<i)) &&
           (properties.memoryTypes[i].propertyFlags & flags)==flags) {type=i;break;}
    if(type==UINT32_MAX)goto cleanup;
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=req.size,.memoryTypeIndex=type};
    CHECK(vkAllocateMemory(device,&alloc,NULL,&memory));
    CHECK(vkBindBufferMemory(device,readback,memory,0));
    VkCommandPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    CHECK(vkCreateCommandPool(device,&pc,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device,&ca,&command));
    VkFenceCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device,&fc,NULL,&fence));

    if(munmap((void*)(uintptr_t)address,bytes)!=0)goto cleanup;
    linux_window_command_t remove={.operation=LINUX_WINDOW_OPERATION_CLOSE};
    if(ioctl(*fd,(unsigned long)LINUX_WINDOW_IOCTL_COMMIT,&remove)!=0)goto cleanup;
    int old_fd=*fd;
    *fd=-1; /* Linux close 会消费该描述符，包括延迟报告的错误。 */
    if(close(old_fd)!=0)goto cleanup;
    puts("VULKAN_LIFETIME_DETACHED cpu_mapping=0 window_fd=0");

    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(command,&begin));
    vkCmdFillBuffer(command,imported,0,bytes,PATTERN);
    VkBufferMemoryBarrier source={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=imported,.offset=0,.size=bytes};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,1,&source,0,NULL);
    VkBufferCopy copy={.size=bytes};
    vkCmdCopyBuffer(command,imported,readback,1,&copy);
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&host,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    CHECK(vkQueueSubmit(queue,1,&submit,fence));
    CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(5000000000)));
    CHECK(vkMapMemory(device,memory,0,bytes,0,&mapped));
    for(uint32_t i=0;i<bytes/4U;++i)if(((const uint32_t*)mapped)[i]!=PATTERN) {
        fprintf(stderr,"VULKAN_LIFETIME_FAIL word=%u\n",i);goto cleanup;
    }
    printf("VULKAN_LIFETIME_PASS words=%u gpu_after_unmap=1 gpu_after_close=1\n",bytes/4U);
    status=0;
cleanup:
    (void)vkDeviceWaitIdle(device);
    if(mapped)vkUnmapMemory(device,memory);
    if(fence)vkDestroyFence(device,fence,NULL);
    if(pool)vkDestroyCommandPool(device,pool,NULL);
    if(readback)vkDestroyBuffer(device,readback,NULL);
    if(memory)vkFreeMemory(device,memory,NULL);
    /* 导入的缓冲区/内存归调用方所有，不属于此回读辅助函数。 */
    return status;
}
