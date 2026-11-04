/* Module handles, LoadLibrary/GetProcAddress, file handles, call statistics. */
#define _GNU_SOURCE
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>

static struct { Module* m; char path[260]; int attached, no_thread_calls; } g_reg[32]; static int g_nreg;
/* loader lock: DllMain calls (process/thread attach) are serialised like on Windows */
static pthread_mutex_t g_loader;
static void loader_init(void) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&g_loader, &a); }
static void loader_lock(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, loader_init);
    pthread_mutex_lock(&g_loader);
}
static void loader_unlock(void) { pthread_mutex_unlock(&g_loader); }
static int is_exe(const Module* m) { size_t n = strlen(m->name); return n > 4 && !strcasecmp(m->name + n - 4, ".exe"); }
void hle_dll_disable_thread_calls(uint32_t base) { for (int i = 0; i < g_nreg; i++) if (g_reg[i].m->base == base) g_reg[i].no_thread_calls = 1; }
void hle_dll_notify(CPU* c, uint32_t reason) {
    loader_lock();
    for (int i = 0; i < g_nreg; i++) {
        Module* m = g_reg[i].m;
        if (!g_reg[i].attached || !m->entry || g_reg[i].no_thread_calls || is_exe(m)) continue;
        uint32_t a[3] = { m->base, reason, 0 };
        rt_call(c, m->entry, 3, a);
    }
    loader_unlock();
}
void hle_register_module(Module* m, const char* path) { g_reg[g_nreg].m = m; snprintf(g_reg[g_nreg].path, 260, "%s", path); g_nreg++; }
static const char* base_name(const char* p) { const char* s = p; for (; *p; p++) if (*p == '\\' || *p == '/') s = p + 1; return s; }
uint32_t hle_module_handle(const char* name) {
    for (int i = 0; i < g_nreg; i++) if (!strcasecmp(base_name(g_reg[i].path), base_name(name))) return g_reg[i].m->base;
    if (!strcasecmp(name, "kernel32.dll") || !strcasecmp(name, "kernel32")) return 0x7C800000u;
    if (!strcasecmp(base_name(name), "openal32.dll") || !strcasecmp(base_name(name), "openal32")) return hle_al_module();   /* hle_al.c */
    return 0;
}
const char* hle_module_path(uint32_t h) {
    for (int i = 0; i < g_nreg; i++) if (g_reg[i].m->base == h) return g_reg[i].path;
    if (h && h == hle_al_module()) return "C:\\Windows\\System32\\OpenAL32.dll";
    return "C:\\MassEffect\\Binaries\\MassEffect.exe";
}
/* Run DllMain(DLL_PROCESS_ATTACH) once per module, as the Windows loader does. */
uint32_t hle_dll_attach(Module* m) {
    loader_lock();
    for (int i = 0; i < g_nreg; i++) if (g_reg[i].m == m) {
        if (g_reg[i].attached) { loader_unlock(); return 1; }
        g_reg[i].attached = 1;
        if (!m->entry) { loader_unlock(); return 1; }
        uint32_t a[3] = { m->base, 1, 0 };
        uint32_t r = rt_call(rt_cur(), m->entry, 3, a);
        fprintf(stderr, "[hle] %s DllMain(PROCESS_ATTACH) = %u\n", m->name, r);
        loader_unlock();
        return r;
    }
    loader_unlock();
    return 0;
}
static int first_lookup(const char* name);
uint32_t hle_load_library(const char* name) {
    uint32_t h = hle_module_handle(name);
    char key[300]; snprintf(key, sizeof key, "LoadLibrary:%s:%x", name, h);   /* the shader compiler loads d3d9.dll per shader: log once */
    if (first_lookup(key)) fprintf(stderr, "[hle] LoadLibraryA(\"%s\") -> %08x%s\n", name, h, h ? "" : " (further calls not logged)");
    loader_lock();
    for (int i = 0; h && i < g_nreg; i++)
        if (g_reg[i].m->base == h && !g_reg[i].attached) {
            CPU* c = rt_cur(); CPU save = *c;   /* DllMain runs nested inside the LoadLibraryA call */
            uint32_t ok = hle_dll_attach(g_reg[i].m);
            c->ebx = save.ebx; c->esi = save.esi; c->edi = save.edi; c->ebp = save.ebp; c->esp = save.esp;
            if (!ok) { fprintf(stderr, "[hle] DllMain failed -> LoadLibrary returns NULL\n"); loader_unlock(); return 0; }
        }
    loader_unlock();
    return h;
}
/* GetProcAddress is called in loops (EncodePointer/DecodePointer ...): log each name once */
static int first_lookup(const char* name) {
    static char* seen[1024]; static int n; static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
    int r = 1;
    pthread_mutex_lock(&mx);
    for (int i = 0; i < n; i++) if (!strcmp(seen[i], name)) { r = 0; break; }
    if (r && n < 1024) seen[n++] = strdup(name);
    pthread_mutex_unlock(&mx);
    return r;
}
uint32_t hle_get_proc(uint32_t h, const char* name, uint32_t ord) {
    for (int i = 0; i < g_nreg; i++) if (g_reg[i].m->base == h) {
        char o[16]; if (!name) { snprintf(o, sizeof o, "#%u", ord & 0xFFFF); name = o; }
        uint32_t v = rt_find_export(g_reg[i].m, name);
        if (!v && first_lookup(name)) fprintf(stderr, "[hle] GetProcAddress(%s, \"%s\") -> 0\n", g_reg[i].m->name, name);
        return v;
    }
    if (h && h == hle_al_module() && name) {
        uint32_t t = hle_resolve_impl("OpenAL32.dll", name);
        if (first_lookup(name) && (!t || getenv("ME1_AL_LOG"))) fprintf(stderr, "[hle] GetProcAddress(OpenAL32, \"%s\") -> %08x\n", name, t);
        return t;
    }
    if (h == 0x7C800000u && name) {
        uint32_t t = hle_resolve_impl("kernel32.dll", name);
        if (first_lookup(name)) fprintf(stderr, "[hle] GetProcAddress(kernel32, \"%s\") -> thunk %08x\n", name, t);
        return t;
    }
    if (first_lookup(name ? name : "#ord")) fprintf(stderr, "[hle] GetProcAddress(%08x, \"%s\") -> 0\n", h, name ? name : "#ord");
    (void)ord; return 0;
}
/* stats: open-addressing table keyed by the (static) name pointer; g_ord keeps first-call order for the report */
#define NCNT 4096
static struct { const char* n; unsigned k; } g_cnt[NCNT]; static int g_ord[NCNT]; static int g_ncnt;
static pthread_mutex_t g_cnt_mx = PTHREAD_MUTEX_INITIALIZER;
void hle_count_call(const char* name) {
    unsigned h = (unsigned)(((uintptr_t)name >> 3) * 2654435761u) & (NCNT - 1);
    for (unsigned probe = 0; probe < NCNT; probe++, h = (h + 1) & (NCNT - 1)) {
        const char* n = __atomic_load_n(&g_cnt[h].n, __ATOMIC_ACQUIRE);
        if (n == name) { __atomic_add_fetch(&g_cnt[h].k, 1, __ATOMIC_RELAXED); return; }
        if (n) continue;
        pthread_mutex_lock(&g_cnt_mx);
        n = g_cnt[h].n;
        if (!n && g_ncnt < NCNT / 2) {
            g_cnt[h].k = 1; g_ord[g_ncnt] = (int)h; __atomic_store_n(&g_cnt[h].n, name, __ATOMIC_RELEASE); g_ncnt++;
            pthread_mutex_unlock(&g_cnt_mx); return;
        }
        pthread_mutex_unlock(&g_cnt_mx);
        if (n == name) { __atomic_add_fetch(&g_cnt[h].k, 1, __ATOMIC_RELAXED); return; }
        if (!n) return;   /* table full */
    }
}
int hle_count_snapshot(const char** names, unsigned* counts, int max) {   /* first-call order is stable: index i is the same API */
    int n = __atomic_load_n(&g_ncnt, __ATOMIC_ACQUIRE); if (n > max) n = max;
    for (int i = 0; i < n; i++) { names[i] = g_cnt[g_ord[i]].n; counts[i] = __atomic_load_n(&g_cnt[g_ord[i]].k, __ATOMIC_RELAXED); }
    return n;
}
void hle_report(void) {
    fprintf(stderr, "[hle] %d distinct APIs called:", g_ncnt);
    for (int i = 0; i < g_ncnt; i++) fprintf(stderr, " %s(%u)", g_cnt[g_ord[i]].n, g_cnt[g_ord[i]].k);
    fprintf(stderr, "\n");
    hle_d3d9_report();
    hle_al_report();
}
