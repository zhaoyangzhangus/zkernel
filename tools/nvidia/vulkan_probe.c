/* Guest-only NVIDIA Vulkan transfer test: no display server or software fallback. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define WORDS 1024U
#define PATTERN UINT32_C(0x5070cafe)
static int video_caps(VkInstance instance, VkPhysicalDevice gpu) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL) != VK_SUCCESS) return 1;
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) return 1;
    if (vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, extensions) != VK_SUCCESS) {
        free(extensions); return 1;
    }
    int decode = 0;
    for (uint32_t i=0; i<count; ++i) {
        if (strstr(extensions[i].extensionName, "video"))
            printf("VULKAN_VIDEO_EXTENSION %s\n", extensions[i].extensionName);
        if (!strcmp(extensions[i].extensionName, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME)) decode=1;
    }
    free(extensions);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 query =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties2");
    if (!query) return 1;
    query(gpu, &count, NULL);
    VkQueueFamilyProperties2 *families = calloc(count, sizeof(*families));
    VkQueueFamilyVideoPropertiesKHR *video = calloc(count, sizeof(*video));
    if (!families || !video) {free(families); free(video); return 1;}
    for (uint32_t i=0; i<count; ++i) {
        video[i].sType=VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        families[i].sType=VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        if (decode) families[i].pNext=&video[i];
    }
    query(gpu, &count, families);
    int usable=0;
    for (uint32_t i=0; i<count; ++i) {
        printf("VULKAN_VIDEO_QUEUE family=%u flags=0x%x codecs=0x%x count=%u\n",
               i, families[i].queueFamilyProperties.queueFlags, video[i].videoCodecOperations,
               families[i].queueFamilyProperties.queueCount);
        if (decode && (families[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)
            && families[i].queueFamilyProperties.queueCount && video[i].videoCodecOperations) usable=1;
    }
    free(families); free(video);
    printf("VULKAN_VIDEO_CAPS decode_queue=%d\n", usable);
    return usable ? 0 : 1;
}
int vulkan_render_probe(VkPhysicalDevice, VkDevice, VkQueue, uint32_t, const char *);
int vulkan_shared_support(VkPhysicalDevice);
int vulkan_shared_probe(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,int);
int vulkan_shared_render_probe(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,int);
int vulkan_shared_lifetime_probe(VkPhysicalDevice,VkDevice,VkQueue,uint32_t);
#define CHECK(call) do { VkResult result = (call); if (result != VK_SUCCESS) { \
    fprintf(stderr, "VULKAN_FAIL %s result=%d\n", #call, (int)result); \
    goto cleanup; } } while (0)

int main(int argc, char **argv) {
    int log_output = 0;
    int render = 0;
    int shared = 0, hold_window = 0;
    int lifetime = 0;
    int video = 0;
    const char *log_path = NULL, *image_path = NULL;
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i],"--render") && !render) render=1;
        else if (!strcmp(argv[i],"--video-caps") && !video) video=1;
        else if (!strcmp(argv[i],"--shared-check") && !shared) shared=1;
        else if (!strcmp(argv[i],"--shared-window") && !shared) shared=hold_window=1;
        else if (!strcmp(argv[i],"--shared-lifetime") && !shared) shared=lifetime=1;
        else if (!strcmp(argv[i],"--log") && !log_path && i+1<argc) log_path=argv[++i];
        else if (!strcmp(argv[i],"--image") && !image_path && i+1<argc) image_path=argv[++i];
        else return 2;
    }
    if (image_path && !render) return 2;
    if (shared && image_path) return 2;
    if (lifetime && render) return 2;
    if (video && (render || shared || image_path)) return 2;
    /* The glibc compatibility runtime does not consume native shell fd maps.
     * An explicit log also makes real GPU verification independent of GDB. */
    if (log_path) {
        int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return 2;
        if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
            if (fd > STDERR_FILENO) close(fd);
            return 2;
        }
        if (fd > STDERR_FILENO) close(fd);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        log_output = 1;
    }
    fprintf(stderr, "VULKAN_START\n");
    if (setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
               "/usr/apps/vulkan-probe/egl/10_nvidia.json", 1) != 0) return 1;
    if (setenv("VK_DRIVER_FILES", "/usr/apps/vulkan-probe/vulkan/nvidia_icd.json", 0) != 0) {
        perror("VULKAN_FAIL setenv");
        return 1;
    }
    int status = 1;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDevice *physical = NULL;
    VkQueueFamilyProperties *families = NULL;
    void *mapped = NULL;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "GPU transfer probe", .apiVersion = (shared || video) ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&info, NULL, &instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) { fprintf(stderr, "VULKAN_FAIL no physical devices\n"); goto cleanup; }
    physical = calloc(count, sizeof(*physical));
    if (!physical) goto cleanup;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, physical));
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties;
    for (uint32_t i = 0; i < count; ++i) {
        vkGetPhysicalDeviceProperties(physical[i], &properties);
        if (properties.vendorID == 0x10deU &&
            properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            selected = physical[i]; break;
        }
    }
    if (!selected) { fprintf(stderr, "VULKAN_FAIL no NVIDIA discrete GPU\n"); goto cleanup; }
    if (shared && !vulkan_shared_support(selected)) {
        fprintf(stderr,"VULKAN_SHARED_FAIL external_memory_host not supported\n");goto cleanup;
    }
    printf("VULKAN_GPU name=%s vendor=%04x device=%04x api=%u.%u.%u\n",
        properties.deviceName, properties.vendorID, properties.deviceID,
        VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
        VK_VERSION_PATCH(properties.apiVersion));
    if (video) {status=video_caps(instance, selected); goto cleanup;}
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(selected, &family_count, NULL);
    if (!family_count) goto cleanup;
    families = calloc(family_count, sizeof(*families));
    if (!families) goto cleanup;
    vkGetPhysicalDeviceQueueFamilyProperties(selected, &family_count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i)
        if (families[i].queueCount && (!render || (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) &&
            (families[i].queueFlags &
            (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            family = i; break;
        }
    if (family == UINT32_MAX) goto cleanup;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
    const char *external_memory_host = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if(shared) {device_info.enabledExtensionCount=1;device_info.ppEnabledExtensionNames=&external_memory_host;}
    CHECK(vkCreateDevice(selected, &device_info, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    if (shared) {
        status=lifetime ? vulkan_shared_lifetime_probe(selected,device,queue,family) :
               render ? vulkan_shared_render_probe(selected,device,queue,family,hold_window) :
                       vulkan_shared_probe(selected,device,queue,family,hold_window);
        goto cleanup;
    }
    if (render) {
        status = vulkan_render_probe(selected,device,queue,family,image_path);
        goto cleanup;
    }
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = WORDS * sizeof(uint32_t), .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    vkGetPhysicalDeviceMemoryProperties(selected, &memory_properties);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1U << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i; break;
        }
    if (type == UINT32_MAX) goto cleanup;
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(device, &allocation, NULL, &memory));
    CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family};
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(command, &begin));
    vkCmdFillBuffer(command, buffer, 0, WORDS * sizeof(uint32_t), PATTERN);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
    CHECK(vkMapMemory(device, memory, 0, WORDS * sizeof(uint32_t), 0, &mapped));
    for (uint32_t i = 0; i < WORDS; ++i)
        if (((uint32_t *)mapped)[i] != PATTERN) {
            fprintf(stderr, "VULKAN_FAIL readback word=%u\n", i); goto cleanup;
        }
    puts("VULKAN_TRANSFER_PASS words=1024 hardware=NVIDIA");
    status = 0;
cleanup:
    if (device) {
        /* Also drain submissions after a fence timeout before destroying resources. */
        (void)vkDeviceWaitIdle(device);
        if (mapped) vkUnmapMemory(device, memory);
        if (fence) vkDestroyFence(device, fence, NULL);
        if (pool) vkDestroyCommandPool(device, pool, NULL);
        if (buffer) vkDestroyBuffer(device, buffer, NULL);
        if (memory) vkFreeMemory(device, memory, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    free(families);
    free(physical);
    if (log_output && fsync(STDOUT_FILENO) != 0) {
        perror("VULKAN_FAIL log sync");
        status = 1;
    }
    return status;
}
