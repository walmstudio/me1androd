/* Heartbeat + sampling profiler: makes long silent stages visible (first-run shader build, package loading, hangs).
 * hle_invoke marks the API each guest thread is in (hle_hb_enter/leave). A host thread samples every guest thread
 * ME1_PROF_HZ times per second (default 100): either "blocked in <API>" or, when it runs guest code, the innermost return
 * address on its guest stack (= a call site in the caller of the running function; filtered: the bytes before it must be
 * a call instruction). Every ME1_HB seconds (default 10; 0 = off) it prints
 *   [hb] <t> s | <D3D/D3DX status> | <file I/O> | APIs/<period>: top call-count deltas
 *   [hb]   tid <n> (main): run <r>% [in <API> <b>%] | hot: <fn>+<off> <p>% ... | stack: <fn>+<off> ... (innermost first)
 *   [hb]   blocked: <tid> <API>, ...      (threads that only waited during the period)
 * <fn> is the generated C function containing the address (dx_5a2000 = d3dx9_31.dll, m_10ec4490 = MassEffect.exe, ...),
 * so "rg 'void dx_5a2000'" finds it in out/. */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#define HOTN 256
typedef struct Slot {
    HbSlot pub;                    /* api/seq written by the owning thread */
    CPU* c; int main, dead;
    unsigned run, blk;             /* samples in the current period */
    const char* blk_api; unsigned blk_api_n;
    uint32_t hot_a[HOTN]; unsigned hot_n[HOTN];
    int htid;                      /* v0.10.13: host tid of the thread that runs it now (telemetry) */
    unsigned long long ps_run, ps_wait; double ps_t;   /* previous /proc schedstat sample */
    struct Slot* next;
} Slot;
_Thread_local HbSlot* hle_hb_cur;
static Slot* g_slots;
static pthread_mutex_t g_hbmx = PTHREAD_MUTEX_INITIALIZER;
static double g_t0, g_tlast;
static int g_period = -1;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

HbSlot* hle_hb_new(CPU* c) {   /* g_cpu runs on two host threads (loader, then the big-stack main thread): one slot */
    pthread_mutex_lock(&g_hbmx);
    Slot* s = NULL;
    if (c == &g_cpu) for (Slot* t = g_slots; t; t = t->next) if (t->main && !t->dead) { s = t; break; }
    if (!s) { s = calloc(1, sizeof *s); s->c = c; s->main = c == &g_cpu; s->next = g_slots; g_slots = s; }
#ifdef __linux__
    s->htid = (int)syscall(SYS_gettid); s->ps_t = 0;   /* main: the big-stack thread replaces the loader */
#endif
    pthread_mutex_unlock(&g_hbmx);
    return hle_hb_cur = &s->pub;
}
void hle_hb_thread_exit(void) {
    Slot* s = (Slot*)hle_hb_cur; if (!s) return;
    pthread_mutex_lock(&g_hbmx); s->dead = 1; s->c = NULL; pthread_mutex_unlock(&g_hbmx);
    hle_hb_cur = NULL;
}

/* ---- code addresses ---- */
static const char* mod_prefix(const Module* m) {
    static const char* const map[][2] = { { "MassEffect.exe", "m" }, { "d3dx9_31.dll", "dx" }, { "msvcr80.dll", "crt" },
        { "msvcp80.dll", "cpp" }, { "PhysXCore.dll", "pxc" }, { "PhysXLoader.dll", "pxl" } };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcasecmp(m->name, map[i][0])) return map[i][1];
    return m->name;
}
static const FuncEntry* func_floor(const Module* m, uint32_t va) {
    if (!m->nfuncs || va < m->funcs[0].va) return NULL;
    unsigned lo = 0, hi = m->nfuncs - 1;
    while (lo < hi) { unsigned mid = (lo + hi + 1) / 2; if (m->funcs[mid].va <= va) lo = mid; else hi = mid - 1; }
    return &m->funcs[lo];
}
/* return address of a call: e8 rel32 | ff /2 (call r/m32: reg, [reg], [reg+d8], [reg+d32], with SIB) */
static int is_ret_addr(uint32_t v) {
    Module* m = rt_module_at(v);
    if (!m || v - m->base < 0x1000 || !m->nfuncs || v <= m->funcs[0].va || v > m->funcs[m->nfuncs - 1].va + 0x10000) return 0;
    if (rd8(v - 5) == 0xE8) return 1;
    for (int k = 2; k <= 7; k++) if (rd8(v - k) == 0xFF && ((rd8(v - k + 1) >> 3) & 7) == 2) return 1;
    return 0;
}
static void fmt_addr(char* o, size_t n, uint32_t v) {
    Module* m = rt_module_at(v); const FuncEntry* f = m ? func_floor(m, v) : NULL;
    if (f) snprintf(o, n, "%s_%x+%x", mod_prefix(m), f->va, v - f->va);
    else if (m) snprintf(o, n, "%s+%x", mod_prefix(m), v - m->base);
    else snprintf(o, n, "%08x", v);
}
static int stack_scan(uint32_t esp, uint32_t* out, int max, int depth) {
    int n = 0;
    if (esp < 0x10000 || esp > 0xFFFF0000u) return 0;
    for (int i = 0; i < depth && n < max; i++) { uint32_t v = rd32(esp + 4 * i); if (is_ret_addr(v)) out[n++] = v; }
    return n;
}

/* ---- sampling ---- */
static void sample(void) {
    pthread_mutex_lock(&g_hbmx);
    for (Slot** pp = &g_slots; *pp; ) {
        Slot* s = *pp;
        if (s->dead) { *pp = s->next; free(s); continue; }
        const char* api = s->pub.api;
        if (api) {
            s->blk++;
            if (api == s->blk_api) s->blk_api_n++; else if (s->blk_api_n) s->blk_api_n--; else { s->blk_api = api; s->blk_api_n = 1; }
        } else {
            s->run++;
            uint32_t ra; if (stack_scan(s->c->esp, &ra, 1, 512) == 1) {
                unsigned h = (ra * 2654435761u) >> 24;
                for (unsigned p = 0; p < HOTN; p++, h = (h + 1) & (HOTN - 1)) {
                    if (s->hot_a[h] == ra) { s->hot_n[h]++; break; }
                    if (!s->hot_a[h]) { s->hot_a[h] = ra; s->hot_n[h] = 1; break; }
                }
            }
        }
        pp = &s->next;
    }
    pthread_mutex_unlock(&g_hbmx);
}

#define NAPI 1024
static const char* g_api_name[NAPI]; static unsigned g_api_prev[NAPI]; static int g_api_n;
static void api_deltas(char* o, size_t cap) {
    static const char* nm[NAPI]; static unsigned k[NAPI]; int idx[NAPI];
    int n = hle_count_snapshot(nm, k, NAPI), m = 0;
    for (int i = 0; i < n; i++) {
        unsigned prev = i < g_api_n ? g_api_prev[i] : 0;
        if (k[i] > prev) idx[m++] = i;
    }
    for (int a = 0; a < m; a++) for (int b = a + 1; b < m; b++) {   /* small m: selection of the top 8 */
        unsigned da = k[idx[a]] - (idx[a] < g_api_n ? g_api_prev[idx[a]] : 0), db = k[idx[b]] - (idx[b] < g_api_n ? g_api_prev[idx[b]] : 0);
        if (db > da) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
        if (a >= 8) break;
    }
    size_t L = 0; o[0] = 0;
    for (int a = 0; a < m && a < 8 && L < cap; a++) {
        int i = idx[a]; unsigned d = k[i] - (i < g_api_n ? g_api_prev[i] : 0);
        L += (size_t)snprintf(o + L, cap - L, "%s%s %u", a ? ", " : "", nm[i], d);
    }
    if (!m) snprintf(o, cap, "none");
    for (int i = 0; i < n; i++) { g_api_name[i] = nm[i]; g_api_prev[i] = k[i]; }
    g_api_n = n;
}

/* ---- v0.10.13: host scheduling telemetry (which core, its clock, on-CPU and runqueue share since the last beat) ---- */
static long read_long(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) return -1;
    long v = -1; if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); return v;
}
static size_t host_stats(Slot* s, char* o, size_t cap, double t) {
#ifdef __linux__
    if (!s->htid || cap < 8) return 0;
    char p[96]; snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", s->htid);
    unsigned long long run = 0, wait = 0; FILE* f = fopen(p, "r");
    int ok = f && fscanf(f, "%llu %llu", &run, &wait) == 2; if (f) fclose(f);
    int cpu = -1; snprintf(p, sizeof p, "/proc/self/task/%d/stat", s->htid);
    if ((f = fopen(p, "r"))) {   /* field 39 = processor; skip "pid (comm)" first (comm may contain spaces) */
        char b[1024]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f);
        char* q = strrchr(b, ')');
        if (q) { int fld = 2; for (q++; *q && fld < 39; q++) if (*q == ' ') fld++; if (fld == 39) cpu = atoi(q); }
    }
    size_t L = 0;
    if (cpu >= 0) {
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        long khz = read_long(p);
        L += (size_t)snprintf(o + L, cap - L, " | cpu %d", cpu);
        if (khz > 0) L += (size_t)snprintf(o + L, cap - L, " @%ld MHz", khz / 1000);
    }
    if (ok && s->ps_t > 0 && t > s->ps_t && L < cap) {
        double ns = (t - s->ps_t) * 1e9;
        L += (size_t)snprintf(o + L, cap - L, ", oncpu %.0f%%, rq %.0f%%", 100.0 * (double)(run - s->ps_run) / ns, 100.0 * (double)(wait - s->ps_wait) / ns);
    }
    if (ok) { s->ps_run = run; s->ps_wait = wait; s->ps_t = t; }
    return L < cap ? L : cap - 1;
#else
    (void)s; (void)o; (void)cap; (void)t; return 0;
#endif
}
static long read_last_long(const char* path) {   /* "idx freq" (MediaTek ged) or a single value */
    FILE* f = fopen(path, "r"); if (!f) return -1;
    char b[128]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f);
    long v = -1; for (char* q = b; *q; ) { char* e; long x = strtol(q, &e, 10); if (e == q) q++; else { v = x; q = e; } }
    return v;
}
static void gpu_freq(char* o, size_t cap) {   /* Adreno (kgsl) / Mali on MediaTek (ged) / generic devfreq */
    static const char* const paths[] = { "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "/sys/class/kgsl/kgsl-3d0/gpuclk",
        "/sys/kernel/ged/hal/current_freqency", "/sys/class/misc/mali0/device/cur_freq", NULL };
    for (int i = 0; paths[i]; i++) {
        long v = read_last_long(paths[i]);
        if (v > 0) { snprintf(o, cap, " | gpu %ld MHz", v > 10000000 ? v / 1000000 : v > 10000 ? v / 1000 : v); return; }
    }
    o[0] = 0;
}
static void beat(const char* why) {
    double t = now(), dt = t - g_tlast; g_tlast = t;
    static char d3d[1024], fs[512], api[768], line[2048], blk[1024], al[256], mm[160], ui[256];
    hle_d3d9_heartbeat(d3d, sizeof d3d);
    hle_fs_brief(fs, sizeof fs);
    hle_al_brief(al, sizeof al);
    hle_mm_brief(mm, sizeof mm);
    hle_ui_brief(ui, sizeof ui);
    api_deltas(api, sizeof api);
    char gf[48]; gpu_freq(gf, sizeof gf);
    fprintf(stderr, "[hb] %.0f s%s%s | %s | %s | %s | %s | %s%s | APIs/%.0fs: %s\n", t - g_t0, why ? " " : "", why ? why : "", d3d, fs, al, mm, ui, gf, dt, api);
    size_t bl = 0; blk[0] = 0;
    pthread_mutex_lock(&g_hbmx);
    for (Slot* s = g_slots; s; s = s->next) {
        if (s->dead || !s->c) continue;
        uint32_t tid = rd32(s->c->fs_base + 0x24);
        unsigned tot = s->run + s->blk;
        if (!s->run && tot) {   /* only waited */
            if (bl < sizeof blk - 64) bl += (size_t)snprintf(blk + bl, sizeof blk - bl, "%s%u%s %s", bl ? ", " : "", tid, s->main ? "(main)" : "", s->blk_api ? s->blk_api : "?");
        } else if (tot) {
            size_t L = (size_t)snprintf(line, sizeof line, "[hb]   tid %u%s: run %u%%", tid, s->main ? " (main)" : "", 100 * s->run / tot);
            if (s->blk) L += (size_t)snprintf(line + L, sizeof line - L, ", in %s %u%%", s->blk_api ? s->blk_api : "?", 100 * s->blk / tot);
            L += (size_t)snprintf(line + L, sizeof line - L, " | hot:");
            for (int r = 0; r < 6; r++) {   /* top 6 call sites */
                unsigned best = 0, bi = 0;
                for (unsigned h = 0; h < HOTN; h++) if (s->hot_n[h] > best) { best = s->hot_n[h]; bi = h; }
                if (!best) break;
                char a[64]; fmt_addr(a, sizeof a, s->hot_a[bi]);
                L += (size_t)snprintf(line + L, sizeof line - L, " %s %u%%", a, 100 * best / s->run);
                s->hot_n[bi] = 0;
            }
            if (L < sizeof line - 96) L += host_stats(s, line + L, sizeof line - L, t);
            uint32_t st[12]; int n = stack_scan(s->c->esp, st, 12, 4096);
            L += (size_t)snprintf(line + L, sizeof line - L, " | stack:");
            for (int i = 0; i < n && L < sizeof line - 64; i++) { char a[64]; fmt_addr(a, sizeof a, st[i]); L += (size_t)snprintf(line + L, sizeof line - L, " %s", a); }
            fprintf(stderr, "%s\n", line);
        } else if (why) {   /* on demand (signal/fault): also threads that were not sampled */
            uint32_t st[12]; int n = stack_scan(s->c->esp, st, 12, 4096);
            size_t L = (size_t)snprintf(line, sizeof line, "[hb]   tid %u%s%s%s | stack:", tid, s->main ? " (main)" : "", s->pub.api ? ": in " : "", s->pub.api ? s->pub.api : "");
            for (int i = 0; i < n && L < sizeof line - 64; i++) { char a[64]; fmt_addr(a, sizeof a, st[i]); L += (size_t)snprintf(line + L, sizeof line - L, " %s", a); }
            fprintf(stderr, "%s\n", line);
        }
        s->run = s->blk = 0; s->blk_api = NULL; s->blk_api_n = 0;
        memset(s->hot_a, 0, sizeof s->hot_a); memset(s->hot_n, 0, sizeof s->hot_n);
    }
    pthread_mutex_unlock(&g_hbmx);
    if (bl) fprintf(stderr, "[hb]   blocked: %s\n", blk);
}
void hle_hb_now(const char* why) { if (g_period > 0) beat(why); }

static int g_hz;
static void* hb_main(void* p) {
    (void)p; int hz = g_hz;
    double next = g_t0 + g_period;
    for (;;) {
        usleep(1000000 / hz);
        sample();
        if (now() >= next) { beat(NULL); next += g_period; if (next < now()) next = now() + g_period; }
    }
    return NULL;
}
void hle_hb_start(void) {
    if (g_period >= 0) return;
    const char* e = getenv("ME1_HB"); g_period = e ? atoi(e) : 10;
    g_t0 = g_tlast = now();
    if (g_period <= 0) return;
    e = getenv("ME1_PROF_HZ"); g_hz = e ? atoi(e) : 100; if (g_hz < 1) g_hz = 1; if (g_hz > 1000) g_hz = 1000;
    pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, hb_main, NULL)) { fprintf(stderr, "[hb] cannot start the heartbeat thread\n"); g_period = 0; }
    else fprintf(stderr, "[hb] heartbeat every %d s, profiler %d samples/s (ME1_HB, ME1_PROF_HZ)\n", g_period, g_hz);
}

/* ---- all guest threads' stacks now (watch hits, faults) ---- */
static void stack_line(char* line, size_t cap, const Slot* s, const char* api) {
    uint32_t tid = rd32(s->c->fs_base + 0x24);
    size_t L = (size_t)snprintf(line, cap, "[watch]   tid %u%s%s%s | esp %08x | stack:", tid, s->main ? " (main)" : "", api ? ": in " : "", api ? api : "", s->c->esp);
    uint32_t st[16]; int n = stack_scan(s->c->esp, st, 16, 8192);
    for (int i = 0; i < n && L < cap - 64; i++) { char a[64]; fmt_addr(a, sizeof a, st[i]); L += (size_t)snprintf(line + L, cap - L, " %s", a); }
}
void hle_hb_stacks(const char* why, CPU* me) {
    static char line[2048];
    fprintf(stderr, "[watch] %s: guest threads%s\n", why, me ? " (* = the releasing thread)" : "");
    pthread_mutex_lock(&g_hbmx);
    for (Slot* s = g_slots; s; s = s->next) {
        if (s->dead || !s->c) continue;
        stack_line(line, sizeof line, s, s->pub.api);
        fprintf(stderr, "%s%s\n", line, s->c == me ? " *" : "");
    }
    pthread_mutex_unlock(&g_hbmx);
}

/* ---- memory watch: ME1_WATCH=<hex>[*][,<hex>[*]...] (default 12283ce0* when PhysXCore.dll is loaded; 0 = off)
 * <hex>   : a dword is polled (every 250 us); a change prints the old/new value and every guest thread's stack.
 * <hex>*  : the dword is a pointer; the first dword of the object it points to (its vtable) is polled as well.
 * 12283ce0 is PhysXCore's global user allocator pointer (the FNxAllocator object of the engine): v0.8 crashed calling
 * through its vtable (= 0) in createScene. hle_vmem also reports VirtualFree/decommit of a range holding a watched
 * address, with the stack of the freeing thread (exact culprit). */
typedef struct { uint32_t addr; int deref; uint32_t val, tgt, tval; } Watch;
static Watch g_w[8]; static int g_nw; static volatile int g_wready; static unsigned g_wev;
static void* watch_main(void* p) {
    (void)p;
    for (;;) {
        usleep(250);
        for (int i = 0; i < g_nw; i++) {
            Watch* w = &g_w[i]; uint32_t v = rdo32(w->addr);
            if (v != w->val) {
                if (g_wev++ < 40) {
                    fprintf(stderr, "[watch] %.3f s: [%08x] %08x -> %08x\n", now() - g_t0, w->addr, w->val, v);
                    if (!w->deref || w->val) hle_hb_stacks("value changed", NULL);
                }
                w->val = v; w->tgt = v; w->tval = w->deref && v ? rdo32(v) : 0;
                if (w->deref && v && g_wev < 40) fprintf(stderr, "[watch]   object %08x: first dword %08x\n", v, w->tval);
                continue;
            }
            if (w->deref && w->tgt) {
                uint32_t o = rdo32(w->tgt);
                if (o != w->tval) {
                    if (g_wev++ < 40) {
                        fprintf(stderr, "[watch] %.3f s: object %08x (pointer at [%08x]) first dword %08x -> %08x; bytes:", now() - g_t0, w->tgt, w->addr, w->tval, o);
                        for (int k = 0; k < 16; k++) fprintf(stderr, " %08x", rd32(w->tgt - 16 + 4 * k));
                        fprintf(stderr, " (from %08x)\n", w->tgt - 16);
                        hle_hb_stacks("object changed", NULL);
                    }
                    w->tval = o;
                }
            }
        }
    }
    return NULL;
}
void hle_watch_start(void) {
    const char* e = getenv("ME1_WATCH"); char def[32] = "";
    if (!e) { Module* m = rt_module_at(0x12283ce0u); if (m && !strcasecmp(m->name, "PhysXCore.dll")) snprintf(def, sizeof def, "12283ce0*"); e = def; }
    if (!*e || !strcmp(e, "0")) return;
    for (const char* q = e; *q && g_nw < 8;) {
        char* end; unsigned long a = strtoul(q, &end, 16); if (end == q) break;
        Watch* w = &g_w[g_nw++]; w->addr = (uint32_t)a; w->deref = *end == '*'; if (*end == '*') end++;
        w->val = rd32(w->addr); w->tgt = w->val; w->tval = w->deref && w->val ? rd32(w->val) : 0;
        q = *end == ',' ? end + 1 : end; if (!*end) break;
    }
    if (!g_nw) return;
    if (g_t0 == 0) g_t0 = now();
    pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, watch_main, NULL)) { fprintf(stderr, "[watch] cannot start\n"); g_nw = 0; return; }
    g_wready = 1;
    fprintf(stderr, "[watch] polling %d address(es): %s (ME1_WATCH)\n", g_nw, e);
}
void hle_watch_release(uint32_t a, uint32_t n, const char* what) {   /* hle_vmem: [a, a+n) is being released/decommitted */
    if (!g_wready) return;
    for (int i = 0; i < g_nw; i++) {
        Watch* w = &g_w[i]; uint32_t hit = 0;
        if (w->addr - a < n) hit = w->addr; else if (w->deref && w->tgt && w->tgt - a < n) hit = w->tgt;
        if (!hit) continue;
        static unsigned k; if (k++ >= 20) return;
        CPU* c = rt_cur();
        fprintf(stderr, "[watch] %s [%08x, %08x) contains watched %08x%s\n", what, a, a + n, hit, hit == w->tgt ? " (the object)" : "");
        if (c) {
            static char line[2048]; uint32_t st[16]; int m = stack_scan(c->esp, st, 16, 8192);
            size_t L = (size_t)snprintf(line, sizeof line, "[watch]   freeing thread tid %u | stack:", rd32(c->fs_base + 0x24));
            for (int j = 0; j < m && L < sizeof line - 64; j++) { char b[64]; fmt_addr(b, sizeof b, st[j]); L += (size_t)snprintf(line + L, sizeof line - L, " %s", b); }
            fprintf(stderr, "%s\n", line);
        }
        hle_hb_stacks("memory released", c);
    }
}
