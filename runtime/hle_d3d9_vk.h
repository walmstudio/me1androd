/* v0.10: Vulkan renderer of hle_d3d9.c - included once after hle_d3d9_gl.h (shares Obj, Dev, formats, GlSh shaders, stats).
 *
 * Why: on Mali (Helio G85) the system GLES driver is slow for this game and Mesa zink needs Vulkan features the custom
 * drivers (malisx2, Qualcomm 891.11) do not have (VK_EXT_robustness2). This renderer uses Vulkan 1.0/1.1 core only:
 * no extensions besides VK_KHR_swapchain, every optional feature checked (BC textures, anisotropy, float filtering...).
 *
 * Model (same resource model as the GLES backend): textures/render targets -> VkImage (optimal tiling, own sub-allocator),
 * depth/stencil surfaces -> D24S8 (or D32S8) images, static vertex/index buffers -> host-visible VkBuffers (renamed when
 * the GPU may still read them), dynamic buffers / *UP data / constants -> per-frame rings. Shaders: sm3_glsl.c GLSL,
 * rewritten for Vulkan (uniform blocks, bindings, locations) and compiled to SPIR-V by libme1spv.so (glslang), cached on
 * disk with a VkPipelineCache. One pipeline per (program, vertex layout, render state, render pass format) key.
 * Rows keep D3D's order (row 0 = top = Vulkan row 0): the vertex shader flips y like the GL path, front face = clockwise.
 * Frames: 3 slots (command buffers, rings, fence); at most 2 frames in flight. Present: blit to the swapchain
 * (VK_KHR_android_surface) letterboxed, or CPU copy into the ANativeWindow when the driver has no usable WSI.
 * Env: ME1_RENDERER=vulkan | ME1_GPU_DRIVER=vulkan (system Vulkan) | ME1_GPU_DRIVER=/path/driver.so (custom driver: this
 *      renderer, ME1_GPU_ZINK=1 for Mesa zink instead); ME1_VK_VALIDATE=1 (validation layer), ME1_VK_STRICT=1 (behave as
 *      on malisx2: no BC, no USCALED vertex formats, no float32 filtering/blending), ME1_VK_NOBC=1, ME1_SPV_LIB=path,
 *      ME1_VK_SPLIT=n (submit the frame in chunks of n draws, default 2048, 0 = one submission - Mali DEVICE_LOST workaround). */
#define VK_NO_PROTOTYPES
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>

#define VKG(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)
#define VKI(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#ifdef __ANDROID__
#define VKIA(X) X(vkCreateAndroidSurfaceKHR)
#else
#define VKIA(X)
#endif
#define VKIO(X) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkCreateHeadlessSurfaceEXT) \
    X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT) VKIA(X)
#define VKD(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkInvalidateMappedMemoryRanges) X(vkFlushMappedMemoryRanges) X(vkBindBufferMemory) X(vkBindImageMemory) \
    X(vkGetBufferMemoryRequirements) X(vkGetImageMemoryRequirements) X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) \
    X(vkGetFenceStatus) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkCreateImage) X(vkDestroyImage) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineCache) \
    X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindIndexBuffer) X(vkCmdBindVertexBuffers) X(vkCmdExecuteCommands) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdCopyImage) X(vkCmdBlitImage) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdPipelineBarrier) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCreateQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery) \
    X(vkDestroyQueryPool) X(vkGetQueryPoolResults)
#define VKDO(X) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)
#define VK_DECL(n) static PFN_##n n;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VKG(VK_DECL) VKI(VK_DECL) VKIO(VK_DECL) VKD(VK_DECL) VKDO(VK_DECL)
static PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;

/* v1.1 (Linux port): X11 / Wayland surfaces (me1_platform.h), declared here so no window-system headers are needed */
#ifndef __ANDROID__
typedef struct { VkStructureType sType; const void* pNext; VkFlags flags; void* dpy; unsigned long window; } Me1VkXlibSurfaceCI;
typedef struct { VkStructureType sType; const void* pNext; VkFlags flags; void* display; void* surface; } Me1VkWaylandSurfaceCI;
typedef VkResult (*Me1PFN_CreateWsSurface)(VkInstance, const void*, const VkAllocationCallbacks*, VkSurfaceKHR*);
static Me1PFN_CreateWsSurface vkCreateWsSurface;   /* vkCreateXlibSurfaceKHR or vkCreateWaylandSurfaceKHR */
#endif
static int g_vklost;
static VkInstance VI; static VkPhysicalDevice VP; static VkDevice VD; static VkQueue VQ; static uint32_t VQF;
static VkPhysicalDeviceProperties VPP; static VkPhysicalDeviceMemoryProperties VMP; static VkPhysicalDeviceFeatures VFH, VFE;
static VkDebugUtilsMessengerEXT g_vkdbg;
static struct {
    int bc, aniso, indep, ts, swapext, headless, android, ws, strict, validate, bgravtx, ndebug;
    float maxaniso, tsperiod; uint32_t maxtex; VkFormat ds; int dsstencil;
    VkDeviceSize uboalign; char name[256]; uint32_t api;
    uint32_t split;        /* v0.10.3: submit the frame in chunks of N draws (ME1_VK_SPLIT, Mali default 384, 0 = one submit) */
    uint64_t splitprims;   /* v0.10.12 (P0-4): and/or N primitives (ME1_VK_SPLITPRIMS, Mali default 150000, 0 = off) */
    int oprec;             /* v1.1.3: occlusionQueryPrecise (sample counts; else only zero / non-zero) */
} VX;
static int g_pipeidle;   /* v0.10.4: ME1_VK_PIPEIDLE=1 - device idle before new shader modules/pipelines (Mali DEVICE_LOST) */
static int (*p_spv_compile)(int, const char*, unsigned**, size_t*, char*, size_t); static void (*p_spv_free)(unsigned*);
static VkPipelineCache g_vpc; static char g_vpcpath[1000]; static uint64_t g_vpc_new, g_vpc_saved;
static void vlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void vlog(const char* fmt, ...) { va_list ap; va_start(ap, fmt); fputs("[vk] ", stderr); vfprintf(stderr, fmt, ap); va_end(ap); }
static int g_vokn;   /* reset by vk_device_reset (P2-9): errors of the new device must reach the log */
static int vok(VkResult r, const char* what) {
    if (r >= 0) return 1;
    if (g_vokn++ < 40) vlog("%s: %s failed: VkResult %d\n", g_cur ? g_cur : "-", what, (int)r);
    if (r == VK_ERROR_DEVICE_LOST && !g_vklost) {
        g_vklost = 1; vlog("DEVICE LOST detected (recovery requires ME1_VK_LOSTRESET=1)\n");
        if (g_vpcpath[0]) { if (remove(g_vpcpath) == 0) vlog("pipeline cache %s deleted (a stale cache can crash the driver)\n", g_vpcpath); }
        d3d_device_lost();   /* v0.10.7: the game is told through the D3D9 flow; present() recovers on its own after 5 s */
    }
    return 0;
}
static VkBool32 VKAPI_PTR vk_dbg_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT ty,
                                    const VkDebugUtilsMessengerCallbackDataEXT* d, void* u) {
    (void)ty; (void)u;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) { VX.ndebug++; if (VX.ndebug < 200) vlog("validation: %s\n", d->pMessage); }
    return VK_FALSE;
}

/* ---- memory: blocks per (memory type, kind), first-fit free lists ---- */
typedef struct { VkDeviceMemory mem; VkDeviceSize size; uint32_t type; int kind, ded, nf, cf; uint8_t* map; VkDeviceSize (*fr)[2]; } MBlk;
typedef struct { int blk; VkDeviceSize off, size; } VMem;
static MBlk* g_mb; static int g_nmb, g_cmb; static uint64_t g_vkmem, g_vkallocs;
static int mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags pref) {
    int best = -1;
    for (uint32_t i = 0; i < VMP.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = VMP.memoryTypes[i].propertyFlags;
        if (!(bits >> i & 1) || (f & want) != want) continue;
        if ((f & pref) == pref) return (int)i;
        if (best < 0) best = (int)i;
    }
    return best;
}
static int blk_take(MBlk* b, VkDeviceSize size, VkDeviceSize al, VkDeviceSize* out) {
    for (int i = 0; i < b->nf; i++) {
        VkDeviceSize o = b->fr[i][0], e = o + b->fr[i][1], a = (o + al - 1) / al * al;
        if (a + size > e) continue;
        VkDeviceSize left = a - o, right = e - (a + size);
        if (left && right) {
            if (b->nf == b->cf) { b->cf = b->cf ? b->cf * 2 : 16; b->fr = realloc(b->fr, sizeof *b->fr * (size_t)b->cf); }
            memmove(&b->fr[i + 2], &b->fr[i + 1], sizeof *b->fr * (size_t)(b->nf - i - 1)); b->nf++;
            b->fr[i][1] = left; b->fr[i + 1][0] = a + size; b->fr[i + 1][1] = right;
        } else if (left) b->fr[i][1] = left;
        else if (right) { b->fr[i][0] = a + size; b->fr[i][1] = right; }
        else { memmove(&b->fr[i], &b->fr[i + 1], sizeof *b->fr * (size_t)(b->nf - i - 1)); b->nf--; }
        *out = a; return 1;
    }
    return 0;
}
static int vmem_alloc(const VkMemoryRequirements* rq, VkMemoryPropertyFlags want, VkMemoryPropertyFlags pref, int kind, VMem* m) {
    if (g_vklost) return 0;   /* v0.10.12 (P0-6) */
    int t = mem_type(rq->memoryTypeBits, want, pref);
    memset(m, 0, sizeof *m); m->blk = -1;
    if (t < 0) { vlog("no memory type for bits %x flags %x\n", rq->memoryTypeBits, want); return 0; }
    VkDeviceSize bs = kind == 1 ? (64u << 20) : (16u << 20);
    int ded = rq->size > bs / 4;
    if (!ded) for (int i = 0; i < g_nmb; i++) {
        MBlk* b = &g_mb[i];
        if (!b->mem || b->ded || b->type != (uint32_t)t || b->kind != kind) continue;
        VkDeviceSize off; if (blk_take(b, rq->size, rq->alignment ? rq->alignment : 1, &off)) { m->blk = i; m->off = off; m->size = rq->size; return 1; }
    }
    int slot = -1; for (int i = 0; i < g_nmb; i++) if (!g_mb[i].mem) { slot = i; break; }
    if (slot < 0) { if (g_nmb == g_cmb) { g_cmb = g_cmb ? g_cmb * 2 : 64; g_mb = realloc(g_mb, sizeof *g_mb * (size_t)g_cmb); } slot = g_nmb++; }
    MBlk* b = &g_mb[slot]; memset(b, 0, sizeof *b);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, ded ? rq->size : bs, (uint32_t)t };
    VkResult r = vkAllocateMemory(VD, &ai, NULL, &b->mem);
    if (r < 0 && !ded) { ai.allocationSize = bs = rq->size > (4u << 20) ? rq->size : (4u << 20); r = vkAllocateMemory(VD, &ai, NULL, &b->mem); }
    if (!vok(r, "vkAllocateMemory")) { b->mem = VK_NULL_HANDLE; vlog("out of GPU memory (%llu MB in use)\n", (unsigned long long)(g_vkmem >> 20)); return 0; }
    g_vkallocs++;
    b->size = ai.allocationSize; b->type = (uint32_t)t; b->kind = kind; b->ded = ded; g_vkmem += b->size;
    if (VMP.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* p = NULL; if (vok(vkMapMemory(VD, b->mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory")) b->map = p;
    }
    if (ded) { m->blk = slot; m->off = 0; m->size = rq->size; return 1; }
    b->cf = 16; b->fr = malloc(sizeof *b->fr * 16); b->nf = 1; b->fr[0][0] = 0; b->fr[0][1] = b->size;
    VkDeviceSize off; if (!blk_take(b, rq->size, rq->alignment ? rq->alignment : 1, &off)) return 0;
    m->blk = slot; m->off = off; m->size = rq->size; return 1;
}
static void vmem_free(VMem* m) {
    if (m->blk < 0 || m->blk >= g_nmb) return;
    MBlk* b = &g_mb[m->blk];
    if (b->ded) { vkFreeMemory(VD, b->mem, NULL); g_vkmem -= b->size; free(b->fr); memset(b, 0, sizeof *b); m->blk = -1; return; }
    int j = 0; while (j < b->nf && b->fr[j][0] < m->off) j++;
    if (b->nf == b->cf) { b->cf *= 2; b->fr = realloc(b->fr, sizeof *b->fr * (size_t)b->cf); }
    memmove(&b->fr[j + 1], &b->fr[j], sizeof *b->fr * (size_t)(b->nf - j)); b->nf++;
    b->fr[j][0] = m->off; b->fr[j][1] = m->size;
    if (j + 1 < b->nf && b->fr[j][0] + b->fr[j][1] == b->fr[j + 1][0]) { b->fr[j][1] += b->fr[j + 1][1]; memmove(&b->fr[j + 1], &b->fr[j + 2], sizeof *b->fr * (size_t)(b->nf - j - 2)); b->nf--; }
    if (j > 0 && b->fr[j - 1][0] + b->fr[j - 1][1] == b->fr[j][0]) { b->fr[j - 1][1] += b->fr[j][1]; memmove(&b->fr[j], &b->fr[j + 1], sizeof *b->fr * (size_t)(b->nf - j - 1)); b->nf--; }
    m->blk = -1;
}
/* v0.10.12 (P0-1a): bounds-checked accessors - a stale VMem (blk of the previous allocator after a device reset)
 * used to read a garbage VkDeviceMemory out of g_mb and SIGSEGV inside vkInvalidateMappedMemoryRanges */
static int vmem_ok(const VMem* m) { return m->blk >= 0 && m->blk < g_nmb && g_mb && g_mb[m->blk].mem != VK_NULL_HANDLE; }
static uint8_t* vmem_ptr(const VMem* m) { return vmem_ok(m) && g_mb[m->blk].map ? g_mb[m->blk].map + m->off : NULL; }
static VkDeviceMemory vmem_mem(const VMem* m) { return vmem_ok(m) ? g_mb[m->blk].mem : VK_NULL_HANDLE; }
static void vmem_invalidate(const VMem* m) {
    if (!vmem_ok(m)) return;
    if (VMP.memoryTypes[g_mb[m->blk].type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return;
    VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, vmem_mem(m), 0, VK_WHOLE_SIZE };
    vkInvalidateMappedMemoryRanges(VD, 1, &r);
}
/* a host-visible buffer with its own sub-allocation */
static int mk_buffer(VkDeviceSize size, VkBufferUsageFlags use, VkMemoryPropertyFlags pref, VkBuffer* b, VMem* m) {
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (!vok(vkCreateBuffer(VD, &bi, NULL, b), "vkCreateBuffer")) return 0;
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(VD, *b, &rq);
    if (!vmem_alloc(&rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, pref, 0, m)) { vkDestroyBuffer(VD, *b, NULL); *b = VK_NULL_HANDLE; return 0; }
    if (!vok(vkBindBufferMemory(VD, *b, vmem_mem(m), m->off), "vkBindBufferMemory")) return 0;
    return 1;
}

/* ---- formats ---- */
typedef struct { uint32_t d3d; VkFormat vf; uint8_t conv, nch; const char* sw; const char* swrt; } VFmt;
static const VFmt g_vfmt[] = {
    { 21, VK_FORMAT_B8G8R8A8_UNORM, CV_RAW, 4, "rgba", "rgba" }, { 22, VK_FORMAT_B8G8R8A8_UNORM, CV_RAW, 4, "rgb1", "rgb1" },
    { 23, VK_FORMAT_R5G6B5_UNORM_PACK16, CV_RAW, 3, "rgba", "rgba" },
    { 24, VK_FORMAT_A1R5G5B5_UNORM_PACK16, CV_RAW, 4, "rgb1", "rgb1" }, { 25, VK_FORMAT_A1R5G5B5_UNORM_PACK16, CV_RAW, 4, "rgba", "rgba" },
    { 26, VK_FORMAT_B4G4R4A4_UNORM_PACK16, CV_RAW, 4, "grab", "rgba" }, { 28, VK_FORMAT_R8_UNORM, CV_RAW, 1, "000r", "000r" },
    { 31, VK_FORMAT_A2B10G10R10_UNORM_PACK32, CV_RAW, 4, "rgba", "rgba" },
    { 32, VK_FORMAT_R8G8B8A8_UNORM, CV_RAW, 4, "rgba", "rgba" }, { 33, VK_FORMAT_R8G8B8A8_UNORM, CV_RAW, 4, "rgb1", "rgb1" },
    { 34, VK_FORMAT_R16G16_SFLOAT, CV_U16, 2, "rg11", "rg11" }, { 35, VK_FORMAT_A2B10G10R10_UNORM_PACK32, CV_RAW, 4, "bgra", "rgba" },
    { 36, VK_FORMAT_R16G16B16A16_SFLOAT, CV_U16, 4, "rgba", "rgba" },
    { 50, VK_FORMAT_R8_UNORM, CV_RAW, 1, "rrr1", "rrr1" }, { 51, VK_FORMAT_R8G8_UNORM, CV_RAW, 2, "rrrg", "rrrg" },
    { 60, VK_FORMAT_R8G8_SNORM, CV_RAW, 2, "rg11", "rg11" }, { 63, VK_FORMAT_R8G8B8A8_SNORM, CV_RAW, 4, "rgba", "rgba" },
    { 64, VK_FORMAT_R16G16_SFLOAT, CV_S16, 2, "rg11", "rg11" }, { 81, VK_FORMAT_R16_SFLOAT, CV_U16, 1, "rrr1", "rrr1" },
    { 111, VK_FORMAT_R16_SFLOAT, CV_RAW, 1, "r111", "r111" }, { 112, VK_FORMAT_R16G16_SFLOAT, CV_RAW, 2, "rg11", "rg11" },
    { 113, VK_FORMAT_R16G16B16A16_SFLOAT, CV_RAW, 4, "rgba", "rgba" },
    { 114, VK_FORMAT_R32_SFLOAT, CV_RAW, 1, "r111", "r111" }, { 115, VK_FORMAT_R32G32_SFLOAT, CV_RAW, 2, "rg11", "rg11" },
    { 116, VK_FORMAT_R32G32B32A32_SFLOAT, CV_RAW, 4, "rgba", "rgba" },
    { FCC('D','X','T','1'), VK_FORMAT_BC1_RGBA_UNORM_BLOCK, CV_DXT1, 4, "rgba", "rgba" }, { FCC('D','X','T','2'), VK_FORMAT_BC2_UNORM_BLOCK, CV_DXT3, 4, "rgba", "rgba" },
    { FCC('D','X','T','3'), VK_FORMAT_BC2_UNORM_BLOCK, CV_DXT3, 4, "rgba", "rgba" }, { FCC('D','X','T','4'), VK_FORMAT_BC3_UNORM_BLOCK, CV_DXT5, 4, "rgba", "rgba" },
    { FCC('D','X','T','5'), VK_FORMAT_BC3_UNORM_BLOCK, CV_DXT5, 4, "rgba", "rgba" },
    { FCC('A','T','I','1'), VK_FORMAT_BC4_UNORM_BLOCK, CV_BC4, 1, "rrrr", "rrrr" }, { FCC('A','T','I','2'), VK_FORMAT_BC5_UNORM_BLOCK, CV_BC5, 2, "gr11", "gr11" },
};
static const VFmt* vfmt(uint32_t f) { for (unsigned i = 0; i < sizeof g_vfmt / sizeof *g_vfmt; i++) if (g_vfmt[i].d3d == f) return &g_vfmt[i]; return NULL; }
static struct { VkFormat f; VkFormatProperties p; } g_fp[96]; static int g_nfp;
static int vk_scaled(VkFormat f) {   /* USCALED / SSCALED formats */
    switch (f) {
    case VK_FORMAT_R8_USCALED: case VK_FORMAT_R8_SSCALED: case VK_FORMAT_R8G8_USCALED: case VK_FORMAT_R8G8_SSCALED: case VK_FORMAT_R8G8B8_USCALED:
    case VK_FORMAT_R8G8B8_SSCALED: case VK_FORMAT_B8G8R8_USCALED: case VK_FORMAT_B8G8R8_SSCALED: case VK_FORMAT_R8G8B8A8_USCALED: case VK_FORMAT_R8G8B8A8_SSCALED:
    case VK_FORMAT_B8G8R8A8_USCALED: case VK_FORMAT_B8G8R8A8_SSCALED: case VK_FORMAT_A8B8G8R8_USCALED_PACK32: case VK_FORMAT_A8B8G8R8_SSCALED_PACK32:
    case VK_FORMAT_A2R10G10B10_USCALED_PACK32: case VK_FORMAT_A2R10G10B10_SSCALED_PACK32: case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
    case VK_FORMAT_A2B10G10R10_SSCALED_PACK32: case VK_FORMAT_R16_USCALED: case VK_FORMAT_R16_SSCALED: case VK_FORMAT_R16G16_USCALED:
    case VK_FORMAT_R16G16_SSCALED: case VK_FORMAT_R16G16B16_USCALED: case VK_FORMAT_R16G16B16_SSCALED: case VK_FORMAT_R16G16B16A16_USCALED:
    case VK_FORMAT_R16G16B16A16_SSCALED: return 1;
    default: return 0;
    }
}
static VkFormatFeatureFlags fmt_opt(VkFormat f) {   /* optimal-tiling features, with the strict (malisx2-like) restrictions */
    for (int i = 0; i < g_nfp; i++) if (g_fp[i].f == f) return g_fp[i].p.optimalTilingFeatures;
    VkFormatProperties p; memset(&p, 0, sizeof p); vkGetPhysicalDeviceFormatProperties(VP, f, &p);
    if (VX.strict && (f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT))
        p.optimalTilingFeatures &= ~(VkFormatFeatureFlags)(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
    if ((VX.strict || !VX.bc) && f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) p.optimalTilingFeatures = 0;
    if (VX.strict && vk_scaled(f)) p.bufferFeatures = 0;
    if (g_nfp < 96) { g_fp[g_nfp].f = f; g_fp[g_nfp].p = p; g_nfp++; }
    return p.optimalTilingFeatures;
}
static VkFormatFeatureFlags fmt_buf(VkFormat f) {
    fmt_opt(f);
    for (int i = 0; i < g_nfp; i++) if (g_fp[i].f == f) return g_fp[i].p.bufferFeatures;
    return 0;
}
static VkComponentSwizzle vswz(char c) {
    switch (c) { case 'r': return VK_COMPONENT_SWIZZLE_R; case 'g': return VK_COMPONENT_SWIZZLE_G; case 'b': return VK_COMPONENT_SWIZZLE_B;
                 case 'a': return VK_COMPONENT_SWIZZLE_A; case '0': return VK_COMPONENT_SWIZZLE_ZERO; default: return VK_COMPONENT_SWIZZLE_ONE; }
}

/* ---- resources ---- */
typedef struct VRes {
    uint32_t serial; int kind;           /* 1 color image, 2 depth/stencil image, 3 buffer */
    VkImage img; VMem m; VkFormat vf; const VFmt* f;
    int dec, obpp, drop, lv, w, h, d, layers, rt, is3d, cube, filt, nsub;
    VkImageView sview; VkImageView* av; int32_t* lay;
    VkBuffer buf; uint32_t size;
    uint64_t use, mainuse, bytes;
} VRes;
typedef struct { uint64_t serial; int kind; uint64_t h; VMem m; } Dead;   /* kind 1 image, 2 view, 3 buffer, 4 framebuffer, 5 pipeline, 6 module */
static Dead* g_dead; static int g_ndead, g_cdead;
#define NSLOT 3
static uint64_t g_vserial = 1, g_vdone;
static void dead_add(uint64_t serial, int kind, uint64_t h, const VMem* m) {
    if (!h && (!m || m->blk < 0)) return;
    if (g_ndead == g_cdead) { g_cdead = g_cdead ? g_cdead * 2 : 256; g_dead = realloc(g_dead, sizeof *g_dead * (size_t)g_cdead); }
    Dead* x = &g_dead[g_ndead++]; x->serial = serial; x->kind = kind; x->h = h; if (m) x->m = *m; else x->m.blk = -1;
}
static void dead_one(Dead* x) {
    switch (x->kind) {
    case 1: vkDestroyImage(VD, (VkImage)(uintptr_t)x->h, NULL); break;
    case 2: vkDestroyImageView(VD, (VkImageView)(uintptr_t)x->h, NULL); break;
    case 3: vkDestroyBuffer(VD, (VkBuffer)(uintptr_t)x->h, NULL); break;
    case 4: vkDestroyFramebuffer(VD, (VkFramebuffer)(uintptr_t)x->h, NULL); break;
    case 5: vkDestroyPipeline(VD, (VkPipeline)(uintptr_t)x->h, NULL); break;
    case 6: vkDestroyShaderModule(VD, (VkShaderModule)(uintptr_t)x->h, NULL); break;
    }
    if (x->m.blk >= 0) vmem_free(&x->m);
}
static void dead_reap(void) {
    int k = 0;
    for (int i = 0; i < g_ndead; i++) { if (g_dead[i].serial <= g_vdone) dead_one(&g_dead[i]); else g_dead[k++] = g_dead[i]; }
    g_ndead = k;
}
#define H(x) ((uint64_t)(uintptr_t)(x))

/* ---- frame slots: command buffers, rings, fence ---- */
typedef struct { VkBuffer buf; VMem m; uint8_t* p; VkDeviceSize size, off; VkDescriptorSet uset; } Chunk;
typedef struct { Chunk* c; int n, cap, cur; VkBufferUsageFlags use; VkDeviceSize def, slack; } Ring;
typedef struct { VkCommandPool pool; VkCommandBuffer* cb; uint32_t n, cap, used; } VSecLane;
#define SEC_LANES 4
#define SEC_BATCH 64
typedef struct {
    VkCommandPool pool; VkCommandBuffer cmd, up; int upon;
    VSecLane sec[SEC_LANES];
    VkCommandBuffer xcmd[64]; uint32_t nx;   /* v0.10.3: finished chunks of this frame, submitted without a fence (v0.10.13: 64, freed in slot_begin) */
    uint32_t cdraws;                         /* draws recorded into the current chunk */
    uint64_t cprims;                         /* v0.10.12 (P0-4): primitives recorded into the current chunk */
    VkFence fence; uint64_t serial; int pending, eof;
    Ring vtx, ubo, stg;
    VkDescriptorPool dp[64]; int ndp, curdp;
    VkQueryPool qp; int qon;
    VkSemaphore acq;
    VkBuffer rb; VMem rbm; VkDeviceSize rbsize; int rbw, rbh, rbpend;   /* CPU presentation: this frame's pixels */
    /* v1.1.3: occlusion queries of this slot: pools of VQP, created on demand; on = used, reset at the next slot_begin;
     * ost: 0 recording, 1 submitted, 2 results read (g_ores), 3 never ran (submit failed); rser = serial of this use */
    VkQueryPool op[16]; uint32_t nop, on; int ost; uint64_t rser;
} VSlot;
static VSlot S[NSLOT]; static int g_si;
static int g_sec_on, g_sec_fail_test;
static VkDescriptorSet g_sec_last_tex;
static uint64_t g_sec_batches, g_sec_draws, g_sec_parallel, g_sec_fallback;
/* v1 observability counters */
static uint64_t g_pc_hits, g_pc_misses;          /* pipeline cache */
static double g_spv_compile_s, g_pipe_create_s;  /* shader/SPIR-V compile + pipeline creation time */
static uint64_t g_chunk_flush_n;                 /* chunk flush count */
static uint64_t g_vklost_count, g_vkrecover_count;
static uint64_t g_swap_recreate;
static uint64_t g_ring_pressure, g_stg_pressure; /* ring/staging new-chunk allocations */
static uint64_t g_vk_mip_par_slices;             /* G3: slices decoded via ME1_VK_MIP_PARALLEL outer loop */
static uint64_t g_sec_lanes_used;
static void sec_free_hosts(void) {
    for (int s = 0; s < NSLOT; s++) for (int i = 0; i < SEC_LANES; i++) {
        free(S[s].sec[i].cb); S[s].sec[i].cb = NULL;
    }
}
static void sec_slot_reset(VSlot* s) {
    for (int i = 0; i < SEC_LANES; i++) {
        if (s->sec[i].pool) vkResetCommandPool(VD, s->sec[i].pool, 0);
        s->sec[i].used = 0;
    }
    g_sec_last_tex = VK_NULL_HANDLE;
}
#define VQP 4096u
#define VQPOOLS 16u
static uint32_t g_ores[NSLOT][VQP * VQPOOLS];   /* results of the finished slot (UINT32_MAX: not available) */
static int g_vq_act; static VkQueryPool g_vq_pool; static uint32_t g_vq_idx;   /* the Vulkan query recording now */
static uint32_t g_vq_flush;   /* mid-frame submissions for queries polled in the frame being recorded (per frame) */
#define CS (&S[g_si])
static VkDescriptorSetLayout g_dsl[2]; static VkPipelineLayout g_pl; static VkDescriptorPool g_upool[16]; static int g_nupool;
#define VSR 4416u    /* vertex shader block: dx_fix, dx_vp, dx_bgra/vb, vi[16], vc[256] */
#define PSR 3904u    /* pixel shader block: dx_at, dx_srgb/dx_srgbw/pb, pi[16], pc[224] */
static VkDescriptorSet ubo_set(VkBuffer b) {
    if (g_vklost) return VK_NULL_HANDLE;   /* v0.10.12 (P0-6) */
    VkDescriptorSet s = VK_NULL_HANDLE;
    for (int tries = 0; tries < 2 && !s; tries++) {
        if (!g_nupool || tries) {
            if (g_nupool >= 16) return VK_NULL_HANDLE;
            VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 128 };
            VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 64, 1, &ps };
            if (!vok(vkCreateDescriptorPool(VD, &pi, NULL, &g_upool[g_nupool]), "vkCreateDescriptorPool")) return VK_NULL_HANDLE;
            g_nupool++;
        }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_upool[g_nupool - 1], 1, &g_dsl[0] };
        if (vkAllocateDescriptorSets(VD, &ai, &s) < 0) s = VK_NULL_HANDLE;
    }
    if (!s) return s;
    VkDescriptorBufferInfo bi[2] = { { b, 0, VSR }, { b, 0, PSR } };
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++) { memset(&w[i], 0, sizeof w[i]); w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = s; w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[i].pBufferInfo = &bi[i]; }
    vkUpdateDescriptorSets(VD, 2, w, 0, NULL);
    return s;
}
/* n bytes from the ring; *buf / *off / *set: the chunk's buffer, the offset and (UBO ring) its descriptor set */
static uint8_t* ring_alloc(Ring* r, VkDeviceSize n, VkDeviceSize al, VkBuffer* buf, VkDeviceSize* off, VkDescriptorSet* set) {
    for (;;) {
        if (r->cur < r->n) {
            Chunk* c = &r->c[r->cur]; VkDeviceSize o = (c->off + al - 1) / al * al;
            if (o + n + r->slack <= c->size) { c->off = o + n; *buf = c->buf; *off = o; if (set) *set = c->uset; return c->p + o; }
            if (c->off == 0) return NULL;
            r->cur++; continue;
        }
        VkDeviceSize sz = r->def; while (sz < (n + r->slack) * 2) sz *= 2;
        if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 4; r->c = realloc(r->c, sizeof *r->c * (size_t)r->cap); }
        Chunk* c = &r->c[r->n]; memset(c, 0, sizeof *c);
        if (!mk_buffer(sz, r->use, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &c->buf, &c->m) || !(c->p = vmem_ptr(&c->m))) return NULL;
        c->size = sz;
        if (r->use & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) c->uset = ubo_set(c->buf);
        r->cur = r->n; r->n++;
    }
}
static void ring_reset(Ring* r) { for (int i = 0; i < r->n; i++) r->c[i].off = 0; r->cur = 0; }

/* bound-state cache of the current command buffer */
static struct {
    VkPipeline pipe; VkDescriptorSet set0, set1; uint32_t uoff[2];
    VkViewport vp; VkRect2D sc; float bias[2], bc[4]; uint32_t sref, smask, swmask;
    VkBuffer vb[17]; VkDeviceSize vbo[17]; VkBuffer ib; VkDeviceSize ibo; int it;
    /* constant upload caches */
    uint32_t vcver, vser, pcver, pser; float vfix[8]; int vbgra; float pat[2]; int psrgb, psrgbw; VkDescriptorSet uset; VkBuffer ubuf;
    uint32_t ofs[2]; int uvalid;
    uint64_t tkey; VkDescriptorSet tset;
} VB;
static void vb_cmd_defaults(VkCommandBuffer cb) {
    memset(&VB, 0, sizeof VB);
    VkViewport vp = { 0, 0, 1, 1, 0, 1 }; VkRect2D sc = { { 0, 0 }, { 1, 1 } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc); VB.vp = vp; VB.sc = sc;
    vkCmdSetDepthBias(cb, 0, 0, 0); float bc[4] = { 0, 0, 0, 0 }; vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0); VB.smask = VB.swmask = 0xFF;
}
static struct { uint64_t ns, nf; double wait, waitf; uint64_t subs; } VT;
static void slot_finish(VSlot* s);
static void slot_begin(void) {
    VSlot* s = CS;
    if (s->pending) { double t = mono(); int ok = vok(vkWaitForFences(VD, 1, &s->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"); VT.wait += mono() - t; if (!ok) return; slot_finish(s); }
    if (g_vklost) return;
    vkResetFences(VD, 1, &s->fence);
    /* v0.10.13 (N1): the chunk command buffers of the finished frame go back to the pool - vkResetCommandPool only resets
     * them, so without vkFreeCommandBuffers every split allocated two more for the life of the process */
    { uint32_t k = 0; VkCommandBuffer fr[64];
      for (uint32_t i = 0; i < s->nx; i++) if (s->xcmd[i] && s->xcmd[i] != s->cmd && s->xcmd[i] != s->up) fr[k++] = s->xcmd[i];
      if (k) vkFreeCommandBuffers(VD, s->pool, k, fr); }
    vkResetCommandPool(VD, s->pool, 0);
    sec_slot_reset(s);
    s->nx = 0; s->cdraws = 0; s->cprims = 0;
    ring_reset(&s->vtx); ring_reset(&s->ubo); ring_reset(&s->stg);
    for (int i = 0; i < s->ndp; i++) vkResetDescriptorPool(VD, s->dp[i], 0);
    s->curdp = 0;
    dead_reap();
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    vkBeginCommandBuffer(s->cmd, &bi); s->upon = 0;
    for (uint32_t k = 0; k < s->nop && k * VQP < s->on; k++) {   /* v1.1.3: the occlusion queries used last time (results read) */
        uint32_t m = s->on - k * VQP; vkCmdResetQueryPool(s->cmd, s->op[k], 0, m < VQP ? m : VQP);
    }
    s->on = 0; s->ost = 0; s->rser = g_vserial;
    if (VX.ts) { vkCmdResetQueryPool(s->cmd, s->qp, 0, 2); vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s->qp, 0); s->qon = 1; }
    vb_cmd_defaults(s->cmd);
}
static VkCommandBuffer up_begin(void) {
    VSlot* s = CS;
    if (!s->upon) { VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
                    vkBeginCommandBuffer(s->up, &bi); s->upon = 1; }
    return s->up;
}
/* v0.10.3: end the current command buffer and submit it without a fence (queue order still guarantees that it finishes
 * before the frame's final submission, which carries the fence), then start a fresh one for the rest of the frame.
 * Mali drivers crash (VK_ERROR_DEVICE_LOST) on a single submission with tens of thousands of draw commands.
 * v0.10.12 (P0-0): the chunk is submitted EXACTLY ONCE here, together with the pending upload buffer (its images are
 * sampled by this chunk's draws). The final vb_submit does NOT resubmit s->xcmd[] - one-time-submit command buffers
 * must never be submitted twice (VUID-vkQueueSubmit-pCommandBuffers-00070/00071): that double execution was faulting
 * the GPU on every frame with more than VX.split draws (DEVICE_LOST on Mali AND Adreno). */
static void rp_end(void);
static void vk_chunk_flush(void) {
    VSlot* s = CS;
    if ((!VX.split && !VX.splitprims) || g_vklost) { s->cdraws = 0; s->cprims = 0; return; }
    if (s->nx + 2 > sizeof s->xcmd / sizeof *s->xcmd) {   /* out of chunk slots: stop splitting this frame (log once) */
        static int k; if (k++ < 3) vlog("chunk limit (%d CBs) reached - the rest of the frame goes unsplit\n", (int)(sizeof s->xcmd / sizeof *s->xcmd));
        s->cdraws = 0; s->cprims = 0; return;
    }
    rp_end();
    vkEndCommandBuffer(s->cmd);
    VkCommandBuffer cbs[2]; uint32_t n = 0;
    if (s->upon) { vkEndCommandBuffer(s->up); cbs[n++] = s->up; s->upon = 0; }   /* uploads BEFORE this chunk's draws */
    cbs[n++] = s->cmd;
    VkSubmitInfo si; memset(&si, 0, sizeof si); si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = n; si.pCommandBuffers = cbs;
    if (!vok(vkQueueSubmit(VQ, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit(chunk)")) { s->cdraws = 0; return; }
    VT.subs++;
    for (uint32_t i = 0; i < n; i++) s->xcmd[s->nx++] = cbs[i];   /* only remember them; freed by vkResetCommandPool in slot_begin */
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    if (n == 2) { VkCommandBuffer nu; if (vkAllocateCommandBuffers(VD, &cai, &nu) == VK_SUCCESS) s->up = nu; }   /* the old up is pending */
    VkCommandBuffer nc;
    if (vkAllocateCommandBuffers(VD, &cai, &nc) == VK_SUCCESS) {
        s->cmd = nc;
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
        if (vkBeginCommandBuffer(s->cmd, &bi) == VK_SUCCESS) { vb_cmd_defaults(s->cmd); g_sec_last_tex = VK_NULL_HANDLE; }
    }
    s->cdraws = 0; s->cprims = 0;
}
static VkDescriptorSet tex_set_alloc(void) {
    VSlot* s = CS;
    for (;;) {
        if (s->curdp >= s->ndp) {
            if (s->ndp >= 64) return VK_NULL_HANDLE;
            VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512 * 8 };
            VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 512, 1, &ps };
            if (!vok(vkCreateDescriptorPool(VD, &pi, NULL, &s->dp[s->ndp]), "vkCreateDescriptorPool")) return VK_NULL_HANDLE;
            s->ndp++;
        }
        VkDescriptorSet set; VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, s->dp[s->curdp], 1, &g_dsl[1] };
        if (vkAllocateDescriptorSets(VD, &ai, &set) >= 0) return set;
        s->curdp++;
    }
}

/* ---- layouts and barriers ---- */
static void lay_masks(VkImageLayout l, int src, VkPipelineStageFlags* st, VkAccessFlags* ac) {
    switch (l) {
    case VK_IMAGE_LAYOUT_UNDEFINED: *st = src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; *ac = 0; break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: *st = VK_PIPELINE_STAGE_TRANSFER_BIT; *ac = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: *st = VK_PIPELINE_STAGE_TRANSFER_BIT; *ac = VK_ACCESS_TRANSFER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: *st = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; *ac = VK_ACCESS_SHADER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: *st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; *ac = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: *st = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        *ac = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: *st = src ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; *ac = 0; break;
    default: *st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; *ac = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; break;
    }
}
static VkImageAspectFlags res_aspect(const VRes* r) {
    return r->kind == 2 ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (VX.dsstencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
}
/* subresources [layer, layer + nl) x [lvl, lvl + nv) (Vulkan levels) -> layout nl_; WAW of transfers and attachments get a barrier too */
static void img_to(VkCommandBuffer cb, VRes* r, uint32_t layer, uint32_t nl, uint32_t lvl, uint32_t nv, VkImageLayout to) {
    VkImageMemoryBarrier b[16]; int nb = 0; VkPipelineStageFlags ss = 0, ds = 0;
    for (uint32_t L = layer; L < layer + nl; L++) for (uint32_t v = lvl; v < lvl + nv; v++) {
        int i = (int)(L * (uint32_t)r->lv + v); VkImageLayout from = (VkImageLayout)r->lay[i];
        if (from == to && to != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) continue;
        if (nb == 16) { vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, (uint32_t)nb, b); nb = 0; ss = ds = 0; }
        VkPipelineStageFlags s1, s2; VkAccessFlags a1, a2; lay_masks(from, 1, &s1, &a1); lay_masks(to, 0, &s2, &a2);
        VkImageMemoryBarrier* x = &b[nb++]; memset(x, 0, sizeof *x);
        x->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; x->srcAccessMask = a1; x->dstAccessMask = a2; x->oldLayout = from; x->newLayout = to;
        x->srcQueueFamilyIndex = x->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; x->image = r->img;
        x->subresourceRange.aspectMask = res_aspect(r); x->subresourceRange.baseMipLevel = v; x->subresourceRange.levelCount = 1;
        x->subresourceRange.baseArrayLayer = L; x->subresourceRange.layerCount = 1;
        ss |= s1; ds |= s2; r->lay[i] = (int32_t)to;
    }
    if (nb) vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, (uint32_t)nb, b);
}
static int img_needs(const VRes* r, uint32_t layer, uint32_t nl, uint32_t lvl, uint32_t nv, VkImageLayout to) {
    for (uint32_t L = layer; L < layer + nl; L++) for (uint32_t v = lvl; v < lvl + nv; v++) if (r->lay[L * (uint32_t)r->lv + v] != (int32_t)to) return 1;
    return 0;
}
static void rp_end(void);

/* ---- images ---- */
static uint32_t g_vser;
static VkImageView mk_view(VRes* r, VkImageViewType vt, uint32_t lvl, uint32_t nv, uint32_t layer, uint32_t nl, const char* sw) {
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = r->img; vi.viewType = vt; vi.format = r->vf;
    if (sw) { vi.components.r = vswz(sw[0]); vi.components.g = vswz(sw[1]); vi.components.b = vswz(sw[2]); vi.components.a = vswz(sw[3]); }
    vi.subresourceRange.aspectMask = res_aspect(r); vi.subresourceRange.baseMipLevel = lvl; vi.subresourceRange.levelCount = nv;
    vi.subresourceRange.baseArrayLayer = layer; vi.subresourceRange.layerCount = nl;
    VkImageView v = VK_NULL_HANDLE; vok(vkCreateImageView(VD, &vi, NULL, &v), "vkCreateImageView"); return v;
}
static int mk_image(VRes* r, VkImageUsageFlags use) {
    if (g_vklost) return 0;   /* v0.10.12 (P0-6) */
    VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.flags = r->cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ii.imageType = r->is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D; ii.format = r->vf;
    ii.extent.width = dimv((uint32_t)r->w, (uint32_t)r->drop); ii.extent.height = dimv((uint32_t)r->h, (uint32_t)r->drop);
    ii.extent.depth = r->is3d ? dimv((uint32_t)r->d, (uint32_t)r->drop) : 1;
    ii.mipLevels = (uint32_t)r->lv; ii.arrayLayers = (uint32_t)r->layers; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = use; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vok(vkCreateImage(VD, &ii, NULL, &r->img), "vkCreateImage")) return 0;
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(VD, r->img, &rq);
    if (!vmem_alloc(&rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 1, &r->m)) {
        if (!vmem_alloc(&rq, 0, 0, 1, &r->m)) { vkDestroyImage(VD, r->img, NULL); r->img = VK_NULL_HANDLE; return 0; }
    }
    if (!vok(vkBindImageMemory(VD, r->img, vmem_mem(&r->m), r->m.off), "vkBindImageMemory")) return 0;
    r->nsub = r->layers * r->lv; r->lay = calloc((size_t)r->nsub, sizeof *r->lay); r->av = calloc((size_t)r->nsub, sizeof *r->av);
    r->bytes = rq.size; texmem_add((int64_t)rq.size);
    return 1;
}
static void img_clear_new(VRes* r);
/* GPU image of a root resource (texture, cube, volume texture or stand-alone surface); NULL: none */
static void vtex_upload(Obj* t, VRes* r, uint32_t face, uint32_t lvl, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t w, uint32_t h, uint32_t dz, const uint8_t* src, uint32_t pitch, uint32_t slice);
static VRes* vimg(Obj* t) {
    if (g_vklost) return NULL;   /* v0.10.12 (P0-6): before calloc, or the object would be stuck with a dead gx */
    if (t->gx) { VRes* r = t->gx; return r->kind ? r : NULL; }
    if ((t->type < T_TEX || t->type > T_SURF) || t->parent) return NULL;
    if (t->pool >= 2) return NULL;
    const Fmt* f = fmt_info(t->fmt); const VFmt* vf = vfmt(t->fmt);
    VRes* r = calloc(1, sizeof *r); t->gx = r;
    r->serial = ++g_vser; r->w = (int)t->w; r->h = (int)t->h; r->d = t->d ? (int)t->d : 1; r->lv = (int)t->levels; r->layers = 1;
    if (f && (f->fl & F_DS)) {
        r->kind = 2; r->vf = VX.ds; r->lv = 1;
        if (!mk_image(r, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { r->kind = 0; return NULL; }
        img_clear_new(r);
        return r;
    }
    if (!f || !vf) { static int n; if (n++ < 20) vlog("%s: format %s has no Vulkan mapping\n", g_cur, fmt_name(t->fmt)); return NULL; }
    r->f = vf; r->vf = vf->vf; r->rt = (t->usage & 1) != 0; r->cube = t->type == T_CUBE; r->is3d = t->type == T_VOLTEX;
    r->layers = r->cube ? 6 : 1; r->obpp = f->bytes;
    if (is_block(vf->conv)) {
        VkFormatFeatureFlags ff = r->is3d || g_texbc < (vf->conv <= CV_DXT5 ? 1 : 2) ? 0 : fmt_opt(vf->vf);   /* v1.2.1: ME1_TEX_BC */
        if (!(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            r->dec = 1;
            if (vf->conv == CV_BC4) { r->vf = VK_FORMAT_R8_UNORM; r->obpp = 1; }
            else if (vf->conv == CV_BC5) { r->vf = VK_FORMAT_R8G8_UNORM; r->obpp = 2; }
            else if (g_tex16 == 3 && vf->conv == CV_DXT1) { r->vf = VK_FORMAT_R5G6B5_UNORM_PACK16; r->obpp = 2; }   /* v1.2.1: no alpha */
            else if (g_tex16 && vf->conv == CV_DXT1) { r->vf = VK_FORMAT_A1R5G5B5_UNORM_PACK16; r->obpp = 2; }
            else if (g_tex16 >= 2) { r->vf = VK_FORMAT_B4G4R4A4_UNORM_PACK16; r->obpp = 2; }
            else { r->vf = VK_FORMAT_R8G8B8A8_UNORM; r->obpp = 4; }
        }
    } else if (vf->conv == CV_U16 || vf->conv == CV_S16) r->obpp = 2 * vf->nch;
    VkFormatFeatureFlags ff = fmt_opt(r->vf);
    if (!(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || (r->rt && !(ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))) {
        static int n; if (n++ < 20) vlog("%s: %s %s not supported by the driver (Vulkan format %d)\n", g_cur, fmt_name(t->fmt), r->rt ? "render target" : "texture", (int)r->vf);
        return NULL;
    }
    r->filt = (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    if (!r->rt && !r->is3d) {
        uint32_t lim = g_texmax && (uint32_t)g_texmax < VX.maxtex ? (uint32_t)g_texmax : VX.maxtex;
        while (r->drop < r->lv - 1 && (dimv(t->w, (uint32_t)r->drop) > lim || dimv(t->h, (uint32_t)r->drop) > lim)) r->drop++;
    }
    r->lv -= r->drop;
    VkImageUsageFlags use = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (r->rt) use |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    r->kind = 1;
    if (!mk_image(r, use)) { r->kind = 0; return NULL; }
    /* v0.10.7: after a device reset the new image is empty - upload the persistent guest copy (D3D9 MANAGED) */
    if (t->lockv && !r->is3d) {
        uint32_t faces = r->cube ? 6 : 1;
        for (uint32_t sf = 0; sf < faces; sf++) for (uint32_t lvl = r->drop; lvl < t->levels; lvl++) {
            uint32_t s2 = sf * t->levels + lvl;
            if (s2 >= sub_count(t) || !t->lockv[s2]) continue;
            uint32_t pitch, rows, unit; int blk;
            layout(t->fmt, dimv(t->w, lvl), dimv(t->h, lvl), &pitch, &rows, &unit, &blk);
            vtex_upload(t, r, sf, lvl, 0, 0, 0, dimv(t->w, lvl), dimv(t->h, lvl), 1, M + t->lockv[s2], pitch, pitch * rows);
        }
    }
    r->sview = mk_view(r, r->cube ? VK_IMAGE_VIEW_TYPE_CUBE : r->is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, 0, (uint32_t)r->lv, 0, (uint32_t)r->layers,
                       r->rt ? vf->swrt : vf->sw);
    if (r->rt) img_clear_new(r);
    return r;
}
/* the attachment view of one face/level (identity swizzle) */
static VkImageView att_view(VRes* r, uint32_t face, uint32_t vlvl) {
    int i = (int)(face * (uint32_t)r->lv + vlvl);
    if (!r->av[i]) r->av[i] = mk_view(r, VK_IMAGE_VIEW_TYPE_2D, vlvl, 1, face, 1, NULL);
    return r->av[i];
}
/* G3 helper: worker for optional outer parallelization across depth slices */
typedef struct { int cv, pk; uint32_t bsz, bx, by, ob, ow, oh, pitch, slice; const uint8_t* src; uint8_t* q; } VkMipSliceJob;
static void vk_mip_slice_worker(void* ctx, int lo, int hi) {
    VkMipSliceJob* j = ctx;
    for (int k = lo; k < hi; k++) {
        const uint8_t* s0 = j->src + (size_t)k * j->slice;
        uint8_t* qk = j->q + (size_t)k * j->ow * j->oh * j->ob;
        dec_rect(j->cv, j->pk, j->bsz, j->bx, j->by, j->ob, s0, j->pitch, qk);
    }
}
/* upload the rect of level lvl / face from guest data (D3D layout: rows of 4x4 blocks for compressed formats) */
static void vtex_upload(Obj* t, VRes* r, uint32_t face, uint32_t lvl, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t w, uint32_t h, uint32_t dz, const uint8_t* src, uint32_t pitch, uint32_t slice) {
    if (g_vklost) return;   /* v0.10.12 (P0-6) */
    int vl = (int)lvl - r->drop;
    if (vl < 0 || r->kind != 1 || !w || !h || !dz) return;
    uint32_t lw = dimv(t->w, lvl), lh = dimv(t->h, lvl);
    if (x >= lw || y >= lh) return;
    if (x + w > lw) w = lw - x;
    if (y + h > lh) h = lh - y;
    const VFmt* f = r->f; int blk = is_block(f->conv);
    uint32_t bsz = f->conv == CV_DXT1 || f->conv == CV_BC4 ? 8 : 16, bx = (w + 3) / 4, by = (h + 3) / 4;
    VkDeviceSize n; uint32_t rowlen, ob = (uint32_t)r->obpp;
    if (blk && !r->dec) { n = (VkDeviceSize)bx * by * bsz * dz; rowlen = bx * 4; }
    else if (blk) { n = (VkDeviceSize)bx * 4 * by * 4 * ob * dz; rowlen = bx * 4; }
    else { n = (VkDeviceSize)w * h * dz * ob; rowlen = w; }
    VkBuffer sb; VkDeviceSize so; uint8_t* q = ring_alloc(&CS->stg, n, 16, &sb, &so, NULL);
    if (!q) { static int k; if (k++ < 5) vlog("staging memory exhausted (%llu bytes)\n", (unsigned long long)n); return; }
    GS.texup++; GS.texbytes += (uint64_t)pitch * ((h + 3) / 4) * dz;
    if (blk && !r->dec) {
        for (uint32_t k = 0; k < dz; k++) for (uint32_t rr = 0; rr < by; rr++) memcpy(q + ((size_t)k * by + rr) * bx * bsz, src + (size_t)k * slice + (size_t)rr * pitch, (size_t)bx * bsz);
    } else if (blk) {
        uint32_t ow = bx * 4, oh = by * 4;
        int pk = ob == 4 ? PK_RGBA8 : ob == 1 ? PK_R8 : r->vf == VK_FORMAT_R8G8_UNORM ? PK_RG8 : r->vf == VK_FORMAT_A1R5G5B5_UNORM_PACK16 ? PK_VK1555
               : r->vf == VK_FORMAT_R5G6B5_UNORM_PACK16 ? PK_565 : PK_VK4444;
        /* G3: optional outer parallelization across depth slices (mip/face handled by caller).
         * Default OFF to keep behavior unchanged; ME1_VK_MIP_PARALLEL=1 opts in. */
        static int mip_par = -1; if (mip_par < 0) { const char* e = getenv("ME1_VK_MIP_PARALLEL"); mip_par = (e && *e == '1'); }
        if (mip_par && dz > 1) {
            VkMipSliceJob js = { f->conv, pk, bsz, bx, by, ob, ow, oh, pitch, slice, src, q };
            gm_parallel_for((int)dz, vk_mip_slice_worker, &js, 1);
            g_vk_mip_par_slices += dz;
        } else {
            for (uint32_t k = 0; k < dz; k++) {
                const uint8_t* s0 = src + (size_t)k * slice; uint8_t* qk = q + (size_t)k * ow * oh * ob;
                dec_rect(f->conv, pk, bsz, bx, by, ob, s0, pitch, qk);   /* v1.2.1: on GEARMASS workers */
            }
        }
        GS.decoded++;
    } else if (f->conv == CV_RAW) {
        for (uint32_t k = 0; k < dz; k++) for (uint32_t rr = 0; rr < h; rr++) memcpy(q + ((size_t)k * h + rr) * w * ob, src + (size_t)k * slice + (size_t)rr * pitch, (size_t)w * ob);
    } else {
        int nc = f->nch;
        for (uint32_t k = 0; k < dz; k++) for (uint32_t rr = 0; rr < h; rr++) {
            const uint8_t* s = src + (size_t)k * slice + (size_t)rr * pitch; uint8_t* o = q + ((size_t)k * h + rr) * w * ob;
            for (uint32_t c = 0; c < w; c++) for (int i = 0; i < nc; i++) {
                uint16_t v; memcpy(&v, s + (size_t)(c * nc + i) * 2, 2);
                float fv = f->conv == CV_S16 ? fmaxf((float)(int16_t)v / 32767.f, -1.f) : (float)v / 65535.f;
                uint16_t hv = f2h(fv); memcpy(o + (size_t)(c * nc + i) * 2, &hv, 2);
            }
        }
    }
    VkCommandBuffer cb;
    if (r->mainuse == g_vserial) { rp_end(); cb = CS->cmd; } else cb = up_begin();
    uint32_t L = r->cube ? face : 0;
    img_to(cb, r, L, 1, (uint32_t)vl, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c; memset(&c, 0, sizeof c);
    c.bufferOffset = so; c.bufferRowLength = rowlen; c.bufferImageHeight = blk ? by * 4 : h;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)vl; c.imageSubresource.baseArrayLayer = L; c.imageSubresource.layerCount = 1;
    c.imageOffset.x = (int32_t)x; c.imageOffset.y = (int32_t)y; c.imageOffset.z = (int32_t)(r->is3d ? z : 0);
    c.imageExtent.width = w; c.imageExtent.height = h; c.imageExtent.depth = r->is3d ? dz : 1;
    if (!r->is3d && dz > 1) dz = 1;
    vkCmdCopyBufferToImage(cb, sb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    img_to(cb, r, L, 1, (uint32_t)vl, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    r->use = g_vserial;
}
static void vb_tex_unlock(Obj* t, uint32_t s, uint32_t lvl, const uint32_t* L, const uint8_t* p) {
    if (!L || !p) return;
    if (L[6] & 0x10u) return;
    VRes* r = vimg(t); if (!r || r->kind != 1) return;
    uint32_t pitch, rows, unit; int blk; layout(t->fmt, dimv(t->w, lvl), dimv(t->h, lvl), &pitch, &rows, &unit, &blk);
    uint32_t face = t->type == T_CUBE ? s / t->levels : 0;
    vtex_upload(t, r, face, lvl, L[0], L[1], L[4], L[2] - L[0], L[3] - L[1], L[5] - L[4], p, pitch, pitch * rows);
}
/* static vertex/index buffers: host-visible, renamed when the GPU may still read the old contents */
static VRes* vbuf(Obj* b) {
    if (g_vklost) return NULL;   /* v0.10.12 (P0-6): before calloc */
    if (b->gx) return b->gx;
    if (b->shadow) return NULL;
    VRes* r = calloc(1, sizeof *r); b->gx = r; r->serial = ++g_vser; r->kind = 3; r->size = b->size;
    if (!mk_buffer(b->size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &r->buf, &r->m)) { r->kind = 0; return NULL; }
    r->bytes = b->size; texmem_add((int64_t)r->bytes);
    return r;
}
static void vb_buf_unlock(Obj* b, uint32_t off, uint32_t end, uint32_t flags) {
    if (g_vklost) return;   /* v0.10.12 (P0-6) */
    if ((flags & 0x10u) || !b->lockv || !b->lockv[0] || end <= off) return;
    VRes* r = vbuf(b); if (!r || r->kind != 3) return;
    if (end > b->size) end = b->size;
    if (r->use > g_vdone && !(flags & 0x1000u)) {   /* in flight (and not NOOVERWRITE): a new buffer, the rest of the old contents copied */
        VkBuffer nb; VMem nm;
        if (mk_buffer(b->size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &nb, &nm)) {
            uint8_t *o = vmem_ptr(&r->m), *n = vmem_ptr(&nm);
            if (!(flags & 0x2000u) && o && n) { if (off) memcpy(n, o, off); if (end < b->size) memcpy(n + end, o + end, b->size - end); }
            dead_add(r->use, 3, H(r->buf), &r->m);
            r->buf = nb; r->m = nm; r->use = 0; GS.ring += 0;
        }
    }
    uint8_t* p = vmem_ptr(&r->m);
    if (p) memcpy(p + off, M + b->lockv[0] + off, end - off);
    GS.bufup++; GS.bufbytes += end - off;
}

/* ---- render passes, framebuffers, targets ---- */
typedef struct { VkFormat cf[4]; VkFormat df; int nc; VkRenderPass rp[64]; } RPC;
static RPC g_rpc[64]; static int g_nrpc;
static int rpc_id(const VkFormat* cf, int nc, VkFormat df) {
    for (int i = 0; i < g_nrpc; i++) if (g_rpc[i].nc == nc && g_rpc[i].df == df && !memcmp(g_rpc[i].cf, cf, sizeof g_rpc[i].cf)) return i;
    if (g_nrpc == 64) {   /* v0.10.12 (P0-7): class 0 has different formats - a pipeline with it is UB, not "compatible" */
        static int k; if (k++ < 3) vlog("render pass classes exhausted (%d formats) - draws are skipped\n", g_nrpc);
        return -1;
    }
    RPC* r = &g_rpc[g_nrpc]; memset(r, 0, sizeof *r); memcpy(r->cf, cf, sizeof r->cf); r->nc = nc; r->df = df;
    return g_nrpc++;
}
/* render pass of compatibility class id; clr bit i: clear color attachment i, bit 4 depth, bit 5 stencil (else load) */
static VkRenderPass rpc_get(int id, uint32_t clr) {
    RPC* c = &g_rpc[id]; clr &= 63;
    if (c->rp[clr]) return c->rp[clr];
    VkAttachmentDescription ad[5]; VkAttachmentReference cr[4], dr; int na = 0;
    memset(ad, 0, sizeof ad);
    for (int i = 0; i < c->nc; i++) {
        if (!c->cf[i]) { cr[i].attachment = VK_ATTACHMENT_UNUSED; cr[i].layout = VK_IMAGE_LAYOUT_UNDEFINED; continue; }
        VkAttachmentDescription* a = &ad[na]; a->format = c->cf[i]; a->samples = VK_SAMPLE_COUNT_1_BIT;
        a->loadOp = clr >> i & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; a->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a->stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a->stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a->initialLayout = a->finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        cr[i].attachment = (uint32_t)na++; cr[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (c->df) {
        VkAttachmentDescription* a = &ad[na]; a->format = c->df; a->samples = VK_SAMPLE_COUNT_1_BIT;
        a->loadOp = clr >> 4 & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; a->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a->stencilLoadOp = !VX.dsstencil ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : clr >> 5 & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a->stencilStoreOp = VX.dsstencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a->initialLayout = a->finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dr.attachment = (uint32_t)na++; dr.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkSubpassDescription sp; memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = (uint32_t)c->nc; sp.pColorAttachments = cr;
    sp.pDepthStencilAttachment = c->df ? &dr : NULL;
    VkSubpassDependency dep; memset(&dep, 0, sizeof dep);
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, (uint32_t)na, ad, 1, &sp, 1, &dep };
    vok(vkCreateRenderPass(VD, &ri, NULL, &c->rp[clr]), "vkCreateRenderPass");
    return c->rp[clr];
}
typedef struct { VRes* r; uint32_t face, lvl; } Att;   /* lvl: Vulkan level */
typedef struct { uint64_t k[5]; int rpc; uint32_t w, h, tick; VkFramebuffer fb; uint64_t use; } VFbo;
#define NVFB 64
static VFbo g_vfb[NVFB]; static uint32_t g_vfbtick;
static uint64_t att_key(const Att* a) { return a->r ? ((uint64_t)a->r->serial << 16 | a->face << 8 | a->lvl) : 0; }
static struct {
    Att c[4], ds; int nc, tf, open, rpc, secondary; uint32_t w, h, clr; VkClearValue cv[6]; VkFramebuffer fb; VkRenderPass pass;
} RP;
static int att_of(Obj* s, Att* a) {
    memset(a, 0, sizeof *a);
    if (!s) return 0;
    Obj* r = s->parent ? s->parent : s;
    VRes* g = vimg(r); if (!g) return 0;
    uint32_t lvl = s->parent ? s->level : 0;
    if ((int)lvl < g->drop) return 0;
    a->r = g; a->face = s->parent && r->type == T_CUBE ? s->face : 0; a->lvl = lvl - (uint32_t)g->drop;
    return 1;
}
static void att_size(const Att* a, uint32_t* w, uint32_t* h) { *w = dimv((uint32_t)a->r->w, a->lvl + (uint32_t)a->r->drop); *h = dimv((uint32_t)a->r->h, a->lvl + (uint32_t)a->r->drop); }
static VkFramebuffer fb_get(void) {
    uint64_t k[5]; for (int i = 0; i < 4; i++) k[i] = att_key(&RP.c[i]); k[4] = att_key(&RP.ds);
    g_vfbtick++; int v = 0;
    for (int i = 0; i < NVFB; i++) {
        VFbo* f = &g_vfb[i];
        if (f->fb && f->rpc == RP.rpc && f->w == RP.w && f->h == RP.h && !memcmp(f->k, k, sizeof k)) { f->tick = g_vfbtick; f->use = g_vserial; return f->fb; }
        if (!f->fb) { if (g_vfb[v].fb) v = i; } else if (g_vfb[v].fb && f->tick < g_vfb[v].tick) v = i;
    }
    VFbo* f = &g_vfb[v];
    if (f->fb) dead_add(f->use, 4, H(f->fb), NULL);
    memset(f, 0, sizeof *f); memcpy(f->k, k, sizeof k); f->rpc = RP.rpc; f->w = RP.w; f->h = RP.h; f->tick = g_vfbtick; f->use = g_vserial;
    VkImageView iv[5]; uint32_t n = 0;
    for (int i = 0; i < RP.nc; i++) if (RP.c[i].r) iv[n++] = att_view(RP.c[i].r, RP.c[i].face, RP.c[i].lvl);
    if (RP.ds.r) iv[n++] = att_view(RP.ds.r, 0, 0);
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rpc_get(RP.rpc, 0), n, iv, RP.w, RP.h, 1 };
    vok(vkCreateFramebuffer(VD, &fi, NULL, &f->fb), "vkCreateFramebuffer");
    GS.fbos++;
    return f->fb;
}
static void fb_purge(uint32_t serial) {
    for (int i = 0; i < NVFB; i++) {
        VFbo* f = &g_vfb[i]; if (!f->fb) continue;
        int hit = 0; for (int k = 0; k < 5; k++) if (f->k[k] && (uint32_t)(f->k[k] >> 16) == serial) hit = 1;
        if (hit) { dead_add(f->use, 4, H(f->fb), NULL); memset(f, 0, sizeof *f); }
    }
}
/* Experimental secondary rendering: immutable draw snapshots, at most four
 * independently owned command pools, joined before primary execution. */
typedef struct {
    VkPipeline pipe; VkDescriptorSet ubo, tex; uint32_t uoff[2];
    VkBuffer vb[17], ib; VkDeviceSize vo[17], io; uint32_t nb; VkIndexType it;
    VkViewport vp; VkRect2D sc; float bias, slope, bc[4]; uint32_t ref, mask, wmask;
    uint32_t count, instances, first; int32_t voff; int indexed;
} VSecDraw;
static VSecDraw g_secd[SEC_BATCH]; static int g_secn;
static void sec_encode(VkCommandBuffer cb, const VSecDraw* d) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 0, 1, &d->ubo, 2, d->uoff);
    if (d->tex) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 1, 1, &d->tex, 0, NULL);
    if (d->nb) vkCmdBindVertexBuffers(cb, 0, d->nb, d->vb, d->vo);
    if (d->indexed) vkCmdBindIndexBuffer(cb, d->ib, d->io, d->it);
    vkCmdSetViewport(cb, 0, 1, &d->vp); vkCmdSetScissor(cb, 0, 1, &d->sc);
    vkCmdSetDepthBias(cb, d->bias * 16777215.f, 0.f, d->slope);
    vkCmdSetBlendConstants(cb, d->bc);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, d->ref);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, d->mask);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, d->wmask);
    if (d->indexed) vkCmdDrawIndexed(cb, d->count, d->instances, d->first, d->voff, 0);
    else vkCmdDraw(cb, d->count, d->instances, d->first, 0);
}
typedef struct { int lanes, count; VkCommandBuffer cb[SEC_LANES]; VkResult result[SEC_LANES]; VkRenderPass pass; VkFramebuffer fb; } VSecJob;
static void sec_worker(void* ctx, int lo, int hi) {
    VSecJob* j = ctx;
    for (int i = lo; i < hi; i++) {
        VkCommandBufferInheritanceInfo inh = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL, j->pass, 0, j->fb, VK_FALSE, 0, 0 };
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &inh };
        VkResult r = vkBeginCommandBuffer(j->cb[i], &bi);
        if (r == VK_SUCCESS) {
            int a = j->count * i / j->lanes, b = j->count * (i + 1) / j->lanes;
            for (int k = a; k < b; k++) sec_encode(j->cb[i], &g_secd[k]);
            r = vkEndCommandBuffer(j->cb[i]);
        }
        j->result[i] = r;
    }
}
static VkResult sec_allocate(VSlot* s, int lane, VkCommandBuffer* out) {
    VSecLane* l = &s->sec[lane];
    if (g_sec_fail_test > 0 && --g_sec_fail_test == 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!l->pool) {
        VkCommandPoolCreateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, VQF };
        VkResult r = vkCreateCommandPool(VD, &ci, NULL, &l->pool); if (r != VK_SUCCESS) return r;
    }
    if (l->used == l->n) {
        if (l->n == l->cap) {
            uint32_t cap = l->cap ? l->cap * 2 : 16;
            VkCommandBuffer* p = realloc(l->cb, cap * sizeof *p); if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
            l->cb = p; l->cap = cap;
        }
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, l->pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1 };
        VkResult r = vkAllocateCommandBuffers(VD, &ai, &l->cb[l->n]); if (r != VK_SUCCESS) return r;
        l->n++;
    }
    *out = l->cb[l->used++]; return VK_SUCCESS;
}
static void rp_begin(void);
static void sec_invalidate_bindings(void) {
    VB.pipe = VK_NULL_HANDLE; VB.set0 = VB.set1 = VK_NULL_HANDLE;
    memset(VB.vb, 0, sizeof VB.vb); VB.ib = VK_NULL_HANDLE;
    VB.vp.width = -1.f; VB.sc.extent.width = UINT32_MAX;
    VB.bias[0] = VB.bias[1] = NAN; for (int i = 0; i < 4; i++) VB.bc[i] = NAN;
    VB.sref = VB.smask = VB.swmask = UINT32_MAX;
}
static void sec_flush(void) {
    if (!g_secn) return;
    VSecJob j; memset(&j, 0, sizeof j); j.count = g_secn;
    j.lanes = (g_secn + 15) / 16; if (j.lanes > SEC_LANES) j.lanes = SEC_LANES;
    j.pass = RP.pass; j.fb = RP.fb; VkResult err = VK_SUCCESS;
    for (int i = 0; i < j.lanes; i++) if ((err = sec_allocate(CS, i, &j.cb[i])) != VK_SUCCESS) break;
    if (err == VK_SUCCESS) {
        gm_parallel_for(j.lanes, sec_worker, &j, 1);
        for (int i = 0; i < j.lanes; i++) if (j.result[i] != VK_SUCCESS) { err = j.result[i]; break; }
    }
    if (err == VK_SUCCESS) {
        vkCmdExecuteCommands(CS->cmd, j.lanes, j.cb);
        g_sec_batches++; g_sec_draws += (uint64_t)g_secn;
        if (j.lanes > 1 && gm_k.workers > 0) g_sec_parallel++;
    } else {
        vok(err, "secondary command recording"); g_sec_fallback++;
        /* No secondary buffer was executed. Reopen the attachments with LOAD
         * and replay every snapshot in order on the primary command buffer. */
        if (!g_vklost) vkCmdEndRenderPass(CS->cmd);
        RP.open = 0; RP.secondary = 0; g_sec_on = 0;
        if (!g_vklost) {
            rp_begin();
            for (int k = 0; k < g_secn; k++) sec_encode(CS->cmd, &g_secd[k]);
            sec_invalidate_bindings();
            vlog("secondary recording failed; reverted to inline draws\n");
        }
    }
    g_secn = 0;
}
static void rp_end(void);
static void rp_begin_mode(int secondary) {
    if (RP.open && RP.secondary != secondary) rp_end();
    if (!g_sec_on) secondary = 0;
    if (RP.open || !RP.tf) return;
    VkCommandBuffer cb = CS->cmd;
    for (int i = 0; i < RP.nc; i++) if (RP.c[i].r) { Att* a = &RP.c[i]; img_to(cb, a->r, a->face, 1, a->lvl, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); a->r->use = a->r->mainuse = g_vserial; }
    if (RP.ds.r) { img_to(cb, RP.ds.r, 0, 1, 0, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL); RP.ds.r->use = RP.ds.r->mainuse = g_vserial; }
    VkClearValue cv[5]; uint32_t n = 0;
    for (int i = 0; i < RP.nc; i++) if (RP.c[i].r) cv[n++] = RP.cv[i];
    if (RP.ds.r) { cv[n].depthStencil.depth = RP.cv[4].depthStencil.depth; cv[n].depthStencil.stencil = RP.cv[5].depthStencil.stencil; n++; }
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rpc_get(RP.rpc, RP.clr), fb_get(), { { 0, 0 }, { RP.w, RP.h } }, n, cv };
    vkCmdBeginRenderPass(cb, &bi, secondary ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
    RP.pass = bi.renderPass; RP.fb = bi.framebuffer; RP.secondary = secondary; RP.open = 1; RP.clr = 0;
}
static void rp_begin(void) { rp_begin_mode(0); }
static void rp_end(void) {
    if (!RP.open && RP.clr && RP.tf) rp_begin();
    if (RP.open) {
        if (RP.secondary) sec_flush();
        if (g_vq_act) { vkCmdEndQuery(CS->cmd, g_vq_pool, g_vq_idx); g_vq_act = 0; }
        if (RP.open) { vkCmdEndRenderPass(CS->cmd); RP.open = 0; }
        RP.secondary = 0;
    }
}
/* the device's render targets; 0: nothing to draw into, else 1 | 2 (depth attached) | 4 (stencil bits) */
static int rp_targets(Dev* d) {
    Att c[4], ds;
    if (!att_of(d->rt[0], &c[0]) || c[0].r->kind != 1) return 0;
    for (int i = 1; i < 4; i++) if (!att_of(d->rt[i], &c[i]) || c[i].r->kind != 1) memset(&c[i], 0, sizeof c[i]);
    uint32_t W, H; att_size(&c[0], &W, &H);
    for (int i = 1; i < 4; i++) if (c[i].r) { uint32_t w, h; att_size(&c[i], &w, &h); if (w < W) W = w; if (h < H) H = h; }
    int tf = 1;
    if (att_of(d->ds, &ds) && ds.r->kind == 2) {
        uint32_t dw, dh; att_size(&ds, &dw, &dh);
        if (dw >= W && dh >= H) { tf |= 2; uint32_t f = d->ds->parent ? d->ds->parent->fmt : d->ds->fmt; if ((f == 75 || f == 83 || f == 73) && VX.dsstencil) tf |= 4; }
        else memset(&ds, 0, sizeof ds);
    } else memset(&ds, 0, sizeof ds);
    int same = RP.tf && RP.w == W && RP.h == H && att_key(&RP.ds) == att_key(&ds);
    for (int i = 0; i < 4 && same; i++) if (att_key(&RP.c[i]) != att_key(&c[i])) same = 0;
    if (same) { RP.tf = tf; return tf; }
    rp_end();
    memcpy(RP.c, c, sizeof c); RP.ds = ds; RP.w = W; RP.h = H; RP.tf = tf; RP.clr = 0;
    RP.nc = 0; VkFormat cf[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) if (c[i].r) { RP.nc = i + 1; cf[i] = c[i].r->vf; }
    RP.rpc = rpc_id(cf, RP.nc, ds.r ? VX.ds : VK_FORMAT_UNDEFINED);
    if (RP.rpc < 0) return 0;   /* v0.10.12 (P0-7): class table full - the draw is skipped instead of UB */
    return tf;
}
static void rp_forget(void) { rp_end(); memset(&RP, 0, sizeof RP); }

/* ---- programs: sm3_glsl GLSL ES 3.00 -> Vulkan GLSL ES 3.10 -> SPIR-V ---- */
typedef struct { char* p; size_t n, cap; } VSb;
static void vsb_put(VSb* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 4096; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void vsb_str(VSb* b, const char* s) { vsb_put(b, s, strlen(s)); }
static void vsb_f(VSb* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void vsb_f(VSb* b, const char* fmt, ...) { char t[600]; va_list ap; va_start(ap, fmt); int n = vsnprintf(t, sizeof t, fmt, ap); va_end(ap); if (n > 0) vsb_put(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1); }
static int vary_loc(const Sm3* ps, const char* name) {
    if (!ps) return -1;
    for (int k = 0; k < ps->nin; k++) { char vn[48]; snprintf(vn, sizeof vn, "V_%s%d", sm3_usage_name(ps->in[k].usage), ps->in[k].index); if (!strcmp(vn, name)) return k; }
    return -1;
}
static void vsb_repl(VSb* b, const char* from, const char* to) {   /* every occurrence, left to right (not in the inserted text) */
    size_t fl = strlen(from), tl = strlen(to), pos = 0; char* at;
    if (!fl) return;
    while (b->p && pos <= b->n && (at = strstr(b->p + pos, from))) {
        size_t i = (size_t)(at - b->p);
        if (b->n + tl - fl + 1 > b->cap) { b->cap = (b->n + tl + 1) * 2; b->p = realloc(b->p, b->cap); }
        memmove(b->p + i + tl, b->p + i + fl, b->n - i - fl + 1); memcpy(b->p + i, to, tl); b->n = b->n - fl + tl;
        pos = i + tl;
    }
}
/* imask / smask: vertex inputs fetched as unsigned / signed integers (no USCALED/SSCALED vertex format on the device) */
static char* vk_glsl(const char* src, int vs, const Sm3* ps, uint32_t imask, uint32_t smask, int nconst) {
    VSb o = { 0 };
    vsb_str(&o, "#version 310 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler3D;\n");
    if (vs) vsb_f(&o, "layout(std140, set = 0, binding = 0) uniform VSB { vec4 dx_fix; vec4 dx_vp; int dx_bgra; int vb; int dx_p0; int dx_p1; ivec4 vi[16]; vec4 vc[%d]; };\n", nconst > 0 ? nconst : 1);
    else vsb_f(&o, "layout(std140, set = 0, binding = 1) uniform PSB { vec4 dx_at; int dx_srgb; int dx_srgbw; int pb; int dx_q0; ivec4 pi[16]; vec4 pc[%d]; };\n", nconst > 0 ? nconst : 1);
    const char* s = src;
    while (*s) {
        const char* e = strchr(s, '\n'); size_t n = e ? (size_t)(e - s) : strlen(s);
        char ln[400]; size_t m = n < sizeof ln - 1 ? n : sizeof ln - 1; memcpy(ln, s, m); ln[m] = 0;
        int keep = 1; char ty[32], nm[64]; int r, k;
        if (!strncmp(ln, "#version", 8) || !strncmp(ln, "precision ", 10)) keep = 0;
        else if (!strncmp(ln, "uniform ", 8)) {
            keep = 0;
            if (sscanf(ln, "uniform %31s %63[^;];", ty, nm) == 2 && !strncmp(ty, "sampler", 7) && (nm[0] == 'p' || nm[0] == 'v') && sscanf(nm + 1, "s_s%d", &k) == 1)
                vsb_f(&o, "layout(set = 1, binding = %d) uniform %s %s;\n", nm[0] == 'v' ? 16 + k : k, ty, nm);
        } else if (vs && sscanf(ln, "out vec4 %63[^;];", nm) == 1 && !strncmp(nm, "V_", 2)) {
            int L = vary_loc(ps, nm); if (L >= 0) vsb_f(&o, "layout(location = %d) out vec4 %s;\n", L, nm); else vsb_f(&o, "vec4 %s;\n", nm);
            keep = 0;
        } else if (!vs && !strncmp(ln, "in ", 3) && strstr(ln, " V_")) {
            const char* v = strstr(ln, " V_") + 1; char nm2[64]; sscanf(v, "%63[^;];", nm2);
            vsb_f(&o, "layout(location = %d) %s\n", vary_loc(ps, nm2), ln); keep = 0;
        } else if (vs && sscanf(ln, "layout(location = %d) in vec4 dx_in%d;", &r, &k) == 2 && r < 32 && ((imask | smask) >> r & 1)) {
            vsb_f(&o, "layout(location = %d) in %s dx_in%d_i;\n#define dx_in%d vec4(dx_in%d_i)\n", r, smask >> r & 1 ? "ivec4" : "uvec4", r, r, r); keep = 0;
        }
        if (keep) { vsb_put(&o, s, n); vsb_put(&o, "\n", 1); }
        s += n; if (*s) s++;
    }
    vsb_repl(&o, "3.4028234e38", "uintBitsToFloat(0x7F7FFFFFu)");   /* glslang 15 turns literals >= ~3e38 into +inf */
    if (vs) {
        vsb_repl(&o, "P.z = 2.0 * P.z - P.w;", "");
        vsb_repl(&o, "(2.0 * P.z - 1.0) * W", "P.z * W");
        if (!strstr(o.p, "gl_PointSize")) vsb_repl(&o, "\n  d3d_main();\n", "\n  gl_PointSize = 1.0; d3d_main();\n");
    }
    return o.p;
}
typedef struct VPipe VPipe;
typedef struct VProg { struct VProg* next; uint32_t vs, ps, im, sm, id; int ok; VkShaderModule m[2]; VPipe* pipes; } VProg;
static VProg* g_vprog[PHASH]; static uint32_t g_vprogid;
static VkShaderModule mk_module(const unsigned* w, size_t n) {
    if (g_vklost) return VK_NULL_HANDLE;   /* v0.10.12 (P0-6) */
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n * 4, w };
    if (g_pipeidle) vkDeviceWaitIdle(VD);   /* v0.10.4: ME1_VK_PIPEIDLE - do not create objects while the queue runs */
    VkShaderModule m = VK_NULL_HANDLE; vok(vkCreateShaderModule(VD, &ci, NULL, &m), "vkCreateShaderModule"); return m;
}
static int spv_load(uint64_t key, unsigned** w, size_t* nv, size_t* nf) {
    if (!g_bincache[0]) return 0;
    char path[1000]; snprintf(path, sizeof path, "%s/vk_%016llx.spv", g_bincache, (unsigned long long)key);
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    uint32_t h[4]; int ok = 0;
    if (fread(h, 4, 4, f) == 4 && h[0] == 0x5631454Du && h[1] && h[2] && h[1] + h[2] < (8u << 20)) {
        *w = malloc((size_t)(h[1] + h[2]) * 4);
        if (fread(*w, 4, h[1] + h[2], f) == h[1] + h[2] && rt_crc32((uint8_t*)*w, (size_t)(h[1] + h[2]) * 4) == h[3]) { *nv = h[1]; *nf = h[2]; ok = 1; }
        else { free(*w); *w = NULL; }
    }
    fclose(f);
    if (!ok) unlink(path);
    return ok;
}
static void spv_store(uint64_t key, const unsigned* a, size_t na, const unsigned* b, size_t nb) {
    if (!g_bincache[0]) return;
    char path[1000], tmp[1010]; snprintf(path, sizeof path, "%s/vk_%016llx.spv", g_bincache, (unsigned long long)key); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    unsigned* all = malloc((na + nb) * 4); memcpy(all, a, na * 4); memcpy(all + na, b, nb * 4);
    uint32_t h[4] = { 0x5631454Du, (uint32_t)na, (uint32_t)nb, rt_crc32((uint8_t*)all, (na + nb) * 4) };   /* "ME1V" */
    FILE* f = fopen(tmp, "wb");
    if (f) { int ok = fwrite(h, 4, 4, f) == 4 && fwrite(all, 4, na + nb, f) == na + nb; ok &= fclose(f) == 0; if (ok) rename(tmp, path); else unlink(tmp); }
    free(all);
}
typedef struct { const char* src[2]; unsigned* w[2]; size_t n[2]; int ok[2]; char log[2][4096]; } SpvJob;
static void* spv_prewarm_main(void* a) {
    (void)a;
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "me1-spvwarm");
#endif
    /* the same "#version 310 es" as vk_glsl: glslang builds the built-in symbol tables of each stage once */
    static const char vs[] = "#version 310 es\nlayout(location = 0) in vec4 a;\nvoid main() { gl_Position = a; }\n";
    static const char fs[] = "#version 310 es\nprecision highp float;\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(1.0); }\n";
    for (int st = 0; st < 2; st++) {
        unsigned* w = NULL; size_t n = 0; char log[256];
        if (p_spv_compile(st, st ? fs : vs, &w, &n, log, sizeof log) && w) p_spv_free(w);
    }
    return NULL;
}
static void spv_prewarm(void) {
    const char* e = getenv("ME1_VK_SPV_MT"); if (e && *e == '0') return;
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 4 << 20);   /* glslang recurses */
    if (pthread_create(&t, &at, spv_prewarm_main, NULL) != 0) { /* compiled on first use as before */ }
    pthread_attr_destroy(&at);
}
static void spv_job(void* ctx, int i0, int i1) {
    SpvJob* j = ctx;
    for (int i = i0; i < i1; i++) j->ok[i] = p_spv_compile(i, j->src[i], &j->w[i], &j->n[i], j->log[i], sizeof j->log[i]) != 0;
}
static VProg* vprog_get(Obj* vso, Obj* pso, uint32_t im, uint32_t sm) {
    GlSh* vs = vso->gx; GlSh* ps = pso->gx;
    if (!vs || !ps) return NULL;
    if (!vs->done) gl_shader_ready(vso);
    if (!ps->done) gl_shader_ready(pso);
    uint32_t hsh = (vs->serial * 2654435761u ^ ps->serial * 40503u ^ im * 977u ^ sm * 131u) & (PHASH - 1);
    for (VProg* p = g_vprog[hsh]; p; p = p->next) if (p->vs == vs->serial && p->ps == ps->serial && p->im == im && p->sm == sm) return p;
    VProg* P = calloc(1, sizeof *P); P->vs = vs->serial; P->ps = ps->serial; P->im = im; P->sm = sm; P->id = ++g_vprogid;
    P->next = g_vprog[hsh]; g_vprog[hsh] = P;
    if (!vs->ok || !ps->ok) return P;
    if (!p_spv_compile) return P;
    double t0 = mono();
    uint64_t key = prog_key(vso, pso); key = fnv(key, "vk1", 3); key = fnv(key, &im, 4); key = fnv(key, &sm, 4);
    unsigned* w = NULL; size_t nv = 0, nf = 0; int cached = spv_load(key, &w, &nv, &nf);
    if (cached) { P->m[0] = mk_module(w, nv); P->m[1] = mk_module(w + nv, nf); free(w); GS.progbin++; }
    else {
        char* vsrc = sm3_glsl_vs(&vs->s, &ps->s); char* psrc = sm3_glsl_ps(&ps->s);
        char* a = vk_glsl(vsrc, 1, &ps->s, im, sm, vs->s.nconst); char* b = vk_glsl(psrc, 0, &ps->s, 0, 0, ps->s.nconst);
        /* v1.2.1: the vertex and the fragment shader compile to SPIR-V at the same time (a GEARMASS worker takes one;
         * glslang compiles independent shaders thread-safely; ME1_VK_SPV_MT=0 one after the other) */
        SpvJob* j = calloc(1, sizeof *j); j->src[0] = a; j->src[1] = b;
        static int mt = -1; if (mt < 0) { const char* e = getenv("ME1_VK_SPV_MT"); mt = !(e && *e == '0'); }
        if (mt) gm_parallel_for(2, spv_job, j, 1); else spv_job(j, 0, 2);
        unsigned *wa = j->w[0], *wb = j->w[1]; size_t na = j->n[0], nb = j->n[1];
        const char* what = !j->ok[0] ? "vertex" : !j->ok[1] ? "fragment" : NULL; const char* log = !j->ok[0] ? j->log[0] : j->log[1];
        char nm[96]; snprintf(nm, sizeof nm, "vkprog_%08x_%08x%s.glsl", vso->crc, pso->crc, what ? "_FAILED" : "");
        if (what) {
            GS.progfail++;
            static int n; if (n++ < 12) { vlog("program vs %08x + ps %08x: %s shader error:\n%.1500s\n", vso->crc, pso->crc, what, log);
                if (n <= 2 && !g_glsldump) vlog("source:\n%.4000s\n", what[0] == 'v' ? a : b); }
            dump_text(nm, a, b, log);
        } else {
            if (g_glsldump && g_gldbg) dump_text(nm, a, b, NULL);
            P->m[0] = mk_module(wa, na); P->m[1] = mk_module(wb, nb);
            spv_store(key, wa, na, wb, nb);
        }
        if (wa) p_spv_free(wa);
        if (wb) p_spv_free(wb);
        free(j); free(vsrc); free(psrc); free(a); free(b);
        if (what) return P;
    }
    GS.tbuilt = mono();
    P->ok = P->m[0] && P->m[1]; if (P->ok) GS.progs++;
    double dt = mono() - t0;
    if (g_log || dt > 0.25 || GS.progs <= 3) vlog("program %llu: vs %08x + ps %08x %s in %.1f ms\n", (unsigned long long)GS.progs, vso->crc, pso->crc, cached ? "loaded" : "built", dt * 1e3);
    return P;
}

/* ---- pipelines ---- */
typedef struct {
    uint32_t prog, rpc, sfront, sback, blend, wmask;
    uint8_t topo, cull, dbias, ztest, zwrite, zfunc, sten, nattr, nbind, pad[3];
    struct { uint8_t loc, bind; uint16_t off; uint32_t fmt; } a[16];
    struct { uint32_t stride, inst; } b[17];
} PKey;
struct VPipe { VPipe* next; VPipe* pnext; uint64_t h; PKey k; VkPipeline p; uint64_t use; };
#define NPH 4096
static VPipe* g_vpipe[NPH]; static uint64_t g_npipes;
static const VkBlendFactor g_vbf[16] = { VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR };
static void vblend_pair(uint32_t s, uint32_t d, VkBlendFactor* vs, VkBlendFactor* vd) {
    if (s == 12) { *vs = VK_BLEND_FACTOR_SRC_ALPHA; *vd = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; return; }
    if (s == 13) { *vs = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; *vd = VK_BLEND_FACTOR_SRC_ALPHA; return; }
    *vs = g_vbf[s & 15]; *vd = g_vbf[d & 15];
}
static VkBlendOp vbop(uint32_t o) { switch (o) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; } }
static VkCompareOp vcmp(uint32_t c) { return c >= 1 && c <= 8 ? (VkCompareOp)(c - 1) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp vsop(uint32_t o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
static VkStencilOpState vstencil(uint32_t s) {
    VkStencilOpState st; memset(&st, 0, sizeof st);
    st.compareOp = vcmp(s & 15); st.failOp = vsop(s >> 4 & 15); st.depthFailOp = vsop(s >> 8 & 15); st.passOp = vsop(s >> 12 & 15);
    st.compareMask = st.writeMask = 0xFF; return st;
}
/* v1.2.3: experimental overlap of a single pipeline creation with independent UBO work.
 * The submitting draw always waits: no speculative pipeline and no missing draws. Only the
 * caller touches the cache/render-pass lists; the worker uses stack-backed create info until
 * the caller joins the task. PIPEIDLE forces the original serial path for fragile drivers. */
typedef struct { VkDevice dev; VkPipelineCache cache; const VkGraphicsPipelineCreateInfo* info; VkPipeline pipe; VkResult result; } VPipeTask;
static struct { pthread_mutex_t mx; pthread_cond_t go, done; pthread_t thread; VPipeTask* task; int tried, available, complete; }
    g_vpa = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER };
static uint64_t g_vpa_jobs, g_vpa_wide;
static double g_vpa_work_s, g_vpa_wait_s;
static void* vpipe_worker(void* unused) {
    (void)unused; pthread_setname_np(pthread_self(), "me1-vkpipe");
    for (;;) {
        pthread_mutex_lock(&g_vpa.mx);
        while (!g_vpa.task) pthread_cond_wait(&g_vpa.go, &g_vpa.mx);
        VPipeTask* t = g_vpa.task; pthread_mutex_unlock(&g_vpa.mx);
        t->result = vkCreateGraphicsPipelines(t->dev, t->cache, 1, t->info, NULL, &t->pipe);
        pthread_mutex_lock(&g_vpa.mx); g_vpa.task = NULL; g_vpa.complete = 1;
        pthread_cond_signal(&g_vpa.done); pthread_mutex_unlock(&g_vpa.mx);
    }
    return NULL;
}
static int vpipe_async_enabled(void) {
    const char* e = getenv("ME1_VK_ASYNC_PIPE");
    if (!e || (strcmp(e, "1") && strcmp(e, "2")) || g_pipeidle) return 0;
    if (!g_vpa.tried) {
        g_vpa.tried = 1; pthread_attr_t a; pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (!pthread_create(&g_vpa.thread, &a, vpipe_worker, NULL)) {
            g_vpa.available = 1; vlog("experimental async pipeline worker enabled\n");
        } else vlog("async pipeline worker unavailable; using serial creation\n");
        pthread_attr_destroy(&a);
    }
    return g_vpa.available;
}
static VkPipeline pipe_get(VProg* P, const PKey* k, void (*work)(void*), void* arg, int wide) {
    if (g_vklost) return VK_NULL_HANDLE;   /* v0.10.12 (P0-6) */
    uint64_t h = fnv(0xcbf29ce484222325ull, k, sizeof *k);
    VPipe** bucket = &g_vpipe[h & (NPH - 1)];
    for (VPipe* p = *bucket; p; p = p->next) if (p->h == h && !memcmp(&p->k, k, sizeof *k)) { p->use = g_vserial; g_pc_hits++; if (p->p) work(arg); return p->p; }
    g_pc_misses++;
    double t0 = mono();
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    for (int i = 0; i < 2; i++) { st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
                                  st[i].module = P->m[i]; st[i].pName = "main"; }
    VkVertexInputBindingDescription bd[17]; VkVertexInputAttributeDescription ad[16];
    for (int i = 0; i < k->nbind; i++) { bd[i].binding = (uint32_t)i; bd[i].stride = k->b[i].stride; bd[i].inputRate = k->b[i].inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX; }
    for (int i = 0; i < k->nattr; i++) { ad[i].location = k->a[i].loc; ad[i].binding = k->a[i].bind; ad[i].format = (VkFormat)k->a[i].fmt; ad[i].offset = k->a[i].off; }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, k->nbind, bd, k->nattr, ad };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, (VkPrimitiveTopology)k->topo, VK_FALSE };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = k->cull == 2 ? VK_CULL_MODE_FRONT_BIT : k->cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.depthBiasEnable = k->dbias; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds; memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; ds.depthTestEnable = k->ztest; ds.depthWriteEnable = k->ztest && k->zwrite;
    ds.depthCompareOp = vcmp(k->zfunc); ds.stencilTestEnable = k->sten; ds.front = vstencil(k->sfront); ds.back = vstencil(k->sback);
    ds.maxDepthBounds = 1.f;
    RPC* rc = &g_rpc[k->rpc];
    VkPipelineColorBlendAttachmentState cb[4]; memset(cb, 0, sizeof cb);
    for (int i = 0; i < rc->nc; i++) {
        uint32_t b = k->blend; int ii = VFE.independentBlend ? i : 0;
        int be = (b & 1) && rc->cf[i] && (fmt_opt(rc->cf[i]) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
        if (!VFE.independentBlend && rc->cf[0]) be = (b & 1) && (fmt_opt(rc->cf[0]) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
        cb[i].blendEnable = be ? VK_TRUE : VK_FALSE;
        vblend_pair(b >> 1 & 15, b >> 5 & 15, &cb[i].srcColorBlendFactor, &cb[i].dstColorBlendFactor);
        vblend_pair(b >> 9 & 15, b >> 13 & 15, &cb[i].srcAlphaBlendFactor, &cb[i].dstAlphaBlendFactor);
        cb[i].colorBlendOp = vbop(b >> 17 & 7); cb[i].alphaBlendOp = vbop(b >> 20 & 7);
        cb[i].colorWriteMask = k->wmask >> (4 * ii) & 15;
    }
    VkPipelineColorBlendStateCreateInfo cbs; memset(&cbs, 0, sizeof cbs);
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = (uint32_t)rc->nc; cbs.pAttachments = cb;
    static const VkDynamicState dyn[7] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 7, dyn };
    VkGraphicsPipelineCreateInfo pi; memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vps; pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = rc->df ? &ds : NULL;
    pi.pColorBlendState = &cbs; pi.pDynamicState = &dy; pi.layout = g_pl; pi.renderPass = rpc_get(k->rpc, 0); pi.subpass = 0; pi.basePipelineIndex = -1;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult pr;
    if (vpipe_async_enabled()) {
        if (wide) { static int noted; if (!noted++) vlog("async pipeline: wide draw preparation enabled\n"); }
        VPipeTask t = { VD, g_vpc, &pi, VK_NULL_HANDLE, VK_ERROR_UNKNOWN };
        pthread_mutex_lock(&g_vpa.mx); g_vpa.complete = 0; g_vpa.task = &t;
        pthread_cond_signal(&g_vpa.go); pthread_mutex_unlock(&g_vpa.mx);
        double w0 = mono(); work(arg); double w1 = mono();
        pthread_mutex_lock(&g_vpa.mx);
        while (!g_vpa.complete) pthread_cond_wait(&g_vpa.done, &g_vpa.mx);
        pthread_mutex_unlock(&g_vpa.mx);
        g_vpa_jobs++; g_vpa_wide += wide != 0; g_vpa_work_s += w1 - w0; g_vpa_wait_s += mono() - w1;
        p = t.pipe; pr = t.result;
    } else {
        if (g_pipeidle) vkDeviceWaitIdle(VD);   /* retain Mali workaround */
        pr = vkCreateGraphicsPipelines(VD, g_vpc, 1, &pi, NULL, &p);
        if (pr >= 0 && p) work(arg);
    }
    if (!vok(pr, "vkCreateGraphicsPipelines")) p = VK_NULL_HANDLE;
    VPipe* e = calloc(1, sizeof *e); e->h = h; e->k = *k; e->p = p; e->use = g_vserial; e->next = *bucket; *bucket = e; e->pnext = P->pipes; P->pipes = e;
    g_npipes++; g_vpc_new++;
    double dt = mono() - t0;
    if (g_log || dt > 0.1 || g_npipes <= 3) vlog("pipeline %llu (program %u) in %.1f ms\n", (unsigned long long)g_npipes, P->id, dt * 1e3);
    return p;
}
static void vprog_drop(uint32_t serial) {
    for (int i = 0; i < PHASH; i++) for (VProg** pp = &g_vprog[i]; *pp;) {
        VProg* p = *pp;
        if (p->vs != serial && p->ps != serial) { pp = &p->next; continue; }
        *pp = p->next;
        for (VPipe* e = p->pipes; e;) {
            VPipe** b = &g_vpipe[e->h & (NPH - 1)]; while (*b && *b != e) b = &(*b)->next;
            if (*b) *b = e->next;
            if (e->p) dead_add(e->use, 5, H(e->p), NULL);
            if (VB.pipe == e->p) VB.pipe = VK_NULL_HANDLE;
            VPipe* nx = e->pnext; free(e); e = nx;
        }
        dead_add(g_vserial, 6, H(p->m[0]), NULL); dead_add(g_vserial, 6, H(p->m[1]), NULL);
        free(p);
    }
}

/* ---- samplers ---- */
typedef struct { uint64_t k; uint32_t border; VkSampler s; } VSmp;
static VSmp g_vsmp[NSMP]; static int g_nvsmp; static VkSampler g_vsmp_near;
static VkSamplerAddressMode vwrap(uint32_t a) {
    switch (a) { case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                 case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; default: return VK_SAMPLER_ADDRESS_MODE_REPEAT; }
}
static VkSampler vsampler(const uint32_t* ss, const VRes* g, const Obj* t) {
    uint32_t u = ss[1] & 7, v = ss[2] & 7, w = ss[3] & 7, mag = ss[5] & 7, mn = ss[6] & 7, mip = ss[7] & 7;
    int nearest = !g->filt;
    uint32_t an = 1;
    uint32_t af = (uint32_t)gm_k.aniso;   /* v1.2.1: GEARMASS anisotropy floor for mipmapped linear sampling */
    if (VFE.samplerAnisotropy && ((mn == 3 || mag == 3) || (af > 1 && mn >= 2 && mip && !nearest))) {
        an = (mn == 3 || mag == 3) && ss[10] ? ss[10] : 1; if (an < af) an = af;
        if (an > (uint32_t)VX.maxaniso) an = (uint32_t)VX.maxaniso; if (an > 16) an = 16; if (sm3_lite && an > 2) an = 2; if (!an) an = 1;
    }
    int lod = (int)(t->lod > ss[9] ? t->lod : ss[9]) - g->drop; if (lod < 0) lod = 0; if (lod > g->lv - 1) lod = g->lv - 1; if (lod > 15) lod = 15;
    if (g->lv <= 1) mip = 0;
    uint32_t border = (u == 4 || v == 4 || w == 4) ? ss[4] : 0;
    int bc = !border ? 0 : (border >> 24) < 128 ? 0 : ((border >> 16 & 255) + (border >> 8 & 255) + (border & 255)) >= 384 ? 2 : 1;
    uint64_t k = u | v << 3 | w << 6 | mag << 9 | mn << 12 | mip << 15 | (uint64_t)an << 18 | (uint64_t)nearest << 23 | (uint64_t)lod << 24 | (uint64_t)bc << 28;
    uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & (NSMP - 1);
    for (int i = 0; i < NSMP; i++, h = (h + 1) & (NSMP - 1)) {
        VSmp* e = &g_vsmp[h];
        if (e->s && e->k == k) return e->s;
        if (e->s) continue;
        if (g_nvsmp >= NSMP * 3 / 4) break;
        VkSamplerCreateInfo si; memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = mag >= 2 && !nearest ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; si.minFilter = mn >= 2 && !nearest ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = mip >= 2 && !nearest ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = vwrap(u); si.addressModeV = vwrap(v); si.addressModeW = vwrap(w);
        si.anisotropyEnable = an > 1; si.maxAnisotropy = (float)an;
        if (mip == 0) { si.minLod = 0.f; si.maxLod = 0.25f; } else { si.minLod = (float)lod; si.maxLod = VK_LOD_CLAMP_NONE; }
        si.borderColor = bc == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : bc == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        if (!vok(vkCreateSampler(VD, &si, NULL, &e->s), "vkCreateSampler")) { e->s = VK_NULL_HANDLE; break; }
        e->k = k; e->border = border; g_nvsmp++;
        return e->s;
    }
    return g_vsmp_near;
}

/* ---- submission ---- */
static uint64_t g_tsmask = ~0ull;
static struct {
    void* win; VkSurfaceKHR surf; VkSwapchainKHR sc; VkImage img[16]; VkSemaphore rs[16]; uint32_t n; VkFormat fmt; VkExtent2D ext;
    int fake, cpu, recreate, lost, failed, gen, ww, wh, gw, gh; VkExtent2D cur;
    int mgen;   /* v1.1: Me1Window.gen the swap chain was made for */
} VW = { .gen = -1 };
static void vcpu_post(VSlot* s);
/* VK_SUBOPTIMAL_KHR: Android 10+ returns it on every acquire/present while preTransform (identity: the compositor rotates)
   differs from the display rotation (landscape game on a portrait panel). Rebuilding the swap chain for it meant a
   vkDeviceWaitIdle + new window buffers every frame (v0.10.0: CPU and GPU serialized, no frames on some drivers).
   Now only a changed window size rebuilds it (checked at most every 0.25 s); OUT_OF_DATE still always does. */
static double g_subopt_t; static uint64_t g_subopt_n; static int g_subopt_test = -1;   /* ME1_VK_TEST_SUBOPT=1: report SUCCESS as SUBOPTIMAL (tests) */
static VkResult subopt_test(VkResult r) {
    if (g_subopt_test < 0) { const char* e = getenv("ME1_VK_TEST_SUBOPT"); g_subopt_test = e && *e == '1'; }
    return g_subopt_test && r == VK_SUCCESS ? VK_SUBOPTIMAL_KHR : r;
}
static void vswap_subopt(void) {
    g_subopt_n++;
    double t = mono();
    if (g_subopt_t > 0 && t - g_subopt_t < 0.25) return;
    g_subopt_t = t;
    VkSurfaceCapabilitiesKHR c; memset(&c, 0, sizeof c);
    if (!VW.surf || vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VP, VW.surf, &c) != VK_SUCCESS) { VW.recreate = 1; return; }
    if (c.currentExtent.width == 0xFFFFFFFFu) return;
    if (c.currentExtent.width != VW.cur.width || c.currentExtent.height != VW.cur.height) {
        static int k; if (k++ < 10) vlog("swap chain suboptimal: window %ux%u -> %ux%u, recreating\n", VW.cur.width, VW.cur.height, c.currentExtent.width, c.currentExtent.height);
        VW.recreate = 1;
    } else { static int k; if (!k++) vlog("swap chain suboptimal (transform 0x%x, the compositor rotates) - kept, not recreated\n", (unsigned)c.currentTransform); }
}
static void slot_finish(VSlot* s) {
    if (!s->pending) return;
    s->pending = 0;
    if (s->serial > g_vdone) g_vdone = s->serial;
    if (s->qon) {
        uint64_t t[2] = { 0, 0 };
        if (vkGetQueryPoolResults(VD, s->qp, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double ns = (double)((t[1] - t[0]) & g_tsmask) * VX.tsperiod;
            if (ns > 0 && ns < 5e9) { VT.ns += (uint64_t)ns; if (s->eof) VT.nf++; gm_gpu_ns((uint64_t)ns, s->eof); }   /* v1.2.1: GEARMASS */
        }
        s->qon = 0;
    }
    if (s->rbpend) { vcpu_post(s); s->rbpend = 0; }
    if (s->on) {   /* v1.1.3: occlusion results of this slot, kept until it is recorded again */
        uint32_t* r = g_ores[s - S];
        for (uint32_t k = 0; k < s->nop && k * VQP < s->on; k++) {
            uint32_t m = s->on - k * VQP; if (m > VQP) m = VQP;
            memset(r + k * VQP, 0xFF, m * 4);
            VkResult e = vkGetQueryPoolResults(VD, s->op[k], 0, m, m * 4, r + k * VQP, 4, 0);
            if (e != VK_SUCCESS && e != VK_NOT_READY) memset(r + k * VQP, 0xFF, m * 4);
        }
    }
    s->ost = 2;
}
static VkDeviceSize ring_used(const Ring* r) { VkDeviceSize n = 0; for (int i = 0; i < r->n && i <= r->cur; i++) n += r->c[i].off; return n; }
/* end the command buffers of this slot, submit them (idx >= 0: after acquiring swap chain image idx, then present it), start the next slot */
static void vb_submit(int32_t idx, int wait, int eof) {
    VSlot* s = CS;
    rp_end();
    if (g_vklost) return;
    if (s->qon) vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->qp, 1);
    vkEndCommandBuffer(s->cmd);
    /* v0.10.12 (P0-0): the final submit carries only [up?, cmd]; earlier chunks were already submitted once by
     * vk_chunk_flush and are covered by this fence through queue order - resubmitting them is a VUID violation */
    VkCommandBuffer cbs[2]; uint32_t n = 0;
    if (s->upon) { vkEndCommandBuffer(s->up); cbs[n++] = s->up; s->upon = 0; }
    cbs[n++] = s->cmd;
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si; memset(&si, 0, sizeof si); si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (idx >= 0) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s->acq; si.pWaitDstStageMask = &ws; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &VW.rs[idx]; }
    si.commandBufferCount = n; si.pCommandBuffers = cbs;
    /* Test-only: simulate a failed frame submission without calling the driver. */
    static int fake_submit_fired;
    const char* fs = getenv("ME1_VK_FAKELOST_SUBMIT");
    int inject = eof && !fake_submit_fired && fs && *fs && g_pframes == (uint64_t)strtoull(fs, NULL, 10);
    if (inject) fake_submit_fired = 1;
    VkResult r = inject || g_vklost ? VK_ERROR_DEVICE_LOST : vkQueueSubmit(VQ, 1, &si, s->fence);
    s->serial = g_vserial; s->eof = eof; VT.subs++;
    s->pending = g_vklost ? 0 : vok(r, "vkQueueSubmit");
    s->ost = s->pending ? 1 : 3;
    if (!s->pending) {   /* nothing of it runs: its resources are free, the acquire semaphore may stay signaled */
        if (!g_vklost) vkDeviceWaitIdle(VD);
        if (s->serial > g_vdone) g_vdone = s->serial;
        s->rbpend = 0; s->qon = 0;
        if (idx >= 0 && !g_vklost) { vkDestroySemaphore(VD, s->acq, NULL); VkSemaphoreCreateInfo sc = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0 }; vkCreateSemaphore(VD, &sc, NULL, &s->acq); VW.recreate = 1; }
    } else if (idx >= 0) {
        uint32_t ui = (uint32_t)idx;
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &VW.rs[idx], 1, &VW.sc, &ui, NULL };
        VkResult pr = subopt_test(vkQueuePresentKHR(VQ, &pi));
        if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) g_shown++;
        if (pr == VK_ERROR_OUT_OF_DATE_KHR) VW.recreate = 1;
        else if (pr == VK_SUBOPTIMAL_KHR) vswap_subopt();
        else if (pr == VK_ERROR_SURFACE_LOST_KHR) VW.lost = 1;
        else if (pr != VK_SUCCESS) vok(pr, "vkQueuePresentKHR");
    }
    if (g_vklost) return;   /* do not touch pools or semaphores of a lost device */
    g_vserial++;
    VSlot* done = s;
    g_si = (g_si + 1) % NSLOT;
    slot_begin();
    if (g_vklost) return;
    if (wait && done->pending && vok(vkWaitForFences(VD, 1, &done->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences")) slot_finish(done);
}
/* a long frame without Present (loading): submit what is recorded so that staging memory gets recycled */
static void vb_maybe_flush(void) { if (ring_used(&CS->stg) > (48u << 20) || ring_used(&CS->vtx) > (32u << 20)) vb_submit(-1, 0, 0); }
/* render targets and depth surfaces start cleared (D3D drivers hand out zeroed memory; tilers would show garbage) */
static void img_clear_new(VRes* r) {
    VkCommandBuffer cb = up_begin();
    img_to(cb, r, 0, (uint32_t)r->layers, 0, (uint32_t)r->lv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange sr = { res_aspect(r), 0, (uint32_t)r->lv, 0, (uint32_t)r->layers };
    if (r->kind == 2) { VkClearDepthStencilValue v = { 1.f, 0 }; vkCmdClearDepthStencilImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &sr); }
    else { VkClearColorValue v; memset(&v, 0, sizeof v); vkCmdClearColorImage(cb, r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &sr); }
    r->use = g_vserial;
}

/* ---- vertex input ---- */
static VkBuffer g_vconst; static VMem g_vconstm;   /* (0, 0, 0, 1) at 0, (1, 1, 1, 1) at 16: attributes without data */
typedef struct {
    int na, nb; uint32_t im, sm; int bgra; int32_t first, voff;
    struct { uint8_t loc, bind; uint16_t off; uint32_t fmt; } a[16];
    struct { uint32_t stride, inst; VkBuffer buf; VkDeviceSize off; } b[17];
} VLay;
/* D3DDECLTYPE -> Vulkan vertex format; *isint 1/2: fetched as uint/int (no USCALED/SSCALED), *bg: D3DCOLOR read as RGBA, swizzled */
static VkFormat vattr_fmt(uint32_t t, int* isint, int* bg) {
    static const VkFormat F[17] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_A2B10G10R10_USCALED_PACK32,
        VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT };
    if (t > 16) return VK_FORMAT_UNDEFINED;
    VkFormat f = F[t];
    if (fmt_buf(f) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) return f;
    switch (t) {
    case 4: f = VK_FORMAT_R8G8B8A8_UNORM; *bg = 1; break;
    case 5: f = VK_FORMAT_R8G8B8A8_UINT; *isint = 1; break;
    case 6: f = VK_FORMAT_R16G16_SINT; *isint = 2; break;
    case 7: f = VK_FORMAT_R16G16B16A16_SINT; *isint = 2; break;
    case 13: f = VK_FORMAT_A2B10G10R10_UINT_PACK32; *isint = 1; break;
    default: return VK_FORMAT_UNDEFINED;
    }
    return (fmt_buf(f) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? f : VK_FORMAT_UNDEFINED;
}
static int vsetup(Dev* d, const Sm3* vs, const DrawCall* dc, VLay* L) {
    memset(L, 0, sizeof *L);
    VElem el[65]; int ne = 0;
    if (d->decl && d->decl->blob) {
        const uint8_t* b = d->decl->blob;
        for (; ne < 64 && b[8 * ne] != 0xFF; ne++) {
            VElem* e = &el[ne]; memcpy(&e->stream, b + 8 * ne, 2); memcpy(&e->off, b + 8 * ne + 2, 2);
            e->type = b[8 * ne + 4]; e->method = b[8 * ne + 5]; e->usage = b[8 * ne + 6]; e->index = b[8 * ne + 7];
        }
    } else if (d->fvf) ne = fvf_decl(d->fvf, el);
    /* D3D vertex range of per-vertex streams */
    int64_t lo, hi;
    if (dc->indexed) { lo = (int64_t)dc->minv + (dc->up ? 0 : dc->base); hi = lo + dc->nv; } else { lo = dc->up ? 0 : dc->start; hi = lo + dc->count; }
    /* streams copied into the ring hold vertices [lo, hi): then every per-vertex stream is addressed from lo */
    const VElem* pick[16]; int ring = dc->up;
    for (int k = 0; k < vs->nin; k++) {
        const Sm3Sem* in = &vs->in[k]; const VElem* e = NULL;
        for (int i = 0; i < ne; i++) if (el[i].usage == in->usage && el[i].index == in->index && el[i].type <= 16) { e = &el[i]; break; }
        if (!e && in->usage == 0) for (int i = 0; i < ne; i++) if (el[i].usage == 9 && el[i].index == in->index && el[i].type <= 16) { e = &el[i]; break; }
        pick[k] = e;
        if (e && !dc->up) { Obj* vb = d->vb[e->stream & 15]; if (vb && vb->shadow) ring = 1; }
    }
    if (ring && lo < 0) { static int n; if (n++ < 10) vlog("%s: negative vertex range (base vertex %d) - draw skipped\n", g_cur, dc->base); return -1; }
    int bmap[16]; for (int i = 0; i < 16; i++) bmap[i] = -1;
    int cbind = -1;
    for (int k = 0; k < vs->nin && k < 16; k++) {
        const Sm3Sem* in = &vs->in[k]; int loc = in->reg & 15; const VElem* e = pick[k];
        int s = e ? e->stream & 15 : 0;
        Obj* vb = dc->up ? NULL : (e ? d->vb[s] : NULL);
        int isint = 0, bg = 0;
        VkFormat f = e && (dc->up || vb) ? vattr_fmt(e->type, &isint, &bg) : VK_FORMAT_UNDEFINED;
        if (e && (dc->up || vb) && !f) { static int n; if (n++ < 10) vlog("%s: vertex type %u not supported by the driver - attribute zeroed\n", g_cur, e->type); }
        if (!f) {
            if (cbind < 0) { cbind = L->nb++; L->b[cbind].buf = g_vconst; L->b[cbind].off = 0; L->b[cbind].stride = 0; }
            int one = vs->rhw && in->usage == 10 && in->index == 0;   /* fixed function: a missing diffuse color is white */
            L->a[L->na].loc = (uint8_t)loc; L->a[L->na].bind = (uint8_t)cbind; L->a[L->na].off = one ? 16 : 0; L->a[L->na].fmt = VK_FORMAT_R32G32B32A32_SFLOAT; L->na++;
            continue;
        }
        uint32_t stride = dc->up ? dc->upstride : d->vbstride[s];
        int inst = !dc->up && dc->ninst > 1 && (d->vbfreq[s] & 0x80000000u);
        if (bmap[s] < 0) {
            int b = bmap[s] = L->nb++;
            uint32_t off0 = dc->up ? 0 : d->vboff[s];
            int64_t slo = ring ? lo : 0, shi = hi;
            if (inst) {
                uint32_t div = d->vbfreq[s] & 0x3FFFFFFFu; if (!div) div = 1;
                if (div > 1) { static int n; if (n++ < 5) vlog("%s: instance divisor %u not supported (no VK_EXT_vertex_attribute_divisor) - treated as 1\n", g_cur, div); }
                slo = 0; shi = dc->ninst;
            }
            if (!stride) { slo = 0; shi = 1; }
            VRes* g = (!dc->up && !vb->shadow) ? vbuf(vb) : NULL;
            if (g && g->kind == 3) {
                int64_t o = (int64_t)off0 + slo * stride;
                if (o < 0 || (uint64_t)o >= g->size) { static int n; if (n++ < 10) vlog("%s: vertex stream %d offset %lld outside the buffer - draw skipped\n", g_cur, s, (long long)o); return -1; }
                L->b[b].buf = g->buf; L->b[b].off = (VkDeviceSize)o; g->use = g_vserial;
            } else if (!dc->up && !vb->shadow) return -1;
            else {
                uint32_t src = dc->up ? dc->upv : d->vbsh[s];   /* v1.2.1: the shadow at the draw (Lock may rename it) */
                uint64_t a = (uint64_t)off0 + (uint64_t)slo * stride, len = (uint64_t)(shi - slo) * stride;
                if (!stride) len = 256;
                if (!dc->up) { if (a >= vb->size) return -1; if (a + len > vb->size) len = vb->size - a; }
                if (!len || len > (256u << 20)) return -1;
                VkBuffer rb; VkDeviceSize ro; uint8_t* p = ring_alloc(&CS->vtx, len, 16, &rb, &ro, NULL);
                if (!p) return -1;
                memcpy(p, M + src + a, (size_t)len); GS.ring += len;
                L->b[b].buf = rb; L->b[b].off = ro;
            }
            L->b[b].stride = stride; L->b[b].inst = (uint32_t)inst;
        }
        L->a[L->na].loc = (uint8_t)loc; L->a[L->na].bind = (uint8_t)bmap[s]; L->a[L->na].off = e->off; L->a[L->na].fmt = (uint32_t)f; L->na++;
        if (isint == 1) L->im |= 1u << loc; else if (isint == 2) L->sm |= 1u << loc;
        if (bg) L->bgra |= 1 << loc;
    }
    if (L->nb > (int)VPP.limits.maxVertexInputBindings) return -1;
    if (ring) { L->first = 0; L->voff = dc->indexed ? -(int32_t)dc->minv : 0; }
    else { L->first = dc->indexed ? 0 : (int32_t)dc->start; L->voff = dc->indexed ? dc->base : 0; }
    return 0;
}

/* ---- draws ---- */
static VRes g_vdummy[3];   /* 1x1 (0, 0, 0, 1): 2D, cube, 3D */
static struct { uint64_t k, serial; VkDescriptorSet s; } g_tcache[1024];
static int rp_is_att(const VRes* g) {
    if (RP.ds.r == g) return 1;
    for (int i = 0; i < RP.nc; i++) if (RP.c[i].r == g) return 1;
    return 0;
}
/* the texture descriptor set of the program's samplers (PS 0-15 -> bindings 0-15, VS 0-3 -> 16-19); *srgb: PS sRGB mask */
static VkDescriptorSet vtex_bind(Dev* d, const Sm3* vs, const Sm3* ps, int* srgb) {
    VkDescriptorImageInfo ii[20]; uint32_t bn[20]; int n = 0;
    uint64_t key = 0xcbf29ce484222325ull;
    for (int j = 0; j < 20; j++) {
        int want = j < 16 ? ps->samp[j] : vs->samp[j - 16]; if (!want) continue;
        int idx = j < 16 ? j : 17 + (j - 16);
        Obj* t = d->tex[idx]; VRes* g = t ? vimg(t) : NULL;
        int ok = g && g->kind == 1 && (want == 3 ? g->cube : want == 4 ? g->is3d : (!g->cube && !g->is3d));
        if (ok && rp_is_att(g)) { ok = 0; static int k; if (k++ < 10) vlog("%s: sampler %d reads the render target being drawn - dummy texture bound\n", g_cur, idx); }
        else if (!ok && g && g->kind == 1) { static int k; if (k++ < 10) vlog("sampler %d: %s texture bound where the shader samples %s\n", idx, g->cube ? "cube" : g->is3d ? "volume" : "2D", want == 3 ? "cube" : want == 4 ? "volume" : "2D"); }
        if (!ok) { VRes* z = &g_vdummy[want == 3 ? 1 : want == 4 ? 2 : 0]; ii[n].sampler = g_vsmp_near; ii[n].imageView = z->sview; }
        else {
            if (img_needs(g, 0, (uint32_t)g->layers, 0, (uint32_t)g->lv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
                rp_end(); img_to(CS->cmd, g, 0, (uint32_t)g->layers, 0, (uint32_t)g->lv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            g->use = g->mainuse = g_vserial;
            ii[n].sampler = vsampler(d->ss[idx], g, t); ii[n].imageView = g->sview;
            if (j < 16 && d->ss[idx][11]) *srgb |= 1 << j;
        }
        ii[n].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bn[n] = (uint32_t)j; key = fnv(key, &ii[n], sizeof ii[n]); key = fnv(key, &bn[n], 4); n++;
    }
    if (!n) return VK_NULL_HANDLE;
    uint32_t h = (uint32_t)(key >> 40) & 1023;
    if (g_tcache[h].serial == g_vserial && g_tcache[h].k == key) return g_tcache[h].s;
    VkDescriptorSet set = tex_set_alloc(); if (!set) return VK_NULL_HANDLE;
    VkWriteDescriptorSet w[20];
    for (int i = 0; i < n; i++) { memset(&w[i], 0, sizeof w[i]); w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = bn[i];
        w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i]; }
    vkUpdateDescriptorSets(VD, (uint32_t)n, w, 0, NULL);
    g_tcache[h].k = key; g_tcache[h].serial = g_vserial; g_tcache[h].s = set;
    return set;
}
static void consts_fill(float (*dst)[4], const Sm3* s, const float (*src)[4]) {
    memcpy(dst, src, (size_t)s->nconst * 16);
    if (s->crel && s->ndef) for (int k = 0; k < s->ndef; k++) if (s->def[k].reg < s->nconst) memcpy(dst[s->def[k].reg], s->def[k].v, 16);
}
/* ---- v1.1.3: occlusion queries ----
 * A D3D query becomes one or more Vulkan queries of the frame slot (one per render pass it spans), begun lazily right
 * before its first draw inside the render pass and ended with the pass or at Issue(END); the results are read when the
 * slot's fence has signaled (slot_finish) and summed. GetData polls the fence; a query of the frame being recorded that
 * is polled again gets that frame submitted early (D3DGETDATA_FLUSH), at most 8 times a frame. */
static int vq_pool_new(VSlot* s) {
    if (s->nop >= VQPOOLS) return 0;
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, VQP, 0 };
    VkQueryPool p; if (vkCreateQueryPool(VD, &qi, NULL, &p) != VK_SUCCESS) return 0;
    vkCmdResetQueryPool(up_begin(), p, 0, VQP);   /* the upload buffer runs before this chunk; render passes forbid resets */
    s->op[s->nop++] = p; return 1;
}
static void vq_draw(void) {
    QRes* q = g_qopen->gx; VSlot* s = CS;
    q->rec++;
    if (!q->rtw) { q->rtw = RP.w; q->rth = RP.h; }
    if (g_vq_act || q->over || !RP.open) return;
    if (q->ser != s->rser) { if (q->n) { q->over = 1; return; } q->slot = (uint32_t)g_si; q->ser = s->rser; }   /* frame submitted since BEGIN */
    if (q->n >= 8) { q->over = 1; return; }
    uint32_t i = s->on, k = i / VQP;
    if (k >= s->nop && !vq_pool_new(s)) {
        static int w; if (!w++) vlog("occlusion queries: more than %u in one frame - the rest report \"visible\"\n", VQP * VQPOOLS);
        q->over = 1; return;
    }
    vkCmdBeginQuery(s->cmd, s->op[k], i % VQP, VX.oprec ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    g_vq_act = 1; g_vq_pool = s->op[k]; g_vq_idx = i % VQP;
    q->sub[q->n++] = i; s->on++;
}
static void vq_begin(Obj* o) { QRes* q = o->gx; q->slot = (uint32_t)g_si; q->ser = CS->rser; }
static void vq_end(Obj* o) {
    (void)o;
    if (g_vq_act && RP.open) { vkCmdEndQuery(CS->cmd, g_vq_pool, g_vq_idx); g_vq_act = 0; }
}
static int vq_sum(const QRes* q, const VSlot* s, uint64_t* out) {
    const uint32_t* r = g_ores[s - S]; uint64_t v = 0;
    for (int i = 0; i < q->n; i++) { if (r[q->sub[i]] == UINT32_MAX) return 0; v += r[q->sub[i]]; }
    *out = v; return 1;
}
static int vq_data(Obj* o, uint32_t flags, uint32_t* out) {
    QRes* q = o->gx; (void)flags;
    if (g_vklost || q->slot >= NSLOT) { q_done(q, q_visible(q), 0); *out = q->res; return 1; }
    VSlot* s = &S[q->slot];
    if (s->rser != q->ser || s->ost == 3) { q_done(q, q_visible(q), 0); *out = q->res; return 1; }   /* slot reused / never ran */
    int poll = q->polls++;
    if (s->ost == 0) {   /* still in the frame being recorded */
        if (!poll || g_occl == 2) return 0;
        if (g_vq_flush >= 8) { q_done(q, q_visible(q), 0); *out = q->res; return 1; }
        g_vq_flush++; g_qs.flushes++;
        vb_submit(-1, 0, 0);
        if (g_vklost) { q_done(q, q_visible(q), 0); *out = q->res; return 1; }
    }
    if (s->ost == 1 && s->pending) {
        double t = mono();
        VkResult r = poll ? vkWaitForFences(VD, 1, &s->fence, VK_TRUE, 2000000ull) : vkGetFenceStatus(VD, s->fence);
        if (poll) { g_qs.waits++; g_qs.wait_s += mono() - t; }
        if (r == VK_SUCCESS) slot_finish(s);
    }
    if (s->ost == 2) {
        uint64_t v;
        if (!vq_sum(q, s, &v)) q_done(q, q_visible(q), 0);
        else q_done(q, !v ? 0 : !VX.oprec ? q_area(q) : v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v, 1);   /* imprecise: visible or not */
        *out = q->res; return 1;
    }
    if (!q->t0) q->t0 = mono();
    else if (mono() - q->t0 > 0.25) { q_done(q, q_visible(q), 0); *out = q->res; return 1; }   /* the engine would time out at 0.5 s */
    return 0;
}
/* v1.2: like vq_data, but never flushes or waits (ME1_OCCLUSION=2 and the after-Present poll) */
static int vq_available(Obj* o) {
    QRes* q = o->gx;
    if (g_vklost || q->slot >= NSLOT) { q_done(q, q_visible(q), 0); return 1; }
    VSlot* s = &S[q->slot];
    if (s->rser != q->ser || s->ost == 3) { q_done(q, q_visible(q), 0); return 1; }   /* slot reused / never ran */
    if (s->ost == 0) return 0;   /* still in the frame being recorded */
    if (s->ost == 1 && s->pending) { if (vkGetFenceStatus(VD, s->fence) != VK_SUCCESS) return 0; slot_finish(s); }
    if (s->ost != 2) return 0;
    uint64_t v;
    if (!vq_sum(q, s, &v)) q_done(q, q_visible(q), 0);
    else q_done(q, !v ? 0 : !VX.oprec ? q_area(q) : v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v, 1);
    return 1;
}
typedef struct { Dev* d; GlSh *vs, *ps; VLay* layout; const uint32_t* rs; int rhw, srgb, vx, vy, vw, vh, ok, prof; double elapsed; } VUniformJob;
static void v_uniform_prepare(void* arg) {
    VUniformJob* j = arg; double ut = j->prof ? mono() : 0; Dev* d = j->d; GlSh* vs = j->vs; GlSh* ps = j->ps; VLay L = *j->layout;
    const uint32_t* rs = j->rs; int rhw=j->rhw, srgb=j->srgb, vx=j->vx, vy=j->vy, vw=j->vw, vh=j->vh;
    /* uniform blocks: re-written when constants or the shader change, both halves in one ring chunk */
    float fix[8] = { 1.f / (float)vw, 1.f / (float)vh, -1.f, 0.f, 0, 0, 0, 0 };
    if (rhw) { GS.rhw++; fix[4] = (float)vx; fix[5] = (float)vy; fix[6] = 2.f / (float)vw; fix[7] = 2.f / (float)vh; }
    float at[2] = { rs[15] ? (float)(rs[25] & 15) : 0.f, (float)(rs[24] & 255) / 255.f };
    int sw = rs[194] != 0;
    int need[2];
    need[0] = !VB.uvalid || VB.vser != vs->serial || VB.vcver != d->cver[0] || memcmp(VB.vfix, fix, sizeof fix) || VB.vbgra != L.bgra;
    need[1] = !VB.uvalid || VB.pser != ps->serial || VB.pcver != d->cver[1] || VB.pat[0] != at[0] || VB.pat[1] != at[1] || VB.psrgb != srgb || VB.psrgbw != sw;
    for (int it2 = 0; it2 < 4 && (need[0] | need[1]); it2++) for (int h = 0; h < 2; h++) {
        if (!need[h]) continue;
        const Sm3* s = h ? &ps->s : &vs->s;
        VkDeviceSize n = (h ? 288 : 304) + (VkDeviceSize)(s->nconst > 0 ? s->nconst : 1) * 16;
        VkBuffer b; VkDeviceSize o; VkDescriptorSet st;
        uint8_t* p = ring_alloc(&CS->ubo, n, VX.uboalign, &b, &o, &st);
        if (!p || !st) { VB.uvalid = 0; return; }
        int32_t iv[4] = { 0, 0, 0, 0 };
        if (!h) {
            memcpy(p, fix, 32);
            int mb = 0; for (int q = 0; q < 16; q++) if (d->vsb[q]) mb |= 1 << q;
            iv[0] = L.bgra; iv[1] = mb; memcpy(p + 32, iv, 16); memcpy(p + 48, d->vsi, 256);
            if (s->nconst) consts_fill((float (*)[4])(p + 304), s, (const float (*)[4])d->vsf);
            VB.vser = vs->serial; VB.vcver = d->cver[0]; memcpy(VB.vfix, fix, sizeof fix); VB.vbgra = L.bgra;
        } else {
            float a4[4] = { at[0], at[1], 0.f, 0.f }; memcpy(p, a4, 16);
            int mb = 0; for (int q = 0; q < 16; q++) if (d->psb[q]) mb |= 1 << q;
            iv[0] = srgb; iv[1] = sw; iv[2] = mb; memcpy(p + 16, iv, 16); memcpy(p + 32, d->psi, 256);
            if (s->nconst) consts_fill((float (*)[4])(p + 288), s, (const float (*)[4])d->psf);
            VB.pser = ps->serial; VB.pcver = d->cver[1]; VB.pat[0] = at[0]; VB.pat[1] = at[1]; VB.psrgb = srgb; VB.psrgbw = sw;
        }
        VB.ofs[h] = (uint32_t)o; need[h] = 0;
        if (b != VB.ubuf) { VB.ubuf = b; VB.uset = st; need[1 - h] = 1; }
    }
    VB.uvalid = 1;
    j->ok = 1; if (j->prof) j->elapsed = mono() - ut;
}
/* Mode 2 overlaps real work for this same draw with pipeline creation. The
 * render-pass class and shader modules are fixed before dispatch. The draw
 * waits before issuing the draw or releasing objects used by the worker. */
typedef struct {
    Dev* d; DrawCall* dc; GlSh *vs, *ps; VLay* layout;
    VkBuffer ib; VkDeviceSize ibo; uint32_t first; VkIndexType it;
    VkDescriptorSet tset; int srgb, ok, prof; double vert_time, tex_time;
    VUniformJob* uniform;
} VDrawPrep;
static void v_draw_data_prepare(VDrawPrep* j) {
    Dev* d = j->d; DrawCall* dc = j->dc; VLay L = *j->layout;
    VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize ibo = 0; uint32_t first = (uint32_t)L.first; VkIndexType it = VK_INDEX_TYPE_UINT16;
    double t = j->prof ? mono() : 0;
    if (dc->indexed) {
        uint32_t ifmt = dc->up ? dc->upifmt : d->ib->fmt, isz = ifmt == 102 ? 4 : 2;
        it = isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        uint64_t len = (uint64_t)dc->count * isz; uint32_t src = 0;
        first = 0;
        if (dc->up) src = dc->upi;
        else {
            Obj* io = d->ib; uint64_t a = (uint64_t)dc->start * isz;
            if (a + len > io->size) { return; }
            VRes* g = io->shadow ? NULL : vbuf(io);
            if (g && g->kind == 3) { ib = g->buf; ibo = 0; first = dc->start; g->use = g_vserial; }
            else if (io->shadow) src = d->ibsh + (uint32_t)a;
            else { return; }
        }
        if (!ib) { uint8_t* p = ring_alloc(&CS->vtx, len, 16, &ib, &ibo, NULL); if (!p) { return; } memcpy(p, M + src, (size_t)len); GS.ring += len; }
    }
    j->ib = ib; j->ibo = ibo; j->first = first; j->it = it;
    if (j->prof) { double now = mono(); j->vert_time = now - t; t = now; }
    int srgb = 0;
    j->tset = vtex_bind(d, &j->vs->s, &j->ps->s, &srgb);
    j->srgb = srgb;
    if (j->prof) j->tex_time = mono() - t;
    if (!g_vklost) j->ok = 1;
}
static void v_draw_wide_prepare(void* arg) {
    VDrawPrep* j = arg;
    v_draw_data_prepare(j);
    if (j->ok) { j->uniform->srgb = j->srgb; v_uniform_prepare(j->uniform); }
}
static void vb_draw(Dev* d, DrawCall* dc) {
    if (g_vklost) return;
    dc->count = prim_verts(dc->pt, dc->prims);
    if (!dc->count || dc->pt < 1 || dc->pt > 6) return;
    int rhw = draw_rhw(d);
    Obj* vso = rhw ? rhw_vs() : d->vs;
    if (!vso || !d->ps || !vso->gx || !d->ps->gx) { GS.skipped++; return; }
    GlSh* vs = vso->gx; GlSh* ps = d->ps->gx;
    int prof = (g_gp.n = g_gp.n + 1, (g_gp.n & 15) == 1); g_gp.nprof += prof;   /* v0.10.2, see hle_d3d9_gl.h */
    double tp = prof ? mono() : 0;
#define GP_MARK(st) do { if (prof) { double tn = mono(); g_gp.ns[st] += (uint64_t)((tn - tp) * 1e9); tp = tn; } } while (0)
    if (!vs->done) gl_shader_ready(vso);
    if (!ps->done) gl_shader_ready(d->ps);
    if (!vs->ok || !ps->ok) { GS.skipped++; return; }
    GP_MARK(GP_PROG);
    int tf = rp_targets(d);
    if (!tf) { GS.skipped++; return; }
    if (dc->indexed && !dc->up && !d->ib) { GS.skipped++; return; }
    dc->ninst = 0;
    if (dc->indexed && !dc->up && (d->vbfreq[0] & 0x40000000u)) { dc->ninst = d->vbfreq[0] & 0x3FFFFFFFu; if (!dc->ninst) dc->ninst = 1; }
    VLay L; if (vsetup(d, &vs->s, dc, &L) < 0) { GS.skipped++; return; }
    GP_MARK(GP_VERT);
    VProg* P = vprog_get(vso, d->ps, L.im, L.sm);
    if (!P || !P->ok) { GS.skipped++; return; }
    GP_MARK(GP_PROG);
    static int wide_opt = -1;
    if (wide_opt < 0) { const char* e = getenv("ME1_VK_ASYNC_PIPE"); wide_opt = e && !strcmp(e, "2"); }
    int wide = wide_opt && !g_pipeidle && (g_vpa.available || vpipe_async_enabled());
    VDrawPrep prep = {0}; prep.d = d; prep.dc = dc; prep.vs = vs; prep.ps = ps; prep.layout = &L; prep.prof = prof;
    if (!wide) {
        v_draw_data_prepare(&prep);
        if (!prep.ok) { GS.skipped++; return; }
        if (prof) { g_gp.ns[GP_VERT] += (uint64_t)(prep.vert_time * 1e9);
                    g_gp.ns[GP_TEX] += (uint64_t)(prep.tex_time * 1e9); tp = mono(); }
    }
    /* pipeline */
    const uint32_t* rs = d->rs;
    PKey k; memset(&k, 0, sizeof k);
    k.prog = P->id; k.rpc = (uint32_t)RP.rpc; k.topo = (uint8_t)(dc->pt - 1);
    if (rs[27]) {
        uint32_t sb = rs[19] & 15, db = rs[20] & 15, sa = rs[206] ? rs[207] & 15 : sb, da = rs[206] ? rs[208] & 15 : db;
        uint32_t op = rs[171] & 7, opa = rs[206] ? rs[209] & 7 : op;
        k.blend = 1 | sb << 1 | db << 5 | sa << 9 | da << 13 | op << 17 | opa << 20;
    }
    uint32_t m[4] = { rs[168] & 15, rs[190] & 15, rs[191] & 15, rs[192] & 15 };
    for (int i = 0; i < 4; i++) { if (d->rt[i] && no_alpha_fmt(d->rt[i]->fmt)) m[i] &= 7; k.wmask |= m[i] << (4 * i); }
    int hasds = (tf & 2) != 0;
    k.ztest = hasds && rs[7]; if (k.ztest) { k.zwrite = rs[14] != 0; k.zfunc = (uint8_t)(rs[23] & 15); }
    if ((tf & 4) && rs[52]) {
        int two = rs[185] != 0; k.sten = 1;
        k.sfront = (rs[56] & 15) | (rs[53] & 15) << 4 | (rs[54] & 15) << 8 | (rs[55] & 15) << 12;
        k.sback = two ? (rs[189] & 15) | (rs[186] & 15) << 4 | (rs[187] & 15) << 8 | (rs[188] & 15) << 12 : k.sfront;
    }
    k.cull = (uint8_t)(rs[22] == 2 || rs[22] == 3 ? rs[22] : 1);
    float bias = bitsf(rs[195]), slope = bitsf(rs[175]);
    k.dbias = hasds && (bias != 0.f || slope != 0.f);
    k.nattr = (uint8_t)L.na; for (int i = 0; i < L.na; i++) { k.a[i].loc = L.a[i].loc; k.a[i].bind = L.a[i].bind; k.a[i].off = L.a[i].off; k.a[i].fmt = L.a[i].fmt; }
    k.nbind = (uint8_t)L.nb; for (int i = 0; i < L.nb; i++) { k.b[i].stride = L.b[i].stride; k.b[i].inst = L.b[i].inst; }
    /* viewport */
    int vx = (int)d->vp[0], vy = (int)d->vp[1], vw = (int)d->vp[2], vh = (int)d->vp[3];
    if (vw <= 0) vw = 1;
    if (vh <= 0) vh = 1;
    float z0 = bitsf(d->vp[4]), z1 = bitsf(d->vp[5]);
    z0 = z0 < 0.f ? 0.f : z0 > 1.f ? 1.f : z0; z1 = z1 < 0.f ? 0.f : z1 > 1.f ? 1.f : z1;
    VUniformJob uj = { d, vs, ps, &L, rs, rhw, prep.srgb, vx, vy, vw, vh, 0, prof, 0 };
    prep.uniform = &uj;
    VkPipeline pipe = pipe_get(P, &k, wide ? v_draw_wide_prepare : v_uniform_prepare, wide ? (void*)&prep : (void*)&uj, wide);
    if (!pipe || !prep.ok || !uj.ok || g_vklost) { VB.uvalid = 0; GS.skipped++; return; }
    if (prof) { double tn = mono(), all = tn - tp;
        double v = wide ? prep.vert_time : 0, t = wide ? prep.tex_time : 0, u = uj.elapsed;
        if (v + t + u > all) { double factor = all / (v + t + u); v *= factor; t *= factor; u *= factor; }
        g_gp.ns[GP_VERT] += (uint64_t)(v * 1e9); g_gp.ns[GP_TEX] += (uint64_t)(t * 1e9);
        g_gp.ns[GP_CONST] += (uint64_t)(u * 1e9); g_gp.ns[GP_STATE] += (uint64_t)((all - v - t - u) * 1e9); tp = tn; }
    VkBuffer ib = prep.ib; VkDeviceSize ibo = prep.ibo; uint32_t first = prep.first;
    VkIndexType it = prep.it; VkDescriptorSet tset = prep.tset;
    /* Record immutable state for secondary buffers, or complete state inline
     * while an occlusion query is open. The normal path stays unchanged. */
    if (g_sec_on) {
        if (tset) g_sec_last_tex = tset;
        VSecDraw d0; memset(&d0, 0, sizeof d0);
        d0.pipe = pipe; d0.ubo = VB.uset; d0.tex = g_sec_last_tex; d0.uoff[0] = VB.ofs[0]; d0.uoff[1] = VB.ofs[1];
        d0.nb = (uint32_t)L.nb; for (int i = 0; i < L.nb; i++) { d0.vb[i] = L.b[i].buf; d0.vo[i] = L.b[i].off; }
        d0.ib = ib; d0.io = ibo; d0.it = it; d0.vp = (VkViewport){ (float)vx, (float)vy, (float)vw, (float)vh, z0, z1 };
        int sc[4] = { 0, 0, (int)RP.w, (int)RP.h };
        if (rs[174]) rect_and(sc, (int)d->scissor[0], (int)d->scissor[1], (int)d->scissor[2], (int)d->scissor[3]);
        d0.sc = (VkRect2D){ { sc[0], sc[1] }, { sc[2] > sc[0] ? (uint32_t)(sc[2] - sc[0]) : 0, sc[3] > sc[1] ? (uint32_t)(sc[3] - sc[1]) : 0 } };
        d0.bias = bias; d0.slope = slope; uint32_t bc = rs[193];
        d0.bc[0] = (bc >> 16 & 255) / 255.f; d0.bc[1] = (bc >> 8 & 255) / 255.f; d0.bc[2] = (bc & 255) / 255.f; d0.bc[3] = (bc >> 24) / 255.f;
        d0.ref = rs[57] & 255; d0.mask = rs[58] & 255; d0.wmask = rs[59] & 255;
        d0.indexed = dc->indexed; d0.count = dc->count; d0.instances = dc->ninst > 1 ? dc->ninst : 1; d0.first = first; d0.voff = L.voff;
        rp_begin_mode(g_qopen ? 0 : 1);
        if (g_qopen) { vq_draw(); sec_encode(CS->cmd, &d0); }
        else if (g_sec_on && RP.secondary) { g_secd[g_secn++] = d0; if (g_secn == SEC_BATCH) sec_flush(); }
        else { sec_encode(CS->cmd, &d0); sec_invalidate_bindings(); }
    } else {
    rp_begin();
    VkCommandBuffer cb = CS->cmd;
    if (VB.pipe != pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); VB.pipe = pipe; }
    if (VB.set0 != VB.uset || VB.uoff[0] != VB.ofs[0] || VB.uoff[1] != VB.ofs[1]) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 0, 1, &VB.uset, 2, VB.ofs);
        VB.set0 = VB.uset; VB.uoff[0] = VB.ofs[0]; VB.uoff[1] = VB.ofs[1];
    }
    if (tset && VB.set1 != tset) { vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 1, 1, &tset, 0, NULL); VB.set1 = tset; }
    for (int i = 0; i < L.nb; i++) if (VB.vb[i] != L.b[i].buf || VB.vbo[i] != L.b[i].off) {
        vkCmdBindVertexBuffers(cb, (uint32_t)i, 1, &L.b[i].buf, &L.b[i].off); VB.vb[i] = L.b[i].buf; VB.vbo[i] = L.b[i].off;
    }
    if (dc->indexed && (VB.ib != ib || VB.ibo != ibo || VB.it != (int)it)) { vkCmdBindIndexBuffer(cb, ib, ibo, it); VB.ib = ib; VB.ibo = ibo; VB.it = (int)it; }
    VkViewport vp = { (float)vx, (float)vy, (float)vw, (float)vh, z0, z1 };
    if (memcmp(&vp, &VB.vp, sizeof vp)) { vkCmdSetViewport(cb, 0, 1, &vp); VB.vp = vp; }
    int sc[4] = { 0, 0, (int)RP.w, (int)RP.h };
    if (rs[174]) rect_and(sc, (int)d->scissor[0], (int)d->scissor[1], (int)d->scissor[2], (int)d->scissor[3]);
    VkRect2D sr = { { sc[0], sc[1] }, { sc[2] > sc[0] ? (uint32_t)(sc[2] - sc[0]) : 0, sc[3] > sc[1] ? (uint32_t)(sc[3] - sc[1]) : 0 } };
    if (memcmp(&sr, &VB.sc, sizeof sr)) { vkCmdSetScissor(cb, 0, 1, &sr); VB.sc = sr; }
    if (k.dbias && (VB.bias[0] != bias || VB.bias[1] != slope)) { vkCmdSetDepthBias(cb, bias * 16777215.f, 0.f, slope); VB.bias[0] = bias; VB.bias[1] = slope; }
    if (k.blend) {
        uint32_t c = rs[193]; float bc[4] = { (c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f };
        if (memcmp(bc, VB.bc, sizeof bc)) { vkCmdSetBlendConstants(cb, bc); memcpy(VB.bc, bc, sizeof bc); }
    }
    if (k.sten) {
        uint32_t ref = rs[57] & 255, msk = rs[58] & 255, wm = rs[59] & 255;
        if (VB.sref != ref) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ref); VB.sref = ref; }
        if (VB.smask != msk) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, msk); VB.smask = msk; }
        if (VB.swmask != wm) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, wm); VB.swmask = wm; }
    }
    uint32_t ni = dc->ninst > 1 ? dc->ninst : 1;
    if (g_qopen) vq_draw();   /* v1.1.3: inside an occlusion query */
    if (!dc->indexed) vkCmdDraw(cb, dc->count, ni, first, 0);
    else vkCmdDrawIndexed(cb, dc->count, ni, first, L.voff, 0);
    }
    uint32_t ni = dc->ninst > 1 ? dc->ninst : 1;
    GP_MARK(GP_DRAW);
#undef GP_MARK
    GS.draws++;
    CS->cprims += (uint64_t)dc->prims * ni;   /* v0.10.12 (P0-4): split by primitives too (v0.10.13 N3: primitives, not vertices) */
    if ((VX.split && ++CS->cdraws >= VX.split) || (VX.splitprims && CS->cprims >= VX.splitprims)) vk_chunk_flush();
}

/* ---- clears, copies, readback ---- */
static void vb_clear(Dev* d, uint32_t n, uint32_t rects, uint32_t flags, uint32_t color, uint32_t zbits, uint32_t stencil) {
    if (g_vklost) return;
    int tf = rp_targets(d); if (!tf) return;
    int doc = (flags & 1) != 0, dod = (flags & 2) && (tf & 2), dos = (flags & 4) && (tf & 4);
    if (!doc && !dod && !dos) return;
    VkClearValue cc[4]; memset(cc, 0, sizeof cc);
    for (int i = 0; i < 4; i++) {
        float a = (color >> 24) / 255.f; if (d->rt[i] && no_alpha_fmt(d->rt[i]->fmt)) a = 1.f;
        cc[i].color.float32[0] = (color >> 16 & 255) / 255.f; cc[i].color.float32[1] = (color >> 8 & 255) / 255.f; cc[i].color.float32[2] = (color & 255) / 255.f; cc[i].color.float32[3] = a;
    }
    float z = bitsf(zbits); z = z < 0.f ? 0.f : z > 1.f ? 1.f : z;
    int area[4] = { (int)d->vp[0], (int)d->vp[1], (int)(d->vp[0] + d->vp[2]), (int)(d->vp[1] + d->vp[3]) };
    rect_and(area, 0, 0, (int)RP.w, (int)RP.h);
    if (d->rs[174]) rect_and(area, (int)d->scissor[0], (int)d->scissor[1], (int)d->scissor[2], (int)d->scissor[3]);
    int nr = rects ? (int)n : 0; if (nr > 256) nr = 256;
    GS.clears++;
    if (!nr && area[0] == 0 && area[1] == 0 && area[2] == (int)RP.w && area[3] == (int)RP.h && !RP.open) {   /* whole targets: load-op clear */
        if (doc) for (int i = 0; i < RP.nc; i++) if (RP.c[i].r) { RP.clr |= 1u << i; RP.cv[i] = cc[i]; }
        if (dod) { RP.clr |= 16; RP.cv[4].depthStencil.depth = z; }
        if (dos) { RP.clr |= 32; RP.cv[5].depthStencil.stencil = stencil & 255; }
        return;
    }
    VkClearRect cr[256]; uint32_t ncr = 0;
    for (int i = 0; i < (nr ? nr : 1); i++) {
        int r[4] = { area[0], area[1], area[2], area[3] };
        if (nr) rect_and(r, (int)rd32(rects + 16 * i), (int)rd32(rects + 16 * i + 4), (int)rd32(rects + 16 * i + 8), (int)rd32(rects + 16 * i + 12));
        if (r[2] <= r[0] || r[3] <= r[1]) continue;
        cr[ncr].rect.offset.x = r[0]; cr[ncr].rect.offset.y = r[1]; cr[ncr].rect.extent.width = (uint32_t)(r[2] - r[0]); cr[ncr].rect.extent.height = (uint32_t)(r[3] - r[1]);
        cr[ncr].baseArrayLayer = 0; cr[ncr].layerCount = 1; ncr++;
    }
    if (!ncr) return;
    rp_begin();
    VkClearAttachment ca[5]; uint32_t na = 0; memset(ca, 0, sizeof ca);
    if (doc) for (int i = 0; i < RP.nc; i++) if (RP.c[i].r) { ca[na].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[na].colorAttachment = (uint32_t)i; ca[na].clearValue = cc[i]; na++; }
    if (dod || dos) { ca[na].aspectMask = (dod ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (dos ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
                      ca[na].clearValue.depthStencil.depth = z; ca[na].clearValue.depthStencil.stencil = stencil & 255; na++; }
    vkCmdClearAttachments(CS->cmd, na, ca, ncr, cr);
}
static void vb_stretch(Obj* src, uint32_t sr, Obj* dst, uint32_t dr, uint32_t filter) {
    if (g_vklost) return;
    Att a, b;
    if (!att_of(src, &a) || !att_of(dst, &b) || a.r->kind != b.r->kind) { static int n; if (n++ < 10) vlog("StretchRect: unsupported source/destination\n"); return; }
    uint32_t sw, sh, dw, dh; att_size(&a, &sw, &sh); att_size(&b, &dw, &dh);
    int s[4], t[4]; rect_of(sr, sw, sh, s); rect_of(dr, dw, dh, t); rect_and(s, 0, 0, (int)sw, (int)sh); rect_and(t, 0, 0, (int)dw, (int)dh);
    if (s[2] <= s[0] || s[3] <= s[1] || t[2] <= t[0] || t[3] <= t[1]) return;
    if (a.r == b.r && a.face == b.face && a.lvl == b.lvl) { static int n; if (n++ < 5) vlog("StretchRect within one surface not supported\n"); return; }
    int depth = a.r->kind == 2, same = s[2] - s[0] == t[2] - t[0] && s[3] - s[1] == t[3] - t[1];
    VkFormatFeatureFlags fa = fmt_opt(a.r->vf), fb = fmt_opt(b.r->vf);
    int copy = same && a.r->vf == b.r->vf, blit = (fa & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (fb & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if (!copy && !blit) { static int n; if (n++ < 10) vlog("StretchRect: %s %ux%u -> %ux%u not possible (format %d -> %d)\n", depth ? "depth" : "color", sw, sh, dw, dh, (int)a.r->vf, (int)b.r->vf); return; }
    rp_end();
    VkCommandBuffer cb = CS->cmd;
    img_to(cb, a.r, a.face, 1, a.lvl, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); img_to(cb, b.r, b.face, 1, b.lvl, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceLayers sa = { res_aspect(a.r), a.lvl, a.face, 1 }, sb = { res_aspect(b.r), b.lvl, b.face, 1 };
    if (copy) {
        VkImageCopy c = { sa, { s[0], s[1], 0 }, sb, { t[0], t[1], 0 }, { (uint32_t)(s[2] - s[0]), (uint32_t)(s[3] - s[1]), 1 } };
        vkCmdCopyImage(cb, a.r->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    } else {
        VkImageBlit bl; bl.srcSubresource = sa; bl.dstSubresource = sb;
        bl.srcOffsets[0].x = s[0]; bl.srcOffsets[0].y = s[1]; bl.srcOffsets[0].z = 0; bl.srcOffsets[1].x = s[2]; bl.srcOffsets[1].y = s[3]; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[0].x = t[0]; bl.dstOffsets[0].y = t[1]; bl.dstOffsets[0].z = 0; bl.dstOffsets[1].x = t[2]; bl.dstOffsets[1].y = t[3]; bl.dstOffsets[1].z = 1;
        int lin = !depth && filter >= 2 && (fa & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        vkCmdBlitImage(cb, a.r->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    a.r->use = a.r->mainuse = b.r->use = b.r->mainuse = g_vserial;
    GS.blits++;
}
static void vb_colorfill(Obj* surf, uint32_t rect, uint32_t color) {
    if (g_vklost) return;
    Att a; if (!att_of(surf, &a) || a.r->kind != 1) return;
    uint32_t w, h; att_size(&a, &w, &h); int r[4]; rect_of(rect, w, h, r); rect_and(r, 0, 0, (int)w, (int)h);
    if (r[2] <= r[0] || r[3] <= r[1]) return;
    VkClearValue cv; cv.color.float32[0] = (color >> 16 & 255) / 255.f; cv.color.float32[1] = (color >> 8 & 255) / 255.f; cv.color.float32[2] = (color & 255) / 255.f; cv.color.float32[3] = (color >> 24) / 255.f;
    rp_end();
    VkCommandBuffer cb = CS->cmd;
    if ((r[0] == 0 && r[1] == 0 && r[2] == (int)w && r[3] == (int)h) || !a.r->rt) {
        if (r[0] || r[1] || r[2] != (int)w || r[3] != (int)h) { static int n; if (n++ < 5) vlog("ColorFill: partial fill of a non-render-target surface - whole surface filled\n"); }
        img_to(cb, a.r, a.face, 1, a.lvl, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, a.lvl, 1, a.face, 1 };
        vkCmdClearColorImage(cb, a.r->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv.color, 1, &rg);
    } else {   /* a rectangle: a one-attachment pass of its own */
        __typeof__(RP) save = RP;
        memset(&RP, 0, sizeof RP); RP.c[0] = a; RP.nc = 1; RP.w = w; RP.h = h; RP.tf = 1;
        VkFormat cf[4] = { a.r->vf, 0, 0, 0 }; RP.rpc = rpc_id(cf, 1, VK_FORMAT_UNDEFINED);
        if (RP.rpc < 0) { RP = save; return; }   /* v0.10.12 (P0-7) */
        rp_begin();
        VkClearAttachment ca; memset(&ca, 0, sizeof ca); ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca.clearValue = cv;
        VkClearRect cr = { { { r[0], r[1] }, { (uint32_t)(r[2] - r[0]), (uint32_t)(r[3] - r[1]) } }, 0, 1 };
        vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        rp_end(); RP = save;
    }
    a.r->use = a.r->mainuse = g_vserial;
}
static void vb_update_surface(Obj* src, uint32_t sr, Obj* dst, uint32_t dpt) {
    if (src->fmt != dst->fmt) return;
    Obj* root = dst->parent ? dst->parent : dst; uint32_t lvl = dst->parent ? dst->level : 0, face = dst->parent && root->type == T_CUBE ? dst->face : 0;
    VRes* g = vimg(root); if (!g || g->kind != 1) return;
    Obj* sroot; uint32_t si; uint32_t mem = sub_mem(src, &sroot, &si); if (!mem) return;
    uint32_t sw = src->w, sh = src->h; int s[4]; rect_of(sr, sw, sh, s); rect_and(s, 0, 0, (int)sw, (int)sh);
    if (s[2] <= s[0] || s[3] <= s[1]) return;
    uint32_t dx = dpt ? rd32(dpt) : 0, dy = dpt ? rd32(dpt + 4) : 0;
    uint32_t pitch, rows, unit; int blk; layout(src->fmt, sw, sh, &pitch, &rows, &unit, &blk);
    uint32_t k = blk ? 4 : 1;
    const uint8_t* p = M + mem + (size_t)((uint32_t)s[1] / k) * pitch + (size_t)((uint32_t)s[0] / k) * unit;
    vtex_upload(root, g, face, lvl, dx, dy, 0, (uint32_t)(s[2] - s[0]), (uint32_t)(s[3] - s[1]), 1, p, pitch, pitch * rows);
    vb_maybe_flush();
}
static void vb_update_texture(Obj* src, Obj* dst) {
    if (src->fmt != dst->fmt || src->type != dst->type) return;
    VRes* g = vimg(dst); if (!g || g->kind != 1 || !src->lockv) return;
    uint32_t faces = dst->type == T_CUBE ? 6 : 1;
    for (uint32_t l = 0; l < dst->levels; l++) {
        uint32_t w = dimv(dst->w, l), h = dimv(dst->h, l), dd = dst->type == T_VOLTEX ? dimv(dst->d, l) : 1, sl;
        for (sl = 0; sl < src->levels; sl++) if (dimv(src->w, sl) == w && dimv(src->h, sl) == h) break;
        if (sl == src->levels) continue;
        uint32_t pitch, rows, unit; int blk; layout(dst->fmt, w, h, &pitch, &rows, &unit, &blk);
        for (uint32_t f = 0; f < faces; f++) {
            uint32_t m = src->lockv[f * src->levels + sl]; if (!m) continue;
            vtex_upload(dst, g, f, l, 0, 0, 0, w, h, dd, M + m, pitch, pitch * rows);
        }
    }
    vb_maybe_flush();
}
/* copy (x, y, w, h) of one image level into the host read-back buffer, wait for it; returns the tightly packed texels */
static VkBuffer g_rbk; static VMem g_rbkm; static VkDeviceSize g_rbksz;
static void host_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static const uint8_t* vread_raw(const Att* a, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t bpp) {
    if (g_vklost || !w || !h) return NULL;
    VkDeviceSize n = (VkDeviceSize)w * h * bpp;
    if (n > g_rbksz) {
        if (g_rbk) { vkDeviceWaitIdle(VD); vkDestroyBuffer(VD, g_rbk, NULL); vmem_free(&g_rbkm); g_rbk = VK_NULL_HANDLE; g_rbksz = 0; }
        VkDeviceSize sz = n < (4u << 20) ? (4u << 20) : n;
        if (!mk_buffer(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &g_rbk, &g_rbkm)) return NULL;
        g_rbksz = sz;
    }
    rp_end();
    VkCommandBuffer cb = CS->cmd;
    img_to(cb, a->r, a->face, 1, a->lvl, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c; memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = a->lvl; c.imageSubresource.baseArrayLayer = a->face; c.imageSubresource.layerCount = 1;
    c.imageOffset.x = (int32_t)x; c.imageOffset.y = (int32_t)y; c.imageExtent.width = w; c.imageExtent.height = h; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, a->r->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rbk, 1, &c);
    host_barrier(cb);
    a->r->use = a->r->mainuse = g_vserial;
    vb_submit(-1, 1, 0);
    if (g_vklost) return NULL;
    vmem_invalidate(&g_rbkm);
    return vmem_ptr(&g_rbkm);
}
/* read (x, y, w, h) of a color image into guest memory in the D3D layout of fmt */
static int vread(const Att* a, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* dst, uint32_t pitch, uint32_t fmt) {
    const VRes* r = a->r;
    if (!r || r->kind != 1 || !r->f || r->dec || (r->f->conv != CV_RAW && r->f->conv != CV_U16)) return 0;
    uint32_t bpp = (uint32_t)r->obpp;
    const uint8_t* p = vread_raw(a, x, y, w, h, bpp); if (!p) return 0;
    for (uint32_t rr = 0; rr < h; rr++) {
        const uint8_t* s = p + (size_t)rr * w * bpp; uint8_t* o = dst + (size_t)rr * pitch;
        if (r->f->conv == CV_RAW) {
            memcpy(o, s, (size_t)w * bpp);
            if (fmt == 35) for (uint32_t c = 0; c < w; c++) { uint32_t v; memcpy(&v, o + 4 * c, 4); v = (v & 0xC00FFC00u) | (v >> 20 & 0x3FFu) | (v & 0x3FFu) << 20; memcpy(o + 4 * c, &v, 4); }
        } else for (uint32_t c = 0; c < w * r->f->nch; c++) {
            uint16_t hv; memcpy(&hv, s + 2 * c, 2); float v = h2f(hv);
            uint16_t u = (uint16_t)(v < 0 ? 0 : v > 1 ? 65535 : v * 65535.f + .5f); memcpy(o + 2 * c, &u, 2);
        }
    }
    return 1;
}
static void vb_get_rt_data(Obj* rt, Obj* dst) {
    if (rt->w != dst->w || rt->h != dst->h) return;
    Att a; if (!att_of(rt, &a)) return;
    Obj* r; uint32_t si; uint32_t mem = sub_mem(dst, &r, &si); if (!mem) return;
    uint32_t pitch, rows, unit; int blk; layout(dst->fmt, dst->w, dst->h, &pitch, &rows, &unit, &blk);
    if (!vread(&a, 0, 0, dst->w, dst->h, M + mem, pitch, dst->fmt)) { static int n; if (n++ < 5) vlog("GetRenderTargetData: %s readback not supported\n", fmt_name(dst->fmt)); }
}
static void vb_rt_lock(Obj* r, uint32_t s, uint32_t lvl, uint32_t mem) {
    VRes* g = vimg(r); if (!g || g->kind != 1 || (int)lvl < g->drop) return;
    Att a; a.r = g; a.face = r->type == T_CUBE ? s / r->levels : 0; a.lvl = lvl - (uint32_t)g->drop;
    uint32_t pitch, rows, unit; int blk; layout(r->fmt, dimv(r->w, lvl), dimv(r->h, lvl), &pitch, &rows, &unit, &blk);
    vread(&a, 0, 0, dimv(r->w, lvl), dimv(r->h, lvl), M + mem, pitch, r->fmt);
}

/* ---- presentation ---- */
static void vdump(const Att* a, uint32_t w, uint32_t h) {
    char path[1100]; snprintf(path, sizeof path, "%s/frame_%06llu.png", g_fdump, (unsigned long long)g_pframes);
    const VRes* r = a->r;
    const uint8_t* p = r->f && r->f->conv == CV_RAW && r->obpp == 4 && !r->dec ? vread_raw(a, 0, 0, w, h, 4) : NULL;
    if (!p) { static int n; if (n++ < 3) vlog("frame dump: back buffer not readable\n"); return; }
    uint8_t* buf = malloc((size_t)w * h * 4); memcpy(buf, p, (size_t)w * h * 4);
    if (r->vf == VK_FORMAT_B8G8R8A8_UNORM) for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t t = buf[4 * i]; buf[4 * i] = buf[4 * i + 2]; buf[4 * i + 2] = t; }
    if (!png_write(path, buf, (int)w, (int)h, 0)) { static int n; if (n++ < 3) vlog("cannot write %s\n", path); }
    else if (g_pframes == 1 || g_log) vlog("frame %llu saved: %s\n", (unsigned long long)g_pframes, path);
    free(buf);
}
static void vswap_drop(int surf) {
    if (VW.sc || (surf && VW.surf)) vkDeviceWaitIdle(VD);
    for (uint32_t i = 0; i < 16; i++) if (VW.rs[i]) { vkDestroySemaphore(VD, VW.rs[i], NULL); VW.rs[i] = VK_NULL_HANDLE; }
    if (VW.sc) { vkDestroySwapchainKHR(VD, VW.sc, NULL); VW.sc = VK_NULL_HANDLE; }
    VW.n = 0;
    if (surf && VW.surf) { vkDestroySurfaceKHR(VI, VW.surf, NULL); VW.surf = VK_NULL_HANDLE; }
}
static void vk_driver_hopeless(void);
static int g_ws_mis; static double g_ws_mist;   /* v1.1: swap chain size != window size (X11 resize not applied yet): retries */
static int vsurf_make(void) {
    if (VW.surf) return 1;
#ifdef __ANDROID__
    if (VW.win && vkCreateAndroidSurfaceKHR) {
        VkAndroidSurfaceCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, (struct ANativeWindow*)VW.win };
        VkResult r = vkCreateAndroidSurfaceKHR(VI, &ci, NULL, &VW.surf);
        if (r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {   /* v0.10.12 (P0-3): the lost device's swap chain still owns the window */
            vlog("window still owned by the lost device's swap chain -> restart\n");
            vk_driver_hopeless();
            return 0;
        }
        return vok(r, "vkCreateAndroidSurfaceKHR");
    }
#else
    if (VW.win && !VW.fake && VX.ws && vkCreateWsSurface) {   /* v1.1: desktop window */
        Me1Window* w = (Me1Window*)VW.win; VkResult r;
        if (w->kind == ME1_WS_WAYLAND) {
            Me1VkWaylandSurfaceCI ci = { (VkStructureType)1000006000 /* WAYLAND_SURFACE_CREATE_INFO_KHR */, NULL, 0, w->display, w->surface };
            r = vkCreateWsSurface(VI, &ci, NULL, &VW.surf);
        } else {
            Me1VkXlibSurfaceCI ci = { (VkStructureType)1000004000 /* XLIB_SURFACE_CREATE_INFO_KHR */, NULL, 0, w->display, w->xwindow };
            r = vkCreateWsSurface(VI, &ci, NULL, &VW.surf);
        }
        VW.mgen = w->gen;
        return vok(r, w->kind == ME1_WS_WAYLAND ? "vkCreateWaylandSurfaceKHR" : "vkCreateXlibSurfaceKHR");
    }
#endif
    if (VW.fake && vkCreateHeadlessSurfaceEXT) {
        VkHeadlessSurfaceCreateInfoEXT ci = { VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT, NULL, 0 };
        return vok(vkCreateHeadlessSurfaceEXT(VI, &ci, NULL, &VW.surf), "vkCreateHeadlessSurfaceEXT");
    }
    return 0;
}
/* (re)create the swap chain of the current window: FIFO, letterboxed blits into it (TRANSFER_DST), identity transform */
static int vswap_make(void) {
    VW.recreate = 0;
    if (!vsurf_make()) return 0;
    VkBool32 sup = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(VP, VQF, VW.surf, &sup) < 0 || !sup) { vlog("swap chain: the graphics queue cannot present to this window\n"); return 0; }
    VkSurfaceCapabilitiesKHR c; memset(&c, 0, sizeof c);
    if (!vok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VP, VW.surf, &c), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return 0;
    VkSurfaceFormatKHR fm[64]; uint32_t nf = 64;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(VP, VW.surf, &nf, fm) < 0) nf = 0;
    int pick = -1;
    for (uint32_t i = 0; i < nf && pick < 0; i++)
        if ((fm[i].format == VK_FORMAT_R8G8B8A8_UNORM || fm[i].format == VK_FORMAT_B8G8R8A8_UNORM) && (fmt_opt(fm[i].format) & VK_FORMAT_FEATURE_BLIT_DST_BIT)) pick = (int)i;
    for (uint32_t i = 0; i < nf && pick < 0; i++) if (fmt_opt(fm[i].format) & VK_FORMAT_FEATURE_BLIT_DST_BIT) pick = (int)i;
    if (pick < 0) { vlog("swap chain: no format usable as a blit destination (%u formats)\n", nf); return 0; }
    if (!(c.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { vlog("swap chain: images cannot be transfer destinations (usage 0x%x)\n", c.supportedUsageFlags); return 0; }
    VkExtent2D ext = c.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {   /* the surface takes the swap chain's size (Wayland, headless) */
        ext.width = (uint32_t)(g_fakew > 0 ? g_fakew : 1280); ext.height = (uint32_t)(g_fakeh > 0 ? g_fakeh : 720);
#ifndef __ANDROID__
        if (VW.win && !VW.fake && ((Me1Window*)VW.win)->w > 0) { ext.width = (uint32_t)((Me1Window*)VW.win)->w; ext.height = (uint32_t)((Me1Window*)VW.win)->h; }
#endif
    }
#ifndef __ANDROID__
    if (VW.win && !VW.fake) {
        Me1Window* mw = (Me1Window*)VW.win;
        VW.mgen = mw->gen;
        /* X11: the server may not have applied the new size yet (fullscreen toggle): retry a few times, 0.1 s apart */
        int mis = c.currentExtent.width != 0xFFFFFFFFu && mw->w > 0 && (c.currentExtent.width != (uint32_t)mw->w || c.currentExtent.height != (uint32_t)mw->h);
        g_ws_mis = mis ? g_ws_mis + 1 : 0; g_ws_mist = mono();
    }
#endif
    if (ext.width < c.minImageExtent.width) ext.width = c.minImageExtent.width;
    if (ext.height < c.minImageExtent.height) ext.height = c.minImageExtent.height;
    if (c.maxImageExtent.width && ext.width > c.maxImageExtent.width) ext.width = c.maxImageExtent.width;
    if (c.maxImageExtent.height && ext.height > c.maxImageExtent.height) ext.height = c.maxImageExtent.height;
    if (!ext.width || !ext.height) { vlog("swap chain: window has no size\n"); return 0; }
    uint32_t n = c.minImageCount + 1; if (c.maxImageCount && n > c.maxImageCount) n = c.maxImageCount; if (n > 16) n = 16;
    VkCompositeAlphaFlagBitsKHR ca = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    static const VkCompositeAlphaFlagBitsKHR cas[4] = { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR };
    for (int i = 3; i >= 0; i--) if (c.supportedCompositeAlpha & cas[i]) ca = cas[i];
    VkSurfaceTransformFlagBitsKHR tr = (c.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : c.currentTransform;
    VkSwapchainCreateInfoKHR ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR; ci.surface = VW.surf; ci.minImageCount = n; ci.imageFormat = fm[pick].format;
    ci.imageColorSpace = fm[pick].colorSpace; ci.imageExtent = ext; ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = tr; ci.compositeAlpha = ca; ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; ci.clipped = VK_TRUE; ci.oldSwapchain = VW.sc;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(VD, &ci, NULL, &sc);
    vswap_drop(0);   /* the old one (retired by oldSwapchain) */
    if (!vok(r, "vkCreateSwapchainKHR")) return 0;
    VW.sc = sc; VW.n = 16;
    if (!vok(vkGetSwapchainImagesKHR(VD, sc, &VW.n, VW.img), "vkGetSwapchainImagesKHR") || !VW.n) { vswap_drop(0); return 0; }
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0 };
    for (uint32_t i = 0; i < VW.n; i++) if (!vok(vkCreateSemaphore(VD, &si, NULL, &VW.rs[i]), "vkCreateSemaphore")) { vswap_drop(0); return 0; }
    VW.fmt = fm[pick].format; VW.ext = ext; VW.cur = c.currentExtent;
    static int logs; if (logs++ < 20) vlog("swap chain %ux%u, %u images, format %d, transform 0x%x (current 0x%x), alpha 0x%x%s\n", ext.width, ext.height, VW.n,
                                         (int)VW.fmt, (unsigned)tr, (unsigned)c.currentTransform, (unsigned)ca, VW.fake ? " (headless test surface)" : "");
    return 1;
}
/* no usable swap chain: copy the frames into the window on the CPU (ANativeWindow_lock) */
static int vswap_fail(void) {
    vswap_drop(1);
    if (VW.win && anw_load()) {
        VW.cpu = 1; g_anw.geom(VW.win, 0, 0, 2); VW.ww = g_anw.getw(VW.win); VW.wh = g_anw.geth(VW.win); VW.gw = VW.gh = -1;
        vlog("no swap chain for this window -> CPU copy into the window (%dx%d)\n", VW.ww, VW.wh);
        return VW.ww > 0 && VW.wh > 0;
    }
    VW.failed = 1; vlog("no swap chain and no CPU copy: frames are not shown\n");
    return 0;
}
/* a window change (me1gl_set_window) is applied here, by the rendering thread, under the D3D lock */
static void vwin_apply(void) {
    pthread_mutex_lock(&g_pmx); int gen = g_win_gen; void* w = g_win_pending; pthread_mutex_unlock(&g_pmx);
    if (gen == VW.gen) return;
    VW.gen = gen;
    vswap_drop(1);
    VW.win = w; VW.lost = VW.failed = VW.recreate = 0; VW.ww = VW.wh = 0; VW.gw = VW.gh = -1;
    int ok = 0;
    if (VW.cpu) {
        if (w && anw_load()) { g_anw.geom(w, 0, 0, 2); VW.ww = g_anw.getw(w); VW.wh = g_anw.geth(w); ok = VW.ww > 0 && VW.wh > 0; }
        if (ok) vlog("window %dx%d, CPU copy (%s)\n", VW.ww, VW.wh, VX.swapext ? "ME1_GL_PRESENT=cpu" : "driver without VK_KHR_swapchain");
        else if (w) vlog("CPU copy impossible for window %p (%s)\n", w, anw_load() ? "window without size" : "no ANativeWindow API");
    } else if (w || VW.fake) {
        ok = vswap_make();
        if (!ok) ok = vswap_fail();
        else if (VW.win) vlog("window %ux%u, Vulkan swap chain\n", VW.ext.width, VW.ext.height);
    }
    pthread_mutex_lock(&g_pmx); g_haswin = ok; g_win_ack = gen; pthread_cond_broadcast(&g_pcv); pthread_mutex_unlock(&g_pmx);
}
static void vwin_apply_cb(void* a) { (void)a; vwin_apply(); }
static void vb_set_window(void* anw) {
    pthread_mutex_lock(&g_pmx); g_win_pending = anw; g_win_gen++; pthread_mutex_unlock(&g_pmx);
    /* switch now if the renderer is idle (the old window may disappear when this returns); else at the next Present */
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 3;
    if (pthread_mutex_timedlock(&g_mx, &ts) == 0) {
        if (q_on()) q_call(vwin_apply_cb, NULL);   /* v1.2.1: ME1_D3D_CMD - on the replay thread, after the recorded frame */
        else vwin_apply();
        pthread_mutex_unlock(&g_mx);
    }
    else vlog("me1gl_set_window: renderer busy for 3 s, the window is switched at the next Present\n");
}
static int vswap_acquire(uint32_t* idx) {
    for (int tries = 0; tries < 3; tries++) {
        if (VW.lost || VW.failed) return 0;
#ifndef __ANDROID__
        if (VW.sc && VW.win && !VW.fake && (((Me1Window*)VW.win)->gen != VW.mgen ||
            (g_ws_mis && g_ws_mis < 10 && mono() - g_ws_mist > 0.1))) VW.recreate = 1;   /* v1.1: the front end resized the window */
#endif
        if ((!VW.sc || VW.recreate) && !vswap_make()) { if (!vswap_fail()) return 0; return 0; }
        double t = mono();
        VkResult r = subopt_test(vkAcquireNextImageKHR(VD, VW.sc, 1000000000ull, CS->acq, VK_NULL_HANDLE, idx));
        VT.wait += mono() - t;
        if (r == VK_SUCCESS) return 1;
        if (r == VK_SUBOPTIMAL_KHR) { vswap_subopt(); return 1; }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { VW.recreate = 1; continue; }
        if (r == VK_ERROR_SURFACE_LOST_KHR) { VW.lost = 1; vlog("window surface lost - frames are not shown until the next window\n"); return 0; }
        if (r == VK_TIMEOUT || r == VK_NOT_READY) { static int n; if (n++ < 10) vlog("vkAcquireNextImageKHR: timeout - frame not shown\n"); return 0; }
        vok(r, "vkAcquireNextImageKHR"); return 0;
    }
    return 0;
}
/* CPU presentation: the frame read back into s->rb goes into the locked window buffer (window aspect, black bars) */
static void vcpu_post(VSlot* s) {
    if (!VW.cpu || !VW.win || VW.ww <= 0 || VW.wh <= 0 || !s->rb) return;
    int fw = s->rbw, fh = s->rbh, bw = fw, bh = fh, ww = VW.ww, wh = VW.wh;
    if (g_stretch == 1) { bw = ww; bh = wh; } /* stretch */
    else if (g_stretch == 2) { if ((int64_t)ww * fh > (int64_t)wh * fw) bw = (int)(((int64_t)fh * ww + wh / 2) / wh); else bh = (int)(((int64_t)fw * wh + ww / 2) / ww); }
    else { if ((int64_t)ww * fh > (int64_t)wh * fw) bw = (int)(((int64_t)fh * ww + wh / 2) / wh); else bh = (int)(((int64_t)fw * wh + ww / 2) / ww); }
    bw = (bw + 1) & ~1; if (bw < fw) bw = fw; if (bh < fh) bh = fh;
    if (bw != VW.gw || bh != VW.gh) { g_anw.geom(VW.win, bw, bh, 2 /* WINDOW_FORMAT_RGBX_8888 */); VW.gw = bw; VW.gh = bh; }
    pthread_mutex_lock(&g_pmx); set_view(ww, wh, fw, fh); pthread_mutex_unlock(&g_pmx);
    AnwBuf b; memset(&b, 0, sizeof b);
    if (g_anw.lock(VW.win, &b, NULL) != 0 || !b.bits) { static int k; if (k++ < 5) vlog("CPU present: ANativeWindow_lock failed\n"); return; }
    double t0 = mono();
    vmem_invalidate(&s->rbm);
    const uint8_t* src = vmem_ptr(&s->rbm);
    if (src && b.width >= fw && b.height >= fh && b.stride >= b.width) {
        int ox = (b.width - fw) / 2, oy = (b.height - fh) / 2, rx = b.width - ox - fw; uint8_t* px = b.bits; size_t st = (size_t)b.stride * 4;
        for (int y = 0; y < b.height; y++) {
            uint8_t* row = px + st * (size_t)y;
            if (y < oy || y >= oy + fh) { memset(row, 0, (size_t)b.width * 4); continue; }
            if (ox) memset(row, 0, (size_t)ox * 4);
            if (rx > 0) memset(row + (size_t)(ox + fw) * 4, 0, (size_t)rx * 4);
            const uint8_t* sr = src + (size_t)(y - oy) * (size_t)fw * 4; uint8_t* o = row + (size_t)ox * 4;
            if (s->rbpend == 2) for (int x = 0; x < fw; x++) { uint32_t v; memcpy(&v, sr + 4 * x, 4); v = (v & 0xFF00FF00u) | (v >> 16 & 0xFFu) | (v & 0xFFu) << 16; memcpy(o + 4 * x, &v, 4); }
            else memcpy(o, sr, (size_t)fw * 4);
        }
    } else { static int k; if (k++ < 5) vlog("CPU present: window buffer %dx%d (stride %d) smaller than the frame %dx%d\n", b.width, b.height, b.stride, fw, fh); }
    g_cpucopy_us += (uint64_t)((mono() - t0) * 1e6);
    g_anw.post(VW.win); g_shown++;
}
static void vpc_save(void);
/* ---- v0.10.7: DEVICE_LOST recovery ----
 * The driver crashed (VK_ERROR_DEVICE_LOST). D3D9 semantics to the rescue: the game is told "device lost"
 * (TestCooperativeLevel/Present return D3DERR_DEVICELOST) and calls Reset; Reset re-creates the whole Vulkan
 * device and every renderer resource. Textures/buffers survive because D3D9 MANAGED objects now keep their
 * guest copies (persistent() includes pool 1) and re-upload lazily on first use. ME1_VK_LOSTRESET=0: old
 * behaviour (black screen). If the game does not Reset within 5 s, present() recovers on its own. */
static int vb_init(void);
static void vb_teardown(void);
static void vb_free(Obj* o);
static int g_nrecover; static double g_recover_t0 = -1; static uint64_t g_recover_frame;
static int vk_recover_max(void) {   /* v0.10.12 (P0-2): Mali gets one in-process attempt, others four */
    const char* e = getenv("ME1_VK_RECOVER_MAX"); if (e && *e) return atoi(e);
    return VPP.vendorID == 0x13b5 ? 1 : 4;
}
/* v0.10.10: if the driver dies again and again (4 recoveries in 30 s), a fresh Vulkan device in THIS process is
 * pointless - the broken driver state stays with the process. Restart the whole game instead: the process exits
 * with code 77 and GameActivity re-launches itself; shader caches are warm, so the restart is quick. */
static void vk_driver_hopeless(void) {
    fprintf(stderr, "[vk] the driver keeps dying - restarting the game process (code 77)\n");
    hle_report(); fflush(stdout); fflush(stderr);
    if (hle_restart_hook) { hle_restart_hook(); usleep(1500 * 1000); }
    else if (hle_toast_hook) hle_toast_hook("Видеодрайвер умирает серийно — нужен перезапуск игры.");
    _exit(77);
}
static int vk_device_reset(void) {
    if (!g_lostreset_ok) return 0;   /* v0.10.11 rollback: the recovery flow is opt-in */
    /* v0.10.12 (P0-2): a re-loss shortly after a recovery means the broken driver state persists - restart, do not burn
     * hundreds of MB on another leaked device */
    if (g_nrecover > 0 && g_st.frames - g_recover_frame < 300) {
        vlog("re-lost %llu frames after recovery #%d - the driver cannot go on\n",
             (unsigned long long)(g_st.frames - g_recover_frame), g_nrecover);
        vk_driver_hopeless();
        return 0;
    }
    if (g_nrecover >= vk_recover_max()) {
        vlog("recovery limit reached (%d)\n", g_nrecover);
        vk_driver_hopeless();
        return 0;
    }
    int lost = g_vklost;
    if (lost) { g_secn = 0; memset(&RP, 0, sizeof RP); g_vq_act = 0; }
    else rp_end();  /* flush before destroying snapshotted resources */
    unsigned freed = 0;
    vlog("DEVICE LOST recovery #%d: re-creating the device and all renderer resources (vkmem %llu MB)\n",
         g_nrecover + 1, (unsigned long long)(g_vkmem >> 20));
    memset(g_vprog, 0, sizeof g_vprog); g_vprogid = 0;   /* first: the caches vb_free touches */
    memset(g_vpipe, 0, sizeof g_vpipe); g_npipes = 0;
    for (int i = 0; i < g_nobj; i++) {
        Obj* o = g_obj[i];
        if (!o || !o->gx || o->type < T_TEX) continue;
        if (o->type == T_QUERY) { QRes* q = o->gx; if (q->st == 1 || q->st == 2) q_done(q, q_visible(q), 0); continue; }
        if (o->type == T_VS || o->type == T_PS) continue;  /* translation is CPU-only; GPU programs were discarded */
        vb_free(o); freed++;   /* not gl_free: the GLES header macro maps that name to gles_free here */
    }
    g_qopen = NULL; g_qpend_n = 0; g_vq_act = 0;
    g_ndead = 0;   /* the old device is gone - its handles are gone with it */
    memset(g_tcache, 0, sizeof g_tcache);
    memset(g_vsmp, 0, sizeof g_vsmp); g_nvsmp = 0; g_vsmp_near = VK_NULL_HANDLE;
    g_vconst = VK_NULL_HANDLE; g_vconstm.blk = -1;   /* v0.10.12 (P0-1): blk 0 is a VALID allocator slot, "empty" is -1 */
    g_dsl[0] = g_dsl[1] = VK_NULL_HANDLE; g_pl = VK_NULL_HANDLE; g_nupool = 0;
    rp_forget(); memset(&RP, 0, sizeof RP); memset(&VB, 0, sizeof VB); memset(&VT, 0, sizeof VT);
    /* v0.10.12 (P0-1): device-owned caches with stale handles of the old device */
    if (!lost && VD) {   /* the device is alive: destroy honestly, before vkDestroyDevice */
        for (int i = 0; i < g_nrpc; i++) for (int k = 0; k < 64; k++) if (g_rpc[i].rp[k]) vkDestroyRenderPass(VD, g_rpc[i].rp[k], NULL);
        for (int i = 0; i < NVFB; i++) if (g_vfb[i].fb) vkDestroyFramebuffer(VD, g_vfb[i].fb, NULL);
        if (g_rbk) { vkDestroyBuffer(VD, g_rbk, NULL); vmem_free(&g_rbkm); }
    }
    memset(g_rpc, 0, sizeof g_rpc); g_nrpc = 0;
    memset(g_vfb, 0, sizeof g_vfb); g_vfbtick = 0;
    g_rbk = VK_NULL_HANDLE; memset(&g_rbkm, 0, sizeof g_rbkm); g_rbkm.blk = -1; g_rbksz = 0;
    g_vokn = 0;   /* (P2-9) errors of the new device must reach the log */
    g_vklost = 0;
    if (lost) {
        /* v0.10.8: the device was LOST - its driver threads may still touch their objects, so destroy NOTHING
         * (a destroy-after-loss crashed the process inside the driver, log 0.10.7: SIGSEGV in libGLES_mali).
         * The old instance/device/swapchain/memory are leaked deliberately; a fresh instance is created below. */
        vlog("the lost device is leaked (destroying it crashes the driver); %u resources freed\n", freed);
        VI = VK_NULL_HANDLE; VP = VK_NULL_HANDLE; VD = VK_NULL_HANDLE; VQ = VK_NULL_HANDLE; g_vkdbg = VK_NULL_HANDLE;
        sec_free_hosts(); memset(S, 0, sizeof S); g_secn = 0; g_sec_on = 0; g_sec_fail_test = 0; g_sec_last_tex = VK_NULL_HANDLE;
        g_mb = NULL; g_nmb = g_cmb = 0;   /* host allocations of the dead device: leaked with it */
        g_vpc = VK_NULL_HANDLE;
        memset(VW.rs, 0, sizeof VW.rs); memset(VW.img, 0, sizeof VW.img); VW.n = 0;   /* old-device handles: only forget */
        g_vkmem = 0; g_vkallocs = 0;
        struct timespec ts = { 0, 300 * 1000 * 1000 }; nanosleep(&ts, NULL);   /* let the driver's threads settle */
    } else {
        vlog("%u resources freed, vkmem %llu MB\n", freed, (unsigned long long)(g_vkmem >> 20));
        vswap_drop(1);   /* swap chain + surface + acquire semaphores of the live device */
        vb_teardown();
    }
    VW.sc = VK_NULL_HANDLE; VW.surf = VK_NULL_HANDLE;
    VW.cpu = 0; VW.failed = 0; VW.lost = 0; VW.recreate = 1;
    g_nrecover++;
    GS.texmem = 0;   /* old-device accounting includes dummy images; the new init accounts its own */
    if (!vb_init()) { g_vklost = 1; vlog("recovery failed - black screen (ME1_VK_LOSTRESET=0 disables this flow)\n");
        if (hle_toast_hook) hle_toast_hook("Видеодрайвер упал повторно — черный экран. Смените GPU-драйвер в лаунчере (обычно помогает системный GLES).");
        return 0; }
    /* Reapply the live window to the new instance, even when its pointer did not change. */
    pthread_mutex_lock(&g_pmx); g_win_gen++; pthread_mutex_unlock(&g_pmx);
    g_recover_frame = g_st.frames;
    vlog("recovery complete - rendering continues (frame %llu, skipped draws now %llu)\n", (unsigned long long)g_st.frames, (unsigned long long)GS.skipped);
    return 1;
}

static void vb_present(Obj* bb) {
    if (!bb) return;
    {   /* v0.10.12 (P2-6): reproducible DEVICE_LOST for tests - ME1_VK_FAKELOST=<present frame> */
        const char* fl = getenv("ME1_VK_FAKELOST");
        static int fired;
        if (!fired && fl && *fl && g_pframes + 1 == (uint64_t)strtoull(fl, NULL, 10)) { fired = 1; vok(VK_ERROR_DEVICE_LOST, "ME1_VK_FAKELOST"); }
    }
    /* Recovery runs on the caller's Reset or timeout Present, not midway through
     * this callback: an Att acquired from the old device cannot survive reset. */
    if (g_vklost) return;
    vwin_apply();
    Att a; if (!att_of(bb, &a) || a.r->kind != 1) return;
    uint32_t w, h; att_size(&a, &w, &h);
    g_pframes++; g_vq_flush = 0;
    if (g_fdump && (g_pframes == 1 || g_pframes % (uint64_t)g_dumpevery == 0)) vdump(&a, w, h);
    rp_end();
    VSlot* s = CS; VkCommandBuffer cb = s->cmd; int32_t idx = -1;
    if (VW.cpu) {
        if (VW.win && VW.ww > 0 && a.r->f && a.r->f->conv == CV_RAW && a.r->obpp == 4 && !a.r->dec) {
            VkDeviceSize n = (VkDeviceSize)w * h * 4;
            if (n > s->rbsize) {   /* this slot's previous frame is finished (slot_begin waited for it) */
                if (s->rb) { vkDestroyBuffer(VD, s->rb, NULL); vmem_free(&s->rbm); s->rb = VK_NULL_HANDLE; s->rbsize = 0; }
                if (mk_buffer(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &s->rb, &s->rbm)) s->rbsize = n;
            }
            if (s->rb) {
                img_to(cb, a.r, a.face, 1, a.lvl, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy c; memset(&c, 0, sizeof c);
                c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = a.lvl; c.imageSubresource.baseArrayLayer = a.face;
                c.imageSubresource.layerCount = 1; c.imageExtent.width = w; c.imageExtent.height = h; c.imageExtent.depth = 1;
                vkCmdCopyImageToBuffer(cb, a.r->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->rb, 1, &c);
                host_barrier(cb);
                s->rbw = (int)w; s->rbh = (int)h; s->rbpend = a.r->vf == VK_FORMAT_B8G8R8A8_UNORM ? 2 : 1;
                a.r->use = a.r->mainuse = g_vserial;
            }
        }
    } else if ((VW.win || VW.fake) && !VW.lost && !VW.failed) {
        uint32_t ui = 0;
        if (vswap_acquire(&ui)) {
            VkImage si = VW.img[ui];
            VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkImageMemoryBarrier b; memset(&b, 0, sizeof b);
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = si; b.subresourceRange = rg;
            b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            pthread_mutex_lock(&g_pmx); set_view((int)VW.ext.width, (int)VW.ext.height, (int)w, (int)h);
            int vx = g_view[0], vy = g_view[1], vw = g_view[2], vh = g_view[3]; pthread_mutex_unlock(&g_pmx);
            if (vx > 0 || vy > 0 || vw < (int)VW.ext.width || vh < (int)VW.ext.height) {   /* bars */
                VkClearColorValue black; memset(&black, 0, sizeof black); black.float32[3] = 1.f;
                vkCmdClearColorImage(cb, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.oldLayout = b.newLayout;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            }
            img_to(cb, a.r, a.face, 1, a.lvl, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageBlit bl; memset(&bl, 0, sizeof bl);
            bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.srcSubresource.mipLevel = a.lvl; bl.srcSubresource.baseArrayLayer = a.face; bl.srcSubresource.layerCount = 1;
            bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.dstSubresource.layerCount = 1;
            bl.srcOffsets[1].x = (int32_t)w; bl.srcOffsets[1].y = (int32_t)h; bl.srcOffsets[1].z = 1;
            bl.dstOffsets[0].x = vx; bl.dstOffsets[0].y = vy; bl.dstOffsets[1].x = vx + vw; bl.dstOffsets[1].y = vy + vh; bl.dstOffsets[1].z = 1;
            int lin = (fmt_opt(a.r->vf) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && (vw != (int)w || vh != (int)h);
            vkCmdBlitImage(cb, a.r->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            a.r->use = a.r->mainuse = g_vserial;
            idx = (int32_t)ui;
        }
    }
    vb_submit(idx, 0, 1);
    if (g_vklost) return;
    if (VW.cpu) {   /* post the previous frame as soon as it is done: one frame of latency, the GPU keeps working on this one */
        VSlot* p = &S[(g_si + NSLOT - 2) % NSLOT];
        if (p->pending && p->rbpend) { double t = mono(); int ok = vok(vkWaitForFences(VD, 1, &p->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"); VT.wait += mono() - t; if (ok) slot_finish(p); }
    }
    if (!g_vklost && g_pframes % 900 == 0) vpc_save();
}

/* ---- statistics, lifetime ---- */
static void vb_frame_times(char* o, size_t cap, uint64_t frames) {
    size_t k = 0; o[0] = 0;
    if (VT.nf) k += (size_t)snprintf(o + k, cap - k, ", GPU %.1f ms/frame", (double)VT.ns / 1e6 / (double)VT.nf);
    if (frames && k < cap) k += (size_t)snprintf(o + k, cap - k, ", Present waits %.1f ms/frame", (VT.wait - VT.waitf) * 1e3 / (double)frames);
    gp_report(o, cap, &k);
    VT.ns = VT.nf = 0; VT.waitf = VT.wait;
}
static void vpc_save(void) {
    if (!g_vpc || !g_vpcpath[0] || g_vpc_new == g_vpc_saved) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(VD, g_vpc, &n, NULL) < 0 || !n) return;
    void* p = malloc(n);
    if (p && vkGetPipelineCacheData(VD, g_vpc, &n, p) >= 0) {
        char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", g_vpcpath);
        FILE* f = fopen(tmp, "wb"); int ok = f && fwrite(p, 1, n, f) == n; if (f && fclose(f)) ok = 0;
        if (ok && rename(tmp, g_vpcpath) == 0) { if (g_log) vlog("pipeline cache saved: %zu bytes\n", n); } else remove(tmp);
    }
    free(p); g_vpc_saved = g_vpc_new;
}
static void vb_report(void) {
    if (g_vpa_jobs) vlog("async pipeline: %llu jobs (%llu wide), preparation %.1f ms, post-preparation wait %.1f ms (wall time)\n",
        (unsigned long long)g_vpa_jobs, (unsigned long long)g_vpa_wide, g_vpa_work_s * 1e3, g_vpa_wait_s * 1e3);
    if (g_sec_batches || g_sec_fallback) vlog("secondary: %llu batches, %llu draws, %llu multi-lane batches, %llu fallbacks\n",
        (unsigned long long)g_sec_batches, (unsigned long long)g_sec_draws,
        (unsigned long long)g_sec_parallel, (unsigned long long)g_sec_fallback);
    vlog("report: %llu draws (%llu skipped, %llu pre-transformed), %llu clears, %llu blits, %llu programs (%llu SPIR-V from cache, %llu failed), "
         "%llu shaders translated (%llu not), %llu pipelines, %llu texture uploads (%llu MB, %llu CPU-decoded), %llu buffer uploads (%llu MB), "
         "%llu MB streamed, %llu MB images, %llu MB device memory in %llu allocations, %llu submits; %llu frames, %llu shown\n",
         (unsigned long long)GS.draws, (unsigned long long)GS.skipped, (unsigned long long)GS.rhw, (unsigned long long)GS.clears, (unsigned long long)GS.blits,
         (unsigned long long)GS.progs, (unsigned long long)GS.progbin, (unsigned long long)GS.progfail, (unsigned long long)GS.shtrans, (unsigned long long)GS.shfail,
         (unsigned long long)g_npipes, (unsigned long long)GS.texup, (unsigned long long)(GS.texbytes >> 20), (unsigned long long)GS.decoded,
         (unsigned long long)GS.bufup, (unsigned long long)(GS.bufbytes >> 20), (unsigned long long)(GS.ring >> 20), (unsigned long long)(GS.texmem >> 20),
         (unsigned long long)(g_vkmem >> 20), (unsigned long long)g_vkallocs, (unsigned long long)VT.subs, (unsigned long long)g_pframes, (unsigned long long)g_shown);
    if (VX.validate) vlog("validation layer: %d messages\n", VX.ndebug);
    if (VW.cpu && g_shown) vlog("CPU present: %.2f ms per frame (copy into the window)\n", g_cpucopy_us / 1e3 / (double)g_shown);
    if (g_subopt_n) vlog("swap chain: %llu suboptimal acquires/presents kept without recreating\n", (unsigned long long)g_subopt_n);
    vpc_save();
}
static void vb_free(Obj* o) {
    if (!o->gx) return;
    if (o->type == T_VS || o->type == T_PS) {
        GlSh* s = o->gx;
        if (VD) vprog_drop(s->serial);
        sm3_free(&s->s); free(s);
    } else {
        VRes* g = o->gx;
        if (VD && (g->kind == 1 || g->kind == 2) && g->img) {
            if (rp_is_att(g)) rp_forget();
            fb_purge(g->serial);
            if (g->sview) dead_add(g_vserial, 2, H(g->sview), NULL);
            if (g->av) for (int i = 0; i < g->nsub; i++) if (g->av[i]) dead_add(g_vserial, 2, H(g->av[i]), NULL);
            dead_add(g_vserial, 1, H(g->img), &g->m);
        } else if (VD && g->kind == 3 && g->buf) dead_add(g_vserial, 3, H(g->buf), &g->m);
        free(g->av); free(g->lay);
        texmem_add(-(int64_t)g->bytes);
        free(g);
    }
    o->gx = NULL;
}

/* ---- initialization ---- */
static void* vk_lib_open(int* custom) {
    const char* e = getenv("ME1_GPU_DRIVER"); void* lib = NULL; *custom = 0;
    if (e && e[0] == '/') {
        lib = vk_open_custom(e);
        if (!lib) { vlog("custom driver %s did not load\n", e); return NULL; }
        *custom = 1; return lib;
    }
    lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) vlog("no libvulkan (%s)\n", dlerror());
    return lib;
}
static int vx_has(const VkExtensionProperties* x, uint32_t n, const char* name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(x[i].extensionName, name)) return 1; return 0; }
static void vb_teardown(void) {
    if (VD) { vkDeviceWaitIdle(VD);
        for (int si = 0; si < NSLOT; si++) for (int i = 0; i < SEC_LANES; i++)
            if (S[si].sec[i].pool) vkDestroyCommandPool(VD, S[si].sec[i].pool, NULL);
        vkDestroyDevice(VD, NULL); }
    if (g_vkdbg && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(VI, g_vkdbg, NULL);
    if (VI && vkDestroyInstance) vkDestroyInstance(VI, NULL);
    VI = VK_NULL_HANDLE; VP = VK_NULL_HANDLE; VD = VK_NULL_HANDLE; VQ = VK_NULL_HANDLE; g_vkdbg = VK_NULL_HANDLE;
    sec_free_hosts(); memset(S, 0, sizeof S); g_secn = 0; free(g_mb); g_mb = NULL; g_nmb = g_cmb = 0; g_vpc = VK_NULL_HANDLE; g_nupool = 0; g_nfp = 0;
}
static int vb_init(void) {
    const char* e;
    /* the same switches as the GLES backend */
    g_gldbg = (e = getenv("ME1_GL_DEBUG")) ? atoi(e) : 0;
    if ((e = getenv("ME1_TEX_MAXSIZE")) && atoi(e) >= 16) g_texmax = atoi(e);
    g_tex16 = (e = getenv("ME1_TEX_16")) ? atoi(e) : 1;
    if ((e = getenv("ME1_TEX_BC")) && *e >= '0' && *e <= '2') g_texbc = *e - '0';   /* v1.2.1 */
    if ((e = getenv("ME1_FRAME_DUMP")) && *e) { g_fdump = e; mkdir(e, 0755); }
    if ((e = getenv("ME1_FRAME_DUMP_EVERY")) && atoi(e) > 0) g_dumpevery = atoi(e);
    if ((e = getenv("ME1_GLSL_DUMP")) && *e) { g_glsldump = e; mkdir(e, 0755); }
    if ((e = getenv("ME1_GL_CACHE")) && *e && strcmp(e, "off")) { snprintf(g_bincache, sizeof g_bincache, "%s", e); mkdir(g_bincache, 0755); }
    { const char* fe = getenv("ME1_FLOAT_EMU"); if (fe && *fe >= '0' && *fe <= '2') sm3_float_emu = *fe - '0'; }
    { const char* iv = getenv("ME1_INVARIANT"); if (iv && *iv >= '0' && *iv <= '1') sm3_invariant = *iv - '0'; }
    { const char* sl = getenv("ME1_SHADER_LITE"); sm3_lite = sl && *sl == '1'; }
    { const char* sm = getenv("ME1_STRETCH_MODE");
        if (!sm || !*sm) g_stretch = 0;
        else if (!strcmp(sm, "stretch")) g_stretch = 1;
        else if (!strcmp(sm, "fill")) g_stretch = 2;
        else g_stretch = 0; }
    VX.strict = (e = getenv("ME1_VK_STRICT")) && *e == '1';
    VX.split = 2048;   /* pre-init default; the vendor-specific one is set after vkGetPhysicalDeviceProperties */
    if ((e = getenv("ME1_VK_SPLIT")) && *e) VX.split = (uint32_t)atoi(e);
    VX.splitprims = 0;   /* v0.10.12 (P0-4): ME1_VK_SPLITPRIMS - also split by primitives (Mali default 150000, set after VPP) */
    if ((e = getenv("ME1_VK_SPLITPRIMS")) && *e) VX.splitprims = (uint64_t)strtoull(e, NULL, 10);
    g_pipeidle = (e = getenv("ME1_VK_PIPEIDLE")) && *e == '1';
    g_sec_on = (e = getenv("ME1_VK_SECONDARY")) && !strcmp(e, "1");
    g_sec_fail_test = (e = getenv("ME1_VK_SEC_TEST_FAIL")) ? atoi(e) : 0;
    g_secn = 0; g_sec_last_tex = VK_NULL_HANDLE;
    /* Mali (0x13b5): secondary command buffers are unstable - force off by default even if requested. */
    if (g_sec_on) {
        /* vendor not yet known (VPP set later); keep flag, but guard at device init below */
        vlog("experimental secondary command recording requested (Mali: off by default)\n");
    }
    VX.validate = (e = getenv("ME1_VK_VALIDATE")) && *e == '1';
    /* GLSL -> SPIR-V compiler (glslang) */
    const char* sp = getenv("ME1_SPV_LIB"); if (!sp || !*sp) sp = "libme1spv.so";
    void* sl = dlopen(sp, RTLD_NOW | RTLD_LOCAL);
    if (!sl && !strchr(sp, '/')) {   /* not on the search path (test executable without LD_LIBRARY_PATH): next to this library */
        Dl_info di; char pth[1024];
        if (dladdr((void*)vb_init, &di) && di.dli_fname && strrchr(di.dli_fname, '/')) {
            snprintf(pth, sizeof pth, "%.*s/%s", (int)(strrchr(di.dli_fname, '/') - di.dli_fname), di.dli_fname, sp);
            sl = dlopen(pth, RTLD_NOW | RTLD_LOCAL);
        }
    }
    if (sl) { p_spv_compile = (__typeof__(p_spv_compile))dlsym(sl, "me1spv_compile"); p_spv_free = (__typeof__(p_spv_free))dlsym(sl, "me1spv_free"); }
    if (!p_spv_compile || !p_spv_free) { vlog("SPIR-V compiler %s not available (%s)\n", sp, sl ? "symbols missing" : dlerror()); return 0; }
    spv_prewarm();   /* v1.2.1: glslang's one-time set-up (~0.1 s) on a helper thread while the device is created */
    int custom = 0;
    void* lib = vk_lib_open(&custom); if (!lib) return 0;
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) { vlog("no vkGetInstanceProcAddr\n"); return 0; }
#define VK_LG(n) n = (PFN_##n)vkGetInstanceProcAddr(NULL, #n); if (!n) { vlog("%s missing\n", #n); return 0; }
    VKG(VK_LG)
    vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    uint32_t iv = VK_API_VERSION_1_0; if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&iv);
    uint32_t api = iv >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    /* instance */
    static VkExtensionProperties ix[256]; uint32_t nix = 256;
    if (vkEnumerateInstanceExtensionProperties(NULL, &nix, ix) < 0) nix = 0;
    const char* ien[8]; uint32_t nien = 0; const char* lay[1]; uint32_t nlay = 0;
    int surf = vx_has(ix, nix, VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef __ANDROID__
    VX.android = surf && vx_has(ix, nix, "VK_KHR_android_surface");
    if (VX.android) { ien[nien++] = VK_KHR_SURFACE_EXTENSION_NAME; ien[nien++] = "VK_KHR_android_surface"; }
#else
    VX.headless = surf && vx_has(ix, nix, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    { const char* wse = g_ws_kind == ME1_WS_X11 ? "VK_KHR_xlib_surface" : g_ws_kind == ME1_WS_WAYLAND ? "VK_KHR_wayland_surface" : NULL;
      VX.ws = surf && wse && vx_has(ix, nix, wse);   /* v1.1: desktop window */
      if (wse && !VX.ws) vlog("instance without %s: no window output\n", wse);
      if (VX.ws || VX.headless) ien[nien++] = VK_KHR_SURFACE_EXTENSION_NAME;
      if (VX.ws) ien[nien++] = wse;
      if (VX.headless) ien[nien++] = VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME; }
#endif
    if (VX.validate) {
        VkLayerProperties lp[64]; uint32_t nlp = 64; if (vkEnumerateInstanceLayerProperties(&nlp, lp) < 0) nlp = 0;
        for (uint32_t i = 0; i < nlp; i++) if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) { lay[0] = "VK_LAYER_KHRONOS_validation"; nlay = 1; }
        if (!nlay) vlog("ME1_VK_VALIDATE=1: VK_LAYER_KHRONOS_validation not installed\n");
        if (vx_has(ix, nix, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) ien[nien++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        else if (nlay) ien[nien++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;   /* provided by the layer */
    }
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "MassEffect", 1, "me1port", 10, api };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, nlay, lay, nien, ien };
    VkResult r = vkCreateInstance(&ici, NULL, &VI);
    if (r < 0 && nlay) { vlog("vkCreateInstance with the validation layer failed (%d) - without\n", r); nlay = 0; ici.enabledLayerCount = 0;
                         if (nien && !strcmp(ien[nien - 1], VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) ici.enabledExtensionCount = --nien; r = vkCreateInstance(&ici, NULL, &VI); }
    if (!vok(r, "vkCreateInstance")) { VI = VK_NULL_HANDLE; return 0; }
#define VK_LI(n) n = (PFN_##n)vkGetInstanceProcAddr(VI, #n); if (!n) { vlog("%s missing\n", #n); vb_teardown(); return 0; }
#define VK_LIO(n) n = (PFN_##n)vkGetInstanceProcAddr(VI, #n);
    VKI(VK_LI) VKIO(VK_LIO)
#ifndef __ANDROID__
    if (VX.ws) vkCreateWsSurface = (Me1PFN_CreateWsSurface)vkGetInstanceProcAddr(VI, g_ws_kind == ME1_WS_X11 ? "vkCreateXlibSurfaceKHR" : "vkCreateWaylandSurfaceKHR");
#endif
    if (VX.validate && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT di; memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        di.pfnUserCallback = vk_dbg_cb;
        vkCreateDebugUtilsMessengerEXT(VI, &di, NULL, &g_vkdbg);
    }
    /* physical device: ME1_VK_DEVICE=n, else the first GPU with a graphics queue: discrete, then integrated/other, then CPU */
    VkPhysicalDevice pds[8]; uint32_t npd = 8;
    if (vkEnumeratePhysicalDevices(VI, &npd, pds) < 0) npd = 0;
    if (!npd) { vlog("no Vulkan device\n"); vb_teardown(); return 0; }
    int want = (e = getenv("ME1_VK_DEVICE")) ? atoi(e) : -1, best = -1, bscore = -1; uint32_t bq = 0;
    for (uint32_t i = 0; i < npd; i++) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp);
        VkQueueFamilyProperties qf[16]; uint32_t nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qf);
        int q = -1; for (uint32_t k = 0; k < nq && q < 0; k++) if (qf[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) q = (int)k;
        int score = q < 0 ? -1 : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 1 : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : 2;   /* v1.1: discrete first (laptops) */
        if (want >= 0) score = (int)i == want && q >= 0 ? 4 : -1;
        if (score > bscore) { bscore = score; best = (int)i; bq = (uint32_t)(q < 0 ? 0 : q); }
    }
    if (best < 0 || bscore < 0) { vlog("no Vulkan device with a graphics queue\n"); vb_teardown(); return 0; }
    VP = pds[best]; VQF = bq;
    vkGetPhysicalDeviceProperties(VP, &VPP); vkGetPhysicalDeviceMemoryProperties(VP, &VMP); vkGetPhysicalDeviceFeatures(VP, &VFH);
    if (!getenv("ME1_VK_SPLIT")) VX.split = VPP.vendorID == 0x13b5 ? 384 : 2048;   /* v0.10.12 (P0-4): Mali - small submissions (set after VPP, unlike the pre-init default) */
    if (!getenv("ME1_VK_SPLITPRIMS")) VX.splitprims = VPP.vendorID == 0x13b5 ? 150000 : 0;
    /* V3: secondary command buffers default off on Mali unless explicitly re-enabled via ME1_VK_SECONDARY=force */
    if (VPP.vendorID == 0x13b5 && g_sec_on) {
        const char* e = getenv("ME1_VK_SECONDARY");
        if (!e || strcmp(e, "force")) { g_sec_on = 0; vlog("secondary command recording disabled on Mali (default off)\n"); }
    }
    VkQueueFamilyProperties qf[16]; uint32_t nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(VP, &nq, qf);
    VX.api = VPP.apiVersion < api ? VPP.apiVersion : api;
    snprintf(VX.name, sizeof VX.name, "%s", VPP.deviceName);
    /* device */
    static VkExtensionProperties dx[512]; uint32_t ndx = 512;
    if (vkEnumerateDeviceExtensionProperties(VP, NULL, &ndx, dx) < 0) ndx = 0;
    const char* den[4]; uint32_t nden = 0;
    VX.swapext = (VX.android || VX.headless || VX.ws) && vx_has(dx, ndx, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (VX.swapext) den[nden++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    memset(&VFE, 0, sizeof VFE);
    VFE.samplerAnisotropy = VFH.samplerAnisotropy;
    VFE.textureCompressionBC = VFH.textureCompressionBC && !VX.strict && !getenv("ME1_VK_NOBC");
    VFE.independentBlend = VFH.independentBlend;
    VFE.fullDrawIndexUint32 = VFH.fullDrawIndexUint32;
    VFE.occlusionQueryPrecise = VFH.occlusionQueryPrecise;   /* v1.1.3: D3D9 occlusion queries count samples */
    float prio = 1.f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, VQF, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, nden, den, &VFE };
    if (!vok(vkCreateDevice(VP, &dci, NULL, &VD), "vkCreateDevice")) { VD = VK_NULL_HANDLE; vb_teardown(); return 0; }
#define VK_LD(n) n = (PFN_##n)vkGetDeviceProcAddr(VD, #n); if (!n) { vlog("%s missing\n", #n); vb_teardown(); return 0; }
#define VK_LDO(n) n = (PFN_##n)vkGetDeviceProcAddr(VD, #n);
    VKD(VK_LD)
    if (VX.swapext) { VKDO(VK_LDO) if (!vkCreateSwapchainKHR || !vkAcquireNextImageKHR || !vkQueuePresentKHR || !vkGetSwapchainImagesKHR) VX.swapext = 0; }
    vkGetDeviceQueue(VD, VQF, 0, &VQ);
    VX.oprec = VFE.occlusionQueryPrecise; g_vq_act = 0;
    VX.bc = VFE.textureCompressionBC && !((e = getenv("ME1_VK_BC")) && *e == '0'); VX.aniso = VFE.samplerAnisotropy; VX.maxaniso = VPP.limits.maxSamplerAnisotropy; VX.indep = VFE.independentBlend;
    VX.maxtex = VPP.limits.maxImageDimension2D; VX.tsperiod = VPP.limits.timestampPeriod;
    VX.uboalign = VPP.limits.minUniformBufferOffsetAlignment < 16 ? 16 : VPP.limits.minUniformBufferOffsetAlignment;
    uint32_t tsb = VQF < nq ? qf[VQF].timestampValidBits : 0;
    VX.ts = tsb > 0 && VX.tsperiod > 0 && !((e = getenv("ME1_GPU_TIMER")) && *e == '0');
    g_tsmask = tsb >= 64 ? ~0ull : tsb ? (1ull << tsb) - 1 : 0;
    VX.bgravtx = (fmt_buf(VK_FORMAT_B8G8R8A8_UNORM) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    static const VkFormat dsf[3] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM };
    VX.ds = VK_FORMAT_UNDEFINED;
    for (int i = 0; i < 3 && !VX.ds; i++) if (fmt_opt(dsf[i]) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { VX.ds = dsf[i]; VX.dsstencil = i < 2; }
    if (!VX.ds) { vlog("no depth/stencil format\n"); vb_teardown(); return 0; }
    /* descriptor set layouts: set 0 = the two dynamic constant blocks, set 1 = 16 PS + 4 VS samplers */
    VkDescriptorSetLayoutBinding b0[2] = { { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
                                           { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutBinding b1[20];
    for (uint32_t i = 0; i < 20; i++) { b1[i].binding = i; b1[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b1[i].descriptorCount = 1;
                                        b1[i].stageFlags = i < 16 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; b1[i].pImmutableSamplers = NULL; }
    VkDescriptorSetLayoutCreateInfo l0 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b0 }, l1 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 20, b1 };
    if (!vok(vkCreateDescriptorSetLayout(VD, &l0, NULL, &g_dsl[0]), "vkCreateDescriptorSetLayout") ||
        !vok(vkCreateDescriptorSetLayout(VD, &l1, NULL, &g_dsl[1]), "vkCreateDescriptorSetLayout")) { vb_teardown(); return 0; }
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, g_dsl, 0, NULL };
    if (!vok(vkCreatePipelineLayout(VD, &pli, NULL, &g_pl), "vkCreatePipelineLayout")) { vb_teardown(); return 0; }
    /* pipeline cache (header checked: same driver / device) */
    size_t pcn = 0; void* pcd = NULL;
    if (g_bincache[0]) {
        snprintf(g_vpcpath, sizeof g_vpcpath, "%s/vk_pipeline_cache.bin", g_bincache);
        FILE* f = fopen(g_vpcpath, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            if (n >= 32 && n < (256l << 20) && (pcd = malloc((size_t)n)) && fread(pcd, 1, (size_t)n, f) == (size_t)n) {
                uint32_t hd[4]; memcpy(hd, pcd, 16);
                if (hd[0] < 32 || hd[1] != 1 || hd[2] != VPP.vendorID || hd[3] != VPP.deviceID || memcmp((uint8_t*)pcd + 16, VPP.pipelineCacheUUID, 16)) {
                    vlog("pipeline cache from another driver - ignored\n"); free(pcd); pcd = NULL;
                } else pcn = (size_t)n;
            } else { free(pcd); pcd = NULL; }
            fclose(f);
        }
    }
    VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, pcn, pcd };
    if (vkCreatePipelineCache(VD, &pci, NULL, &g_vpc) < 0) { pci.initialDataSize = 0; pci.pInitialData = NULL; pcn = 0; if (vkCreatePipelineCache(VD, &pci, NULL, &g_vpc) < 0) g_vpc = VK_NULL_HANDLE; }
    free(pcd);
    /* frame slots */
    for (int i = 0; i < NSLOT; i++) {
        VSlot* s = &S[i];
        VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, VQF };
        if (!vok(vkCreateCommandPool(VD, &cpi, NULL, &s->pool), "vkCreateCommandPool")) { vb_teardown(); return 0; }
        VkCommandBuffer cb[2]; VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2 };
        if (!vok(vkAllocateCommandBuffers(VD, &cai, cb), "vkAllocateCommandBuffers")) { vb_teardown(); return 0; }
        s->cmd = cb[0]; s->up = cb[1];
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 }; VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0 };
        if (!vok(vkCreateFence(VD, &fi, NULL, &s->fence), "vkCreateFence") || !vok(vkCreateSemaphore(VD, &si, NULL, &s->acq), "vkCreateSemaphore")) { vb_teardown(); return 0; }
        if (VX.ts) { VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2, 0 };
                     if (vkCreateQueryPool(VD, &qi, NULL, &s->qp) < 0) VX.ts = 0; }
        s->vtx.use = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT; s->vtx.def = 4u << 20;
        s->ubo.use = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; s->ubo.def = 1u << 20; s->ubo.slack = VSR > PSR ? VSR : PSR;
        s->stg.use = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; s->stg.def = 8u << 20;
    }
    g_si = 0; slot_begin();
    /* constant vertex data, dummy textures, the nearest sampler */
    if (!mk_buffer(32, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_vconst, &g_vconstm) || !vmem_ptr(&g_vconstm)) { vb_teardown(); return 0; }
    { static const float cv[8] = { 0, 0, 0, 1, 1, 1, 1, 1 }; memcpy(vmem_ptr(&g_vconstm), cv, sizeof cv); }
    for (int k = 0; k < 3; k++) {
        VRes* z = &g_vdummy[k]; memset(z, 0, sizeof *z);
        z->serial = ++g_vser; z->kind = 1; z->vf = VK_FORMAT_R8G8B8A8_UNORM; z->w = z->h = z->d = 1; z->lv = 1; z->layers = k == 1 ? 6 : 1; z->cube = k == 1; z->is3d = k == 2;
        if (!mk_image(z, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { vb_teardown(); return 0; }
        z->sview = mk_view(z, k == 1 ? VK_IMAGE_VIEW_TYPE_CUBE : k == 2 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, (uint32_t)z->layers, NULL);
        VkCommandBuffer cb = up_begin();
        img_to(cb, z, 0, (uint32_t)z->layers, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue v; memset(&v, 0, sizeof v); v.float32[3] = 1.f; VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, (uint32_t)z->layers };
        vkCmdClearColorImage(cb, z->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg);
        img_to(cb, z, 0, (uint32_t)z->layers, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    {
        VkSamplerCreateInfo si; memset(&si, 0, sizeof si); si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.maxLod = 0.25f;
        if (!vok(vkCreateSampler(VD, &si, NULL, &g_vsmp_near), "vkCreateSampler")) { vb_teardown(); return 0; }
    }
    /* presentation */
    e = getenv("ME1_GL_PRESENT");
    VW.cpu = (e && !strcmp(e, "cpu")) || !VX.swapext;
    e = getenv("ME1_GL_FAKEWIN");
    if (e && sscanf(e, "%dx%d", &g_fakew, &g_fakeh) == 2 && g_fakew > 0 && g_fakeh > 0) { if (!VW.cpu && VX.headless) VW.fake = 1; } else g_fakew = g_fakeh = 0;
    if (VW.fake) { pthread_mutex_lock(&g_pmx); g_win_gen++; pthread_mutex_unlock(&g_pmx); }
    g_cpupresent = VW.cpu;
    snprintf(g_drvinfo, sizeof g_drvinfo, "%s, Vulkan %u.%u%s", VX.name, VX.api >> 22, (VX.api >> 12) & 0x3FF, custom ? " (custom driver)" : "");
    vlog("Vulkan renderer: %s, Vulkan %u.%u.%u (instance %u.%u), driver 0x%08x, vendor 0x%04x device 0x%04x%s\n", VX.name, VPP.apiVersion >> 22,
         (VPP.apiVersion >> 12) & 0x3FF, VPP.apiVersion & 0xFFF, iv >> 22, (iv >> 12) & 0x3FF, VPP.driverVersion, VPP.vendorID, VPP.deviceID,
         custom ? ", custom driver" : "");
    vlog("BC textures %d%s (ME1_TEX_BC %d, ME1_TEX_16 %d), anisotropy %d (%.0fx), independent blend %d, GPU timer %d, swap chain %d%s, depth format %d%s, max texture %u, UBO align %u, "
         "SPIR-V compiler %s, pipeline cache %s (%zu bytes)%s\n", VX.bc, VX.strict ? " (ME1_VK_STRICT)" : "", g_texbc, g_tex16, VX.aniso, VX.maxaniso, VX.indep, VX.ts, VX.swapext,
         VW.cpu ? " (frames by CPU copy)" : "", (int)VX.ds, VX.dsstencil ? "" : " (no stencil)", VX.maxtex, (unsigned)VX.uboalign, sp,
         g_vpcpath[0] ? g_vpcpath : "off", pcn, VX.validate ? ", validation on" : "");
    vlog("D3D9 float emulation %d (ME1_FLOAT_EMU), invariant position %d (ME1_INVARIANT), simplified calculation %d (ME1_SHADER_LITE: engine lighting/effects switches, FP16 for _pp values, cheap sRGB, anisotropy <= 2x, mm timer 5 ms, audio 32 kHz), submits split every %u draws (ME1_VK_SPLIT)\n",
         sm3_float_emu, sm3_invariant, sm3_lite, VX.split);
    vlog("occlusion queries: %s (ME1_OCCLUSION=%d)\n", !g_occl ? "off - always visible" : VX.oprec ? "precise sample counts" : "visible / hidden only (no occlusionQueryPrecise)", g_occl);
    return 1;
}

/* ---- the D3D9 layer's renderer hooks: Vulkan renderer or the GLES backend (hle_d3d9_gl.h, renamed gles_*) ---- */
#undef gl_free
#undef gl_tex_unlock
#undef gl_buf_unlock
#undef gl_init
#undef gl_release
#undef gl_present
#undef gl_frame_times
#undef gl_clear
#undef gl_draw_x
#undef gl_stretch
#undef gl_colorfill
#undef gl_update_surface
#undef gl_update_texture
#undef gl_get_rt_data
#undef gl_rt_lock
#undef gl_report
#undef me1gl_set_window
#undef me1gl_frame_times
static void q_free(Obj* o) {   /* v1.1.3: occlusion query object (QRes) */
    QRes* q = o->gx;
    if (g_qopen == o) { if (g_gl == 1) gl_query_end(o); g_qopen = NULL; }
    qpend_del(o);
    if (g_gl == 1 && !g_vk) gles_query_free(q);
    free(q); o->gx = NULL;
}
static void gl_free(Obj* o) { if (o->type == T_QUERY) q_free(o); else if (g_vk) vb_free(o); else gles_free(o); }
static void gl_query_begin(Obj* q) { if (g_vk) vq_begin(q); else gles_query_begin(q); }
static void gl_query_end(Obj* q) { if (g_vk) vq_end(q); else gles_query_end(q); }
static int gl_query_data(Obj* q, uint32_t flags, uint32_t* out) { return g_vk ? vq_data(q, flags, out) : gles_query_data(q, flags, out); }
static int gl_query_available(Obj* q) { return g_vk ? vq_available(q) : gles_query_available(q); }
static void gl_tex_unlock(Obj* r, uint32_t s, uint32_t lvl, const uint32_t* L, const uint8_t* p) { if (g_vk) vb_tex_unlock(r, s, lvl, L, p); else gles_tex_unlock(r, s, lvl, L, p); }
static void gl_buf_unlock(Obj* b, uint32_t off, uint32_t end, uint32_t flags) { if (g_vk) vb_buf_unlock(b, off, end, flags); else gles_buf_unlock(b, off, end, flags); }
static void gl_release(void) { if (!g_vk) gles_release(); }
static void gl_present(Obj* bb) { if (g_vk) vb_present(bb); else gles_present(bb); }
static void gl_frame_times(char* o, size_t cap, uint64_t frames) { if (g_vk) vb_frame_times(o, cap, frames); else gles_frame_times(o, cap, frames); }
static void gl_clear(Dev* d, uint32_t n, uint32_t rects, uint32_t flags, uint32_t color, uint32_t zbits, uint32_t stencil) {
    if (g_vk) vb_clear(d, n, rects, flags, color, zbits, stencil); else gles_clear(d, n, rects, flags, color, zbits, stencil);
}
static void gl_draw_x(Dev* d, uint32_t pt, uint32_t prims, int indexed, int32_t base, uint32_t minv, uint32_t nv, uint32_t start,
                      int up, uint32_t upv, uint32_t upstride, uint32_t upi, uint32_t upifmt) {
    if (!g_vk) { gles_draw_x(d, pt, prims, indexed, base, minv, nv, start, up, upv, upstride, upi, upifmt); return; }
    DrawCall dc; memset(&dc, 0, sizeof dc);
    dc.pt = pt; dc.prims = prims; dc.indexed = indexed; dc.base = base; dc.minv = minv; dc.nv = nv; dc.start = start;
    dc.up = up; dc.upv = upv; dc.upstride = upstride; dc.upi = upi; dc.upifmt = upifmt;
    vb_draw(d, &dc);
}
static void gl_stretch(Obj* src, uint32_t sr, Obj* dst, uint32_t dr, uint32_t filter) { if (g_vk) vb_stretch(src, sr, dst, dr, filter); else gles_stretch(src, sr, dst, dr, filter); }
static void gl_colorfill(Obj* surf, uint32_t rect, uint32_t color) { if (g_vk) vb_colorfill(surf, rect, color); else gles_colorfill(surf, rect, color); }
static void gl_update_surface(Obj* src, uint32_t sr, Obj* dst, uint32_t dpt) { if (g_vk) vb_update_surface(src, sr, dst, dpt); else gles_update_surface(src, sr, dst, dpt); }
static void gl_update_texture(Obj* src, Obj* dst) { if (g_vk) vb_update_texture(src, dst); else gles_update_texture(src, dst); }
static void gl_get_rt_data(Obj* rt, Obj* dst) { if (g_vk) vb_get_rt_data(rt, dst); else gles_get_rt_data(rt, dst); }
static void gl_rt_lock(Obj* r, uint32_t s, uint32_t lvl, uint32_t mem) { if (g_vk) vb_rt_lock(r, s, lvl, mem); else gles_rt_lock(r, s, lvl, mem); }
static void gl_report(void) { if (g_gl != 1) return; if (g_vk) vb_report(); else gles_report(); }
/* ME1_RENDERER = vulkan | gles | zink; ME1_GPU_DRIVER = system | vulkan | zink | /path/driver.so (custom drivers: this renderer,
 * ME1_GPU_ZINK=1 or ME1_RENDERER=zink for Mesa zink). Any Vulkan failure -> the system GLES driver. */
static int gl_init(void) {
    const char* e = getenv("ME1_GL");
    if (e && !strcmp(e, "0")) { glog("ME1_GL=0: null backend\n"); return 0; }
    const char* r = getenv("ME1_RENDERER"); const char* d = getenv("ME1_GPU_DRIVER"); const char* z = getenv("ME1_GPU_ZINK");
    int gles = r && (!strcmp(r, "gles") || !strcmp(r, "zink"));
    int want = (r && !strcmp(r, "vulkan")) || (d && !strcmp(d, "vulkan")) || (d && d[0] == '/' && !gles && !(z && !strcmp(z, "1")));
    if (want) {
        if (vb_init()) { g_vk = 1; return 1; }
        vlog("Vulkan renderer not available -> system GLES\n");
        if (d && (d[0] == '/' || !strcmp(d, "vulkan"))) setenv("ME1_GPU_DRIVER", "system", 1);
        int k = gles_init();
        if (k == 1) { size_t n = strlen(g_drvinfo); snprintf(g_drvinfo + n, sizeof g_drvinfo - n, "%s(Vulkan renderer failed)", n ? " " : "system GLES "); }
        return k;
    }
    if (r && !strcmp(r, "zink") && (!d || !*d || !strcmp(d, "system") || !strcmp(d, "gles"))) setenv("ME1_GPU_DRIVER", "zink", 1);
    return gles_init();
}
void me1gl_set_window(void* anw) { if (g_vk) vb_set_window(anw); else gles_set_window(anw); }
/* tests: the "[d3d] frame" timing suffix; 1 if a GPU timer is available */
int me1gl_frame_times(char* o, size_t cap, uint64_t frames) {
    if (g_vk) { vb_frame_times(o, cap, frames); return VX.ts != 0; }
    return gles_frame_times_api(o, cap, frames);
}
/* the renderer in use, for the launcher's overlay / logs */
const char* me1gl_renderer(void) { return g_gl != 1 ? "none" : g_vk ? "Vulkan" : g_zink ? "GLES (zink)" : "GLES"; }
