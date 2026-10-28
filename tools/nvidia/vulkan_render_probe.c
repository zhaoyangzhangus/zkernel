/* 真实图形管线验证；不使用宿主 GPU 或软件后备路径。 */
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "vulkan_triangle_spirv.h"

#define SIDE 64U
#define BYTES (SIDE * SIDE * 4U)
#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
    fprintf(stderr, "VULKAN_RENDER_FAIL %s result=%d\n", #call, (int)r); \
    goto cleanup; } } while (0)

static uint32_t memory_type(VkPhysicalDevice gpu, uint32_t bits,
                            VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1U << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    return UINT32_MAX;
}

/* 像素中心和边缘均可用整数精确表示，且像素中心不会落在边缘上。
 * 验证整幅图像，而不只检查成功标记或单个中心像素。 */
static int validate_surface(const volatile uint8_t *pixels, uint32_t side,
                            int bgra) {
    uint32_t colored = 0;
    for (uint32_t y = 0; y < side; ++y) for (uint32_t x = 0; x < side; ++x) {
        uint32_t X = 2U*x+1U, Y = 2U*y+1U;
        int inside = Y > side/4U && Y < 7U*side/4U &&
                     2U*X > Y+side/4U && 2U*X+Y < 15U*side/4U;
        const volatile uint8_t *p = pixels + (y*side+x)*4U;
        if (p[bgra ? 2 : 0] != (inside ? 255U : 0U) ||
            p[1] != (inside ? 64U : 0U) ||
            p[bgra ? 0 : 2] != 0U || p[3] != 255U) {
            fprintf(stderr, "VULKAN_RENDER_FAIL pixel=%u,%u rgba=%u,%u,%u,%u expected=%s\n",
                    x,y,p[0],p[1],p[2],p[3],inside ? "triangle" : "background");
            return 1;
        }
        colored += (uint32_t)inside;
    }
    printf("VULKAN_RENDER_PIXELS checked=%u triangle=%u\n", side*side,colored);
    return 0;
}

static int validate_pixels(const uint8_t *pixels) {
    return validate_surface(pixels, SIDE, 0);
}

static int save_image(const uint8_t *pixels, const char *path) {
    if (!path) return 0;
    FILE *file = fopen(path,"wb");
    if (!file) return 1;
    int failed = fprintf(file,"P6\n%u %u\n255\n",SIDE,SIDE) < 0;
    for (uint32_t i=0; i<SIDE*SIDE && !failed; ++i)
        failed = fwrite(pixels+i*4U,1,3,file) != 3;
    if (fflush(file) != 0) failed=1;
    if (!failed && fsync(fileno(file)) != 0) failed=1;
    if (fclose(file) != 0) failed=1;
    return failed;
}

static int render_triangle(VkPhysicalDevice gpu, VkDevice device, VkQueue queue,
    uint32_t family, const char *image_path, uint32_t side,
    VkBuffer target, const volatile uint8_t *target_pixels) {
    int status=1;
    VkImage image=VK_NULL_HANDLE;
    VkDeviceMemory image_memory=VK_NULL_HANDLE, buffer_memory=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE;
    VkRenderPass pass=VK_NULL_HANDLE;
    VkFramebuffer framebuffer=VK_NULL_HANDLE;
    VkShaderModule vert=VK_NULL_HANDLE, frag=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkBuffer buffer=target;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    void *mapped=NULL;
    if (!side || side > 4096U || side%8U ||
        (!!target != !!target_pixels) || (target && image_path)) return 1;
    VkDeviceSize bytes=(VkDeviceSize)side*side*4U;
    VkFormat image_format=target ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(gpu,image_format,&format);
    if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) goto cleanup;
    VkImageCreateInfo im={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=image_format,
        .extent={side,side,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(device,&im,NULL,&image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device,image,&req);
    uint32_t type=memory_type(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type==UINT32_MAX) goto cleanup;
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=req.size,.memoryTypeIndex=type};
    CHECK(vkAllocateMemory(device,&alloc,NULL,&image_memory));
    CHECK(vkBindImageMemory(device,image,image_memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=im.format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    CHECK(vkCreateImageView(device,&vi,NULL,&view));
    VkAttachmentDescription attachment={.format=im.format,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&color};
    VkSubpassDependency dependency={.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,
        .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass,
        .dependencyCount=1,.pDependencies=&dependency};
    CHECK(vkCreateRenderPass(device,&rp,NULL,&pass));
    VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=pass,.attachmentCount=1,.pAttachments=&view,.width=side,.height=side,.layers=1};
    CHECK(vkCreateFramebuffer(device,&fb,NULL,&framebuffer));
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(triangle_vert),.pCode=triangle_vert};
    CHECK(vkCreateShaderModule(device,&sm,NULL,&vert));
    sm.codeSize=sizeof(triangle_frag); sm.pCode=triangle_frag;
    CHECK(vkCreateShaderModule(device,&sm,NULL,&frag));
    VkPipelineShaderStageCreateInfo shaders[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vert,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=frag,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,side,side,0,1}; VkRect2D scissor={{0,0},{side,side}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
        VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    CHECK(vkCreatePipelineLayout(device,&li,NULL,&layout));
    VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=shaders,.pVertexInputState=&vertex,.pInputAssemblyState=&assembly,
        .pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,.pColorBlendState=&cb,
        .layout=layout,.renderPass=pass,.subpass=0};
    fprintf(stderr,"VULKAN_RENDER_PIPELINE_BEGIN\n");
    CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipeline));
    if (!target) {
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device,&bi,NULL,&buffer));
    vkGetBufferMemoryRequirements(device,buffer,&req);
    type=memory_type(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type==UINT32_MAX) goto cleanup;
    alloc.allocationSize=req.size; alloc.memoryTypeIndex=type;
    CHECK(vkAllocateMemory(device,&alloc,NULL,&buffer_memory));
    CHECK(vkBindBufferMemory(device,buffer,buffer_memory,0));
    }
    VkCommandPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    CHECK(vkCreateCommandPool(device,&pc,NULL,&pool));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device,&ca,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(command,&begin));
    VkClearValue clear={.color={{0,0,0,1}}};
    VkRenderPassBeginInfo rb={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass=pass,.framebuffer=framebuffer,.renderArea=scissor,.clearValueCount=1,.pClearValues=&clear};
    vkCmdBeginRenderPass(command,&rb,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdDraw(command,3,1,0,0);
    vkCmdEndRenderPass(command);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={side,side,1}};
    vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device,&fc,NULL,&fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    CHECK(vkQueueSubmit(queue,1,&submit,fence));
    CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(5000000000)));
    if (target) {
        if (validate_surface(target_pixels,side,1) != 0) goto cleanup;
    } else {
        CHECK(vkMapMemory(device,buffer_memory,0,bytes,0,&mapped));
        if (validate_pixels(mapped) != 0) goto cleanup;
        if (save_image(mapped,image_path) != 0) {perror("VULKAN_RENDER_FAIL image write");goto cleanup;}
    }
    printf("VULKAN_RENDER_PASS pixels=%u hardware=NVIDIA shared=%d\n",side*side,!!target);
    status=0;
cleanup:
    (void)vkDeviceWaitIdle(device);
    if (mapped) vkUnmapMemory(device,buffer_memory);
    if (fence) vkDestroyFence(device,fence,NULL);
    if (pool) vkDestroyCommandPool(device,pool,NULL);
    if (pipeline) vkDestroyPipeline(device,pipeline,NULL);
    if (layout) vkDestroyPipelineLayout(device,layout,NULL);
    if (vert) vkDestroyShaderModule(device,vert,NULL);
    if (frag) vkDestroyShaderModule(device,frag,NULL);
    if (framebuffer) vkDestroyFramebuffer(device,framebuffer,NULL);
    if (pass) vkDestroyRenderPass(device,pass,NULL);
    if (view) vkDestroyImageView(device,view,NULL);
    if (image) vkDestroyImage(device,image,NULL);
    if (buffer && !target) vkDestroyBuffer(device,buffer,NULL);
    if (image_memory) vkFreeMemory(device,image_memory,NULL);
    if (buffer_memory) vkFreeMemory(device,buffer_memory,NULL);
    return status;
}

int vulkan_render_probe(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
                         uint32_t family,const char *image_path) {
    return render_triangle(gpu,device,queue,family,image_path,SIDE,
                            VK_NULL_HANDLE,NULL);
}

/* 绘制/fence/回读期间，导入的窗口缓冲区始终由调用方持有。
 * 创建图像时选择 B8G8R8A8，因此无需 CPU 通道转换或复制。 */
int vulkan_render_surface(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
    uint32_t family,VkBuffer target,const volatile uint8_t *pixels,uint32_t side) {
    return render_triangle(gpu,device,queue,family,NULL,side,target,pixels);
}
