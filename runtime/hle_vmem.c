/* Guest virtual memory (VirtualAlloc/VirtualFree/VirtualQuery) - hle_vmem.c
 *
 * MassEffect.exe is not large-address-aware, so everything lives below 0x80000000. Unreal's allocator takes its pools
 * and every big block straight from VirtualAlloc and gives them back with VirtualFree, so releasing really has to
 * return memory: released/decommitted pages are madvise(DONTNEED)'d (they read back as zero, like fresh pages on
 * Windows, and the host gets the RAM back). Reservations use the 64 KiB Windows allocation granularity.
 * Ranges: [0x00800000,0x10000000) [0x12400000,0x20000000) [0x30000000,0x78000000); 0x20000000-0x30000000 is the
 * HLE heap (ghalloc), 0x10000000-0x12400000 the game DLLs, 0x78000000+ msvcr80/msvcp80/TEB/PEB. */
#define _GNU_SOURCE
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

#define GRAN 0x10000u
typedef struct { uint32_t lo, hi; } Rng;
static Rng g_free[8192]; static int g_nfree;                 /* sorted, disjoint, coalesced */
typedef struct { uint32_t base, size; } Res;
static Res g_res[65536]; static int g_nres;                  /* sorted by base */
static pthread_mutex_t g_vm_mx = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_reserved, g_peak;

static void vm_init(void) {
    static int done; if (done) return; done = 1;
    static const Rng init[] = { { 0x00800000u, 0x10000000u }, { 0x12400000u, 0x20000000u }, { 0x30000000u, 0x78000000u } };
    for (unsigned i = 0; i < sizeof init / sizeof init[0]; i++) g_free[g_nfree++] = init[i];
}
static void host_release(uint32_t a, uint32_t n) {   /* zero + give back host pages (aligned middle via madvise) */
    hle_watch_release(a, n, "VirtualFree");
    static uintptr_t pg; if (!pg) pg = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t p = (uintptr_t)(M + a), end = p + n, s = (p + pg - 1) & ~(pg - 1), e = end & ~(pg - 1);
    if (s >= e) { memset((void*)p, 0, n); return; }
    if (s > p) memset((void*)p, 0, s - p);
    madvise((void*)s, e - s, MADV_DONTNEED);
    if (end > e) memset((void*)e, 0, end - e);
}
static int res_find(uint32_t a) {   /* index of the reservation containing a, or -1 */
    int lo = 0, hi = g_nres - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a < g_res[mid].base) hi = mid - 1;
        else if (a - g_res[mid].base >= g_res[mid].size) lo = mid + 1;
        else return mid;
    }
    return -1;
}
static void res_add(uint32_t base, uint32_t size) {
    int i = 0; while (i < g_nres && g_res[i].base < base) i++;
    memmove(&g_res[i + 1], &g_res[i], (g_nres - i) * sizeof(Res)); g_res[i].base = base; g_res[i].size = size; g_nres++;
    g_reserved += size; if (g_reserved > g_peak) g_peak = g_reserved;
}
static uint32_t carve(uint32_t want, uint32_t size, int top) {   /* take [base, base+size) out of the free list */
    for (int k = 0; k < g_nfree; k++) {
        int i = top ? g_nfree - 1 - k : k;
        Rng* r = &g_free[i];
        uint32_t base;
        if (want) { if (want < r->lo || want - r->lo > r->hi - r->lo || r->hi - want < size) continue; base = want; }
        else { if (r->hi - r->lo < size) continue; base = top ? r->hi - size : r->lo; }
        if (base == r->lo && base + size == r->hi) { memmove(r, r + 1, (g_nfree - i - 1) * sizeof(Rng)); g_nfree--; }
        else if (base == r->lo) r->lo += size;
        else if (base + size == r->hi) r->hi -= size;
        else {   /* split */
            if (g_nfree >= (int)(sizeof g_free / sizeof g_free[0])) return 0;
            memmove(r + 2, r + 1, (g_nfree - i - 1) * sizeof(Rng)); g_nfree++;
            r[1].lo = base + size; r[1].hi = r->hi; r->hi = base;
        }
        return base;
    }
    return 0;
}
static void give_back(uint32_t lo, uint32_t hi) {
    int i = 0; while (i < g_nfree && g_free[i].lo < lo) i++;
    int left = i > 0 && g_free[i - 1].hi == lo, right = i < g_nfree && g_free[i].lo == hi;
    if (left && right) { g_free[i - 1].hi = g_free[i].hi; memmove(&g_free[i], &g_free[i + 1], (g_nfree - i - 1) * sizeof(Rng)); g_nfree--; }
    else if (left) g_free[i - 1].hi = hi;
    else if (right) g_free[i].lo = lo;
    else { memmove(&g_free[i + 1], &g_free[i], (g_nfree - i) * sizeof(Rng)); g_free[i].lo = lo; g_free[i].hi = hi; g_nfree++; }
}
uint32_t hle_vmem_alloc(uint32_t addr, uint32_t size, uint32_t type) {
    if (!size) return 0;
    pthread_mutex_lock(&g_vm_mx); vm_init();
    uint32_t r = 0;
    int i = addr ? res_find(addr) : -1;
    if (i >= 0) r = addr & ~0xFFFu;   /* commit (or reset) inside an existing reservation: contents stay */
    else if (!addr || (type & 0x2000u)) {
        uint32_t n = (size + (addr & (GRAN - 1)) + GRAN - 1) & ~(GRAN - 1);
        if (g_nres < (int)(sizeof g_res / sizeof g_res[0])) r = carve(addr & ~(GRAN - 1), n, (type & 0x100000u) != 0);
        if (r) res_add(r, n);
        if (r && addr) r = addr & ~0xFFFu;
    }
    pthread_mutex_unlock(&g_vm_mx);
    return r;
}
int hle_vmem_free(uint32_t addr, uint32_t size, uint32_t type) {
    pthread_mutex_lock(&g_vm_mx); vm_init();
    int i = res_find(addr), ok = 0;
    if (i >= 0) {
        Res* q = &g_res[i];
        if (type & 0x8000u) {          /* MEM_RELEASE: whole reservation, size must be 0 */
            if (addr == q->base) {
                host_release(q->base, q->size); give_back(q->base, q->base + q->size); g_reserved -= q->size;
                memmove(q, q + 1, (g_nres - i - 1) * sizeof(Res)); g_nres--; ok = 1;
            }
        } else if (type & 0x4000u) {   /* MEM_DECOMMIT */
            uint32_t a = addr & ~0xFFFu, e = size ? (addr + size + 0xFFFu) & ~0xFFFu : q->base + q->size;
            if (!size && addr == q->base) e = q->base + q->size;
            if (e > q->base + q->size) e = q->base + q->size;
            host_release(a, e - a); ok = 1;
        }
    } else ok = 1;   /* not ours (e.g. a stack or heap block): ignore like before */
    pthread_mutex_unlock(&g_vm_mx);
    return ok;
}
int hle_vmem_query(uint32_t a, uint32_t* base, uint32_t* size) {
    pthread_mutex_lock(&g_vm_mx);
    int i = res_find(a); if (i >= 0) { *base = g_res[i].base; *size = g_res[i].size; }
    pthread_mutex_unlock(&g_vm_mx);
    return i >= 0;
}
void hle_vmem_stats(uint32_t* nres, uint64_t* reserved, uint64_t* peak) {
    pthread_mutex_lock(&g_vm_mx); *nres = g_nres; *reserved = g_reserved; *peak = g_peak; pthread_mutex_unlock(&g_vm_mx);
}
