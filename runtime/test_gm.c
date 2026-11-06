/* Unit test of GEARMASS (v1.2.1, runtime/gearmass.c): the worker pool (every index exactly once for 0..8 workers,
 * nested and concurrent calls fall back to serial), the presets, the scene classification, the "auto" policy
 * (thermal step-down and recovery, hysteresis, ahead, quality governor) and the ME1_GEARMASS / ME1_GM_* / me_1.2
 * variables. Links gearmass.c alone with stubs for the runtime.   make build/test_gm && ./build/test_gm */
#define _GNU_SOURCE
#include "gearmass.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* runtime stubs */
int hle_heavy_tid(int which) { (void)which; return 0; }
uint64_t hle_heavy_wait_ns(int which) { (void)which; return 0; }
void hle_cpu_replace(int prime) { (void)prime; }
void hle_cpu_uclamp(int v) { (void)v; }
void hle_cpu_place(int heavy) { (void)heavy; }
uint64_t hle_fs_read_bytes(void) { return 0; }

static int g_ok, g_fail;
#define CHECK(c, ...) do { if (c) g_ok++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- pool ---- */
typedef struct { int* hits; int n; int nested; } Ctx;
static void job(void* c, int i0, int i1) {
    Ctx* x = c;
    for (int i = i0; i < i1; i++) x->hits[i]++;
    if (x->nested && i0 == 0) { int h[64] = { 0 }; Ctx y = { h, 64, 0 }; gm_parallel_for(64, job, &y, 1); for (int i = 0; i < 64; i++) if (h[i] != 1) x->nested = -1; }
}
static void* conc(void* a) {
    int* r = a; int* h = calloc(10000, sizeof *h); Ctx x = { h, 10000, 0 };
    for (int k = 0; k < 200; k++) { memset(h, 0, 10000 * sizeof *h); gm_parallel_for(10000, job, &x, 16); for (int i = 0; i < 10000; i++) if (h[i] != 1) { *r = 1; break; } }
    free(h); return NULL;
}
static void test_pool(void) {
    for (int w = 0; w <= 8; w++) {
        gm_k.workers = w;
        static const int ns[] = { 1, 2, 7, 64, 1000, 4097 }, mc[] = { 1, 3, 100 };
        for (unsigned a = 0; a < sizeof ns / sizeof ns[0]; a++) for (unsigned b = 0; b < 3; b++) {
            int n = ns[a]; int* h = calloc((size_t)n, sizeof *h); Ctx x = { h, n, 0 };
            for (int rep = 0; rep < 20; rep++) gm_parallel_for(n, job, &x, mc[b]);
            int bad = 0; for (int i = 0; i < n; i++) if (h[i] != 20) bad++;
            CHECK(!bad, "workers %d, n %d, min_chunk %d: %d indices not hit exactly 20 times", w, n, mc[b], bad);
            free(h);
        }
        int h[256] = { 0 }; Ctx x = { h, 256, 1 }; gm_parallel_for(256, job, &x, 8);
        int bad = 0; for (int i = 0; i < 256; i++) if (h[i] != 1) bad++;
        CHECK(!bad && x.nested == 1, "workers %d: nested call (bad %d, nested %d)", w, bad, x.nested);
    }
    gm_k.workers = 4;
    pthread_t t[3]; int r[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) pthread_create(&t[i], NULL, conc, &r[i]);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
    CHECK(!r[0] && !r[1] && !r[2], "3 threads calling at once");
}

/* ---- presets / classification / policy ---- */
static GmSample sample(double fps, double draws, double gpu, double main_, double render, double heat) {
    GmSample s; memset(&s, 0, sizeof s);
    s.fps = fps; s.draws = draws; s.gpu_busy = gpu; s.heavy[0] = main_; s.heavy[1] = render; s.heavy[2] = -1; s.headroom = heat;
    s.frames = (int)(fps / 2 + 0.5); return s;
}
static void test_policy(void) {
    GmKnobs k;
    gm_preset(GM_ECO, 5, &k);     CHECK(k.workers == 0 && k.ahead == 0 && k.fps_limit == 30 && k.spin == 0 && k.uclamp == 0, "eco preset");
    gm_preset(GM_BALANCE, 5, &k); CHECK(k.workers == 2 && k.ahead == 1 && k.fps_limit == 0 && k.spin == 1 && !k.quality, "balance preset");
    gm_preset(GM_BALANCE, 1, &k); CHECK(k.workers == 1, "balance on a 4-core CPU: 1 worker");
    gm_preset(GM_PERF, 5, &k);    CHECK(k.workers == 5 && k.prime == 1 && k.uclamp == 512 && k.adpf_ms10 == 80 && k.quality, "perf preset");
    gm_preset(GM_MAX, 8, &k);     CHECK(k.workers == 8 && k.ahead == 2 && k.spin == 2 && k.uclamp == 1024, "max preset");
    int w;
    GmSample s = sample(40, 800, 0.5, 0.95, 0.6, -1); CHECK(gm_classify(&s, &w) == GMC_CPU && w == 0, "cpu-bound main");
    s = sample(40, 800, 0.95, 0.5, 0.82, -1);         CHECK(gm_classify(&s, &w) == GMC_GPU, "gpu-bound (render 82 %% < 90 %%)");
    s = sample(40, 800, 0.5, 0.5, 0.82, -1);          CHECK(gm_classify(&s, &w) == GMC_CPU && w == 1, "cpu-bound render");
    s = sample(40, 800, 0.5, 0.5, 0.5, -1);           CHECK(gm_classify(&s, &w) == GMC_WAIT, "wait-bound");
    s = sample(30, 800, 0.3, 0.5, 0.5, -1); s.fps_limit = 30; CHECK(gm_classify(&s, &w) == GMC_CAPPED, "capped");
    s = sample(40, 800, 0.5, 0.5, 0.5, -1); s.fs_mbps = 40;   CHECK(gm_classify(&s, &w) == GMC_LOADING, "loading (file reads)");
    s = sample(0, 0, -1, 0.9, 0.1, -1); s.frames = 0;         CHECK(gm_classify(&s, &w) == GMC_IDLE, "no frames, no reads: idle");

    GmPolicy p; gm_policy_init(&p, GM_MAX, 1); double t = 0;
    s = sample(40, 800, 0.5, 0.95, 0.6, 0.5);
    for (int i = 0; i < 10; i++, t += 0.5) gm_policy_step(&p, &s, gm_classify(&s, NULL), t, 0.9);
    CHECK(p.level == GM_MAX && p.cap == GM_MAX, "cool gameplay stays at MAX (level %d cap %d)", p.level, p.cap);
    s.headroom = 0.92; gm_policy_step(&p, &s, GMC_CPU, t, 0.9); t += 0.5;
    CHECK(p.level == GM_PERF, "headroom 0.92 >= 0.90: one step down at once (level %d)", p.level);
    for (int i = 0; i < 4; i++, t += 0.5) gm_policy_step(&p, &s, GMC_CPU, t, 0.9);
    CHECK(p.level == GM_PERF, "no second step within 10 s (level %d)", p.level);
    for (int i = 0; i < 20; i++, t += 0.5) gm_policy_step(&p, &s, GMC_CPU, t, 0.9);
    CHECK(p.level == GM_BALANCE, "still hot after 10 s: BALANCE (level %d)", p.level);
    s.headroom = 1.05; for (int i = 0; i < 4; i++, t += 0.5) gm_policy_step(&p, &s, GMC_CPU, t, 0.9);
    CHECK(p.level == GM_BALANCE, "throttling: never below BALANCE without a critical status (level %d)", p.level);
    s.thermal_status = 4; gm_policy_step(&p, &s, GMC_CPU, t, 0.9); t += 0.5;
    CHECK(p.level == GM_ECO, "critical thermal status: ECO (level %d)", p.level);
    s.thermal_status = 0; s.headroom = 0.6;
    for (int i = 0; i < 50; i++, t += 0.5) gm_policy_step(&p, &s, GMC_CPU, t, 0.9);
    CHECK(p.level == GM_ECO, "25 s cool after a critical status: still ECO (level %d)", p.level);
    for (int i = 0; i < 20; i++, t += 0.5) gm_policy_step(&p, &s, GMC_CPU, t, 0.9);
    CHECK(p.level == GM_BALANCE, "35 s cool: one step back (level %d)", p.level);
    for (int i = 0; i < 200; i++, t += 0.5) gm_policy_step(&p, &s, GMC_CPU, t, 0.9);
    CHECK(p.level == GM_MAX, "long cool period: back to MAX (level %d)", p.level);
    /* hysteresis: a capped scene needs 2 windows and 3 s */
    GmSample c = sample(30, 800, 0.3, 0.5, 0.5, 0.5); c.fps_limit = 30;
    double tc = t; gm_policy_step(&p, &c, GMC_CAPPED, t, 0.9); t += 0.5;
    CHECK(p.level == GM_MAX, "one capped window: no change");
    gm_policy_step(&p, &c, GMC_CAPPED, t, 0.9); t += 0.5;
    CHECK(p.level == GM_BALANCE || t - tc < 3.0, "two capped windows (level %d)", p.level);
    for (int i = 0; i < 8; i++, t += 0.5) gm_policy_step(&p, &c, GMC_CAPPED, t, 0.9);
    CHECK(p.level == GM_BALANCE, "capped: BALANCE (level %d)", p.level);
    /* loading -> MAX; menus (few draws) -> BALANCE */
    GmSample l = sample(20, 50, -1, 0.5, 0.5, 0.5); l.fs_mbps = 50;
    for (int i = 0; i < 8; i++, t += 0.5) gm_policy_step(&p, &l, gm_classify(&l, NULL), t, 0.9);
    CHECK(p.level == GM_MAX, "loading: MAX (level %d)", p.level);
    GmSample m = sample(60, 30, 0.2, 0.4, 0.3, 0.5);
    for (int i = 0; i < 10; i++, t += 0.5) gm_policy_step(&p, &m, gm_classify(&m, NULL), t, 0.9);
    CHECK(p.level == GM_BALANCE, "menu (30 draws/frame): BALANCE (level %d)", p.level);
    /* ahead: gpu-bound -> 2 after 2 windows */
    GmSample g = sample(40, 800, 0.95, 0.5, 0.5, 0.5);
    for (int i = 0; i < 10; i++, t += 0.5) gm_policy_step(&p, &g, GMC_GPU, t, 0.9);
    CHECK(p.ahead == 2 && p.level == GM_MAX, "gpu-bound: ahead 2 (ahead %d level %d)", p.ahead, p.level);
    GmSample cpu = sample(40, 800, 0.5, 0.95, 0.5, 0.5);
    for (int i = 0; i < 4; i++, t += 0.5) gm_policy_step(&p, &cpu, GMC_CPU, t, 0.9);
    CHECK(p.ahead == 2, "cpu-bound at MAX: preset ahead 2 (ahead %d)", p.ahead);
    /* quality governor: GPU idle -> 2, 4, 8, 16; GPU busy -> back down */
    GmPolicy q; gm_policy_init(&q, GM_MAX, 1); t = 0;
    GmSample idle = sample(40, 800, 0.3, 0.95, 0.5, 0.5);
    for (int i = 0; i < 40; i++, t += 0.5) gm_policy_step(&q, &idle, GMC_CPU, t, 0.9);
    CHECK(q.aniso == 16, "GPU 30 %%: anisotropy floor 16 (got %d)", q.aniso);
    GmSample busy = sample(40, 800, 0.95, 0.5, 0.5, 0.5);
    for (int i = 0; i < 16; i++, t += 0.5) gm_policy_step(&q, &busy, GMC_GPU, t, 0.9);
    CHECK(q.aniso <= 2, "GPU 95 %%: anisotropy floor back down (got %d)", q.aniso);
    GmSample unk = sample(40, 800, -1, 0.95, 0.5, 0.5); int a0 = q.aniso;
    for (int i = 0; i < 8; i++, t += 0.5) gm_policy_step(&q, &unk, GMC_CPU, t, 0.9);
    CHECK(q.aniso == a0, "no GPU timer: anisotropy left alone");
}

/* ---- variables (each case in a child process: gm_init runs once per process) ---- */
static int child(const char* const* env, int want_mode, int want_workers, int want_ahead, int want_fps, int want_cmd) {
    fflush(stdout); fflush(stderr);
    pid_t p = fork();
    if (p == 0) {
        clearenv(); setenv("ME1_GM_LOG", "0", 1);
        for (int i = 0; env[i]; i += 2) setenv(env[i], env[i + 1], 1);
        if (!freopen("/dev/null", "w", stderr)) {}
        gm_init();
        int bad = 0;
        if (gm_mode() != want_mode) bad |= 1;
        if (want_workers >= 0 && gm_k.workers != want_workers) bad |= 2;
        if (want_ahead >= 0 && gm_k.ahead != want_ahead) bad |= 4;
        if (want_fps >= 0 && gm_k.fps_limit != want_fps) bad |= 8;
        if (want_cmd >= 0 && gm_cmd_default() != want_cmd) bad |= 16;
        _exit(bad);
    }
    int st = 0; waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}
static void test_env(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN); int cmd = n >= 4 ? 2 : 0;
    { const char* e[] = { NULL }; int r = child(e, GM_AUTO, -1, 2, 0, cmd); CHECK(!r, "default: auto, ahead 2 (MAX at start), cmd %d (bits %d)", cmd, r); }
    { const char* e[] = { "ME1_GEARMASS", "eco", NULL }; int r = child(e, GM_ECO, 0, 0, 30, 0); CHECK(!r, "eco: no workers, fps 30, cmd 0 (bits %d)", r); }
    { const char* e[] = { "ME1_GEARMASS", "eco", "ME1_FPS_LIMIT", "45", NULL }; int r = child(e, GM_ECO, 0, 0, 45, 0); CHECK(!r, "eco + ME1_FPS_LIMIT=45 (bits %d)", r); }
    { const char* e[] = { "ME1_GEARMASS", "balance", "ME1_GM_WORKERS", "3", NULL }; int r = child(e, GM_BALANCE, 3, 1, 0, cmd); CHECK(!r, "balance + ME1_GM_WORKERS=3 (bits %d)", r); }
    { const char* e[] = { "ME1_GEARMASS", "PERF", "ME1_GM_AHEAD", "0", NULL }; int r = child(e, GM_PERF, -1, 0, 0, cmd); CHECK(!r, "perf + ahead 0 (bits %d)", r); }
    { const char* e[] = { "ME1_GEARMASS", "maximum", NULL }; int r = child(e, GM_MAX, -1, 2, 0, cmd); CHECK(!r, "maximum (bits %d)", r); }
    { const char* e[] = { "ME1_GEARMASS", "off", NULL }; int r = child(e, GM_OFF, 0, 1, 0, 0); CHECK(!r, "off: as before (bits %d)", r); }
    { const char* e[] = { "ME1_GEARMASS", "custom", "ME1_GM_WORKERS", "2", "ME1_GM_AHEAD", "2", "ME1_GM_FPS", "50", NULL }; int r = child(e, GM_CUSTOM, 2, 2, 50, cmd); CHECK(!r, "custom sliders (bits %d)", r); }
    { const char* e[] = { "ME1_CMD_SCHED", "2", NULL }; int r = child(e, GM_ECO, 0, -1, 30, 0); CHECK(!r, "me_1.2 ME1_CMD_SCHED=2 = eco (bits %d)", r); }
    { const char* e[] = { "ME1_CMD_SCHED", "0", "ME1_CMD_THREADS", "4", NULL }; int r = child(e, GM_CUSTOM, 4, -1, -1, -1); CHECK(!r, "me_1.2 ME1_CMD_SCHED=0 + ME1_CMD_THREADS=4 (bits %d)", r); }
    { const char* e[] = { "ME1_CMD_SCHED", "1", NULL }; int r = child(e, GM_AUTO, -1, -1, -1, -1); CHECK(!r, "me_1.2 ME1_CMD_SCHED=1 = auto (bits %d)", r); }
    { const char* e[] = { "ME1_GEARMASS", "nonsense", NULL }; int r = child(e, GM_AUTO, -1, -1, -1, -1); CHECK(!r, "unknown value: auto (bits %d)", r); }
}

int main(void) {
    test_env();
    setenv("ME1_GM_WORKERS", "8", 1); setenv("ME1_GM_LOG", "0", 1); setenv("ME1_GEARMASS", "custom", 1);
    gm_init();
    CHECK(gm_pool_max() == 8, "explicit ME1_GM_WORKERS=8 allows 8 workers (pool %d)", gm_pool_max());
    test_pool();
    test_policy();
    char hud[160]; gm_hud(hud, sizeof hud);
    printf("test_gm: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail != 0;
}
