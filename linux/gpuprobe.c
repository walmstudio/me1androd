/* linux/gpuprobe.c - see gpuprobe.h */
#define _GNU_SOURCE
#include "gpuprobe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <SDL.h>

int gpu_probe_vulkan(GpuDev* out, int max) {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return -1;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) return -1;
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!ci) return -1;
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "me1port-probe", 1, "me1port", 1, VK_MAKE_VERSION(1, 0, 0) };
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL };
    VkInstance inst = VK_NULL_HANDLE;
    if (ci(&ii, NULL, &inst) != VK_SUCCESS || !inst) return -1;
    PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties gqf = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    int n = 0;
    if (epd && gpp && gmp && gqf) {
        VkPhysicalDevice pd[8]; uint32_t np = 8;
        if (epd(inst, &np, pd) < 0) np = 0;
        for (uint32_t i = 0; i < np && n < max; i++) {
            VkPhysicalDeviceProperties p; gpp(pd[i], &p);
            VkQueueFamilyProperties qf[16]; uint32_t nq = 16; gqf(pd[i], &nq, qf);
            int gfx = 0; for (uint32_t k = 0; k < nq; k++) if (qf[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) gfx = 1;
            if (!gfx) continue;
            VkPhysicalDeviceMemoryProperties mp; gmp(pd[i], &mp);
            uint64_t vram = 0; for (uint32_t k = 0; k < mp.memoryHeapCount; k++) if ((mp.memoryHeaps[k].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[k].size > vram) vram = mp.memoryHeaps[k].size;
            GpuDev* d = &out[n++]; memset(d, 0, sizeof *d);
            snprintf(d->name, sizeof d->name, "%s", p.deviceName);
            d->type = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? GPU_INTEGRATED : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? GPU_DISCRETE
                    : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? GPU_VIRTUAL : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? GPU_CPU : GPU_OTHER;
            d->vendor = p.vendorID; d->device = p.deviceID; d->api = p.apiVersion; d->driver = p.driverVersion; d->vram = vram;
        }
    }
    if (di) di(inst, NULL);
    return n;   /* the loader stays loaded: some drivers do not like being unloaded */
}
static int score(const GpuDev* d) { return d->type == GPU_DISCRETE ? 3 : d->type == GPU_CPU ? 1 : 2; }
int gpu_best(const GpuDev* d, int n) {
    const char* e = getenv("ME1_VK_DEVICE");
    if (e && *e) { int k = atoi(e); return k >= 0 && k < n ? k : -1; }
    int b = -1; for (int i = 0; i < n; i++) if (b < 0 || score(&d[i]) > score(&d[b])) b = i;
    return b;
}
int gpu_probe_gl(char* renderer, int cap, char* version, int vcap) {
    renderer[0] = 0; if (version && vcap) version[0] = 0;
    SDL_Window* w = SDL_CreateWindow("probe", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!w) return 0;
    SDL_GLContext c = SDL_GL_CreateContext(w);
    int ok = 0;
    if (c) {
        typedef const unsigned char* (*GS)(unsigned);
        GS gs = (GS)SDL_GL_GetProcAddress("glGetString");
        if (gs) {
            const unsigned char* r = gs(0x1F01 /* GL_RENDERER */), *v = gs(0x1F02 /* GL_VERSION */);
            if (r) { snprintf(renderer, (size_t)cap, "%s", (const char*)r); ok = 1; }
            if (v && version) snprintf(version, (size_t)vcap, "%s", (const char*)v);
        }
        SDL_GL_DeleteContext(c);
    }
    SDL_DestroyWindow(w);
    return ok;
}
const char* gpu_type_name(int t) {
    return t == GPU_INTEGRATED ? "встроенная" : t == GPU_DISCRETE ? "дискретная" : t == GPU_CPU ? "программная (CPU)" : t == GPU_VIRTUAL ? "виртуальная" : "GPU";
}
const char* gpu_vendor_name(uint32_t v) {
    return v == 0x1002 ? "AMD" : v == 0x10de ? "NVIDIA" : v == 0x8086 ? "Intel" : v == 0x13b5 ? "ARM" : v == 0x5143 ? "Qualcomm" : v == 0x10005 ? "Mesa" : "";
}
