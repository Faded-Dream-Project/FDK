/* 12_vulkan_surface.c — a Vulkan swapchain on an FDK extension surface
 * (the EXTENDING.md "Vulkan recipe" made real).
 *
 * The recipe: FDK stays the surface owner; the add-on borrows the
 * NATIVE handles through the documented accessors and drives its own
 * Vulkan loader:
 *
 *   fdk_ext_get_native_display(ctx, &nd)   -> Display* / wl_display*
 *   fdk_ext_surface_native(surface, &h)    -> h.xid / h.pointer (wl_surface*)
 *
 * then creates VkSurfaceKHR with VkXlibSurfaceCreateInfoKHR or
 * VkWaylandSurfaceCreateInfoKHR. Ownership rules (see EXTENDING.md):
 * the add-on destroys ONLY its Vulkan objects (surface included);
 * FDK keeps owning the wl_surface / X Window — destroying the
 * VkSurfaceKHR must not destroy the native surface underneath.
 *
 * What this example does, end to end: a kind surface (X11 plain
 * window / Wayland role-less wl_surface), a Vulkan instance with the
 * right platform extension, the physical device + a graphics-and-
 * present queue, a 1-image FIFO swapchain, acquire + present of one
 * frame, vkQueueWaitIdle, clean teardown of everything.
 *
 * Honest behavior: with no Vulkan ICD (a stripped container) the
 * instance creation fails with VK_ERROR_INCOMPATIBLE_DRIVER and the
 * example prints SKIP and exits 0 — building is the assertion, the
 * run needs a driver. With lavapipe/llvmpipe (mesa-vulkan-drivers)
 * it presents for real, including under Xvfb.
 *
 * Build: cc -Iinclude -I<vulkan headers> examples/12_vulkan_surface.c \
 *          -L<install>/lib -lfdk -lvulkan -lX11 -lwayland-client ...
 * Run:   DISPLAY=... ./12_vulkan_surface   (or WAYLAND_DISPLAY=...)
 */

#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_extend.h"
#include "fdk/fdk_error.h"

#include <X11/Xlib.h>
#include <wayland-client.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xlib.h>
#include <vulkan/vulkan_wayland.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_W 240
#define PANEL_H 160

/* The registry listener (a file-scope fn, the docs' shape). */
static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface,
                            uint32_t version) {
    (void)version;
    if (strcmp(iface, "wl_compositor") == 0) {
        struct wl_compositor **out = data;
        *out = (struct wl_compositor *)wl_registry_bind(
            reg, name, &wl_compositor_interface, 4);
    }
}

/* ---- the kind (the same shapes as 10/11: FDK owns the surface) ---- */

static fdk_result panel_create(fdk_ext_surface *surface,
                               fdk_ext_native_handle *out_native,
                               void *user) {
    (void)user;
    fdk_context *ctx = fdk_ext_surface_context(surface);
    fdk_ext_native_display nd;
    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd))) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (nd.backend == FDK_EXT_BACKEND_X11) {
        Display *dpy = (Display *)nd.display;
        Window win = XCreateSimpleWindow(
            dpy, DefaultRootWindow(dpy), 40, 40, PANEL_W, PANEL_H, 0,
            BlackPixel(dpy, DefaultScreen(dpy)),
            BlackPixel(dpy, DefaultScreen(dpy)));
        if (win == None) {
            return FDK_ERR_UNSUPPORTED;
        }
        out_native->backend = FDK_EXT_BACKEND_X11;
        out_native->xid = (fdk_u64)win;
        return FDK_OK;
    }
    if (nd.backend == FDK_EXT_BACKEND_WAYLAND) {
        struct wl_display *dpy = (struct wl_display *)nd.display;
        struct wl_registry *reg = wl_display_get_registry(dpy);
        struct wl_compositor *comp = NULL;
        struct wl_registry_listener rl = {0};
        rl.global = registry_global;
        wl_registry_add_listener(reg, &rl, &comp);
        wl_display_roundtrip(dpy);
        wl_display_roundtrip(dpy);
        wl_registry_destroy(reg);
        if (comp == NULL) {
            return FDK_ERR_UNSUPPORTED;
        }
        struct wl_surface *surf = wl_compositor_create_surface(comp);
        wl_compositor_destroy(comp); /* the surface keeps its own ref */
        if (surf == NULL) {
            return FDK_ERR_UNSUPPORTED;
        }
        out_native->backend = FDK_EXT_BACKEND_WAYLAND;
        out_native->pointer = surf;
        return FDK_OK;
    }
    return FDK_ERR_UNSUPPORTED;
}

static void panel_destroy(fdk_ext_surface *surface, void *user) {
    (void)surface;
    (void)user;
}

/* ---- Vulkan: the smallest honest swapchain ---- */

static const char *vk_result_str(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "success";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "incompatible driver (no ICD)";
    case VK_ERROR_INITIALIZATION_FAILED: return "initialization failed";
    case VK_ERROR_OUT_OF_DATE_KHR: return "out of date";
    case VK_ERROR_SURFACE_LOST_KHR: return "surface lost";
    default: return "other";
    }
}

int main(void) {
    int rc = 2;
    VkInstance inst = VK_NULL_HANDLE;
    VkSurfaceKHR vsurf = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swap = VK_NULL_HANDLE;
    uint32_t qfam = 0;

    /* ---- FDK: the context and the kind surface ---- */
    fdk_init_options io;
    memset(&io, 0, sizeof io);
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, &io))) {
        printf("[vk] init failed\n");
        return 2;
    }
    fdk_ext_surface_ops ops = {0};
    ops.struct_size = sizeof(ops);
    ops.create = panel_create;
    ops.destroy = panel_destroy;
    fdk_ext_surface_kind *kind = NULL;
    if (!fdk_ok(fdk_ext_register_kind(ctx, "com.fadeddream.vk-surface",
                                      &ops, NULL, &kind))) {
        printf("[vk] kind registration failed\n");
        fdk_shutdown(ctx);
        return 2;
    }
    fdk_ext_surface *surf = NULL;
    if (!fdk_ok(fdk_ext_surface_create(kind, &surf))) {
        printf("[vk] surface refused\n");
        fdk_ext_kind_destroy(kind);
        fdk_shutdown(ctx);
        return 2;
    }
    fdk_ext_surface_show(surf);
    for (int i = 0; i < 10; i++) {
        fdk_pump_events(ctx, 16);
    }

    /* ---- THE RECIPE: the native accessors -> VkSurfaceKHR ---- */
    fdk_ext_native_display nd;
    fdk_ext_native_handle nh;
    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd)) ||
        !fdk_ok(fdk_ext_surface_native(surf, &nh))) {
        printf("[vk] the native accessors failed\n");
        goto out;
    }
    const char *exts[2] = {"VK_KHR_surface", NULL};
    const char *xext = "VK_KHR_xlib_surface";
    const char *wext = "VK_KHR_wayland_surface";
    if (nd.backend == FDK_EXT_BACKEND_X11) {
        exts[1] = xext;
    } else {
        exts[1] = wext;
    }

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "fdk-vk-surface-example";
    app.apiVersion = VK_MAKE_VERSION(1, 1, 0);
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    VkResult vr = vkCreateInstance(&ici, NULL, &inst);
    printf("[vk] vkCreateInstance: %s\n", vk_result_str(vr));
    if (vr == VK_ERROR_INCOMPATIBLE_DRIVER) {
        printf("[vk] SKIP: no Vulkan ICD in this environment (install "
               "mesa-vulkan-drivers for lavapipe) — building was the "
               "assertion\n");
        rc = 0;
        goto out;
    }
    if (vr != VK_SUCCESS) {
        printf("[vk] instance creation failed for real\n");
        goto out;
    }

    if (nd.backend == FDK_EXT_BACKEND_X11) {
        VkXlibSurfaceCreateInfoKHR sci = {0};
        sci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        sci.dpy = (Display *)nd.display;
        sci.window = (Window)nh.xid;
        vr = vkCreateXlibSurfaceKHR(inst, &sci, NULL, &vsurf);
    } else {
        VkWaylandSurfaceCreateInfoKHR sci = {0};
        sci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        sci.display = (struct wl_display *)nd.display;
        sci.surface = (struct wl_surface *)nh.pointer;
        vr = vkCreateWaylandSurfaceKHR(inst, &sci, NULL, &vsurf);
    }
    printf("[vk] VkSurfaceKHR from the FDK native handles: %s\n",
           vk_result_str(vr));
    if (vr != VK_SUCCESS) {
        goto out;
    }

    /* ---- device: any physical device with graphics+present on one queue ---- */
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (n == 0) {
        printf("[vk] SKIP: no physical device (ICD present, no adapter)\n");
        rc = 0;
        goto out;
    }
    VkPhysicalDevice devs[8];
    if (n > 8) {
        n = 8;
    }
    vkEnumeratePhysicalDevices(inst, &n, devs);
    pdev = devs[0];
    uint32_t nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nfam, NULL);
    VkQueueFamilyProperties fams[16];
    if (nfam > 16) {
        nfam = 16;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nfam, fams);
    VkBool32 present_ok = VK_FALSE;
    for (uint32_t f = 0; f < nfam; f++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pdev, f, vsurf, &ok);
        if (ok && (fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            qfam = f;
            present_ok = VK_TRUE;
            break;
        }
    }
    if (!present_ok) {
        printf("[vk] no graphics+present queue on the first device\n");
        goto out;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    vr = vkCreateDevice(pdev, &dci, NULL, &dev);
    printf("[vk] device (queue family %u, graphics+present): %s\n",
           qfam, vk_result_str(vr));
    if (vr != VK_SUCCESS) {
        goto out;
    }
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    /* ---- the swapchain: the surface's current extent, one FIFO image ---- */
    VkSurfaceCapabilitiesKHR caps = {0};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, vsurf, &caps);
    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, vsurf, &nfmt, NULL);
    VkSurfaceFormatKHR fmts[8];
    if (nfmt > 8) {
        nfmt = 8;
    }
    vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, vsurf, &nfmt, fmts);
    VkFormat fmt = (nfmt && fmts[0].format != VK_FORMAT_UNDEFINED)
                       ? fmts[0].format
                       : VK_FORMAT_B8G8R8A8_UNORM;
    VkSwapchainCreateInfoKHR scci = {0};
    scci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    scci.surface = vsurf;
    scci.minImageCount = 1 < caps.minImageCount ? caps.minImageCount : 1;
    scci.imageFormat = fmt;
    scci.imageColorSpace = (nfmt && fmts[0].format != VK_FORMAT_UNDEFINED)
                               ? fmts[0].colorSpace
                               : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scci.imageExtent = caps.currentExtent;
    if (scci.imageExtent.width == 0xFFFFFFFFu) {
        scci.imageExtent.width = PANEL_W;
        scci.imageExtent.height = PANEL_H;
    }
    scci.imageArrayLayers = 1;
    scci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scci.preTransform = caps.currentTransform;
    scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    scci.presentMode = VK_PRESENT_MODE_FIFO_KHR; /* always supported */
    scci.clipped = VK_TRUE;
    vr = vkCreateSwapchainKHR(dev, &scci, NULL, &swap);
    printf("[vk] swapchain %ux%u: %s\n", scci.imageExtent.width,
           scci.imageExtent.height, vk_result_str(vr));
    if (vr != VK_SUCCESS) {
        goto out;
    }

    /* ---- present ONE frame (contents undefined; the contract is the
     * acquire -> queue -> present -> idle cycle through FDK's surface) ---- */
    uint32_t idx = 0;
    vr = vkAcquireNextImageKHR(dev, swap, UINT64_MAX,
                               VK_NULL_HANDLE, VK_NULL_HANDLE, &idx);
    printf("[vk] acquire: %s\n", vk_result_str(vr));
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        goto out;
    }
    VkPresentInfoKHR pi = {0};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swap;
    pi.pImageIndices = &idx;
    vr = vkQueuePresentKHR(queue, &pi);
    printf("[vk] present: %s\n", vk_result_str(vr));
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        goto out;
    }
    vkQueueWaitIdle(queue);

    /* FDK's surface still paints through its own path; pump a few
     * rounds so the compositor/protocol stays fed during teardown. */
    for (int i = 0; i < 10; i++) {
        fdk_pump_events(ctx, 16);
    }
    printf("[vk] PASS: instance -> surface (FDK native handles) -> "
           "device -> swapchain -> present -> idle\n");
    rc = 0;

out:
    if (swap != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(dev, swap, NULL);
    }
    if (dev != VK_NULL_HANDLE) {
        vkDestroyDevice(dev, NULL);
    }
    if (vsurf != VK_NULL_HANDLE) {
        /* The Vulkan surface only; FDK still owns the wl_surface /
         * X Window underneath (the ownership rule). */
        vkDestroySurfaceKHR(inst, vsurf, NULL);
    }
    if (inst != VK_NULL_HANDLE) {
        vkDestroyInstance(inst, NULL);
    }
    fdk_ext_surface_destroy(surf);
    fdk_ext_kind_destroy(kind);
    fdk_shutdown(ctx);
    printf("[vk] clean teardown\n");
    return rc;
}
