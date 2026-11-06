/* gearmass.c - GEARMASS, the load governor (v1.2.1). See gearmass.h and docs/PLAN_v1.2.1.md.
 *
 * Variables (the launcher writes them; every ME1_GM_* value pins its knob in every mode):
 *   ME1_GEARMASS   auto (default) | eco | balance | perf | max | custom | off
 *                  me_1.2 alias ME1_CMD_SCHED: 0 = custom, 1 = auto, 2 = eco
 *   ME1_GM_AHEAD   0..2   frames the D3D9 replay thread may lag (ME1_D3D_CMD=2)
 *   ME1_GM_WORKERS 0..8   worker threads (alias ME1_CMD_THREADS=N|auto)
 *   ME1_GM_SPIN    0..2   active waits of the heavy threads (legacy ME1_SPIN_HEAVY=1 -> 1)
 *   ME1_GM_SPIN_US        spin length before a heavy thread blocks (spin 2; default 100 us)
 *   ME1_GM_PRIME   0|1    game thread alone on the prime cores (legacy ME1_CPU_PRIME)
 *   ME1_GM_ADPF_MS        ADPF target work duration in ms (alias ME1_CMD_TARGET_MS)
 *   ME1_GM_UCLAMP  0..1024 utilization floor of the heavy threads (legacy ME1_UCLAMP)
 *   ME1_GM_FPS            frame limit (ME1_FPS_LIMIT, set by the launcher's FpsLimit, wins)
 *   ME1_GM_ANISO   1..16  anisotropy floor; ME1_GM_QUALITY=0|1 the quality governor
 *   ME1_GM_THERMAL        thermal headroom at which "auto" steps down (default 0.90; 1.0 = the system throttles)
 *   ME1_GM_HUD=1          indicator line under the FPS counter (also shown with ME1_SHOW_FPS=1)
 *   ME1_GM_LOG            seconds between [gearmass] log lines (default 5, 0 = off)
 *   ME1_D3D_CMD           the replay thread; default from GEARMASS: 2 with >= 4 cores outside "eco"/"off", else 0
 */
#define _GNU_SOURCE
#include "gearmass.h"
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

/* from hle_thread.c / hle_fs.c */
int hle_heavy_tid(int which);
uint64_t hle_heavy_wait_ns(int which);
void hle_cpu_replace(int prime);
void hle_cpu_uclamp(int v);
void hle_cpu_place(int heavy);
uint64_t hle_fs_read_bytes(void);

volatile GmKnobs gm_k = { GM_BALANCE, 1, 0, 0, 0, 0, 0, 0, 1, 0 };   /* "off" values until gm_init */

static double gm_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }
static const char* const MODE_NAMES[] = { "ECO", "BALANCE", "PERF", "MAX", "CUSTOM", "AUTO", "OFF" };
const char* gm_mode_name(int m) { return m >= 0 && m <= GM_OFF ? MODE_NAMES[m] : "?"; }
static const char* const CLASS_NAMES[] = { "idle", "loading", "capped", "gpu-bound", "cpu-bound", "wait-bound" };
const char* gm_class_name(int c) { return c >= 0 && c <= GMC_WAIT ? CLASS_NAMES[c] : "?"; }
static const char* const HEAVY_NAMES[] = { "main", "render", "d3d" };
static int ci_has(const char* h, const char* n) {   /* case-insensitive substring (no strcasestr: portability) */
    size_t ln = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, ln)) return 1;
    return 0;
}

/* ================= presets and policy (pure) ================= */
void gm_preset(int level, int pool_max, GmKnobs* k) {
    if (level < GM_ECO) level = GM_ECO;
    if (level > GM_MAX) level = GM_MAX;
    static const int AHEAD[4] = { 0, 1, 1, 2 }, SPIN[4] = { 0, 1, 1, 2 }, PRIME[4] = { 0, 0, 1, 1 };
    static const int ADPF[4] = { 0, 0, 80, 80 }, UCL[4] = { 0, 0, 512, 1024 }, FPS[4] = { 30, 0, 0, 0 }, QUAL[4] = { 0, 0, 1, 1 };
    k->level = level; k->ahead = AHEAD[level]; k->spin = SPIN[level]; k->prime = PRIME[level];
    k->adpf_ms10 = ADPF[level]; k->uclamp = UCL[level]; k->fps_limit = FPS[level]; k->quality = QUAL[level];
    k->workers = level == GM_ECO ? 0 : level == GM_BALANCE ? (pool_max < 2 ? pool_max : 2) : pool_max;
    k->aniso = 1;
}
int gm_classify(const GmSample* s, int* which) {
    int w = -1; double hu = -1;
    for (int i = 0; i < 3; i++) if (s->heavy[i] > hu) { hu = s->heavy[i]; w = i; }
    if (which) *which = w;
    if (s->frames <= 0) return s->fs_mbps >= 4 ? GMC_LOADING : GMC_IDLE;   /* no Present: start-up, a loading hitch */
    if (s->fs_mbps >= 16 || s->fps < 3) return GMC_LOADING;
    if (s->fps_limit > 0 && s->fps >= 0.95 * s->fps_limit) return GMC_CAPPED;
    if (hu >= 0.90) return GMC_CPU;
    if (s->gpu_busy >= 0.85) return GMC_GPU;
    if (hu >= 0.80) return GMC_CPU;
    return GMC_WAIT;
}
void gm_policy_init(GmPolicy* p, int start_level, int quality) {
    memset(p, 0, sizeof *p);
    p->level = start_level; p->cap = GM_MAX; p->cand = -1; p->ahead = -1; p->ahead_cand = -1; p->aniso = 1; p->quality = quality;
    p->t_change = p->t_cool = p->t_q = -1e9; p->t_hot = -1e9;
}
void gm_policy_step(GmPolicy* p, const GmSample* s, int cls, double now, double thr) {
    /* 1. thermal: a cap on the level, stepped down before the system throttles, raised after 30 s of headroom */
    int st = s->thermal_status;
    if (st >= 2 || s->headroom >= thr - 0.1) p->t_hot = now;
    if (st >= 4) { if (p->cap != GM_ECO) { p->cap = GM_ECO; p->t_cool = now; } }   /* critical: frame limit 30 */
    else if ((st == 3 || s->headroom >= 1.0) && p->cap > GM_BALANCE) { p->cap = GM_BALANCE; p->t_cool = now; }
    else if (s->headroom >= thr && p->cap > GM_BALANCE && now - p->t_cool >= 10) {
        int c = (p->cap > p->level ? p->level : p->cap) - 1; p->cap = c < GM_BALANCE ? GM_BALANCE : c; p->t_cool = now;
    }
    else if (p->cap < GM_MAX && now - p->t_hot >= 30 && now - p->t_cool >= 30) { p->cap++; p->t_cool = now; }
    /* 2. the level the scene wants */
    int want;
    switch (cls) {
    case GMC_LOADING: want = GM_MAX; break;           /* shorter loading screens */
    case GMC_CAPPED: case GMC_IDLE: want = GM_BALANCE; break;   /* the frame limit is met / nothing to draw: clocks would only heat */
    default: want = s->draws > 0 && s->draws < 60 ? GM_BALANCE : GM_MAX;   /* menus, videos: light */
    }
    if (want > p->cap) want = p->cap;
    /* 3. hysteresis: 2 windows in a row and >= 3 s since the last change; a thermal cap applies at once */
    if (want != p->level) {
        if (want == p->cand) p->cand_n++; else { p->cand = want; p->cand_n = 1; }
        int urgent = p->level > p->cap;
        if (urgent || (p->cand_n >= 2 && now - p->t_change >= 3.0)) { p->level = want; p->t_change = now; p->cand_n = 0; p->cand = -1; }
    } else { p->cand_n = 0; p->cand = -1; }
    /* 4. how far the replay may run behind: GPU-bound -> 2 frames, waits in the chain -> one more than the preset */
    static const int AHEAD[4] = { 0, 1, 1, 2 };
    int base = AHEAD[p->level], aw = cls == GMC_GPU ? 2 : cls == GMC_WAIT ? base + 1 : base;
    if (aw > 2) aw = 2;
    if (p->ahead < 0) p->ahead = base;
    if (aw != p->ahead) {
        if (aw == p->ahead_cand) p->ahead_n++; else { p->ahead_cand = aw; p->ahead_n = 1; }
        if (p->ahead_n >= 2) { p->ahead = aw; p->ahead_n = 0; p->ahead_cand = -1; }
    } else { p->ahead_n = 0; p->ahead_cand = -1; }
    /* 5. quality governor (every 2 s): the GPU's idle time goes into anisotropic filtering, back down above 90 % */
    if (now - p->t_q >= 2.0) {
        p->t_q = now;
        int hot = s->headroom >= thr - 0.05 || s->thermal_status >= 2;
        if (!p->quality || p->level < GM_PERF || hot || (s->gpu_busy >= 0 && s->gpu_busy > 0.90)) { if (p->aniso > 1) p->aniso /= 2; }
        else if (s->gpu_busy >= 0 && s->gpu_busy < 0.60 && cls != GMC_LOADING && cls != GMC_IDLE && p->aniso < 16) p->aniso *= 2;
    }
}

/* ================= state ================= */
static struct {
    int mode, pool_max, ncpu, nfast, nslow, nprime, hud, log_s, started;
    double thr;
    GmKnobs pin; int pinned[10];   /* indices: K_* */
    /* counters fed by the renderer / present */
    uint64_t frames, draws, gpu_ns, gpu_frames, slept_ns; int limit;
    /* the last sample (monitor) */
    GmSample s; int cls, which; double cpu_cores, gpu_ms, freq_ghz[3], freq_pct[3]; int cpu_of[3];
    char hud_s[160]; pthread_mutex_t hmx;
    /* thermal */
    int th_src; double th_temp, th_trip; char th_name[48];
} G = { .hmx = PTHREAD_MUTEX_INITIALIZER };
enum { K_AHEAD, K_WORKERS, K_SPIN, K_PRIME, K_ADPF, K_UCLAMP, K_FPS, K_ANISO, K_QUALITY };
int gm_mode(void) { return G.mode; }
int gm_pool_max(void) { return G.pool_max; }

static long rd_long(const char* path) {
    FILE* f = fopen(path, "r"); long v = -1;
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); }
    return v;
}
static void topology(void) {
    long n = sysconf(_SC_NPROCESSORS_CONF); if (n < 1) n = 1; if (n > 256) n = 256;
    int avail = (int)n;
#ifdef __linux__
    cpu_set_t cur; CPU_ZERO(&cur); if (sched_getaffinity(0, sizeof cur, &cur) == 0 && CPU_COUNT(&cur) > 0) avail = CPU_COUNT(&cur);
#endif
    G.ncpu = avail;
    long fmin = 0, fmax = 0; int nf = 0; long f[256];
    for (int i = 0; i < n; i++) {
        char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        f[i] = rd_long(p);
        if (f[i] > 0) { if (!nf || f[i] < fmin) fmin = f[i]; if (!nf || f[i] > fmax) fmax = f[i]; nf++; }
    }
    G.nfast = avail; G.nslow = 0; G.nprime = 0;
    if (nf == n && fmax > fmin) {
        G.nfast = G.nslow = 0;
        for (int i = 0; i < n; i++) { if (f[i] == fmin) G.nslow++; else G.nfast++; if (f[i] == fmax) G.nprime++; }
        if (G.nprime == G.nfast) G.nprime = 0;   /* two tiers: no separate prime */
    }
}
static int env_int(const char* a, const char* b, int lo, int hi, int* out) {
    const char* e = getenv(a); if ((!e || !*e) && b) e = getenv(b);
    if (!e || !*e || !strcasecmp(e, "auto")) return 0;
    char* end; long v = strtol(e, &end, 10); if (end == e) return 0;
    *out = v < lo ? lo : v > hi ? hi : (int)v; return 1;
}
static int env_ms10(const char* a, const char* b, int* out) {
    const char* e = getenv(a); if ((!e || !*e) && b) e = getenv(b);
    if (!e || !*e) return 0;
    double v = atof(e); if (v <= 0) return 0;
    if (v > 100) v = 100;
    *out = (int)(v * 10 + 0.5); return 1;
}
static void pin_knobs(void) {
    int v; GmKnobs* p = &G.pin;
    if (env_int("ME1_GM_AHEAD", "ME1_CMD_AHEAD", 0, 2, &v)) { p->ahead = v; G.pinned[K_AHEAD] = 1; }
    if (env_int("ME1_GM_WORKERS", "ME1_CMD_THREADS", 0, 8, &v)) { p->workers = v; G.pinned[K_WORKERS] = 1; }
    if (env_int("ME1_GM_SPIN", "ME1_SPIN_HEAVY", 0, 2, &v)) { p->spin = v; G.pinned[K_SPIN] = 1; }
    if (env_int("ME1_GM_PRIME", "ME1_CPU_PRIME", 0, 1, &v)) { p->prime = v; G.pinned[K_PRIME] = 1; }
    if (env_ms10("ME1_GM_ADPF_MS", "ME1_CMD_TARGET_MS", &v)) { p->adpf_ms10 = v; G.pinned[K_ADPF] = 1; }
    if (env_int("ME1_GM_UCLAMP", "ME1_UCLAMP", 0, 1024, &v)) { p->uclamp = v; G.pinned[K_UCLAMP] = 1; }
    if (env_int("ME1_FPS_LIMIT", "ME1_GM_FPS", 0, 240, &v)) { p->fps_limit = v <= 0 ? 0 : v < 10 ? 10 : v; G.pinned[K_FPS] = 1; }
    if (env_int("ME1_GM_ANISO", NULL, 1, 16, &v)) { p->aniso = v; G.pinned[K_ANISO] = 1; }
    if (env_int("ME1_GM_QUALITY", NULL, 0, 1, &v)) { p->quality = v; G.pinned[K_QUALITY] = 1; }
}
static void apply_pins(GmKnobs* k) {
    const GmKnobs* p = &G.pin;
    if (G.pinned[K_AHEAD]) k->ahead = p->ahead;
    if (G.pinned[K_WORKERS]) k->workers = p->workers;
    if (G.pinned[K_SPIN]) k->spin = p->spin;
    if (G.pinned[K_PRIME]) k->prime = p->prime;
    if (G.pinned[K_ADPF]) k->adpf_ms10 = p->adpf_ms10;
    if (G.pinned[K_UCLAMP]) k->uclamp = p->uclamp;
    if (G.pinned[K_FPS]) k->fps_limit = p->fps_limit;
    if (G.pinned[K_ANISO]) k->aniso = p->aniso;
    if (G.pinned[K_QUALITY]) k->quality = p->quality;
}
static int parse_mode(void) {
    const char* e = getenv("ME1_GEARMASS");
    if (e && *e) {
        if (!strcasecmp(e, "auto") || !strcmp(e, "1")) return GM_AUTO;
        if (!strcasecmp(e, "eco")) return GM_ECO;
        if (!strcasecmp(e, "balance") || !strcasecmp(e, "balanced")) return GM_BALANCE;
        if (!strcasecmp(e, "perf") || !strcasecmp(e, "performance")) return GM_PERF;
        if (!strcasecmp(e, "max") || !strcasecmp(e, "maximum")) return GM_MAX;
        if (!strcasecmp(e, "custom")) return GM_CUSTOM;
        if (!strcasecmp(e, "off") || !strcmp(e, "0")) return GM_OFF;
        fprintf(stderr, "[gearmass] ME1_GEARMASS=%s not understood - auto\n", e);
        return GM_AUTO;
    }
    if ((e = getenv("ME1_CMD_SCHED")) && *e) return *e == '0' ? GM_CUSTOM : *e == '2' ? GM_ECO : GM_AUTO;   /* me_1.2 names */
    return GM_AUTO;
}

/* ================= worker pool ================= */
#define GM_WMAX 8
static struct {
    pthread_mutex_t mx; pthread_cond_t go, fin;
    int started; gm_job_fn fn; void* ctx; int n, chunk, want, left, next, busy; unsigned gen;
    uint64_t jobs, jobs_par, items_par, items_ser;
} P = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER };
static void run_chunks(void) {
    for (;;) {
        int i0 = __atomic_fetch_add(&P.next, P.chunk, __ATOMIC_ACQ_REL);
        if (i0 >= P.n) break;
        int i1 = i0 + P.chunk; if (i1 > P.n) i1 = P.n;
        P.fn(P.ctx, i0, i1);
    }
}
static unsigned g_wgen0[GM_WMAX];   /* P.gen when the worker was created: a job posted before it waits is not missed */
static void* worker_main(void* a) {
    int id = (int)(intptr_t)a;
#if defined(__linux__)
    char nm[16]; snprintf(nm, sizeof nm, "me1-w%d", id); pthread_setname_np(pthread_self(), nm);
#endif
    hle_cpu_place(4);
    unsigned seen = 0;
    seen = g_wgen0[id];
    pthread_mutex_lock(&P.mx);
    for (;;) {
        while (P.gen == seen) pthread_cond_wait(&P.go, &P.mx);
        seen = P.gen;
        if (id >= P.want) continue;
        pthread_mutex_unlock(&P.mx);
        run_chunks();
        pthread_mutex_lock(&P.mx);
        if (--P.left == 0) pthread_cond_signal(&P.fin);
    }
    return NULL;
}
static int pool_start(int w) {   /* the caller holds P.busy */
    if (w > GM_WMAX) w = GM_WMAX;
    while (P.started < w) {
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 4 << 20);   /* glslang recurses (SPIR-V jobs) */
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        g_wgen0[P.started] = P.gen;
        int r = pthread_create(&t, &at, worker_main, (void*)(intptr_t)P.started);
        pthread_attr_destroy(&at);
        if (r) { fprintf(stderr, "[gearmass] worker thread: %s\n", strerror(r)); break; }
        P.started++;
    }
    return P.started < w ? P.started : w;
}
void gm_parallel_for(int n, gm_job_fn fn, void* ctx, int min_chunk) {
    if (n <= 0) return;
    if (min_chunk < 1) min_chunk = 1;
    int w = gm_k.workers; if (w > G.pool_max) w = G.pool_max;
    __atomic_add_fetch(&P.jobs, 1, __ATOMIC_RELAXED);
    if (w <= 0 || n < 2 * min_chunk || __atomic_exchange_n(&P.busy, 1, __ATOMIC_ACQUIRE)) {
        fn(ctx, 0, n); __atomic_add_fetch(&P.items_ser, (uint64_t)n, __ATOMIC_RELAXED); return;
    }
    w = pool_start(w);
    int parts = (w + 1) * 4, chunk = (n + parts - 1) / parts; if (chunk < min_chunk) chunk = min_chunk;
    int nchunks = (n + chunk - 1) / chunk; if (w > nchunks - 1) w = nchunks - 1;
    if (w <= 0) { __atomic_store_n(&P.busy, 0, __ATOMIC_RELEASE); fn(ctx, 0, n); __atomic_add_fetch(&P.items_ser, (uint64_t)n, __ATOMIC_RELAXED); return; }
    pthread_mutex_lock(&P.mx);
    P.fn = fn; P.ctx = ctx; P.n = n; P.chunk = chunk; P.want = w; P.left = w; P.gen++;
    __atomic_store_n(&P.next, 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&P.go);
    pthread_mutex_unlock(&P.mx);
    run_chunks();
    pthread_mutex_lock(&P.mx);
    while (P.left > 0) pthread_cond_wait(&P.fin, &P.mx);
    pthread_mutex_unlock(&P.mx);
    P.jobs_par++; P.items_par += (uint64_t)n;
    __atomic_store_n(&P.busy, 0, __ATOMIC_RELEASE);
}

void gm_report(void) {
    if (!G.mode && !G.ncpu) return;
    fprintf(stderr, "[gearmass] report: %s, worker pool %d of %d started, %llu jobs (%llu parallel: %llu items, %llu serial items)\n",
            gm_mode_name(G.mode), P.started, G.pool_max, (unsigned long long)P.jobs, (unsigned long long)P.jobs_par,
            (unsigned long long)P.items_par, (unsigned long long)P.items_ser);
}

/* ================= counters from the renderer ================= */
void gm_frame(uint64_t draws_total, double slept_s, int fps_limit) {
    __atomic_add_fetch(&G.frames, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&G.draws, draws_total, __ATOMIC_RELAXED);
    if (slept_s > 0) __atomic_add_fetch(&G.slept_ns, (uint64_t)(slept_s * 1e9), __ATOMIC_RELAXED);
    G.limit = fps_limit;
}
void gm_gpu_ns(uint64_t ns, int eof) {
    __atomic_add_fetch(&G.gpu_ns, ns, __ATOMIC_RELAXED);
    if (eof) __atomic_add_fetch(&G.gpu_frames, 1, __ATOMIC_RELAXED);
}

/* ================= thermal ================= */
#define TZ_MAX 16
static struct { int n; char temp[TZ_MAX][320]; double trip[TZ_MAX]; char type[TZ_MAX][32]; } TZ;
#if defined(__ANDROID__)
static struct { void* mgr; float (*headroom)(void*, int); int (*status)(void*); } AT;
#endif
static void thermal_init(void) {
#if defined(__ANDROID__)
    void* h = dlopen("libandroid.so", RTLD_NOW);
    void* (*acq)(void) = h ? (void* (*)(void))dlsym(h, "AThermal_acquireManager") : NULL;   /* API 30 */
    if (acq) {
        AT.headroom = (float (*)(void*, int))dlsym(h, "AThermal_getThermalHeadroom");      /* API 31 */
        AT.status = (int (*)(void*))dlsym(h, "AThermal_getCurrentThermalStatus");          /* API 30 */
        AT.mgr = acq();
    }
    if (AT.mgr && (AT.headroom || AT.status)) { G.th_src = AT.headroom ? 1 : 2; snprintf(G.th_name, sizeof G.th_name, AT.headroom ? "Android thermal headroom" : "Android thermal status"); return; }
#endif
    DIR* d = opendir("/sys/class/thermal"); struct dirent* de;
    while (d && (de = readdir(d)) && TZ.n < TZ_MAX) {
        if (strncmp(de->d_name, "thermal_zone", 12)) continue;
        char p[320], ty[32] = ""; snprintf(p, sizeof p, "/sys/class/thermal/%s/type", de->d_name);
        FILE* f = fopen(p, "r"); if (!f) continue;
        if (!fgets(ty, sizeof ty, f)) ty[0] = 0;
        fclose(f); ty[strcspn(ty, "\n")] = 0;
        static const char* const skip[] = { "batt", "bms", "charg", "usb", "wifi", "modem", "pa_", "xo", "skin", "quiet", "conn", "pmic", "sdm", "ibat", "vbat", "soc_max", NULL };
        static const char* const want[] = { "cpu", "soc", "gpu", "pkg", "tsens", "mtkts", "acpitz", "core", "big", "little", "mid", "cluster", "tz", "zen", NULL };
        int ok = 0; for (int i = 0; want[i]; i++) if (ci_has(ty, want[i])) ok = 1;
        for (int i = 0; skip[i]; i++) if (ci_has(ty, skip[i])) ok = 0;
        if (!ok) continue;
        snprintf(p, sizeof p, "/sys/class/thermal/%s/temp", de->d_name);
        long t = rd_long(p); if (t <= 0 || t > 150000) continue;
        double trip = 0, hot = 0;
        for (int k = 0; k < 12; k++) {
            char tp[320], tt[24] = ""; snprintf(tp, sizeof tp, "/sys/class/thermal/%s/trip_point_%d_type", de->d_name, k);
            FILE* g = fopen(tp, "r"); if (!g) break;
            if (!fgets(tt, sizeof tt, g)) tt[0] = 0;
            fclose(g);
            snprintf(tp, sizeof tp, "/sys/class/thermal/%s/trip_point_%d_temp", de->d_name, k);
            long v = rd_long(tp); if (v < 30000 || v > 150000) continue;
            if (!strncmp(tt, "passive", 7)) { if (!trip || v < trip) trip = (double)v; }
            else if (!strncmp(tt, "hot", 3) || !strncmp(tt, "critical", 8)) { if (!hot || v < hot) hot = (double)v; }
        }
        if (!trip) trip = hot ? hot - 10000 : 85000;
        snprintf(TZ.temp[TZ.n], sizeof TZ.temp[0], "%s", p); TZ.trip[TZ.n] = trip; snprintf(TZ.type[TZ.n], sizeof TZ.type[0], "%s", ty);
        TZ.n++;
    }
    if (d) closedir(d);
    if (!TZ.n) {   /* desktop CPUs without thermal zones (AMD): hwmon k10temp / coretemp / zenpower */
        d = opendir("/sys/class/hwmon");
        while (d && (de = readdir(d)) && TZ.n < TZ_MAX) {
            if (de->d_name[0] == '.') continue;
            char p[320], nm[32] = ""; snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", de->d_name);
            FILE* f = fopen(p, "r"); if (!f) continue;
            if (!fgets(nm, sizeof nm, f)) nm[0] = 0;
            fclose(f); nm[strcspn(nm, "\n")] = 0;
            if (strcmp(nm, "k10temp") && strcmp(nm, "coretemp") && strcmp(nm, "zenpower") && strcmp(nm, "cpu_thermal") && strcmp(nm, "amdgpu")) continue;
            snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp1_input", de->d_name);
            long t = rd_long(p); if (t <= 0 || t > 150000) continue;
            char q[320]; snprintf(q, sizeof q, "/sys/class/hwmon/%s/temp1_max", de->d_name); long mx = rd_long(q);
            if (mx < 50000 || mx > 120000) { snprintf(q, sizeof q, "/sys/class/hwmon/%s/temp1_crit", de->d_name); mx = rd_long(q); if (mx >= 60000 && mx <= 130000) mx -= 10000; else mx = 90000; }
            snprintf(TZ.temp[TZ.n], sizeof TZ.temp[0], "%s", p); TZ.trip[TZ.n] = (double)mx; snprintf(TZ.type[TZ.n], sizeof TZ.type[0], "%s", nm);
            TZ.n++;
        }
        if (d) closedir(d);
    }
    if (TZ.n) { G.th_src = 3; snprintf(G.th_name, sizeof G.th_name, "%d sensor%s (%s...)", TZ.n, TZ.n == 1 ? "" : "s", TZ.type[0]); }
    else snprintf(G.th_name, sizeof G.th_name, "none");
}
static void thermal_read(int tick, double* headroom, int* status) {
#if defined(__ANDROID__)
    if (G.th_src == 1 || G.th_src == 2) {
        if (AT.status) *status = AT.status(AT.mgr);
        if (AT.headroom && (tick & 1) == 0) {   /* at most once per second (the API returns NaN when asked more often) */
            float h = AT.headroom(AT.mgr, 5);
            if (h == h && h >= 0 && h < 10) *headroom = h;
        }
        return;
    }
#endif
    (void)tick; (void)status;
    if (G.th_src != 3) return;
    double best = -1, bt = 0, btrip = 0;
    for (int i = 0; i < TZ.n; i++) {
        long t = rd_long(TZ.temp[i]); if (t <= 0) continue;
        double r = (double)t / TZ.trip[i];
        if (r > best) { best = r; bt = (double)t; btrip = TZ.trip[i]; }
    }
    if (best >= 0) { *headroom = best; G.th_temp = bt / 1000; G.th_trip = btrip / 1000; }
}

/* ================= the monitor ================= */
#define TH_MAX 192
static struct { int tid; uint64_t run; int seen; } TH[TH_MAX]; static int TH_n;
static uint64_t thread_run_ns(int tid) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid);
    FILE* f = fopen(p, "r");
    if (f) { unsigned long long v = 0; int ok = fscanf(f, "%llu", &v) == 1; fclose(f); if (ok) return (uint64_t)v; }
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);   /* no schedstat: utime + stime in clock ticks */
    f = fopen(p, "r"); if (!f) return 0;
    char b[640]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char* q = strrchr(b, ')'); if (!q) return 0;
    unsigned long long ut = 0, st = 0; int k = 2;   /* q + 2 starts field 3 (state); utime = 14, stime = 15 */
    for (char* c = q + 1; *c; c++) if (*c == ' ') { k++; if (k == 14) ut = strtoull(c + 1, NULL, 10); else if (k == 15) { st = strtoull(c + 1, NULL, 10); break; } }
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    return (uint64_t)((ut + st) * (1000000000ull / (unsigned long long)hz));
}
static int thread_cpu(int tid) {   /* the core it last ran on (stat field 39) */
    char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    FILE* f = fopen(p, "r"); if (!f) return -1;
    char b[640]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char* q = strrchr(b, ')'); if (!q) return -1;
    int k = 2; for (char* s = q + 1; *s; s++) if (*s == ' ' && ++k == 39) return atoi(s + 1);
    return -1;
}
/* per-thread CPU shares since the previous call; total in cores; heavy[] for the three heavy tids */
static double scan_threads(double dt, const int htid[3], double hrun[3]) {
    for (int i = 0; i < TH_n; i++) TH[i].seen = 0;
    for (int i = 0; i < 3; i++) hrun[i] = -1;
    double total = 0;
    DIR* d = opendir("/proc/self/task"); if (!d) return -1;
    struct dirent* de;
    while ((de = readdir(d))) {
        char* end; long tid = strtol(de->d_name, &end, 10); if (end == de->d_name || *end || tid <= 0) continue;
        uint64_t run = thread_run_ns((int)tid);
        int i; for (i = 0; i < TH_n; i++) if (TH[i].tid == tid) break;
        if (i == TH_n) { if (TH_n >= TH_MAX) continue; TH_n++; TH[i].tid = (int)tid; TH[i].run = run; TH[i].seen = 1; continue; }
        double u = run > TH[i].run ? (double)(run - TH[i].run) / (dt * 1e9) : 0;
        TH[i].run = run; TH[i].seen = 1;
        total += u;
        for (int h = 0; h < 3; h++) if (htid[h] == tid) hrun[h] = u;
    }
    closedir(d);
    for (int i = 0; i < TH_n; ) if (!TH[i].seen) TH[i] = TH[--TH_n]; else i++;
    return total;
}
static double core_freq(int cpu, double* pct) {
    *pct = 0; if (cpu < 0) return 0;
    char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    long cur = rd_long(p); if (cur <= 0) return 0;
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    long mx = rd_long(p); if (mx > 0) *pct = 100.0 * (double)cur / (double)mx;
    return (double)cur / 1e6;
}
static GmKnobs g_applied; static int g_applied_tids[3], g_applied_ok;
static void knobs_apply(const GmKnobs* k) {
    int t[3] = { hle_heavy_tid(0), hle_heavy_tid(1), hle_heavy_tid(2) };
    int tids_changed = !g_applied_ok || memcmp(t, g_applied_tids, sizeof t);
    gm_k.ahead = k->ahead; gm_k.workers = k->workers; gm_k.spin = k->spin; gm_k.adpf_ms10 = k->adpf_ms10;
    gm_k.fps_limit = k->fps_limit; gm_k.aniso = k->aniso; gm_k.quality = k->quality; gm_k.level = k->level;
    if (G.mode != GM_OFF) {
        if (tids_changed || k->prime != g_applied.prime) hle_cpu_replace(k->prime);
        if (tids_changed || k->uclamp != g_applied.uclamp) if (k->uclamp || g_applied.uclamp) hle_cpu_uclamp(k->uclamp);
    }
    gm_k.prime = k->prime; gm_k.uclamp = k->uclamp;
    g_applied = *k; memcpy(g_applied_tids, t, sizeof t); g_applied_ok = 1;
}
static void make_hud(void) {
    char b[160]; int n = snprintf(b, sizeof b, "GM %s", gm_mode_name(G.mode));
    if (G.mode == GM_AUTO) n += snprintf(b + n, sizeof b - n, ":%s", gm_mode_name(gm_k.level));
    n += snprintf(b + n, sizeof b - n, " | %s", gm_class_name(G.cls));
    if (G.cls == GMC_CPU && G.which >= 0) n += snprintf(b + n, sizeof b - n, " %s", HEAVY_NAMES[G.which]);
    if (G.cpu_cores >= 0) n += snprintf(b + n, sizeof b - n, " | CPU %.0f/%d", G.cpu_cores, G.ncpu);
    if (G.s.gpu_busy >= 0) n += snprintf(b + n, sizeof b - n, " | GPU %.0f%%", G.s.gpu_busy * 100);
    if (G.s.headroom >= 0) n += snprintf(b + n, sizeof b - n, " | heat %.0f%%", G.s.headroom * 100);
    if (gm_k.aniso > 1) snprintf(b + n, sizeof b - n, " | AF%d", gm_k.aniso);
    pthread_mutex_lock(&G.hmx); memcpy(G.hud_s, b, sizeof b); pthread_mutex_unlock(&G.hmx);
}
int gm_hud(char* out, size_t cap) {
    if (!cap) return 0;
    pthread_mutex_lock(&G.hmx); snprintf(out, cap, "%s", G.hud_s); pthread_mutex_unlock(&G.hmx);
    return out[0] != 0;
}
static void* monitor_main(void* a) {
    (void)a;
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "me1-gm");
#endif
    hle_cpu_place(0);
    GmPolicy pol; gm_policy_init(&pol, G.mode == GM_AUTO ? GM_MAX : (G.mode <= GM_MAX ? G.mode : GM_BALANCE), G.pinned[K_QUALITY] ? G.pin.quality : 1);
    double t0 = gm_now(), tlog = t0; uint64_t f0 = G.frames, d0 = G.draws, g0 = G.gpu_ns, gf0 = G.gpu_frames, fs0 = hle_fs_read_bytes();
    uint64_t w0[3] = { hle_heavy_wait_ns(0), hle_heavy_wait_ns(1), hle_heavy_wait_ns(2) };
    double gpu_last = -1; int gpu_age = 99, tick = 0;
    double headroom = -1; int tstat = 0;
    int htid0[3] = { 0, 0, 0 }; double hr0[3]; scan_threads(1, htid0, hr0);
    /* log accumulators */
    double acc_fps = 0, acc_gpu = 0; int acc_n = 0, acc_gpu_n = 0;
    for (;;) {
        struct timespec ts = { 0, 500000000L }; nanosleep(&ts, NULL);
        double t = gm_now(), dt = t - t0; if (dt < 0.05) continue;
        tick++;
        uint64_t f = __atomic_load_n(&G.frames, __ATOMIC_RELAXED), dr = __atomic_load_n(&G.draws, __ATOMIC_RELAXED);
        uint64_t g = __atomic_load_n(&G.gpu_ns, __ATOMIC_RELAXED), gf = __atomic_load_n(&G.gpu_frames, __ATOMIC_RELAXED), fs = hle_fs_read_bytes();
        GmSample s; memset(&s, 0, sizeof s);
        s.frames = (int)(f - f0); s.fps = (double)(f - f0) / dt; s.draws = f > f0 ? (double)(dr - d0) / (double)(f - f0) : 0;
        if (gf > gf0 && g >= g0) { gpu_last = (double)(g - g0) / (dt * 1e9); if (gpu_last > 1) gpu_last = 1; G.gpu_ms = (double)(g - g0) / 1e6 / (double)(gf - gf0); gpu_age = 0; }
        else if (++gpu_age > 4) gpu_last = -1;
        s.gpu_busy = gpu_last;
        s.fs_mbps = (double)(fs - fs0) / dt / 1e6;
        int htid[3] = { hle_heavy_tid(0), hle_heavy_tid(1), hle_heavy_tid(2) };
        if (htid[2] && (htid[2] == htid[1] || htid[2] == htid[0])) htid[2] = 0;
        if (htid[1] && htid[1] == htid[0]) htid[1] = 0;
        double hrun[3]; double total = scan_threads(dt, htid, hrun);
        double waits = 0;
        for (int i = 0; i < 3; i++) {
            uint64_t w = hle_heavy_wait_ns(i); double wu = (double)(w - w0[i]) / (dt * 1e9); w0[i] = w;
            if (hrun[i] >= 0) { s.heavy[i] = hrun[i] - wu; if (s.heavy[i] < 0) s.heavy[i] = 0; waits += hrun[i] - s.heavy[i]; }
            else s.heavy[i] = -1;
        }
        G.cpu_cores = total >= 0 ? total - waits : -1;
        thermal_read(tick, &headroom, &tstat);
        s.headroom = headroom; s.thermal_status = tstat;
        s.fps_limit = G.limit;
        int which; int cls = gm_classify(&s, &which);
        G.s = s; G.cls = cls; G.which = which;
        if (G.mode == GM_AUTO) {
            gm_policy_step(&pol, &s, cls, t, G.thr);
            GmKnobs k; gm_preset(pol.level, G.pool_max, &k);
            k.ahead = pol.ahead >= 0 ? pol.ahead : k.ahead; k.aniso = pol.aniso; k.quality = pol.quality;
            apply_pins(&k);
            knobs_apply(&k);
        } else if (G.mode != GM_OFF) {
            GmKnobs k = g_applied;
            if (k.quality && !G.pinned[K_ANISO]) {   /* fixed presets keep their knobs; only the quality governor moves */
                pol.level = k.level; pol.quality = 1; gm_policy_step(&pol, &s, cls, t, G.thr); k.aniso = pol.aniso;
            }
            knobs_apply(&k);   /* also re-applies placement/uclamp when a heavy thread appears */
        }
        make_hud();
        acc_fps += s.fps; acc_n++; if (s.gpu_busy >= 0) { acc_gpu += G.gpu_ms; acc_gpu_n++; }
        if (G.log_s > 0 && t - tlog >= G.log_s && s.frames > 0) {
            char hb[200]; int hn = 0; hb[0] = 0;
            for (int i = 0; i < 3; i++) if (s.heavy[i] >= 0 && hn < (int)sizeof hb - 60) {
                double pct; int cpu = thread_cpu(htid[i]); double ghz = core_freq(cpu, &pct);
                hn += snprintf(hb + hn, sizeof hb - hn, "%s%s %.0f%%", hn ? ", " : "", HEAVY_NAMES[i], s.heavy[i] * 100);
                if (cpu >= 0) hn += snprintf(hb + hn, sizeof hb - hn, " cpu%d", cpu);
                if (ghz > 0) hn += snprintf(hb + hn, sizeof hb - hn, " %.2f GHz (%.0f%%)", ghz, pct);
            }
            char th[96] = "";
            if (s.headroom >= 0) { int k = snprintf(th, sizeof th, ", heat %.2f", s.headroom); if (G.th_src == 3) snprintf(th + k, sizeof th - k, " (%.0f C / trip %.0f C)", G.th_temp, G.th_trip); }
            if (s.thermal_status) { size_t k = strlen(th); snprintf(th + k, sizeof th - k, ", thermal status %d", s.thermal_status); }
            uint64_t jp = P.jobs_par, js = P.jobs - P.jobs_par;
            fprintf(stderr, "[gearmass] %s%s%s: %s%s%s | %.1f fps, %.0f draws/frame | CPU %.1f/%d cores: %s%s | ahead %d, workers %d/%d (jobs %llu parallel, %llu serial), spin %d, prime %d, "
                    "adpf %.1f ms, uclamp %d, fps limit %d, aniso floor %d\n",
                    gm_mode_name(G.mode), G.mode == GM_AUTO ? ":" : "", G.mode == GM_AUTO ? gm_mode_name(gm_k.level) : "", gm_class_name(cls),
                    cls == GMC_CPU && which >= 0 ? " " : "", cls == GMC_CPU && which >= 0 ? HEAVY_NAMES[which] : "",
                    acc_fps / acc_n, s.draws, G.cpu_cores, G.ncpu, hb[0] ? hb : "-", th,
                    gm_k.ahead, gm_k.workers, P.started, (unsigned long long)jp, (unsigned long long)js, gm_k.spin, gm_k.prime,
                    gm_k.adpf_ms10 / 10.0, gm_k.uclamp, gm_k.fps_limit, gm_k.aniso);
            if (acc_gpu_n) fprintf(stderr, "[gearmass]   GPU %.1f ms/frame, busy %.0f%%\n", acc_gpu / acc_gpu_n, s.gpu_busy >= 0 ? s.gpu_busy * 100 : 0.0);
            tlog = t; acc_fps = acc_gpu = 0; acc_n = acc_gpu_n = 0;
        }
        t0 = t; f0 = f; d0 = dr; g0 = g; gf0 = gf; fs0 = fs;
    }
    return NULL;
}

/* ================= init ================= */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void init_once(void) {
    topology();
    G.mode = parse_mode();
    G.pool_max = G.ncpu >= 4 ? G.ncpu - 3 : 0;
    if (G.pool_max > GM_WMAX) G.pool_max = GM_WMAX;
    pin_knobs();
    if (G.pinned[K_WORKERS] && G.pin.workers > G.pool_max) G.pool_max = G.pin.workers;   /* explicit: allowed on small machines (tests) */
    const char* e;
    G.thr = (e = getenv("ME1_GM_THERMAL")) && atof(e) > 0.3 ? atof(e) : 0.90; if (G.thr > 1.5) G.thr = 1.5;
    G.log_s = (e = getenv("ME1_GM_LOG")) ? atoi(e) : 5;
    G.hud = ((e = getenv("ME1_GM_HUD")) && *e == '1') || ((e = getenv("ME1_SHOW_FPS")) && *e == '1');
    GmKnobs k;
    if (G.mode == GM_OFF) {   /* as before GEARMASS: only the legacy variables */
        memset(&k, 0, sizeof k); k.level = GM_BALANCE; k.ahead = 1; k.aniso = 1;
        k.spin = (e = getenv("ME1_SPIN_HEAVY")) && *e == '1';
        k.prime = (e = getenv("ME1_CPU_PRIME")) && *e == '1';
        k.uclamp = (e = getenv("ME1_UCLAMP")) ? atoi(e) : 0;
        if ((e = getenv("ME1_FPS_LIMIT")) && atoi(e) > 0) { int l = atoi(e); k.fps_limit = l < 10 ? 10 : l > 240 ? 240 : l; }
        if (G.pinned[K_WORKERS]) k.workers = G.pin.workers;
    } else {
        gm_preset(G.mode == GM_AUTO ? GM_MAX : G.mode == GM_CUSTOM ? GM_BALANCE : G.mode, G.pool_max, &k);
        if (G.mode == GM_CUSTOM) k.quality = 0;
        apply_pins(&k);
    }
    gm_k.ahead = k.ahead; gm_k.workers = k.workers; gm_k.spin = k.spin; gm_k.prime = k.prime; gm_k.adpf_ms10 = k.adpf_ms10;
    gm_k.uclamp = k.uclamp; gm_k.fps_limit = k.fps_limit; gm_k.aniso = k.aniso; gm_k.quality = k.quality; gm_k.level = k.level;
    g_applied = k;
    char topo[96];
    if (G.nslow) snprintf(topo, sizeof topo, "%d cores (%d fast%s, %d slow)", G.ncpu, G.nfast, G.nprime ? ", prime tier" : "", G.nslow);
    else snprintf(topo, sizeof topo, "%d cores (symmetric)", G.ncpu);
    thermal_init();
    fprintf(stderr, "[gearmass] %s, %s, worker pool up to %d, thermal: %s (step down at %.2f); knobs: ahead %d, workers %d, spin %d, prime %d, adpf %.1f ms, "
            "uclamp %d, fps limit %d, aniso floor %d, quality %d (ME1_GEARMASS=auto|eco|balance|perf|max|custom|off, ME1_GM_*)\n",
            gm_mode_name(G.mode), topo, G.pool_max, G.th_name, G.thr, k.ahead, k.workers, k.spin, k.prime, k.adpf_ms10 / 10.0, k.uclamp, k.fps_limit,
            k.aniso, k.quality);
    if (G.mode != GM_OFF || G.hud) {
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 << 10);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, monitor_main, NULL) == 0) G.started = 1;
        pthread_attr_destroy(&at);
    }
}
void gm_init(void) { pthread_once(&g_once, init_once); }
int gm_cmd_default(void) {
    gm_init();
    if (G.mode == GM_OFF || G.mode == GM_ECO) return 0;
    return G.ncpu >= 4 ? 2 : 0;
}
