#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <errno.h>
#include <uapi/linux_abi.h>
#include <uapi/window.h>

#define SIDE 256U
#define BYTES (SIDE*SIDE*4U)
#define PIXEL UINT32_C(0xffff4000)
#define CHECK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { \
 fprintf(stderr,"VULKAN_SHARED_FAIL %s result=%d\n",#call,(int)r);goto cleanup;} } while(0)

int vulkan_render_surface(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,VkBuffer,
                           const volatile uint8_t *,uint32_t);
int vulkan_import_lifetime(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,VkBuffer,
                            uint64_t,uint32_t,int *);

int vulkan_shared_support(VkPhysicalDevice gpu) {
    uint32_t count=0;
    VkResult result=vkEnumerateDeviceExtensionProperties(gpu,NULL,&count,NULL);
    if(result!=VK_SUCCESS || count==0 || count>4096) return 0;
    VkExtensionProperties *properties=calloc(count,sizeof(*properties));
    if(!properties)return 0;
    result=vkEnumerateDeviceExtensionProperties(gpu,NULL,&count,properties);
    int supported=0;
    if(result==VK_SUCCESS) for(uint32_t i=0;i<count;++i)
        if(!strcmp(properties[i].extensionName,VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))supported=1;
    free(properties);
    printf("VULKAN_SHARED_CAP external_memory_host=%d\n",supported);
    return supported;
}

/* 呈现预检：由真实 GPU DMA 写入合成器自己的 surface。
 * 生成 surface 时不使用 memcpy、像素转换或 CPU 填充。 */
static int shared_surface(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
                         uint32_t family,int hold_window,int render) {
    int status=1,fd=-1;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    linux_window_create_t window={.x=100,.y=100,.width=SIDE,.height=SIDE,
        .address=UINT64_C(0x08000000),.title="Vulkan GPU shared buffer"};
    if(render)snprintf(window.title,sizeof(window.title),"Vulkan triangle");
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&host};
    vkGetPhysicalDeviceProperties2(gpu,&properties);
    VkDeviceSize alignment=host.minImportedHostPointerAlignment;
    printf("VULKAN_SHARED_ALIGNMENT bytes=%llu\n",(unsigned long long)alignment);
    if(!alignment || (alignment & (alignment-1)) || BYTES%alignment || window.address%alignment) {
        fprintf(stderr,"VULKAN_SHARED_FAIL surface alignment\n");goto cleanup;
    }
    VkPhysicalDeviceExternalBufferInfo external_info={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT | (render==2 ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0),
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkExternalBufferProperties external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(gpu,&external_info,&external);
    printf("VULKAN_SHARED_BUFFER features=%x compatible=%x\n",
           external.externalMemoryProperties.externalMemoryFeatures,
           external.externalMemoryProperties.compatibleHandleTypes);
    if(!(external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        fprintf(stderr,"VULKAN_SHARED_FAIL buffer not importable\n");goto cleanup;
    }
    long created=syscall((long)LINUX_SYS_WINDOW_CREATE,&window);
    if(created<0 || created>INT32_MAX) {perror("VULKAN_SHARED_FAIL window create");goto cleanup;}
    fd=(int)created;
    if(window.address%alignment || window.buffer_size<BYTES || !window.identifier) goto cleanup;
    PFN_vkGetMemoryHostPointerPropertiesEXT get_pointer=(PFN_vkGetMemoryHostPointerPropertiesEXT)
        vkGetDeviceProcAddr(device,"vkGetMemoryHostPointerPropertiesEXT");
    if(!get_pointer)goto cleanup;
    VkMemoryHostPointerPropertiesEXT pointer={.sType=VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    CHECK(get_pointer(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                      (void*)(uintptr_t)window.address,&pointer));
    VkExternalMemoryBufferCreateInfo eb={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&eb,
        .size=BYTES,.usage=external_info.usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device,&bi,NULL,&buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device,buffer,&req);
    printf("VULKAN_SHARED_MEMORY bytes=%llu buffer_types=%x pointer_types=%x\n",
           (unsigned long long)req.size,req.memoryTypeBits,pointer.memoryTypeBits);
    if(req.size>BYTES)goto cleanup;
    VkPhysicalDeviceMemoryProperties types;
    vkGetPhysicalDeviceMemoryProperties(gpu,&types);
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<types.memoryTypeCount;++i)
        if((req.memoryTypeBits & pointer.memoryTypeBits & (1U<<i)) &&
           (types.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {type=i;break;}
    if(type==UINT32_MAX)goto cleanup;
    printf("VULKAN_SHARED_MEMORY_TYPE index=%u flags=%x\n",type,types.memoryTypes[type].propertyFlags);
    VkMemoryDedicatedAllocateInfo dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.buffer=buffer};
    VkImportMemoryHostPointerInfoEXT import={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .pNext=&dedicated,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer=(void*)(uintptr_t)window.address};
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&import,
        .allocationSize=BYTES,.memoryTypeIndex=type};
    fprintf(stderr,"VULKAN_SHARED_IMPORT_BEGIN\n");
    CHECK(vkAllocateMemory(device,&alloc,NULL,&memory));
    CHECK(vkBindBufferMemory(device,buffer,memory,0));
    if(render==2) {
        status=vulkan_import_lifetime(gpu,device,queue,family,buffer,window.address,BYTES,&fd);
        goto cleanup;
    }
    if(render) {
        if(vulkan_render_surface(gpu,device,queue,family,buffer,
            (const volatile uint8_t*)(uintptr_t)window.address,SIDE)!=0)goto cleanup;
    } else {
    VkCommandPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    CHECK(vkCreateCommandPool(device,&pc,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device,&ca,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(command,&begin));
    vkCmdFillBuffer(command,buffer,0,BYTES,PIXEL);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device,&fc,NULL,&fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    CHECK(vkQueueSubmit(queue,1,&submit,fence));
    CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(5000000000)));
    const volatile uint32_t *pixels=(const volatile uint32_t*)(uintptr_t)window.address;
    for(uint32_t i=0;i<SIDE*SIDE;++i) if(pixels[i]!=PIXEL) {
        fprintf(stderr,"VULKAN_SHARED_FAIL pixel=%u value=%x\n",i,pixels[i]);goto cleanup;
    }
    }
    linux_window_command_t present={.operation=LINUX_WINDOW_OPERATION_PRESENT,.width=SIDE,.height=SIDE,
        .stride=SIDE*4,.address=window.address,.length=BYTES};
    if(ioctl(fd,(unsigned long)LINUX_WINDOW_IOCTL_COMMIT,&present)!=0)goto cleanup;
    linux_window_command_t visible={.operation=LINUX_WINDOW_OPERATION_VISIBILITY,.width=1};
    if(ioctl(fd,(unsigned long)LINUX_WINDOW_IOCTL_COMMIT,&visible)!=0)goto cleanup;
    printf("VULKAN_SHARED_PASS pixels=65536 producer=GPU cpu_pixel_copies=0 graphics=%d\n",render);
    (void)fsync(STDOUT_FILENO);
    status=0;
    while(hold_window) {
        struct pollfd pollfd={.fd=fd,.events=POLLIN};
        if(poll(&pollfd,1,250)<0) {if(errno==EINTR)continue;status=1;break;}
        if(pollfd.revents & (POLLERR|POLLHUP|POLLNVAL))break;
        if(pollfd.revents & POLLIN) {
            os_window_event_t event;
            ssize_t n=read(fd,&event,sizeof(event));
            if(n==(ssize_t)sizeof(event) && event.type==OS_WINDOW_EVENT_CLOSE_REQUEST)break;
            if(n<0 && errno!=EAGAIN && errno!=EINTR) {status=1;break;}
        }
    }
cleanup:
    (void)vkDeviceWaitIdle(device);
    if(fence)vkDestroyFence(device,fence,NULL);
    if(pool)vkDestroyCommandPool(device,pool,NULL);
    if(buffer)vkDestroyBuffer(device,buffer,NULL);
    if(memory)vkFreeMemory(device,memory,NULL);
    /* GPU 释放导入对象前，保持共享所有者/映射存活。 */
    if(fd>=0)close(fd);
    return status;
}

int vulkan_shared_probe(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
                         uint32_t family,int hold_window) {
    return shared_surface(gpu,device,queue,family,hold_window,0);
}

int vulkan_shared_render_probe(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
                                uint32_t family,int hold_window) {
    return shared_surface(gpu,device,queue,family,hold_window,1);
}

int vulkan_shared_lifetime_probe(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
                                  uint32_t family) {
    return shared_surface(gpu,device,queue,family,0,2);
}
