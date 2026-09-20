/* SPDX-License-Identifier: MIT */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <wlr/util/log.h>
#include "android_dmabuf.h"

struct android_dmabuf_bridge {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceMemoryProperties memory;
    VkCommandPool pool;
    PFN_vkGetMemoryFdPropertiesKHR get_fd_properties;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_properties;
};

static bool has_extension(const VkExtensionProperties *props, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; i++) {
        if (!strcmp(props[i].extensionName, name)) return true;
    }
    return false;
}

static uint32_t memory_type(struct android_dmabuf_bridge *b, uint32_t bits) {
    for (uint32_t i = 0; i < b->memory.memoryTypeCount; i++) {
        if (bits & (1u << i)) return i;
    }
    return UINT32_MAX;
}

void android_dmabuf_bridge_destroy(struct android_dmabuf_bridge *b) {
    if (!b) return;
    if (b->pool) vkDestroyCommandPool(b->device, b->pool, NULL);
    if (b->device) vkDestroyDevice(b->device, NULL);
    if (b->instance) vkDestroyInstance(b->instance, NULL);
    free(b);
}

struct android_dmabuf_bridge *android_dmabuf_bridge_create(void) {
    struct android_dmabuf_bridge *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "wlroots Android buffer bridge", .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app};
    if (vkCreateInstance(&instance, NULL, &b->instance) != VK_SUCCESS) goto fail;
    uint32_t count = 1;
    /* Android's system loader normally exposes one GPU. No driver-name rules. */
    VkResult result = vkEnumeratePhysicalDevices(b->instance, &count, &b->physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) goto fail;
    count = 0;
    if (vkEnumerateDeviceExtensionProperties(b->physical, NULL, &count, NULL) != VK_SUCCESS) goto fail;
    VkExtensionProperties *props = calloc(count, sizeof(*props));
    if (!props) goto fail;
    const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    bool supported = vkEnumerateDeviceExtensionProperties(b->physical, NULL, &count, props) == VK_SUCCESS;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        supported = supported && has_extension(props, count, extensions[i]);
    }
    free(props);
    if (!supported) goto fail;
    const VkPhysicalDeviceExternalBufferInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkExternalBufferProperties external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(b->physical, &buffer, &external);
    VkExternalMemoryFeatureFlags flags = external.externalMemoryProperties.externalMemoryFeatures;
    if (!(flags & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
            (flags & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)) goto fail;
    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(b->physical, &count, NULL);
    VkQueueFamilyProperties *queues = calloc(count, sizeof(*queues));
    if (!queues) goto fail;
    vkGetPhysicalDeviceQueueFamilyProperties(b->physical, &count, queues);
    for (b->family = 0; b->family < count; b->family++) {
        if (queues[b->family].queueCount && (queues[b->family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) break;
    }
    free(queues);
    if (b->family == count) goto fail;
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = b->family, .queueCount = 1, .pQueuePriorities = &priority};
    const VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]),
        .ppEnabledExtensionNames = extensions};
    if (vkCreateDevice(b->physical, &device, NULL, &b->device) != VK_SUCCESS) goto fail;
    b->get_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(b->device, "vkGetMemoryFdPropertiesKHR");
    b->get_ahb_properties = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
        vkGetDeviceProcAddr(b->device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!b->get_fd_properties || !b->get_ahb_properties) goto fail;
    vkGetPhysicalDeviceMemoryProperties(b->physical, &b->memory);
    vkGetDeviceQueue(b->device, b->family, 0, &b->queue);
    const VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = b->family, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT};
    if (vkCreateCommandPool(b->device, &pool, NULL, &b->pool) != VK_SUCCESS) goto fail;
    wlr_log(WLR_INFO, "Android Vulkan bridge: linear DMA-BUF import available");
    return b;
fail:
    android_dmabuf_bridge_destroy(b);
    return NULL;
}

AHardwareBuffer *android_dmabuf_bridge_copy(struct android_dmabuf_bridge *b,
        const struct wlr_dmabuf_attributes *a) {
    if (!b || a->n_planes != 1 || a->modifier != DRM_FORMAT_MOD_LINEAR ||
            a->width <= 0 || a->height <= 0 || a->width > 16384 || a->height > 16384 ||
            a->stride[0] < (uint64_t)a->width * 4 || a->stride[0] % 4 || a->offset[0] % 4 ||
            (a->format != DRM_FORMAT_ABGR8888 && a->format != DRM_FORMAT_XBGR8888 &&
             a->format != DRM_FORMAT_ARGB8888 && a->format != DRM_FORMAT_XRGB8888)) return NULL;
    uint64_t size = (uint64_t)a->offset[0] + (uint64_t)a->stride[0] * (a->height - 1) + a->width * 4;
    off_t allocation_size = lseek(a->fd[0], 0, SEEK_END);
    if (allocation_size < 0 || (uint64_t)allocation_size < size) return NULL;
    /* DMA-BUF poll(POLLIN) waits for implicit writers. No explicit-sync
     * protocol is advertised by this bridge. Do not consume an in-flight image. */
    struct pollfd ready = {.fd = a->fd[0], .events = POLLIN};
    int polled;
    do { polled = poll(&ready, 1, 1000); } while (polled < 0 && errno == EINTR);
    if (polled <= 0 || !(ready.revents & POLLIN) || (ready.revents & (POLLERR | POLLNVAL))) return NULL;
    VkBuffer source = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory source_memory = VK_NULL_HANDLE, image_memory = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    AHardwareBuffer *ahb = NULL;
    int import_fd = -1;
    bool ok = false, submitted = false;
    VkResult result = VK_SUCCESS;
#define CHECK(call) do { result = (call); if (result != VK_SUCCESS) goto done; } while (0)
    const VkExternalMemoryBufferCreateInfo external_buffer = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_buffer, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(b->device, &buffer, NULL, &source));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(b->device, source, &requirements);
    if ((uint64_t)allocation_size < requirements.size) goto done;
    VkMemoryFdPropertiesKHR fd_props = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    CHECK(b->get_fd_properties(b->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, a->fd[0], &fd_props));
    uint32_t type = memory_type(b, requirements.memoryTypeBits & fd_props.memoryTypeBits);
    if (type == UINT32_MAX) goto done;
    import_fd = fcntl(a->fd[0], F_DUPFD_CLOEXEC, 0);
    if (import_fd < 0) goto done;
    const VkImportMemoryFdInfoKHR imported = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = import_fd};
    VkMemoryAllocateInfo memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &imported, .allocationSize = requirements.size, .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(b->device, &memory, NULL, &source_memory));
    import_fd = -1; /* Vulkan owns the duplicate after successful import. */
    CHECK(vkBindBufferMemory(b->device, source, source_memory, 0));
    const AHardwareBuffer_Desc desc = {.width = a->width, .height = a->height, .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT};
    if (AHardwareBuffer_allocate(&desc, &ahb) != 0) goto done;
    VkAndroidHardwareBufferFormatPropertiesANDROID format = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID properties = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &format};
    CHECK(b->get_ahb_properties(b->device, ahb, &properties));
    if (format.format != VK_FORMAT_R8G8B8A8_UNORM) goto done;
    const VkExternalMemoryImageCreateInfo external_image = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    const VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image, .imageType = VK_IMAGE_TYPE_2D, .format = format.format,
        .extent = {a->width, a->height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(b->device, &image_info, NULL, &image));
    const VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image};
    const VkImportAndroidHardwareBufferInfoANDROID imported_ahb = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext = &dedicated, .buffer = ahb};
    memory.pNext = &imported_ahb;
    memory.allocationSize = properties.allocationSize;
    memory.memoryTypeIndex = memory_type(b, properties.memoryTypeBits);
    if (memory.memoryTypeIndex == UINT32_MAX) goto done;
    CHECK(vkAllocateMemory(b->device, &memory, NULL, &image_memory));
    CHECK(vkBindImageMemory(b->device, image, image_memory, 0));
    const VkCommandBufferAllocateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = b->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(b->device, &commands, &cmd));
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cmd, &begin));
    VkBufferMemoryBarrier acquire = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .dstQueueFamilyIndex = b->family,
        .buffer = source, .size = VK_WHOLE_SIZE};
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .dstQueueFamilyIndex = b->family,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 1, &acquire, 1, &barrier);
    const VkBufferImageCopy copy = {.bufferOffset = a->offset[0], .bufferRowLength = a->stride[0] / 4,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {a->width, a->height, 1}};
    vkCmdCopyBufferToImage(cmd, source, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    acquire.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    acquire.dstAccessMask = 0;
    acquire.srcQueueFamilyIndex = b->family;
    acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = b->family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 1, &acquire, 1, &barrier);
    CHECK(vkEndCommandBuffer(cmd));
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(b->device, &fence_info, NULL, &fence));
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(b->queue, 1, &submit, fence));
    submitted = true;
    /* Match the existing synchronous renderer lifetime. Do not release source
     * memory before the transfer completes; async fences are a separate step. */
    CHECK(vkWaitForFences(b->device, 1, &fence, VK_TRUE, UINT64_MAX));
    ok = true;
done:
    if (submitted && !ok && result != VK_ERROR_DEVICE_LOST) vkDeviceWaitIdle(b->device);
    if (!ok) wlr_log(WLR_DEBUG, "Android DMA-BUF copy failed: Vulkan %d", result);
    if (import_fd >= 0) close(import_fd);
    if (fence) vkDestroyFence(b->device, fence, NULL);
    if (cmd) vkFreeCommandBuffers(b->device, b->pool, 1, &cmd);
    if (image) vkDestroyImage(b->device, image, NULL);
    if (image_memory) vkFreeMemory(b->device, image_memory, NULL);
    if (source) vkDestroyBuffer(b->device, source, NULL);
    if (source_memory) vkFreeMemory(b->device, source_memory, NULL);
    if (!ok && ahb) { AHardwareBuffer_release(ahb); ahb = NULL; }
    return ahb;
#undef CHECK
}
