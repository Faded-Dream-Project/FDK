/*
 * vk_present.c — the Vulkan present lane (the GLX/EGL lanes' third
 * sibling; 2026-11-11).
 *
 * Design contract (mirrors the GL lanes' posture, stated once here
 * and never contradicted below):
 *
 *  - CPU rasterization unchanged: the window's staging buffer (the
 *    same XRGB8888 little-endian layout the GL lanes upload) is the
 *    frame's source of truth. The lane replaces ONLY present()'s
 *    "pixels -> compositor" half.
 *  - ZERO new build dependencies: the loader is dlopen'd
 *    (libvulkan.so.1), every entry point resolved by name, every
 *    structure and constant hand-declared from the spec's ABI —
 *    every size and offset below pinned against Khronos'
 *    vulkan_core.h by scripts/vk-probe.c (the one-off compile that
 *    printed the layout table this file was written from).
 *  - SOFTWARE-ICD POLICY (the GL lanes' llvmpipe rule, restated for
 *    the Vulkan equivalent): lavapipe and SwiftShader stay refused
 *    by default — the lane is for HARDWARE. FDK_GL_FORCE=1 overrides
 *    (the rigs and the smoke apps ride that knob), FDK_NO_GL opts
 *    the whole GPU stack out, exactly as for GL.
 *  - FAILURE = DOWNGRADE, never a broken frame: any lane error
 *    returns failure to the caller's existing downgrade path (the
 *    same salvage contract the GL lanes live under).
 *
 * The present's shape, per frame:
 *   vkAcquireNextImageKHR(sem_ready)
 *   record into the per-frame command buffer:
 *     image barrier UNDEFINED -> TRANSFER_DST_OPTIMAL
 *     vkCmdCopyBufferToImage the damage rects (one whole-image copy
 *       on full damage — the fast path every real frame takes)
 *     image barrier TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR
 *   vkQueueSubmit(sem_ready -> fence), vkWaitForFences(fence)
 *     (the staging buffer is rewritten by the NEXT CPU paint; an
 *     in-flight copy must complete before those bytes may move)
 *   vkQueuePresentKHR(sem_done)
 * The staging buffer is HOST_VISIBLE|HOST_COHERENT with a persistent
 * mapping — no per-frame map/unmap. FIFO present (the spec's always-
 * supported mode) keeps damage semantics honest. The X byte of every
 * staging pixel is ZERO by format; compositeAlpha OPAQUE means the
 * compositor never reads it (the GL lanes' "alpha 1.0" note,
 * restated for Vulkan).
 *
 * Not part of the public API — never installed.
 */

#include "vk_present.h"

#define FDK_LOG_TAG "render"
#include "../core/log_internal.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The hand-declared Vulkan ABI subset.
 * Layout table (scripts/vk-probe.c, vulkan_core.h 1.4.309):
 *   VkPhysicalDeviceProperties 824 bytes: apiVersion@0 driver@4
 *     vendor@8 device@12 deviceType@16 deviceName@20[256]
 *   VkQueueFamilyProperties 24 bytes: queueFlags@0
 *   VkPhysicalDeviceMemoryProperties 520 bytes: typeCount@0
 *     memoryTypes[32]@4 (VkMemoryType 8: propertyFlags@0 heapIndex@4)
 *     heapCount@260
 *   VkMemoryRequirements 24 bytes: size@0 alignment@8 memoryTypeBits@16
 *   VkSurfaceCapabilitiesKHR 52 bytes: minImageCount@0 maxImageCount@4
 *     currentExtent@8 minImageExtent@16 maxImageExtent@24
 *     maxImageArrayLayers@32 supportedTransforms@36 currentTransform@40
 *     supportedCompositeAlpha@44 supportedUsageFlags@48
 *   VkImageSubresourceRange 20: aspectMask@0 baseMipLevel@4
 *     levelCount@8 baseArrayLayer@12 layerCount@16
 *   VkBufferImageCopy 56: bufferOffset@0 bufferRowLength@8
 *     bufferImageHeight@12 imageSubresource@16 (offset@32 extent@44)
 *   VkImageMemoryBarrier 72: ... image@40 subresourceRange@48
 *   VkCommandBufferBeginInfo 32 (SIMULTANEOUS_USE not set: 24 + pad)
 */

typedef uint32_t VkFlags_;
typedef uint32_t VkBool32_;
typedef uint64_t VkDeviceSize_;

/* Handles (pointer-sized per the spec's dispatch model). */
typedef struct VkInstance_T *VkInstance_;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice_;
typedef struct VkDevice_T *VkDevice_;
typedef struct VkQueue_T *VkQueue_;
typedef struct VkCommandPool_T *VkCommandPool_;
typedef struct VkCommandBuffer_T *VkCommandBuffer_;
typedef struct VkSemaphore_T *VkSemaphore_;
typedef struct VkFence_T *VkFence_;
typedef struct VkSurfaceKHR_T *VkSurfaceKHR_;
typedef struct VkSwapchainKHR_T *VkSwapchainKHR_;
typedef struct VkBuffer_T *VkBuffer_;
typedef struct VkImage_T *VkImage_;
typedef struct VkDeviceMemory_T *VkDeviceMemory_;
#define VK_NULL_H NULL
#define VK_WHOLE_SZ ((VkDeviceSize_)~0ull)

typedef int32_t VkResult_;
#define VK_OK 0
#define VK_SUBOPTIMAL 1000001003
#define VK_ERR_OUT_OF_DATE ((int32_t)-1000001004)
#define VK_ERR_SURFACE_LOST ((int32_t)-1000000001)

#define VK_API_VER_1_0 0u
#define VK_QUEUE_GRAPHICS 0x00000001u
#define VK_IMG_USAGE_TRANSFER_DST 0x00000002u
#define VK_BUF_USAGE_TRANSFER_SRC 0x00000001u
#define VK_MEM_DEVICE_LOCAL 0x00000001u
#define VK_MEM_HOST_VISIBLE 0x00000002u
#define VK_MEM_HOST_COHERENT 0x00000004u
#define VK_IMG_ASPECT_COLOR 0x00000001u
#define VK_ACCESS_TRANSFER_WRITE 0x00001000u
#define VK_ACCESS_MEMORY_READ 0x00000800u
#define VK_STAGE_TOP 0x00000001u
#define VK_STAGE_TRANSFER 0x00001000u
#define VK_STAGE_BOTTOM 0x00000400u
#define VK_POOL_RESET_CMDBUF 0x00000002u
#define VK_LAYOUT_UNDEFINED 0
#define VK_LAYOUT_TRANSFER_DST 7
#define VK_LAYOUT_PRESENT 1000001002
#define VK_FORMAT_B8G8R8A8_UNORM 44
#define VK_COLORSPACE_SRGB_NONLINEAR 0
#define VK_PRESENT_MODE_FIFO 2
#define VK_SHARING_EXCLUSIVE 0
#define VK_TRANSFORM_IDENTITY 0x00000001u
#define VK_COMPOSITE_OPAQUE 0x00000001u
#define VK_QFAMILY_IGNORED 0xFFFFFFFFu

/* Structures: verified layouts (see the table above). */
typedef struct {
    int32_t sType;        /* VkStructureType */
    const void *pNext;
} VkCodaHead_; /* the shared sType/pNext prefix */

typedef struct {
    int32_t sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} VkAppInfo_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
    const VkAppInfo_ *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstanceCI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
    void *dpy;    /* Display* */
    uintptr_t win; /* Window */
} VkXlibSurfaceCI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
    void *display; /* struct wl_display* */
    void *surface; /* struct wl_surface* */
} VkWaylandSurfaceCI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
} VkDeviceQueueCI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCI_ *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
    VkSurfaceKHR_ surface;
    uint32_t minImageCount;
    int32_t imageFormat;
    int32_t imageColorSpace;
    struct { uint32_t width, height; } imageExtent;
    uint32_t imageArrayLayers;
    uint32_t imageUsage;
    uint32_t imageSharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    uint32_t preTransform;
    uint32_t compositeAlpha;
    uint32_t presentMode;
    VkBool32_ clipped;
    VkSwapchainKHR_ oldSwapchain;
} VkSwapchainCI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
    uint32_t queueFamilyIndex;
} VkCommandPoolCI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkCommandPool_ commandPool;
    uint32_t level;
    uint32_t commandBufferCount;
} VkCommandBufferAI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
    VkDeviceSize_ size;
    uint32_t usage;
    uint32_t sharingMode;
} VkBufferCI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkDeviceSize_ allocationSize;
    uint32_t memoryTypeIndex;
} VkMemoryAI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
} VkSemaphoreCI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
} VkFenceCI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ flags;
    const void *pInheritanceInfo;
} VkCommandBufferBI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore_ *pWaitSemaphores;
    const VkFlags_ *pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer_ *pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const VkSemaphore_ *pSignalSemaphores;
} VkSubmitI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore_ *pWaitSemaphores;
    uint32_t swapchainCount;
    const VkSwapchainKHR_ *pSwapchains;
    const uint32_t *pImageIndices;
    void *pResults;
} VkPresentI_;

typedef struct {
    int32_t sType;
    const void *pNext;
    VkFlags_ srcAccessMask;
    VkFlags_ dstAccessMask;
    uint32_t oldLayout;
    uint32_t newLayout;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
    VkImage_ image;
    struct {
        uint32_t aspectMask, baseMipLevel, levelCount, baseArrayLayer,
            layerCount;
    } subresourceRange;
} VkImageBarrier_;

typedef struct {
    VkDeviceSize_ bufferOffset;
    uint32_t bufferRowLength;
    uint32_t bufferImageHeight;
    struct {
        uint32_t aspectMask, mipLevel, baseArrayLayer, layerCount;
    } imageSubresource;
    struct { int32_t x, y, z; } imageOffset;
    struct { uint32_t width, height, depth; } imageExtent;
} VkBufferImageCopy_;

typedef struct {
    uint32_t minImageCount;
    uint32_t maxImageCount;
    struct { uint32_t width, height; } currentExtent;
    struct { uint32_t width, height; } minImageExtent;
    struct { uint32_t width, height; } maxImageExtent;
    uint32_t maxImageArrayLayers;
    VkFlags_ supportedTransforms;
    uint32_t currentTransform;
    VkFlags_ supportedCompositeAlpha;
    VkFlags_ supportedUsageFlags;
} VkSurfaceCaps_;

typedef struct {
    uint32_t memoryTypeCount;
    struct { VkFlags_ propertyFlags; uint32_t heapIndex; } memoryTypes[32];
    uint32_t memoryHeapCount;
    struct { VkDeviceSize_ size; VkFlags_ flags; } memoryHeaps[16];
} VkMemProps_;

typedef struct {
    VkDeviceSize_ size;
    VkDeviceSize_ alignment;
    uint32_t memoryTypeBits;
} VkMemReqs_;

/* ------------------------------------------------------------------ */
/* Entry points (dlsym'd; POSIX void* -> function via the dereference
 * idiom, as in the GL lanes). */

struct fdk_vk_fns {
    VkResult_ (*vkCreateInstance)(const VkInstanceCI_ *, const void *,
                                  VkInstance_ *);
    void (*vkDestroyInstance)(VkInstance_, const void *);
    VkResult_ (*vkEnumeratePhysicalDevices)(VkInstance_, uint32_t *,
                                            VkPhysicalDevice_ *);
    void (*vkGetPhysicalDeviceProperties)(VkPhysicalDevice_, void *);
    void (*vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice_,
                                                     uint32_t *, void *);
    VkResult_ (*vkCreateXlibSurfaceKHR)(VkInstance_, const VkXlibSurfaceCI_ *,
                                        const void *, VkSurfaceKHR_ *);
    VkResult_ (*vkCreateWaylandSurfaceKHR)(VkInstance_,
                                           const VkWaylandSurfaceCI_ *,
                                           const void *, VkSurfaceKHR_ *);
    void (*vkDestroySurfaceKHR)(VkInstance_, VkSurfaceKHR_, const void *);
    VkResult_ (*vkGetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice_,
                                                      uint32_t,
                                                      VkSurfaceKHR_,
                                                      VkBool32_ *);
    VkResult_ (*vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(
        VkPhysicalDevice_, VkSurfaceKHR_, VkSurfaceCaps_ *);
    void (*vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice_,
                                                VkMemProps_ *);
    VkResult_ (*vkCreateDevice)(VkPhysicalDevice_, const VkDeviceCI_ *,
                                const void *, VkDevice_ *);
    void (*vkDestroyDevice)(VkDevice_, const void *);
    void (*vkGetDeviceQueue)(VkDevice_, uint32_t, uint32_t, VkQueue_ *);
    VkResult_ (*vkCreateSwapchainKHR)(VkDevice_, const VkSwapchainCI_ *,
                                      const void *, VkSwapchainKHR_ *);
    void (*vkDestroySwapchainKHR)(VkDevice_, VkSwapchainKHR_, const void *);
    VkResult_ (*vkGetSwapchainImagesKHR)(VkDevice_, VkSwapchainKHR_,
                                         uint32_t *, VkImage_ *);
    VkResult_ (*vkAcquireNextImageKHR)(VkDevice_, VkSwapchainKHR_,
                                       uint64_t, VkSemaphore_, VkFence_,
                                       uint32_t *);
    VkResult_ (*vkQueuePresentKHR)(VkQueue_, const VkPresentI_ *);
    VkResult_ (*vkCreateCommandPool)(VkDevice_, const VkCommandPoolCI_ *,
                                     const void *, VkCommandPool_ *);
    void (*vkDestroyCommandPool)(VkDevice_, VkCommandPool_, const void *);
    VkResult_ (*vkAllocateCommandBuffers)(VkDevice_,
                                          const VkCommandBufferAI_ *,
                                          VkCommandBuffer_ *);
    void (*vkFreeCommandBuffers)(VkDevice_, VkCommandPool_, uint32_t,
                                 const VkCommandBuffer_ *);
    VkResult_ (*vkCreateBuffer)(VkDevice_, const VkBufferCI_ *,
                                const void *, VkBuffer_ *);
    void (*vkDestroyBuffer)(VkDevice_, VkBuffer_, const void *);
    void (*vkGetBufferMemoryRequirements)(VkDevice_, VkBuffer_,
                                          VkMemReqs_ *);
    VkResult_ (*vkAllocateMemory)(VkDevice_, const VkMemoryAI_ *,
                                  const void *, VkDeviceMemory_ *);
    void (*vkFreeMemory)(VkDevice_, VkDeviceMemory_, const void *);
    VkResult_ (*vkMapMemory)(VkDevice_, VkDeviceMemory_, VkDeviceSize_,
                             VkDeviceSize_, VkFlags_, void **);
    void (*vkUnmapMemory)(VkDevice_, VkDeviceMemory_);
    VkResult_ (*vkBindBufferMemory)(VkDevice_, VkBuffer_, VkDeviceMemory_,
                                    VkDeviceSize_);
    VkResult_ (*vkCreateSemaphore)(VkDevice_, const VkSemaphoreCI_ *,
                                   const void *, VkSemaphore_ *);
    void (*vkDestroySemaphore)(VkDevice_, VkSemaphore_, const void *);
    VkResult_ (*vkCreateFence)(VkDevice_, const VkFenceCI_ *, const void *,
                               VkFence_ *);
    void (*vkDestroyFence)(VkDevice_, VkFence_, const void *);
    VkResult_ (*vkResetFences)(VkDevice_, uint32_t, const VkFence_ *);
    VkResult_ (*vkWaitForFences)(VkDevice_, uint32_t, const VkFence_ *,
                                 VkBool32_, uint64_t);
    VkResult_ (*vkBeginCommandBuffer)(VkCommandBuffer_,
                                      const VkCommandBufferBI_ *);
    VkResult_ (*vkEndCommandBuffer)(VkCommandBuffer_);
    VkResult_ (*vkResetCommandBuffer)(VkCommandBuffer_, VkFlags_);
    void (*vkCmdPipelineBarrier)(VkCommandBuffer_, VkFlags_, VkFlags_,
                                 VkFlags_, uint32_t, const void *, uint32_t,
                                 const void *, uint32_t,
                                 const VkImageBarrier_ *);
    void (*vkCmdCopyBufferToImage)(VkCommandBuffer_, VkBuffer_, VkImage_,
                                   uint32_t, uint32_t,
                                   const VkBufferImageCopy_ *);
    VkResult_ (*vkQueueSubmit)(VkQueue_, uint32_t, const VkSubmitI_ *,
                               VkFence_);
    void (*vkQueueWaitIdle)(VkQueue_);
    VkResult_ (*vkDeviceWaitIdle)(VkDevice_);
};

/* ------------------------------------------------------------------ */
/* The lane                                                              */

struct fdk_vk_lane {
    void *lib;
    struct fdk_vk_fns fn;
    int is_x11; /* platform chosen at load: X11 or Wayland */
    VkInstance_ instance;
    VkPhysicalDevice_ pdev;
    VkDevice_ device;
    VkQueue_ queue;
    uint32_t queue_family;
    VkCommandPool_ pool;
    char device_name[256];
    int software;
    int destroyed; /* the connection teardown fired */
};

struct fdk_vk_window {
    struct fdk_vk_lane *lane;
    VkSurfaceKHR_ surface;
    VkSwapchainKHR_ swapchain;
    VkImage_ *images;
    uint32_t image_count;
    VkSemaphore_ sem_ready;
    VkSemaphore_ sem_done;
    VkFence_ fence;
    VkCommandBuffer_ cmd;
    VkBuffer_ staging_buf;
    VkDeviceMemory_ staging_mem;
    void *staging_map;
    VkDeviceSize_ staging_size;
    fdk_u32 *staging; /* == staging_map, typed */
    fdk_i32 w, h;
    int dead;
    int presented_once;
};

static struct fdk_vk_lane *g_vk_lane = NULL;

/* POSIX dlsym returns void*; ISO C (pedantic) forbids direct
 * object->function conversion — the dereference idiom. */
#define VK_STORE(dst, sym) (*(void **)&(dst) = (sym))

static int vk_software_icd(const char *name) {
    if (name == NULL) {
        return 0;
    }
    /* The GL lanes' llvmpipe rule, restated for Vulkan ICDs. */
    return strstr(name, "llvmpipe") != NULL ||
           strstr(name, "lavapipe") != NULL ||
           strstr(name, "SwiftShader") != NULL ||
           strstr(name, "swiftshader") != NULL;
}

struct fdk_vk_lane *fdk_vk_lane_try_load(fdk_platform_connection *conn,
                                         const char *platform) {
    (void)conn;
    if (g_vk_lane != NULL) {
        /* One lane per process; the platform must match (an X11
         * session and a Wayland session cannot share an instance's
         * platform extension choice). */
        int want_x11 = strcmp(platform, "x11") == 0;
        if (want_x11 == g_vk_lane->is_x11 && !g_vk_lane->destroyed) {
            return g_vk_lane;
        }
        return NULL;
    }
    if (getenv("FDK_NO_GL") != NULL) {
        /* The GPU-stack opt-out applies to the third lane too. */
        return NULL;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) {
        FDK_DEBUG("libvulkan.so.1 not loadable — Vulkan lane off (%s)",
                  dlerror() != NULL ? dlerror() : "unknown");
        return NULL;
    }

    struct fdk_vk_lane *lane = calloc(1, sizeof(*lane));
    if (lane == NULL) {
        dlclose(lib);
        return NULL;
    }
    lane->lib = lib;
    lane->is_x11 = strcmp(platform, "x11") == 0;

#define VK_NEED(field, name)                                          \
    do {                                                              \
        void *sym_ = dlsym(lib, name);                                \
        if (sym_ == NULL) {                                           \
            FDK_DEBUG("%s unresolvable — Vulkan lane off", name);     \
            goto fail;                                                \
        }                                                             \
        VK_STORE(lane->fn.field, sym_);                               \
    } while (0)
    /* vkCreateXlibSurfaceKHR / vkCreateWaylandSurfaceKHR are both
     * resolved (whichever the platform needs); a missing one only
     * matters for THAT platform, checked at the surface step. */

    VK_NEED(vkCreateInstance, "vkCreateInstance");
    VK_NEED(vkDestroyInstance, "vkDestroyInstance");
    VK_NEED(vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    VK_NEED(vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    VK_NEED(vkGetPhysicalDeviceQueueFamilyProperties,
            "vkGetPhysicalDeviceQueueFamilyProperties");
    VK_NEED(vkDestroySurfaceKHR, "vkDestroySurfaceKHR");
    VK_NEED(vkGetPhysicalDeviceSurfaceSupportKHR,
            "vkGetPhysicalDeviceSurfaceSupportKHR");
    VK_NEED(vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VK_NEED(vkGetPhysicalDeviceMemoryProperties,
            "vkGetPhysicalDeviceMemoryProperties");
    VK_NEED(vkCreateDevice, "vkCreateDevice");
    VK_NEED(vkDestroyDevice, "vkDestroyDevice");
    VK_NEED(vkGetDeviceQueue, "vkGetDeviceQueue");
    VK_NEED(vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
    VK_NEED(vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
    VK_NEED(vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
    VK_NEED(vkAcquireNextImageKHR, "vkAcquireNextImageKHR");
    VK_NEED(vkQueuePresentKHR, "vkQueuePresentKHR");
    VK_NEED(vkCreateCommandPool, "vkCreateCommandPool");
    VK_NEED(vkDestroyCommandPool, "vkDestroyCommandPool");
    VK_NEED(vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    VK_NEED(vkFreeCommandBuffers, "vkFreeCommandBuffers");
    VK_NEED(vkCreateBuffer, "vkCreateBuffer");
    VK_NEED(vkDestroyBuffer, "vkDestroyBuffer");
    VK_NEED(vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    VK_NEED(vkAllocateMemory, "vkAllocateMemory");
    VK_NEED(vkFreeMemory, "vkFreeMemory");
    VK_NEED(vkMapMemory, "vkMapMemory");
    VK_NEED(vkUnmapMemory, "vkUnmapMemory");
    VK_NEED(vkBindBufferMemory, "vkBindBufferMemory");
    VK_NEED(vkCreateSemaphore, "vkCreateSemaphore");
    VK_NEED(vkDestroySemaphore, "vkDestroySemaphore");
    VK_NEED(vkCreateFence, "vkCreateFence");
    VK_NEED(vkDestroyFence, "vkDestroyFence");
    VK_NEED(vkResetFences, "vkResetFences");
    VK_NEED(vkWaitForFences, "vkWaitForFences");
    VK_NEED(vkBeginCommandBuffer, "vkBeginCommandBuffer");
    VK_NEED(vkEndCommandBuffer, "vkEndCommandBuffer");
    VK_NEED(vkResetCommandBuffer, "vkResetCommandBuffer");
    VK_NEED(vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
    VK_NEED(vkCmdCopyBufferToImage, "vkCmdCopyBufferToImage");
    VK_NEED(vkQueueSubmit, "vkQueueSubmit");
    VK_NEED(vkQueueWaitIdle, "vkQueueWaitIdle");
    VK_NEED(vkDeviceWaitIdle, "vkDeviceWaitIdle");
    if (lane->is_x11) {
        VK_NEED(vkCreateXlibSurfaceKHR, "vkCreateXlibSurfaceKHR");
    } else {
        VK_NEED(vkCreateWaylandSurfaceKHR, "vkCreateWaylandSurfaceKHR");
    }
#undef VK_NEED

    /* Instance: VK_KHR_surface + the platform extension. */
    const char *exts[2];
    uint32_t nexts = 0;
    exts[nexts++] = "VK_KHR_surface";
    exts[nexts++] =
        lane->is_x11 ? "VK_KHR_xlib_surface" : "VK_KHR_wayland_surface";
    VkAppInfo_ app;
    memset(&app, 0, sizeof(app));
    app.sType = 0 /* APPLICATION_INFO */;
    app.pApplicationName = "fdk";
    app.applicationVersion = VK_API_VER_1_0;
    app.pEngineName = "fdk-vk-lane";
    app.engineVersion = VK_API_VER_1_0;
    app.apiVersion = VK_API_VER_1_0;
    VkInstanceCI_ ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = 1 /* INSTANCE_CREATE_INFO */;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = nexts;
    ci.ppEnabledExtensionNames = exts;
    VkResult_ r = lane->fn.vkCreateInstance(&ci, NULL, &lane->instance);
    if (r != VK_OK) {
        FDK_DEBUG("vkCreateInstance failed (%d) — Vulkan lane off", (int)r);
        goto fail;
    }

    /* Physical device + the graphics queue family. */
    uint32_t ndev = 0;
    if (lane->fn.vkEnumeratePhysicalDevices(lane->instance, &ndev, NULL) !=
            VK_OK ||
        ndev == 0) {
        FDK_DEBUG("no Vulkan physical devices — Vulkan lane off");
        goto fail;
    }
    VkPhysicalDevice_ *devs = malloc(sizeof(VkPhysicalDevice_) * ndev);
    if (devs == NULL ||
        lane->fn.vkEnumeratePhysicalDevices(lane->instance, &ndev, devs) !=
            VK_OK) {
        free(devs);
        goto fail;
    }
    lane->pdev = devs[0];
    free(devs);

    /* Properties: read through the 824-byte spec layout; only the
     * head and the name are interpreted. */
    {
        unsigned char props[824];
        memset(props, 0, sizeof(props));
        lane->fn.vkGetPhysicalDeviceProperties(lane->pdev, props);
        uint32_t api = *(const uint32_t *)(const void *)(props + 0);
        const char *name = (const char *)(props + 20);
        (void)snprintf(lane->device_name, sizeof(lane->device_name), "%s",
                       name);
        lane->software = vk_software_icd(name);
        if (api == 0) {
            FDK_DEBUG("Vulkan device \"%s\" pre-1.0 — lane off", name);
            goto fail;
        }
        if (lane->software && getenv("FDK_GL_FORCE") == NULL &&
            getenv("FDK_GL_VULKAN") == NULL) {
            FDK_INFO("Vulkan ICD is software (\"%s\") — staying off the "
                     "Vulkan lane (FDK_GL_FORCE=1 overrides)",
                     name);
            goto fail;
        }
    }

    /* Queue family: the first with GRAPHICS (a graphics-capable
     * family is required to exist on every device). */
    {
        uint32_t nq = 0;
        lane->fn.vkGetPhysicalDeviceQueueFamilyProperties(lane->pdev, &nq,
                                                          NULL);
        if (nq == 0) {
            goto fail;
        }
        unsigned char *qprops = calloc((size_t)nq, 24);
        if (qprops == NULL) {
            goto fail;
        }
        lane->fn.vkGetPhysicalDeviceQueueFamilyProperties(
            lane->pdev, &nq, (void *)qprops);
        uint32_t chosen = VK_QFAMILY_IGNORED;
        for (uint32_t i = 0; i < nq; i++) {
            VkFlags_ flags =
                *(const VkFlags_ *)(const void *)(qprops + (size_t)i * 24);
            if ((flags & VK_QUEUE_GRAPHICS) != 0) {
                chosen = i;
                break;
            }
        }
        free(qprops);
        if (chosen == VK_QFAMILY_IGNORED) {
            goto fail;
        }
        lane->queue_family = chosen;
    }

    /* Device: VK_KHR_swapchain on the chosen family. */
    {
        const char *dev_exts[1] = { "VK_KHR_swapchain" };
        float prio = 1.0f;
        VkDeviceQueueCI_ qci;
        memset(&qci, 0, sizeof(qci));
        qci.sType = 2 /* DEVICE_QUEUE_CREATE_INFO */;
        qci.queueFamilyIndex = lane->queue_family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCI_ dci;
        memset(&dci, 0, sizeof(dci));
        dci.sType = 3 /* DEVICE_CREATE_INFO */;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = dev_exts;
        if (lane->fn.vkCreateDevice(lane->pdev, &dci, NULL, &lane->device) !=
            VK_OK) {
            FDK_DEBUG("vkCreateDevice failed — Vulkan lane off");
            goto fail;
        }
        lane->fn.vkGetDeviceQueue(lane->device, lane->queue_family, 0,
                                  &lane->queue);
    }

    /* Command pool: per-lane, resettable command buffers. */
    {
        VkCommandPoolCI_ pci;
        memset(&pci, 0, sizeof(pci));
        pci.sType = 39 /* COMMAND_POOL_CREATE_INFO */;
        pci.flags = VK_POOL_RESET_CMDBUF;
        pci.queueFamilyIndex = lane->queue_family;
        if (lane->fn.vkCreateCommandPool(lane->device, &pci, NULL,
                                         &lane->pool) != VK_OK) {
            goto fail;
        }
    }

    g_vk_lane = lane;
    FDK_INFO("Vulkan present lane active (device \"%s\"%s)",
             lane->device_name,
             lane->software ? ", software ICD — forced" : "");
    return lane;

fail:
    if (lane->device != VK_NULL_H) {
        lane->fn.vkDestroyDevice(lane->device, NULL);
    }
    if (lane->instance != VK_NULL_H) {
        lane->fn.vkDestroyInstance(lane->instance, NULL);
    }
    free(lane);
    dlclose(lib);
    return NULL;
}

const char *fdk_vk_lane_device(const struct fdk_vk_lane *lane) {
    if (lane == NULL || lane->device_name[0] == '\0') {
        return "<unknown>";
    }
    return lane->device_name;
}

/* ---- per-window ------------------------------------------------------ */

static void vk_win_teardown_swapchain(struct fdk_vk_window *vwin) {
    if (vwin->swapchain != VK_NULL_H) {
        vwin->lane->fn.vkDestroySwapchainKHR(vwin->lane->device,
                                             vwin->swapchain, NULL);
        vwin->swapchain = VK_NULL_H;
    }
    free(vwin->images);
    vwin->images = NULL;
    vwin->image_count = 0;
}

static int vk_win_build_swapchain(struct fdk_vk_window *vwin, fdk_i32 w,
                                  fdk_i32 h) {
    struct fdk_vk_lane *lane = vwin->lane;
    VkSurfaceCaps_ caps;
    memset(&caps, 0, sizeof(caps));
    VkResult_ r = lane->fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        lane->pdev, vwin->surface, &caps);
    if (r != VK_OK) {
        FDK_WARN("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (%d) — "
                 "Vulkan lane refusing this window",
                 (int)r);
        return -1;
    }
    /* FIFO is ALWAYS supported (the spec's guarantee) and keeps
     * damage semantics honest (no mailbox tearing). */
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && images > caps.maxImageCount) {
        images = caps.maxImageCount;
    }
    if (images < caps.minImageCount) {
        images = caps.minImageCount;
    }
    /* Extent: the caps' current size when the platform has one
     * (X11 always does); the 0xFFFFFFFF sentinel means "you choose"
     * (some Wayland compositors, pre-first-commit) — clamp. */
    uint32_t sw = (uint32_t)w, sh = (uint32_t)h;
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        sw = caps.currentExtent.width;
        sh = caps.currentExtent.height;
    } else {
        if (sw < caps.minImageExtent.width) sw = caps.minImageExtent.width;
        if (sw > caps.maxImageExtent.width) sw = caps.maxImageExtent.width;
        if (sh < caps.minImageExtent.height) sh = caps.minImageExtent.height;
        if (sh > caps.maxImageExtent.height) sh = caps.maxImageExtent.height;
    }
    if (sw == 0 || sh == 0) {
        return -1; /* pre-configure zero size: retry at the next
                    * acquisition (the EGL lane's zero-size gate) */
    }
    VkSwapchainCI_ sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = 1000001000 /* SWAPCHAIN_CREATE_INFO_KHR */;
    sci.surface = vwin->surface;
    sci.minImageCount = images;
    sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sci.imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR;
    sci.imageExtent.width = sw;
    sci.imageExtent.height = sh;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMG_USAGE_TRANSFER_DST;
    sci.imageSharingMode = VK_SHARING_EXCLUSIVE;
    sci.preTransform = VK_TRANSFORM_IDENTITY;
    sci.compositeAlpha = VK_COMPOSITE_OPAQUE;
    sci.presentMode = VK_PRESENT_MODE_FIFO;
    sci.clipped = 1;
    sci.oldSwapchain = vwin->swapchain;
    VkSwapchainKHR_ fresh = VK_NULL_H;
    r = lane->fn.vkCreateSwapchainKHR(lane->device, &sci, NULL, &fresh);
    if (r != VK_OK) {
        FDK_WARN("vkCreateSwapchainKHR failed (%d, %ux%u) — Vulkan lane "
                 "refusing this window",
                 (int)r, sw, sh);
        return -1;
    }
    /* Old swapchain dies AFTER the new one exists (the spec's
     * retirement rule — in-flight frames retire into the new one). */
    if (vwin->swapchain != VK_NULL_H) {
        lane->fn.vkDeviceWaitIdle(lane->device);
        lane->fn.vkDestroySwapchainKHR(lane->device, vwin->swapchain, NULL);
    }
    vwin->swapchain = fresh;
    vwin->w = (fdk_i32)sw;
    vwin->h = (fdk_i32)sh;
    uint32_t n = 0;
    if (lane->fn.vkGetSwapchainImagesKHR(lane->device, vwin->swapchain, &n,
                                         NULL) != VK_OK ||
        n == 0) {
        return -1;
    }
    free(vwin->images);
    vwin->images = malloc(sizeof(VkImage_) * n);
    if (vwin->images == NULL ||
        lane->fn.vkGetSwapchainImagesKHR(lane->device, vwin->swapchain, &n,
                                         vwin->images) != VK_OK) {
        return -1;
    }
    vwin->image_count = n;
    return 0;
}

struct fdk_vk_window *fdk_vk_window_try_init(struct fdk_vk_lane *lane,
                                             void *native_a, void *native_b,
                                             fdk_i32 w, fdk_i32 h) {
    if (lane == NULL || w < 1 || h < 1) {
        return NULL;
    }
    struct fdk_vk_window *vwin = calloc(1, sizeof(*vwin));
    if (vwin == NULL) {
        return NULL;
    }
    vwin->lane = lane;

    /* The surface: the LANE's platform decides the create call (an
     * X11 lane passes (Display*, Window); a Wayland lane passes
     * (wl_display*, wl_surface*)). */
    VkResult_ r;
    if (lane->is_x11) {
        VkXlibSurfaceCI_ xi;
        memset(&xi, 0, sizeof(xi));
        xi.sType = 1000004000 /* XLIB_SURFACE_CREATE_INFO_KHR */;
        xi.dpy = native_a;
        xi.win = (uintptr_t)native_b;
        r = lane->fn.vkCreateXlibSurfaceKHR(lane->instance, &xi, NULL,
                                            &vwin->surface);
    } else {
        VkWaylandSurfaceCI_ wi;
        memset(&wi, 0, sizeof(wi));
        wi.sType = 1000006000 /* WAYLAND_SURFACE_CREATE_INFO_KHR */;
        wi.display = native_a;
        wi.surface = native_b;
        r = lane->fn.vkCreateWaylandSurfaceKHR(lane->instance, &wi, NULL,
                                               &vwin->surface);
    }
    if (r != VK_OK) {
        FDK_DEBUG("Vulkan surface creation failed (%d) — refusing this "
                  "window",
                  (int)r);
        free(vwin);
        return NULL;
    }

    /* Presentation support for THIS surface on THIS queue family. */
    VkBool32_ supported = 0;
    if (lane->fn.vkGetPhysicalDeviceSurfaceSupportKHR(
            lane->pdev, lane->queue_family, vwin->surface, &supported) !=
            VK_OK ||
        !supported) {
        FDK_DEBUG("queue family cannot present to this surface — Vulkan "
                  "lane refusing this window");
        lane->fn.vkDestroySurfaceKHR(lane->instance, vwin->surface, NULL);
        free(vwin);
        return NULL;
    }

    if (vk_win_build_swapchain(vwin, w, h) != 0) {
        lane->fn.vkDestroySurfaceKHR(lane->instance, vwin->surface, NULL);
        free(vwin);
        return NULL;
    }

    VkSemaphoreCI_ semi;
    memset(&semi, 0, sizeof(semi));
    semi.sType = 9 /* SEMAPHORE_CREATE_INFO */;
    VkFenceCI_ fi;
    memset(&fi, 0, sizeof(fi));
    fi.sType = 11 /* FENCE_CREATE_INFO */;
    if (lane->fn.vkCreateSemaphore(lane->device, &semi, NULL,
                                   &vwin->sem_ready) != VK_OK ||
        lane->fn.vkCreateSemaphore(lane->device, &semi, NULL,
                                   &vwin->sem_done) != VK_OK ||
        lane->fn.vkCreateFence(lane->device, &fi, NULL, &vwin->fence) !=
            VK_OK) {
        goto fail;
    }
    VkCommandBufferAI_ cai;
    memset(&cai, 0, sizeof(cai));
    cai.sType = 40 /* COMMAND_BUFFER_ALLOCATE_INFO */;
    cai.commandPool = lane->pool;
    cai.level = 0; /* PRIMARY */
    cai.commandBufferCount = 1;
    if (lane->fn.vkAllocateCommandBuffers(lane->device, &cai, &vwin->cmd) !=
        VK_OK) {
        goto fail;
    }

    FDK_INFO("Vulkan window lane up (%dx%d, %u swapchain images)", vwin->w,
             vwin->h, vwin->image_count);
    return vwin;

fail:
    fdk_vk_window_destroy(lane, vwin);
    return NULL;
}

static int vk_win_ensure_staging(struct fdk_vk_window *vwin, fdk_i32 w,
                                 fdk_i32 h) {
    struct fdk_vk_lane *lane = vwin->lane;
    VkDeviceSize_ need = (VkDeviceSize_)w * (VkDeviceSize_)h * 4u;
    if (vwin->staging_map != NULL && vwin->staging_size >= need) {
        return 0; /* big enough (the swapchain's extent governs w/h) */
    }
    /* Grow: destroy the old pair, mint a bigger one. */
    if (vwin->staging_buf != VK_NULL_H) {
        lane->fn.vkDeviceWaitIdle(lane->device);
        lane->fn.vkDestroyBuffer(lane->device, vwin->staging_buf, NULL);
        vwin->staging_buf = VK_NULL_H;
    }
    if (vwin->staging_mem != VK_NULL_H) {
        if (vwin->staging_map != NULL) {
            lane->fn.vkUnmapMemory(lane->device, vwin->staging_mem);
            vwin->staging_map = NULL;
            vwin->staging = NULL;
        }
        lane->fn.vkFreeMemory(lane->device, vwin->staging_mem, NULL);
        vwin->staging_mem = VK_NULL_H;
    }
    VkBufferCI_ bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = 12 /* BUFFER_CREATE_INFO */;
    bi.size = need;
    bi.usage = VK_BUF_USAGE_TRANSFER_SRC;
    if (lane->fn.vkCreateBuffer(lane->device, &bi, NULL,
                                &vwin->staging_buf) != VK_OK) {
        return -1;
    }
    VkMemReqs_ reqs;
    memset(&reqs, 0, sizeof(reqs));
    lane->fn.vkGetBufferMemoryRequirements(lane->device, vwin->staging_buf,
                                           &reqs);
    /* HOST_VISIBLE|HOST_COHERENT among the buffer's allowed types. */
    VkMemProps_ mp;
    memset(&mp, 0, sizeof(mp));
    lane->fn.vkGetPhysicalDeviceMemoryProperties(lane->pdev, &mp);
    int32_t mtype = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount && i < 32; i++) {
        if ((reqs.memoryTypeBits & (1u << i)) == 0) {
            continue;
        }
        VkFlags_ want = VK_MEM_HOST_VISIBLE | VK_MEM_HOST_COHERENT;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) {
            mtype = (int32_t)i;
            break;
        }
    }
    if (mtype < 0) {
        FDK_WARN("Vulkan staging: no HOST_VISIBLE|HOST_COHERENT memory "
                 "type — refusing this window");
        return -1;
    }
    VkMemoryAI_ mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = 5 /* MEMORY_ALLOCATE_INFO */;
    mai.allocationSize = reqs.size > need ? reqs.size : need;
    mai.memoryTypeIndex = (uint32_t)mtype;
    if (lane->fn.vkAllocateMemory(lane->device, &mai, NULL,
                                  &vwin->staging_mem) != VK_OK) {
        return -1;
    }
    if (lane->fn.vkBindBufferMemory(lane->device, vwin->staging_buf,
                                    vwin->staging_mem, 0) != VK_OK) {
        return -1;
    }
    if (lane->fn.vkMapMemory(lane->device, vwin->staging_mem, 0,
                             VK_WHOLE_SZ, 0, &vwin->staging_map) != VK_OK) {
        return -1;
    }
    vwin->staging_size = need;
    vwin->staging = (fdk_u32 *)vwin->staging_map;
    return 0;
}

fdk_u32 *fdk_vk_window_staging(struct fdk_vk_window *vwin, fdk_i32 w,
                               fdk_i32 h) {
    if (vwin == NULL || vwin->dead || w < 1 || h < 1) {
        return NULL;
    }
    if (vwin->w != w || vwin->h != h) {
        if (vk_win_build_swapchain(vwin, w, h) != 0) {
            return NULL;
        }
    }
    if (vk_win_ensure_staging(vwin, w, h) != 0) {
        return NULL;
    }
    return vwin->staging;
}

int fdk_vk_window_render_path(const struct fdk_vk_window *vwin) {
    return vwin != NULL && !vwin->dead &&
           vwin->swapchain != VK_NULL_H;
}

int fdk_vk_window_present(struct fdk_vk_window *vwin,
                          const fdk_platform_damage *damage, fdk_i32 w,
                          fdk_i32 h) {
    if (vwin == NULL || vwin->dead || vwin->staging == NULL ||
        damage == NULL) {
        return -1;
    }
    struct fdk_vk_lane *lane = vwin->lane;
    if (w != vwin->w || h != vwin->h) {
        /* Resize (or the caps' extent moved): rebuild before the
         * acquire. OUT_OF_DATE below handles the driver-initiated
         * variant. */
        if (vk_win_build_swapchain(vwin, w, h) != 0 ||
            vk_win_ensure_staging(vwin, w, h) != 0) {
            vwin->dead = 1;
            return -1;
        }
    }
    uint32_t idx = 0;
    VkResult_ r = lane->fn.vkAcquireNextImageKHR(
        lane->device, vwin->swapchain, UINT64_MAX, vwin->sem_ready,
        VK_NULL_H, &idx);
    if (r == VK_ERR_OUT_OF_DATE) {
        if (vk_win_build_swapchain(vwin, w, h) != 0) {
            vwin->dead = 1;
            return -1;
        }
        r = lane->fn.vkAcquireNextImageKHR(lane->device, vwin->swapchain,
                                           UINT64_MAX, vwin->sem_ready,
                                           VK_NULL_H, &idx);
    }
    if (r != VK_OK && r != VK_SUBOPTIMAL) {
        FDK_WARN("vkAcquireNextImageKHR failed (%d) — Vulkan lane "
                 "downgrade for this window",
                 (int)r);
        vwin->dead = 1;
        return -1;
    }
    if (idx >= vwin->image_count) {
        vwin->dead = 1; /* driver lied; never index out of bounds */
        return -1;
    }

    /* Record: barrier -> copies -> barrier. */
    VkCommandBufferBI_ bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = 42 /* COMMAND_BUFFER_BEGIN_INFO */;
    if (lane->fn.vkBeginCommandBuffer(vwin->cmd, &bi) != VK_OK) {
        vwin->dead = 1;
        return -1;
    }
    VkImage_ image = vwin->images[idx];
    VkImageBarrier_ to_dst;
    memset(&to_dst, 0, sizeof(to_dst));
    to_dst.sType = 45 /* IMAGE_MEMORY_BARRIER */;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE;
    to_dst.oldLayout = VK_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_LAYOUT_TRANSFER_DST;
    to_dst.srcQueueFamilyIndex = VK_QFAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QFAMILY_IGNORED;
    to_dst.image = image;
    to_dst.subresourceRange.aspectMask = VK_IMG_ASPECT_COLOR;
    to_dst.subresourceRange.levelCount = 1;
    to_dst.subresourceRange.layerCount = 1;
    lane->fn.vkCmdPipelineBarrier(vwin->cmd, VK_STAGE_TOP, VK_STAGE_TRANSFER,
                                  0, 0, NULL, 0, NULL, 1, &to_dst);

    /* The copies: full damage = one whole-image copy; partial = one
     * region per rect (bounded by the damage struct's own 64). */
    VkBufferImageCopy_ copies[64];
    uint32_t ncopies = 0;
    if (damage->full || damage->count == 0) {
        memset(&copies[0], 0, sizeof(copies[0]));
        copies[0].bufferOffset = 0;
        copies[0].bufferRowLength = 0; /* tightly packed */
        copies[0].imageSubresource.aspectMask = VK_IMG_ASPECT_COLOR;
        copies[0].imageSubresource.layerCount = 1;
        copies[0].imageExtent.width = (uint32_t)w;
        copies[0].imageExtent.height = (uint32_t)h;
        copies[0].imageExtent.depth = 1;
        ncopies = 1;
    } else {
        uint32_t cnt = damage->count > 64 ? 64u : (uint32_t)damage->count;
        for (uint32_t i = 0; i < cnt; i++) {
            const fdk_rect *rc = &damage->rects[i];
            memset(&copies[i], 0, sizeof(copies[i]));
            copies[i].bufferOffset = (VkDeviceSize_)rc->y *
                                         (VkDeviceSize_)w * 4u +
                                     (VkDeviceSize_)rc->x * 4u;
            copies[i].bufferRowLength = (uint32_t)w;
            copies[i].imageSubresource.aspectMask = VK_IMG_ASPECT_COLOR;
            copies[i].imageSubresource.layerCount = 1;
            copies[i].imageOffset.x = rc->x;
            copies[i].imageOffset.y = rc->y;
            copies[i].imageExtent.width = (uint32_t)rc->width;
            copies[i].imageExtent.height = (uint32_t)rc->height;
            copies[i].imageExtent.depth = 1;
        }
        ncopies = cnt;
    }
    lane->fn.vkCmdCopyBufferToImage(vwin->cmd, vwin->staging_buf, image,
                                    VK_LAYOUT_TRANSFER_DST, ncopies, copies);

    VkImageBarrier_ to_present;
    memset(&to_present, 0, sizeof(to_present));
    to_present.sType = 45 /* IMAGE_MEMORY_BARRIER */;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE;
    to_present.dstAccessMask = VK_ACCESS_MEMORY_READ;
    to_present.oldLayout = VK_LAYOUT_TRANSFER_DST;
    to_present.newLayout = VK_LAYOUT_PRESENT;
    to_present.srcQueueFamilyIndex = VK_QFAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QFAMILY_IGNORED;
    to_present.image = image;
    to_present.subresourceRange.aspectMask = VK_IMG_ASPECT_COLOR;
    to_present.subresourceRange.levelCount = 1;
    to_present.subresourceRange.layerCount = 1;
    lane->fn.vkCmdPipelineBarrier(vwin->cmd, VK_STAGE_TRANSFER,
                                  VK_STAGE_BOTTOM, 0, 0, NULL, 0, NULL, 1,
                                  &to_present);
    if (lane->fn.vkEndCommandBuffer(vwin->cmd) != VK_OK) {
        vwin->dead = 1;
        return -1;
    }

    VkFlags_ wait_stage = VK_STAGE_TRANSFER;
    VkSubmitI_ si;
    memset(&si, 0, sizeof(si));
    si.sType = 4 /* SUBMIT_INFO */;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &vwin->sem_ready;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &vwin->cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &vwin->sem_done;
    if (lane->fn.vkResetFences(lane->device, 1, &vwin->fence) != VK_OK) {
        vwin->dead = 1;
        return -1;
    }
    r = lane->fn.vkQueueSubmit(lane->queue, 1, &si, vwin->fence);
    if (r != VK_OK) {
        FDK_WARN("vkQueueSubmit failed (%d) — Vulkan lane downgrade",
                 (int)r);
        vwin->dead = 1;
        return -1;
    }
    /* The fence is waited HERE, not at the next present: the CPU
     * rewrites the staging buffer during the NEXT paint, and that
     * paint may begin the moment present() returns — an in-flight
     * copy must complete first. (FIFO presents at vsync; this wait
     * is the present's own pacing, not an added stall.) */
    if (lane->fn.vkWaitForFences(lane->device, 1, &vwin->fence, 1,
                                UINT64_MAX) != VK_OK) {
        vwin->dead = 1;
        return -1;
    }

    VkPresentI_ pi;
    memset(&pi, 0, sizeof(pi));
    pi.sType = 1000001001 /* PRESENT_INFO_KHR */;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vwin->sem_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &vwin->swapchain;
    pi.pImageIndices = &idx;
    r = lane->fn.vkQueuePresentKHR(lane->queue, &pi);
    vwin->presented_once = 1;
    if (r == VK_ERR_OUT_OF_DATE || r == VK_SUBOPTIMAL) {
        /* Rebuild now so the next acquire is valid. */
        if (vk_win_build_swapchain(vwin, w, h) != 0) {
            vwin->dead = 1;
            return -1;
        }
    } else if (r != VK_OK) {
        FDK_WARN("vkQueuePresentKHR failed (%d) — Vulkan lane downgrade",
                 (int)r);
        vwin->dead = 1;
        return -1;
    }
    return 0;
}

void fdk_vk_window_destroy(struct fdk_vk_lane *lane,
                           struct fdk_vk_window *vwin) {
    if (vwin == NULL) {
        return;
    }
    if (lane != NULL) {
        lane->fn.vkDeviceWaitIdle(lane->device);
        if (vwin->staging_buf != VK_NULL_H) {
            lane->fn.vkDestroyBuffer(lane->device, vwin->staging_buf, NULL);
        }
        if (vwin->staging_mem != VK_NULL_H) {
            if (vwin->staging_map != NULL) {
                lane->fn.vkUnmapMemory(lane->device, vwin->staging_mem);
            }
            lane->fn.vkFreeMemory(lane->device, vwin->staging_mem, NULL);
        }
        if (vwin->cmd != VK_NULL_H) {
            lane->fn.vkFreeCommandBuffers(lane->device, lane->pool, 1,
                                          &vwin->cmd);
        }
        if (vwin->fence != VK_NULL_H) {
            lane->fn.vkDestroyFence(lane->device, vwin->fence, NULL);
        }
        if (vwin->sem_ready != VK_NULL_H) {
            lane->fn.vkDestroySemaphore(lane->device, vwin->sem_ready, NULL);
        }
        if (vwin->sem_done != VK_NULL_H) {
            lane->fn.vkDestroySemaphore(lane->device, vwin->sem_done, NULL);
        }
        vk_win_teardown_swapchain(vwin);
        if (vwin->surface != VK_NULL_H) {
            lane->fn.vkDestroySurfaceKHR(lane->instance, vwin->surface,
                                         NULL);
        }
    }
    free(vwin);
}

void fdk_vk_lane_destroy(struct fdk_vk_lane *lane) {
    if (lane == NULL) {
        return;
    }
    if (g_vk_lane == lane) {
        g_vk_lane = NULL;
    }
    if (lane->pool != VK_NULL_H) {
        lane->fn.vkDestroyCommandPool(lane->device, lane->pool, NULL);
    }
    lane->fn.vkDestroyDevice(lane->device, NULL);
    lane->fn.vkDestroyInstance(lane->instance, NULL);
    dlclose(lane->lib);
    lane->destroyed = 1;
    free(lane);
}
