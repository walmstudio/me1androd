/* linux/gpuprobe.h - which GPUs the game can use (v1.1): Vulkan devices through the system loader (dlopen, no link
 * dependency) and, without Vulkan, the OpenGL renderer string from a hidden SDL window. Used by me1-launcher (automatic
 * preset, renderer choice) and me1-linux (default renderer). */
#pragma once
#include <stdint.h>
enum { GPU_OTHER, GPU_INTEGRATED, GPU_DISCRETE, GPU_VIRTUAL, GPU_CPU };
typedef struct { char name[256]; int type; uint32_t vendor, device, api, driver; uint64_t vram; } GpuDev;
int  gpu_probe_vulkan(GpuDev* out, int max);   /* number of devices; -1: no Vulkan loader / instance */
int  gpu_best(const GpuDev* d, int n);         /* the runtime's choice: discrete > integrated/other > CPU (ME1_VK_DEVICE) */
int  gpu_probe_gl(char* renderer, int cap, char* version, int vcap);   /* needs SDL video; 1 ok */
const char* gpu_type_name(int type);           /* Russian: "дискретная", ... */
const char* gpu_vendor_name(uint32_t vendor);
