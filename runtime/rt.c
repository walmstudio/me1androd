/* ME1 recomp runtime core: arena, module loader, dispatch, faults. */
#define _GNU_SOURCE
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <strings.h>

uint8_t* M;
CPU g_cpu;                      /* main thread */
_Thread_local CPU* rt_tcpu;     /* guest threads (NULL on the main thread -> g_cpu) */
_Thread_local RtEhFrame* rt_eh_top;
_Thread_local int rt_eh_depth, rt_hle_depth;
/* misaligned LOCK operations: global spinlock (aligned ones use host atomics directly) */
static volatile int g_lk_spin;
void rt_lk_slow_lock(void) { while (__atomic_exchange_n(&g_lk_spin, 1, __ATOMIC_ACQUIRE)) while (__atomic_load_n(&g_lk_spin, __ATOMIC_RELAXED)) RT_PAUSE(); }
void rt_lk_slow_unlock(void) { __atomic_store_n(&g_lk_spin, 0, __ATOMIC_RELEASE); }
#define MAX_MODULES 32
static Module* g_mods[MAX_MODULES]; static int g_nmods;

#define TEB_VA     0x7FFDE000u
#define PEB_VA     0x7FFDF000u
#define STACK_TOP  0x00400000u   /* 3 MB main-thread guest stack (MassEffect.exe reserves 3 MB) */
#define STACK_SIZE 0x00300000u

void rt_init(void) {
    M = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (M == MAP_FAILED) { perror("arena"); exit(1); }
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.esp = STACK_TOP - 16;
    g_cpu.fs_base = TEB_VA;
    rt_fninit(&g_cpu); g_cpu.fcw = 0x27F;      /* MSVC CRT default: 53-bit precision, round to nearest */
    g_cpu.mxcsr = 0x1F80;
    wr32(TEB_VA + 0x00, 0xFFFFFFFFu);          /* SEH chain end */
    wr32(TEB_VA + 0x04, STACK_TOP);            /* stack base */
    wr32(TEB_VA + 0x08, STACK_TOP - STACK_SIZE);
    wr32(TEB_VA + 0x18, TEB_VA);               /* self */
    wr32(TEB_VA + 0x20, 0x1000);               /* pid */
    wr32(TEB_VA + 0x24, 0x1004);               /* tid */
    wr32(TEB_VA + 0x2C, TEB_VA + 0xE10);       /* TLS slot array */
    wr32(TEB_VA + 0x30, PEB_VA);
    hle_init();
}

static int fcmp(const void* k, const void* e) {
    uint32_t a = *(const uint32_t*)k, b = ((const FuncEntry*)e)->va;
    return a < b ? -1 : a > b;
}

static uint32_t g_expect_crc;
void rt_expect_image_crc(uint32_t crc) { g_expect_crc = crc; }
uint32_t rt_crc32(const uint8_t* p, size_t n) {
    static uint32_t T[256];
    if (!T[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = T[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }
/* Maps an original PE file (DLL/EXE from the game's Binaries folder) the way the Windows loader does:
 * headers + raw section data at their RVAs, base relocations when the build uses another base.
 * Returns 0 if the file is not a PE32 or does not match the recompiled module. */
static int map_pe(Module* m, const uint8_t* d, size_t n, const char* path) {
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') return 0;
    uint32_t pe = le32(d + 0x3C);
    if (pe > n - 0xF8 || memcmp(d + pe, "PE\0\0", 4) || le16(d + pe + 24) != 0x10B) { fprintf(stderr, "[rt] %s: not a PE32 file\n", path); return 0; }
    unsigned nsec = le16(d + pe + 6), optsz = le16(d + pe + 20);
    const uint8_t* o = d + pe + 24;
    uint32_t ibase = le32(o + 28), isize = le32(o + 56), hsize = le32(o + 60), ndirs = le32(o + 92);
    if (isize != m->size) { fprintf(stderr, "[rt] %s: SizeOfImage %x, this build expects %x (different game version?)\n", path, isize, m->size); return 0; }
    uint8_t* img = M + m->base;
    memset(img, 0, m->size);
    memcpy(img, d, hsize < n && hsize < m->size ? hsize : 0x400);
    const uint8_t* sh = o + optsz;
    if ((size_t)(sh - d) + 40u * nsec > n) return 0;
    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t* s = sh + 40 * i;
        uint32_t va = le32(s + 12), rs = le32(s + 16), rp = le32(s + 20);
        if (va >= m->size || rp >= n) continue;
        if (rs > m->size - va) rs = m->size - va;
        if (rs > n - rp) rs = (uint32_t)(n - rp);
        memcpy(img + va, d + rp, rs);
    }
    if (ibase != m->base && ndirs > 5) {     /* recompiled with --base: apply IMAGE_REL_BASED_HIGHLOW fixups */
        uint32_t rva = le32(o + 96 + 40), sz = le32(o + 96 + 44), delta = m->base - ibase, nfix = 0;
        for (uint32_t p = rva; p + 8 <= rva + sz && p + 8 <= m->size; ) {
            uint32_t page = le32(img + p), bsz = le32(img + p + 4);
            if (bsz < 8) break;
            for (uint32_t k = 8; k + 2 <= bsz; k += 2) {
                uint16_t e = le16(img + p + k);
                if (e >> 12 == 3 && page + (e & 0xFFF) + 4 <= m->size) {
                    uint8_t* q = img + page + (e & 0xFFF); uint32_t v = le32(q) + delta;
                    q[0] = v; q[1] = v >> 8; q[2] = v >> 16; q[3] = v >> 24; nfix++;
                }
            }
            p += bsz;
        }
        fprintf(stderr, "[rt] %s: rebased %08x -> %08x (%u fixups)\n", path, ibase, m->base, nfix);
    }
    return 1;
}

static int load_module(Module* m, const char* image_path, int soft);
void rt_load_module(Module* m, const char* image_path) { load_module(m, image_path, 0); }
int rt_try_load_module(Module* m, const char* image_path) { return load_module(m, image_path, 1); }
static int load_module(Module* m, const char* image_path, int soft) {
    FILE* f = fopen(image_path, "rb");
    if (!f) { perror(image_path); if (soft) { g_expect_crc = 0; return 0; } exit(1); }
    fseek(f, 0, SEEK_END); long fl = ftell(f); fseek(f, 0, SEEK_SET);
    if (fl <= 0) { fprintf(stderr, "empty image %s\n", image_path); fclose(f); if (soft) { g_expect_crc = 0; return 0; } exit(1); }
    uint8_t* buf = malloc((size_t)fl);
    size_t n = buf ? fread(buf, 1, (size_t)fl, f) : 0; fclose(f);
    if (n != (size_t)fl) { fprintf(stderr, "read error %s\n", image_path); exit(1); }
    if (n >= 2 && buf[0] == 'M' && buf[1] == 'Z' && n != m->size) {      /* original PE file */
        if (!map_pe(m, buf, n, image_path)) { free(buf); if (soft) { g_expect_crc = 0; return 0; } exit(1); }
    } else {                                                             /* image.bin written by recomp.py */
        if (n != m->size) { fprintf(stderr, "short image %s\n", image_path); free(buf); if (soft) { g_expect_crc = 0; return 0; } exit(1); }
        memcpy(M + m->base, buf, n);
    }
    free(buf);
    if (g_expect_crc) {
        uint32_t crc = rt_crc32(M + m->base, m->size);
        if (crc != g_expect_crc) {
            fprintf(stderr, "[rt] %s: image CRC %08x, this build was recompiled from %08x - the file is a different version "
                            "(the recompiled code would not match its data)\n", image_path, crc, g_expect_crc);
            g_expect_crc = 0;
            if (soft) { memset(M + m->base, 0, m->size); return 0; }
            exit(1);
        }
        g_expect_crc = 0;
    }
    unsigned nguest = 0, nmiss = 0;
    for (unsigned i = 0; i < m->nimports; i++) {
        const ImportEntry* im = &m->imports[i];
        Module* dm = rt_module_by_name(im->dll);       /* statically linked guest DLL (msvcr80, msvcp80, ...) */
        uint32_t v = dm ? rt_find_export(dm, im->name) : 0;
        if (v && rt_import_prefer_hle && rt_import_prefer_hle(m, im->dll, im->name)) dm = NULL, v = 0;
        if (dm && !v) fprintf(stderr, "[rt] %s: import %s!%s not exported -> HLE\n", m->name, im->dll, im->name);
        if (v) nguest++;
        else if (!hle_resolve_impl(im->dll, im->name)) {   /* would fault when called: list them (ME1_LIST_MISSING=1 prints all) */
            static int all = -1; if (all < 0) all = getenv("ME1_LIST_MISSING") != NULL;
            if (all) fprintf(stderr, "[rt] %s: no HLE for %s!%s\n", m->name, im->dll, im->name);
            nmiss++;
        }
        wr32(im->slot, v ? v : hle_resolve(im->dll, im->name));
    }
    g_mods[g_nmods++] = m;
    fprintf(stderr, "[rt] loaded %s at %08x (%u funcs, %u imports, %u from guest DLLs, %u without HLE)\n", m->name, m->base, m->nfuncs, m->nimports, nguest, nmiss);
    return 1;
}

uint32_t rt_find_export(Module* m, const char* name) {
    for (const ExportEntry* e = m->exports; e->name; e++) if (!strcmp(e->name, name)) return e->va;
    return 0;
}

Module* rt_module_at(uint32_t va) {
    for (int i = 0; i < g_nmods; i++) if (va >= g_mods[i]->base && va - g_mods[i]->base < g_mods[i]->size) return g_mods[i];
    return NULL;
}
int (*rt_import_prefer_hle)(const Module* importer, const char* dll, const char* name);
Module* rt_module_by_name(const char* dll) {
    for (int i = 0; i < g_nmods; i++) if (!strcasecmp(g_mods[i]->name, dll)) return g_mods[i];
    /* d3dx9_31.dll links against the system msvcrt.dll: msvcr80 exports the same functions with the same ABI */
    if (!strcasecmp(dll, "msvcrt.dll")) return rt_module_by_name("msvcr80.dll");
    return NULL;
}
/* v0.10.13 (R3): direct-mapped cache of indirect-call targets (vtable calls, call [eax+N]) in front of the
   module scan + bsearch. Entries point into the static per-module function tables, which are never freed (no
   module unloading), so a stale entry is impossible; a slot is valid only when its va equals the target. */
#define DCACHE_BITS 16
static const FuncEntry* g_dcache[1u << DCACHE_BITS];
static const FuncEntry* lookup(uint32_t t) {
    uint32_t h = ((t >> 2) ^ (t >> (2 + DCACHE_BITS))) & ((1u << DCACHE_BITS) - 1);
    const FuncEntry* e = __atomic_load_n(&g_dcache[h], __ATOMIC_RELAXED);
    if (e && e->va == t) return e;
    Module* m = rt_module_at(t);
    e = m ? bsearch(&t, m->funcs, m->nfuncs, sizeof(FuncEntry), fcmp) : NULL;
    if (e) __atomic_store_n(&g_dcache[h], e, __ATOMIC_RELAXED);
    return e;
}

void dispatch_call(CPU* c, uint32_t t) {
    if (hle_is_thunk(t)) { hle_invoke(c, t); return; }
    const FuncEntry* e = lookup(t);
    if (!e) rt_fault(c, t, "dispatch: no function at target");
    if (e->owner && rt_eh_depth) rt_eh_land(c, e);   /* exception transfer into a live frame (does not return if found) */
    e->fn(c);
}

void call_import(CPU* c, uint32_t slot) { dispatch_call(c, rd32(slot)); }

#define RET_SENTINEL 0xFFFFFFF0u
uint32_t rt_call(CPU* c, uint32_t va, int nargs, const uint32_t* args) {
    uint32_t sp = c->esp;
    for (int i = nargs - 1; i >= 0; i--) push32(c, args[i]);
    push32(c, RET_SENTINEL);
    dispatch_call(c, va);
    uint32_t used = sp - c->esp;   /* 0 for stdcall, 4*nargs for cdecl */
    if (used != 0 && used != 4u * nargs)
        fprintf(stderr, "[rt] warning: stack imbalance after %08x: %d bytes\n", va, (int)used);
    c->esp = sp;
    return c->eax;
}

void rt_fault(CPU* c, uint32_t eip, const char* why) {
    fprintf(stderr, "[rt] FAULT at %08x: %s\n  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            eip, why, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    fprintf(stderr, "  stack:");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", rd32(c->esp + 4 * i));
    fprintf(stderr, "\n  code addresses on the stack (likely return addresses):");
    for (int i = 0, n = 0; i < 2048 && n < 32; i++) {
        uint32_t a = c->esp + 4 * i, v = rd32(a); Module* m = rt_module_at(v);
        if (m && v - m->base >= 0x1000) { fprintf(stderr, "%s%s+%x", n % 6 ? " " : "\n    ", m->name, v - m->base); n++; }
    }
    fprintf(stderr, "\n");
    void hle_hb_now(const char*); hle_hb_now("(fault)");
    void hle_report(void); hle_report();
    exit(3);
}
void UNIMPL(CPU* c, uint32_t eip, const char* what) {
    char b[256]; snprintf(b, sizeof b, "unimplemented instruction: %s", what); rt_fault(c, eip, b);
}
void rt_cpuid(CPU* c) {
    uint32_t leaf = c->eax;
    if (leaf == 0) { c->eax = 1; c->ebx = 0x756e6547; c->edx = 0x49656e69; c->ecx = 0x6c65746e; }   /* GenuineIntel */
    else if (leaf == 1) { c->eax = 0x00000F29; c->ebx = 0; c->ecx = 0; c->edx = (1u<<0) | (1u<<4) | (1u<<8) | (1u<<15) | (1u<<23) | (1u<<24) | (1u<<25) | (1u<<26); } /* FPU TSC CX8 CMOV MMX FXSR SSE SSE2 */
    else { c->eax = c->ebx = c->ecx = c->edx = 0; }
}
void rt_rdtsc(CPU* c) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 2000000000ull + (uint64_t)ts.tv_nsec * 2;
    c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32);
}
