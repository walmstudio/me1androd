/* HLE implementations of Win32 APIs (stage 0: what statically-linked MSVC CRT needs). */
#define _GNU_SOURCE
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>

static int g_trace = 1;

/* ---------------- guest heap: first-fit free list in a fixed region ---------------- */
#define HEAP_LO 0x20000000u
#define HEAP_HI 0x30000000u   /* above: VirtualAlloc ranges (hle_vmem.c) */
static uint32_t heap_top = HEAP_LO;
typedef struct FreeBlk { uint32_t p, size; struct FreeBlk* next; } FreeBlk;
static FreeBlk* freelist;
static pthread_mutex_t g_heap_mx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t ghalloc_locked(uint32_t size, int zero);
uint32_t ghalloc(uint32_t size, int zero) {
    pthread_mutex_lock(&g_heap_mx); uint32_t p = ghalloc_locked(size, zero); pthread_mutex_unlock(&g_heap_mx); return p;
}
static uint32_t ghalloc_locked(uint32_t size, int zero) {
    uint32_t need = (size + 15u) & ~15u; if (!need) need = 16;
    for (FreeBlk** pp = &freelist; *pp; pp = &(*pp)->next) {
        FreeBlk* b = *pp;
        if (b->size >= need) { uint32_t p = b->p; *pp = b->next; wr32(p - 16, b->size); free(b); if (zero) memset(M + p, 0, need); wr32(p - 12, size); return p; }
    }
    uint32_t p = heap_top + 16;
    if (p + need > HEAP_HI) return 0;
    heap_top = p + need;
    wr32(p - 16, need); wr32(p - 12, size);
    if (zero) memset(M + p, 0, need);
    return p;
}
void ghfree(uint32_t p) {
    static int noreuse = -1; if (noreuse < 0) noreuse = getenv("ME1_HEAP_NOREUSE") != NULL;   /* debugging use-after-free */
    if (!p || noreuse) return;
    FreeBlk* b = malloc(sizeof *b); b->p = p; b->size = rd32(p - 16);
    pthread_mutex_lock(&g_heap_mx); b->next = freelist; freelist = b; pthread_mutex_unlock(&g_heap_mx);
}
uint32_t ghsize(uint32_t p) { return rd32(p - 12); }
uint32_t gstrdup(const char* s) { uint32_t n = strlen(s) + 1, p = ghalloc(n, 0); memcpy(M + p, s, n); return p; }
static uint32_t gwstrdup(const char* s) {
    uint32_t n = strlen(s) + 1, p = ghalloc(2 * n, 0);
    for (uint32_t i = 0; i < n; i++) wr16(p + 2 * i, (uint8_t)s[i]);
    return p;
}

/* ---------------- state ---------------- */
#define TEB (c->fs_base)
#define set_last_error hle_set_last_error
static const char* g_cmdline = "MassEffect.exe";
void hle_set_cmdline(const char* s) { g_cmdline = s; }
static const char* g_module_path = "C:\\MassEffect\\Binaries\\MassEffect.exe";

/* ---------------- KERNEL32 ---------------- */
static void k_GetVersionExA(CPU* c) {
    uint32_t p = ARG(0), sz = rd32(p);
    wr32(p + 4, 5); wr32(p + 8, 1); wr32(p + 12, 2600); wr32(p + 16, 2);    /* Windows XP */
    memset(M + p + 20, 0, 128); strcpy((char*)M + p + 20, "Service Pack 3");
    if (sz >= 156) { wr16(p + 148, 3); wr16(p + 150, 0); }
    RET(1);
}
static void k_HeapCreate(CPU* c) { RET(0x00A00000u); }
static void k_HeapDestroy(CPU* c) { RET(1); }
static void k_GetProcessHeap(CPU* c) { RET(0x00A00000u); }
static void k_HeapAlloc(CPU* c) { RET(ghalloc(ARG(2), ARG(1) & 8)); }
static void k_HeapFree(CPU* c) { ghfree(ARG(2)); RET(1); }
static void k_HeapSize(CPU* c) { RET(ghsize(ARG(2))); }
static void k_HeapReAlloc(CPU* c) {
    uint32_t old = ARG(2), n = ARG(3), p = ghalloc(n, ARG(1) & 8);
    if (old && p) { uint32_t o = ghsize(old); memcpy(M + p, M + old, o < n ? o : n); ghfree(old); }
    RET(p);
}
static void k_VirtualAlloc(CPU* c) {   /* (addr, size, type, protect) -> hle_vmem.c */
    uint32_t a = ARG(0), r = hle_vmem_alloc(a, ARG(1), ARG(2));
    if (!r && a && !(ARG(2) & 0x2000u)) r = a & ~0xFFFu;   /* commit at an address we don't manage (stack, heap): no-op */
    if (!r) hle_set_last_error(c, a ? 487 : 8);
    RET(r);
}
static void k_VirtualFree(CPU* c) { RET(hle_vmem_free(ARG(0), ARG(1), ARG(2))); }
static void k_GetCurrentThreadId(CPU* c) { RET(rd32(TEB + 0x24)); }
static void k_GetCurrentProcessId(CPU* c) { RET(rd32(TEB + 0x20)); }
static void k_GetCurrentProcess(CPU* c) { RET(0xFFFFFFFFu); }
static void k_GetCommandLineA(CPU* c) { static uint32_t p; if (!p) p = gstrdup(g_cmdline); RET(p); }
static void k_GetCommandLineW(CPU* c) { static uint32_t p; if (!p) p = gwstrdup(g_cmdline); RET(p); }
static void k_TerminateProcess(CPU* c) { fprintf(stderr, "[hle] TerminateProcess(%u)\n", ARG(1)); exit((int)ARG(1)); }
static void k_ExitProcess(CPU* c) { fprintf(stderr, "[hle] ExitProcess(%u)\n", ARG(0)); exit((int)ARG(0)); }
static void k_IsDebuggerPresent(CPU* c) { RET(0); }
static void k_GetModuleHandleA(CPU* c) {
    uint32_t n = ARG(0);
    if (!n) { RET(0x10900000u); return; }
    RET(hle_module_handle(GSTR(n)));
}
static void k_GetModuleHandleW(CPU* c) {
    uint32_t n = ARG(0); char b[260]; int i = 0;
    if (!n) { RET(0x10900000u); return; }
    for (; i < 259 && rd16(n + 2 * i); i++) b[i] = (char)rd16(n + 2 * i);
    b[i] = 0; RET(hle_module_handle(b));
}
static void k_GetModuleFileNameA(CPU* c) {
    uint32_t buf = ARG(1), n = ARG(2); const char* s = hle_module_path(ARG(0));
    uint32_t l = strlen(s); if (l >= n) l = n - 1;
    memcpy(M + buf, s, l); wr8(buf + l, 0); RET(l);
}
static void k_GetModuleFileNameW(CPU* c) {
    uint32_t buf = ARG(1), n = ARG(2); const char* s = hle_module_path(ARG(0));
    uint32_t l = strlen(s); if (l >= n) l = n - 1;
    for (uint32_t i = 0; i < l; i++) wr16(buf + 2 * i, (uint8_t)s[i]);
    wr16(buf + 2 * l, 0); RET(l);
}
static void k_SetLastError(CPU* c) { set_last_error(c, ARG(0)); }
static void k_GetLastError(CPU* c) { RET(rd32(TEB + 0x34)); }
static void k_SetHandleCount(CPU* c) { RET(ARG(0)); }
static void k_GetStdHandle(CPU* c) { RET(0x100u + (uint32_t)(-(int32_t)ARG(0))); }   /* -10,-11,-12 -> 0x10a.. */
static void k_GetFileType(CPU* c) { uint32_t h = ARG(0); RET(h >= 0x10A && h <= 0x10C ? 2 : 1); }
static void k_GetStartupInfoA(CPU* c) { memset(M + ARG(0), 0, 68); wr32(ARG(0), 68); }
static void k_GetEnvironmentStrings(CPU* c) { static uint32_t p; if (!p) { p = ghalloc(8, 1); } RET(p); }
static void k_GetEnvironmentStringsW(CPU* c) { static uint32_t p; if (!p) { p = ghalloc(8, 1); } RET(p); }
static void k_Ret1(CPU* c) { RET(1); }
static void k_Ret0(CPU* c) { RET(0); }
static void k_QueryPerformanceCounter(CPU* c) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    wr64(ARG(0), (uint64_t)ts.tv_sec * 10000000ull + ts.tv_nsec / 100); RET(1);
}
static void k_QueryPerformanceFrequency(CPU* c) { wr64(ARG(0), 10000000ull); RET(1); }
static void k_GetTickCount(CPU* c) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); RET(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u); }
static void k_GetSystemTimeAsFileTime(CPU* c) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    wr64(ARG(0), ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + ts.tv_nsec / 100);
}
static void k_GetACP(CPU* c) { RET(1252); }
static void k_GetOEMCP(CPU* c) { RET(437); }
static void k_GetConsoleCP(CPU* c) { RET(437); }
static void k_IsValidCodePage(CPU* c) { RET(1); }
static void k_GetCPInfo(CPU* c) { uint32_t p = ARG(1); memset(M + p, 0, 20); wr32(p, 1); wr8(p + 4, '?'); RET(1); }
static void k_GetConsoleMode(CPU* c) { RET(0); }
static void k_WriteConsoleA(CPU* c) { fwrite(M + ARG(1), 1, ARG(2), stderr); if (ARG(3)) wr32(ARG(3), ARG(2)); RET(1); }
static void k_WriteConsoleW(CPU* c) { for (uint32_t i = 0; i < ARG(2); i++) fputc((char)rd16(ARG(1) + 2 * i), stderr); if (ARG(3)) wr32(ARG(3), ARG(2)); RET(1); }
static void k_MultiByteToWideChar(CPU* c) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0) n = strlen(GSTR(src)) + 1;
    if (!cap) { RET(n); return; }
    if ((uint32_t)n > cap) { set_last_error(c, 122); RET(0); return; }
    for (int i = 0; i < n; i++) wr16(dst + 2 * i, rd8(src + i));
    RET(n);
}
static void k_WideCharToMultiByte(CPU* c) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    if (!cap) { RET(n); return; }
    if ((uint32_t)n > cap) { set_last_error(c, 122); RET(0); return; }
    for (int i = 0; i < n; i++) { uint16_t ch = rd16(src + 2 * i); wr8(dst + i, ch < 256 ? ch : '?'); }
    RET(n);
}
static uint16_t ctype1(unsigned ch) {
    uint16_t t = 0;
    if (ch >= 128) return 0x100 * 0 + (ch >= 0xC0 ? 0x100 : 0);
    if (isupper(ch)) t |= 0x1; if (islower(ch)) t |= 0x2; if (isdigit(ch)) t |= 0x4; if (isspace(ch)) t |= 0x8;
    if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ' || ch == '\t') t |= 0x40;
    if (isxdigit(ch)) t |= 0x80; if (isalpha(ch)) t |= 0x100;
    return t;
}
static void k_GetStringTypeW(CPU* c) {  /* (dwInfoType, src, cch, out) */
    uint32_t src = ARG(1); int32_t n = (int32_t)ARG(2); uint32_t out = ARG(3);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    for (int i = 0; i < n; i++) wr16(out + 2 * i, ARG(0) == 1 ? ctype1(rd16(src + 2 * i)) : 0);
    RET(1);
}
static void k_GetStringTypeA(CPU* c) {  /* (lcid, dwInfoType, src, cch, out) */
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t out = ARG(4);
    if (n < 0) n = strlen(GSTR(src)) + 1;
    for (int i = 0; i < n; i++) wr16(out + 2 * i, ARG(1) == 1 ? ctype1(rd8(src + i)) : 0);
    RET(1);
}
static void k_LCMapStringW(CPU* c) {  /* (lcid, flags, src, cch, dst, cap) */
    uint32_t fl = ARG(1), src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    if (!cap) { RET(n); return; }
    for (int i = 0; i < n && (uint32_t)i < cap; i++) { uint16_t ch = rd16(src + 2 * i);
        if (ch < 128) ch = (fl & 0x100) ? tolower(ch) : (fl & 0x200) ? toupper(ch) : ch; wr16(dst + 2 * i, ch); }
    RET(n);
}
static void k_LCMapStringA(CPU* c) {
    uint32_t fl = ARG(1), src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4), cap = ARG(5);
    if (n < 0) n = strlen(GSTR(src)) + 1;
    if (!cap) { RET(n); return; }
    for (int i = 0; i < n && (uint32_t)i < cap; i++) { uint8_t ch = rd8(src + i);
        ch = (fl & 0x100) ? tolower(ch) : (fl & 0x200) ? toupper(ch) : ch; wr8(dst + i, ch); }
    RET(n);
}
static void k_GetLocaleInfoA(CPU* c) {  /* (lcid, type, buf, cap) */
    const char* v = "1252"; uint32_t t = ARG(1) & 0xFFFF;
    if (t == 0x1004) v = "1252"; else if (t == 0xB) v = "437"; else if (t == 0x1) v = "0409";
    uint32_t l = strlen(v) + 1; if (!ARG(3)) { RET(l); return; }
    memcpy(M + ARG(2), v, l); RET(l);
}
static void k_LoadLibraryA(CPU* c) { RET(hle_load_library(GSTR(ARG(0)))); }
static void k_GetProcAddress(CPU* c) { RET(hle_get_proc(ARG(0), ARG(1) < 0x10000 ? NULL : GSTR(ARG(1)), ARG(1))); }
static void k_IsProcessorFeaturePresent(CPU* c) { RET(0); }
static void k_EncodePointer(CPU* c) { RET(ARG(0)); }

/* ---------------- ADVAPI32 / IPHLPAPI ---------------- */
/* ---- registry: tiny static view of HKLM, enough for PhysXLoader to find PhysXCore.dll ---- */
#define HKEY_AGEIA 0x5A000001u
static int key_is(const char* k, const char* want) {
    size_t n = strlen(want); if (strncasecmp(k, want, n)) return 0;
    return k[n] == 0 || (k[n] == '\\' && k[n + 1] == 0);
}
static void a_RegOpenKeyExA(CPU* c) {
    const char* k = ARG(1) ? GSTR(ARG(1)) : "";
    int ok = ARG(0) == 0x80000002u && key_is(k, "Software\\Ageia Technologies");
    if (g_trace) fprintf(stderr, "[hle] RegOpenKeyExA(%08x, \"%s\") -> %s\n", ARG(0), k, ok ? "ok" : "not found");
    if (ok) { wr32(ARG(4), HKEY_AGEIA); RET(0); } else RET(2);
}
static void a_RegQueryValueExA(CPU* c) {
    uint32_t h = ARG(0), type = ARG(3), data = ARG(4), pcb = ARG(5);
    const char* name = ARG(1) ? GSTR(ARG(1)) : "";
    const char* val = NULL;
    if (h == HKEY_AGEIA && !strcasecmp(name, "PhysXCore Path")) val = "C:\\MassEffect\\Binaries\\";
    if (g_trace) fprintf(stderr, "[hle] RegQueryValueExA(\"%s\") -> %s\n", name, val ? val : "not found");
    if (!val) { RET(2); return; }
    uint32_t need = strlen(val) + 1;
    if (type) wr32(type, 1 /* REG_SZ */);
    if (!data) { if (pcb) wr32(pcb, need); RET(0); return; }
    if (!pcb || rd32(pcb) < need) { if (pcb) wr32(pcb, need); RET(234); return; }
    memcpy(M + data, val, need); wr32(pcb, need); RET(0);
}
static void a_RegCloseKey(CPU* c) { RET(0); }
static void i_GetAdaptersInfo(CPU* c) { RET(50); }

/* ---- sync objects / misc kernel32 (single-threaded stage: objects never block) ---- */
static void k_Ret0Err(CPU* c) { set_last_error(c, 50 /* ERROR_NOT_SUPPORTED */); RET(0); }
static void k_RetNeg1(CPU* c) { RET(0xFFFFFFFFu); }
static void k_GetWindowsDirectoryA(CPU* c) { const char* s = "C:\\WINDOWS"; uint32_t l = strlen(s); if (ARG(1) > l) { memcpy(M + ARG(0), s, l + 1); RET(l); } else RET(l + 1); }
static void k_GetSystemDirectoryA(CPU* c) { const char* s = "C:\\WINDOWS\\system32"; uint32_t l = strlen(s); if (ARG(1) > l) { memcpy(M + ARG(0), s, l + 1); RET(l); } else RET(l + 1); }
static void k_OpenProcess(CPU* c) { RET(0x40000001u); }
static void k_GetUserDefaultLCID(CPU* c) { RET(0x409); }
static void k_HeapWalk(CPU* c) { set_last_error(c, 259 /* ERROR_NO_MORE_ITEMS */); RET(0); }
static void k_GetLocaleInfoW(CPU* c) { RET(0); }
/* setupapi: no AGEIA PPU hardware present */
static void s_SetupDiGetClassDevsA(CPU* c) { RET(0xFFFFFFFFu); }
static void s_SetupDiEnum(CPU* c) { set_last_error(c, 259); RET(0); }
/* winsock (PhysX remote debugger only): report network unavailable */
static void w_WSAStartup(CPU* c) { RET(10091 /* WSASYSNOTREADY */); }
static void w_WSAGetLastError(CPU* c) { RET(10050 /* WSAENETDOWN */); }
static void w_htons(CPU* c) { uint32_t v = ARG(0) & 0xffff; RET(((v & 0xff) << 8) | (v >> 8)); }


/* KN: pure functions (TEB, clocks, code-page conversion) skip the big HLE lock: a thread polling
 * QueryPerformanceCounter/GetTickCount hundreds of thousands of times per second must not stall the others */
#define KN(n, a, f) { "kernel32.dll", n, a, f, HLE_NOLOCK },
static const HleDef g_defs[] = {

#define K(n, a, f) { "kernel32.dll", n, a, f },
    K("GetVersionExA", 1, k_GetVersionExA) K("HeapCreate", 3, k_HeapCreate) K("HeapDestroy", 1, k_HeapDestroy)
    K("GetProcessHeap", 0, k_GetProcessHeap) K("HeapAlloc", 3, k_HeapAlloc) K("HeapFree", 3, k_HeapFree)
    K("HeapSize", 3, k_HeapSize) K("HeapReAlloc", 4, k_HeapReAlloc) K("VirtualAlloc", 4, k_VirtualAlloc)
    K("VirtualFree", 3, k_VirtualFree) KN("GetCurrentThreadId", 0, k_GetCurrentThreadId)
    KN("GetCurrentProcessId", 0, k_GetCurrentProcessId) KN("GetCurrentProcess", 0, k_GetCurrentProcess)
    K("GetCommandLineA", 0, k_GetCommandLineA) K("GetCommandLineW", 0, k_GetCommandLineW)
    K("TerminateProcess", 2, k_TerminateProcess) K("ExitProcess", 1, k_ExitProcess)
    K("IsDebuggerPresent", 0, k_IsDebuggerPresent) K("GetModuleHandleA", 1, k_GetModuleHandleA) K("GetModuleHandleW", 1, k_GetModuleHandleW)
    K("GetModuleFileNameA", 3, k_GetModuleFileNameA) K("GetModuleFileNameW", 3, k_GetModuleFileNameW)
    
    
    
    KN("SetLastError", 1, k_SetLastError) KN("GetLastError", 0, k_GetLastError) 
    K("SetHandleCount", 1, k_SetHandleCount) K("GetStdHandle", 1, k_GetStdHandle) K("GetFileType", 1, k_GetFileType)
    K("GetStartupInfoA", 1, k_GetStartupInfoA) 
    
    
    K("GetEnvironmentStrings", 0, k_GetEnvironmentStrings) K("GetEnvironmentStringsW", 0, k_GetEnvironmentStringsW)
    K("FreeEnvironmentStringsA", 1, k_Ret1) K("FreeEnvironmentStringsW", 1, k_Ret1)
    KN("QueryPerformanceCounter", 1, k_QueryPerformanceCounter) KN("QueryPerformanceFrequency", 1, k_QueryPerformanceFrequency)
    KN("GetTickCount", 0, k_GetTickCount) KN("GetSystemTimeAsFileTime", 1, k_GetSystemTimeAsFileTime)
    KN("GetACP", 0, k_GetACP) KN("GetOEMCP", 0, k_GetOEMCP) K("GetConsoleCP", 0, k_GetConsoleCP) K("GetConsoleOutputCP", 0, k_GetConsoleCP)
    K("IsValidCodePage", 1, k_IsValidCodePage) K("GetCPInfo", 2, k_GetCPInfo) K("GetConsoleMode", 2, k_GetConsoleMode)
    K("WriteConsoleA", 5, k_WriteConsoleA) K("WriteConsoleW", 5, k_WriteConsoleW)
    KN("MultiByteToWideChar", 6, k_MultiByteToWideChar) KN("WideCharToMultiByte", 8, k_WideCharToMultiByte)
    K("GetStringTypeW", 4, k_GetStringTypeW) K("GetStringTypeA", 5, k_GetStringTypeA)
    K("LCMapStringW", 6, k_LCMapStringW) K("LCMapStringA", 6, k_LCMapStringA) K("GetLocaleInfoA", 4, k_GetLocaleInfoA)
    
    K("SetStdHandle", 2, k_Ret1) K("FreeLibrary", 1, k_Ret1)
    { "kernel32.dll", "LoadLibraryA", 1, k_LoadLibraryA, HLE_NOLOCK }, K("GetProcAddress", 2, k_GetProcAddress)
    K("IsProcessorFeaturePresent", 1, k_IsProcessorFeaturePresent)
    KN("EncodePointer", 1, k_EncodePointer) KN("DecodePointer", 1, k_EncodePointer)
    
#undef K
    { "advapi32.dll", "RegOpenKeyExA", 5, a_RegOpenKeyExA }, { "advapi32.dll", "RegQueryValueExA", 6, a_RegQueryValueExA },
    { "advapi32.dll", "RegCloseKey", 1, a_RegCloseKey }, { "iphlpapi.dll", "GetAdaptersInfo", 2, i_GetAdaptersInfo },
#define K(n, a, f) { "kernel32.dll", n, a, f },
    
    
    K("GetSystemDirectoryA", 2, k_GetSystemDirectoryA) K("GetWindowsDirectoryA", 2, k_GetWindowsDirectoryA) K("DeviceIoControl", 8, k_Ret0Err) 
    K("OpenFileMappingA", 3, k_Ret0Err) K("MapViewOfFile", 5, k_Ret0Err) K("UnmapViewOfFile", 1, k_Ret1) K("OpenProcess", 3, k_OpenProcess)
    
    K("HeapValidate", 3, k_Ret1) K("HeapWalk", 2, k_HeapWalk)
    
    K("GetUserDefaultLCID", 0, k_GetUserDefaultLCID) K("IsValidLocale", 2, k_Ret1) K("EnumSystemLocalesA", 2, k_Ret1)
    K("GetLocaleInfoW", 4, k_GetLocaleInfoW)
#undef K
    { "setupapi.dll", "SetupDiGetClassDevsA", 4, s_SetupDiGetClassDevsA }, { "setupapi.dll", "SetupDiEnumDeviceInterfaces", 5, s_SetupDiEnum },
    { "setupapi.dll", "SetupDiGetDeviceInterfaceDetailA", 6, s_SetupDiEnum }, { "setupapi.dll", "SetupDiDestroyDeviceInfoList", 1, k_Ret1 },
    { "ws2_32.dll", "#115", 2, w_WSAStartup }, { "ws2_32.dll", "#23", 3, k_RetNeg1 }, { "ws2_32.dll", "#9", 1, w_htons },
    { "ws2_32.dll", "#4", 3, k_RetNeg1 }, { "ws2_32.dll", "#3", 1, k_Ret0 }, { "ws2_32.dll", "#16", 4, k_RetNeg1 },
    { "ws2_32.dll", "#19", 4, k_RetNeg1 }, { "ws2_32.dll", "#52", 1, k_Ret0 },
    { "wsock32.dll", "#51", 3, k_Ret0 },
};
#define NDEFS (sizeof g_defs / sizeof g_defs[0])

/* thunk slots: [0, NDEFS) = implemented, then dynamically assigned "missing" slots */
#define MAX_MISSING 4096
static struct { char dll[64], name[96]; } g_missing[MAX_MISSING]; static unsigned g_nmissing;

/* implemented slots: g_defs (this file), hle_fs_defs (hle_fs.c), hle_thread_defs (hle_thread.c), hle_seh_defs (hle_seh.c),
 * hle_exe_defs (hle_exe.c), hle_d3d9_defs (hle_d3d9.c), hle_input_defs (hle_input.c), hle_al_defs (hle_al.c) */
#define NIMPL (NDEFS + hle_fs_ndefs + hle_thread_ndefs + hle_seh_ndefs + hle_exe_ndefs + hle_d3d9_ndefs + hle_input_ndefs + hle_al_ndefs)
static const HleDef* def_at(unsigned i) {
    if (i < NDEFS) return &g_defs[i];
    i -= NDEFS; if (i < hle_fs_ndefs) return &hle_fs_defs[i];
    i -= hle_fs_ndefs; if (i < hle_thread_ndefs) return &hle_thread_defs[i];
    i -= hle_thread_ndefs; if (i < hle_seh_ndefs) return &hle_seh_defs[i];
    i -= hle_seh_ndefs; if (i < hle_exe_ndefs) return &hle_exe_defs[i];
    i -= hle_exe_ndefs; if (i < hle_d3d9_ndefs) return &hle_d3d9_defs[i];
    i -= hle_d3d9_ndefs; if (i < hle_input_ndefs) return &hle_input_defs[i];
    return &hle_al_defs[i - hle_input_ndefs];
}
/* big HLE lock: Win32 emulation state (file table, registry, lazy statics, ...) is touched by one thread at a time;
 * HLE_NOLOCK entries (sync objects, waits, TLS, LoadLibrary) manage their own locking */
static pthread_mutex_t g_hle_mx;
void hle_init(void) {
    static int done; if (done) return; done = 1;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_hle_mx, &a); pthread_mutexattr_destroy(&a);
    hle_thread_init();
}
/* an exception transfer unwound past HLE calls that held the big lock (guest callback threw): release them */
void hle_unwind_locks(int depth) { while (rt_hle_depth > depth) { rt_hle_depth--; pthread_mutex_unlock(&g_hle_mx); } }
uint32_t hle_resolve_impl(const char* dll, const char* name) {
    for (unsigned i = 0; i < NIMPL; i++) {
        const HleDef* d = def_at(i);
        if (!strcasecmp(d->dll, dll) && !strcmp(d->name, name)) return THUNK_BASE + 16 * i;
    }
    return 0;
}
uint32_t hle_call(const char* dll, const char* name, int nargs, const uint32_t* args) {
    uint32_t t = hle_resolve_impl(dll, name);
    if (!t) { fprintf(stderr, "[hle] hle_call: %s!%s not implemented\n", dll, name); return 0; }
    return rt_call(rt_cur(), t, nargs, args);
}
uint32_t hle_wstr(const char* s) {
    uint32_t p = ghalloc(2 * strlen(s) + 2, 1), k = 0;
    for (const unsigned char* q = (const unsigned char*)s; *q;) {
        unsigned cp = *q++;
        if (cp >= 0xC0) { int e = cp >= 0xE0 ? 2 : 1; cp &= e == 2 ? 15 : 31; while (e-- && *q) cp = cp << 6 | (*q++ & 63); }
        wr16(p + 2 * k++, cp);
    }
    return p;
}
void hle_wstr_utf8(uint32_t p, char* o, size_t cap) {
    size_t k = 0;
    for (uint32_t i = 0; rd16(p + 2 * i) && k + 4 < cap; i++) {
        unsigned cp = rd16(p + 2 * i);
        if (cp < 0x80) o[k++] = cp; else if (cp < 0x800) { o[k++] = 0xC0 | cp >> 6; o[k++] = 0x80 | (cp & 63); }
        else { o[k++] = 0xE0 | cp >> 12; o[k++] = 0x80 | (cp >> 6 & 63); o[k++] = 0x80 | (cp & 63); }
    }
    o[k] = 0;
}
uint32_t hle_resolve(const char* dll, const char* name) {
    uint32_t t = hle_resolve_impl(dll, name); if (t) return t;
    for (unsigned i = 0; i < g_nmissing; i++)
        if (!strcasecmp(g_missing[i].dll, dll) && !strcmp(g_missing[i].name, name)) return THUNK_BASE + 16 * (NIMPL + i);
    unsigned i = g_nmissing++;
    snprintf(g_missing[i].dll, 64, "%s", dll); snprintf(g_missing[i].name, 96, "%s", name);
    return THUNK_BASE + 16 * (NIMPL + i);
}
void hle_invoke(CPU* c, uint32_t t) {
    unsigned i = (t - THUNK_BASE) / 16;
    if (i >= NIMPL) {
        char b[200]; i -= NIMPL;
        snprintf(b, sizeof b, "missing HLE import %s!%s", i < g_nmissing ? g_missing[i].dll : "?", i < g_nmissing ? g_missing[i].name : "?");
        rt_fault(c, t, b);
    }
    const HleDef* d = def_at(i);
    if (g_trace > 1) fprintf(stderr, "[hle] %s(%08x, %08x, %08x)\n", d->name, ARG(0), ARG(1), ARG(2));
    hle_count_call(d->name);
    hle_thread_check();
    hle_hb_enter(c, d->name);
    if (d->flags & HLE_NOLOCK) d->fn(c);
    else { pthread_mutex_lock(&g_hle_mx); rt_hle_depth++; d->fn(c); rt_hle_depth--; pthread_mutex_unlock(&g_hle_mx); }
    hle_hb_leave();
    c->esp += 4 + 4 * d->nargs;   /* stdcall: pop return address + args */
}
