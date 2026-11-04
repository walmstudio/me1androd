/* libme1vkshim.so (v0.9.8): a minimal "Vulkan loader" over one ICD, for Mesa zink. Used when a custom driver from
 * GameExDriver cannot be opened through libadrenotools: the runtime dlopens the driver itself (vk_icdGetInstanceProcAddr)
 * and passes this library's handle to zink (ZINK_VK_LOADER_HANDLE), which only needs vkGetInstanceProcAddr and
 * vkGetDeviceProcAddr. No layers, one driver. */
#include <string.h>
#include <stddef.h>
typedef void (*PFN)(void);
typedef PFN (*GIPA)(void*, const char*);
#define EXPORT __attribute__((visibility("default")))
static GIPA g_icd;
static void* g_inst;
static PFN g_gdpa;
EXPORT void me1vk_set_icd(void* gipa) { g_icd = (GIPA)gipa; g_inst = NULL; g_gdpa = NULL; }
EXPORT PFN vkGetDeviceProcAddr(void* dev, const char* name);
EXPORT PFN vkGetInstanceProcAddr(void* inst, const char* name) {
    if (!g_icd || !name) return NULL;
    if (inst && inst != g_inst) { g_inst = inst; g_gdpa = NULL; }
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN)vkGetDeviceProcAddr;
    return g_icd(inst, name);
}
EXPORT PFN vkGetDeviceProcAddr(void* dev, const char* name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN)vkGetDeviceProcAddr;
    if (!g_gdpa && g_icd && g_inst) g_gdpa = g_icd(g_inst, "vkGetDeviceProcAddr");
    return g_gdpa ? ((PFN (*)(void*, const char*))g_gdpa)(dev, name) : NULL;
}
