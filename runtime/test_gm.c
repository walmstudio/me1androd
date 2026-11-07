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
int hle_cpu_avail(void) { return 0; }
int hle_fs_video_open(void) { return 0; }
const char* hle_fs_docs(void) { return getenv("ME1_DOCS"); }

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
    CHECK(p.level == GM_MAX, "v1.2.2: two capped windows are not enough for a step down (level %d)", p.level);
    for (int i = 0; i < 16; i++, t += 0.5) gm_policy_step(&p, &c, GMC_CAPPED, t, 0.9);
    CHECK(p.level == GM_PERF, "capped 8 s: one step down, PERF (level %d)", p.level);
    for (int i = 0; i < 16; i++, t += 0.5) gm_policy_step(&p, &c, GMC_CAPPED, t, 0.9);
    CHECK(p.level == GM_BALANCE, "capped 16 s: BALANCE (level %d)", p.level);
    /* loading -> MAX; menus (few draws) -> BALANCE */
    GmSample l = sample(20, 50, -1, 0.5, 0.5, 0.5); l.fs_mbps = 50;
    for (int i = 0; i < 8; i++, t += 0.5) gm_policy_step(&p, &l, gm_classify(&l, NULL), t, 0.9);
    CHECK(p.level == GM_MAX, "loading: MAX (level %d)", p.level);
    GmSample m = sample(60, 30, 0.2, 0.4, 0.3, 0.5);
    for (int i = 0; i < 20; i++, t += 0.5) gm_policy_step(&p, &m, gm_classify(&m, NULL), t, 0.9);
    CHECK(p.level == GM_BALANCE, "menu (30 draws/frame): BALANCE (level %d)", p.level);
    /* ahead: gpu-bound -> 2 after 2 windows; v1.2.2: the CPU level goes to PERF (CPU clocks do not help the GPU) */
    GmSample g = sample(40, 800, 0.95, 0.5, 0.5, 0.5);
    for (int i = 0; i < 10; i++, t += 0.5) gm_policy_step(&p, &g, GMC_GPU, t, 0.9);
    CHECK(p.ahead == 2 && p.level == GM_PERF, "gpu-bound: ahead 2, PERF (ahead %d level %d)", p.ahead, p.level);
    GmSample cpu = sample(40, 800, 0.5, 0.95, 0.5, 0.5);
    for (int i = 0; i < 4; i++, t += 0.5) gm_policy_step(&p, &cpu, GMC_CPU, t, 0.9);
    CHECK(p.ahead == 2, "cpu-bound at MAX: preset ahead 2 (ahead %d)", p.ahead);
    /* v1.2.2: the swings of the v1.2.1 log (30 fps cap; BALANCE <-> MAX every ~20 s) */
    {
        GmPolicy w; gm_policy_init(&w, GM_MAX, 0); double tw = 0; int ups = 0, last = w.level;
        GmSample cap = sample(30, 150, 0.7, 0.55, 0.35, 0.4); cap.fps_limit = 30;
        GmSample dip = sample(26, 150, 0.7, 0.55, 0.35, 0.4); dip.fps_limit = 30;    /* one slow window (a hitch) */
        GmSample slow = sample(24, 150, 0.6, 0.6, 0.4, 0.4); slow.fps_limit = 30;    /* really below the cap */
        for (int i = 0; i < 40; i++, tw += 0.5) gm_policy_step(&w, &cap, gm_classify(&cap, NULL), tw, 0.9);
        CHECK(w.level == GM_BALANCE, "swing: capped 20 s -> BALANCE (level %d)", w.level);
        gm_policy_step(&w, &dip, gm_classify(&dip, NULL), tw, 0.9); tw += 0.5;
        gm_policy_step(&w, &dip, gm_classify(&dip, NULL), tw, 0.9); tw += 0.5;
        CHECK(w.level == GM_BALANCE, "swing: two 26 fps windows (smoothed fps holds the cap): no step up (level %d)", w.level);
        for (int i = 0; i < 4; i++, tw += 0.5) gm_policy_step(&w, &cap, gm_classify(&cap, NULL), tw, 0.9);
        for (int i = 0; i < 6; i++, tw += 0.5) gm_policy_step(&w, &slow, gm_classify(&slow, NULL), tw, 0.9);
        CHECK(w.level == GM_MAX && w.fails == 1, "swing: 24 fps right after the step down -> MAX, one failed step (level %d, fails %d)", w.level, w.fails);
        for (int i = 0; i < 100; i++, tw += 0.5) {   /* 50 s capped: down to PERF at most, never BALANCE (floor PERF) */
            gm_policy_step(&w, &cap, gm_classify(&cap, NULL), tw, 0.9);
            if (w.level > last) ups++;
            last = w.level;
            if (w.level < GM_PERF) break;
        }
        CHECK(w.level == GM_PERF && !ups, "swing: 50 s capped after a failure: held at PERF (level %d, ups %d)", w.level, ups);
        for (int i = 0; i < 40; i++, tw += 0.5) gm_policy_step(&w, &cap, gm_classify(&cap, NULL), tw, 0.9);
        CHECK(w.level == GM_BALANCE, "swing: after the 60 s hold BALANCE is tried again (level %d)", w.level);
        for (int i = 0; i < 6; i++, tw += 0.5) gm_policy_step(&w, &slow, gm_classify(&slow, NULL), tw, 0.9);
        CHECK(w.fails == 2 && w.hold >= 119 && w.floor == GM_PERF, "swing: second failure doubles the hold (fails %d, hold %.0f, floor %d)", w.fails, w.hold, w.floor);
        int swings = 0; last = w.level;
        for (int i = 0; i < 2 * 600; i++, tw += 0.5) {   /* 10 min of "capped, a slow scene every 20 s": few level changes */
            GmSample* x = (i % 40) < 6 ? &slow : &cap;
            gm_policy_step(&w, x, gm_classify(x, NULL), tw, 0.9);
            if (w.level != last) swings++;
            last = w.level;
        }
        fprintf(stderr, "test_gm: swing scenario: %d level changes in 10 min, %d failed steps, hold %.0f s\n", swings, w.fails, w.hold);
        CHECK(swings <= 30, "swing: 10 min with a slow scene every 20 s: %d level changes (v1.2.1: ~60)", swings);
        int w2; GmSample v = sample(30, 2, 0.1, 0.7, 0.3, 0.4); v.video = 1;
        CHECK(gm_classify(&v, &w2) == GMC_VIDEO, "Bink movie open, 2 draws/frame: video");
        v.video = 0; CHECK(gm_classify(&v, &w2) != GMC_VIDEO, "no movie: not video");
        GmPolicy vp; gm_policy_init(&vp, GM_MAX, 0); double tv = 0; v.video = 1;
        for (int i = 0; i < 20; i++, tv += 0.5) gm_policy_step(&vp, &v, GMC_VIDEO, tv, 0.9);
        CHECK(vp.level == GM_BALANCE, "video at 30 fps: BALANCE (level %d)", vp.level);
        v.fps = 20; v.frames = 10;
        for (int i = 0; i < 6; i++, tv += 0.5) gm_policy_step(&vp, &v, GMC_VIDEO, tv, 0.9);
        CHECK(vp.level == GM_MAX && vp.fails == 0, "video at 20 fps: MAX, not a failed step (level %d, fails %d)", vp.level, vp.fails);
    }
    /* A loading scene can raise the level without resolving an earlier capped step.
     * If heat then caps the level and an idle scene lowers it, a failed-step floor
     * must not push a subsequent CPU scene above the thermal cap. */
    {
        GmPolicy h; gm_policy_init(&h, GM_MAX, 0); double th = 0;
        GmSample capped = sample(30, 800, 0.4, 0.5, 0.5, 0.4); capped.fps_limit = 30;
        for (int i = 0; i < 15; i++, th += 0.5) gm_policy_step(&h, &capped, GMC_CAPPED, th, 0.9);
        CHECK(h.level == GM_PERF && h.down_from == GM_MAX, "thermal/floor setup: capped step down");
        GmSample loading = sample(20, 100, 0.4, 0.5, 0.5, 0.4);
        for (int i = 0; i < 6; i++, th += 0.5) gm_policy_step(&h, &loading, GMC_LOADING, th, 0.9);
        CHECK(h.level == GM_MAX, "thermal/floor setup: loading increases level");
        GmSample hot = sample(20, 800, 0.4, 0.95, 0.5, 0.92);
        gm_policy_step(&h, &hot, GMC_CPU, th, 0.9); th += 0.5;
        CHECK(h.level == GM_PERF && h.cap == GM_PERF, "thermal/floor setup: hot -> PERF cap");
        GmSample quiet = sample(0, 0, -1, -1, -1, 0.85); quiet.frames = 0;
        for (int i = 0; i < 15; i++, th += 0.5) gm_policy_step(&h, &quiet, GMC_IDLE, th, 0.9);
        CHECK(h.level == GM_BALANCE && h.cap == GM_PERF, "thermal/floor setup: idle -> BALANCE");
        GmSample cpuhot = sample(20, 800, 0.4, 0.95, 0.5, 0.85);
        int violations = 0;
        for (int i = 0; i < 8; i++, th += 0.5) { gm_policy_step(&h, &cpuhot, GMC_CPU, th, 0.9); if (h.level > h.cap) violations++; }
        CHECK(!violations, "failed-step floor must not override thermal cap: %d violations", violations);
        CHECK(h.fails == 0, "idle/loading/thermal interrupts must not be counted as FPS failure");
    }
    /* Unknown thermal data must not be mistaken for a cool device. */
    {
        GmPolicy h; gm_policy_init(&h, GM_MAX, 1); double th = 0;
        GmSample x = sample(20, 800, 0.3, 0.95, 0.5, 0.4); x.thermal_status = 4;
        gm_policy_step(&h, &x, GMC_CPU, th, 0.9); th += 0.5;
        CHECK(h.cap == GM_ECO && h.level == GM_ECO, "critical thermal cap active");
        x.thermal_status = -1; x.headroom = -1;
        for (int i = 0; i < 160; i++, th += 0.5) gm_policy_step(&h, &x, GMC_CPU, th, 0.9);
        CHECK(h.cap == GM_ECO && h.level == GM_ECO, "80s unknown thermal data must retain existing cap");
        x.headroom = 0.5;
        for (int i = 0; i < 58; i++, th += 0.5) gm_policy_step(&h, &x, GMC_CPU, th, 0.9);
        CHECK(h.cap == GM_ECO, "29s confirmed cool data must retain cap");
        for (int i = 0; i < 6; i++, th += 0.5) gm_policy_step(&h, &x, GMC_CPU, th, 0.9);
        CHECK(h.cap == GM_BALANCE && h.level == GM_BALANCE, "30s confirmed cool data permits one step");
        x.headroom = -1; x.thermal_status = 0;
        for (int i = 0; i < 64; i++, th += 0.5) gm_policy_step(&h, &x, GMC_CPU, th, 0.9);
        CHECK(h.cap == GM_PERF, "valid Android status alone permits recovery");
    }
    /* One missing sensor must not be interpreted as complete cooling. */
    {
        GmPolicy h; gm_policy_init(&h, GM_MAX, 1); double th = 0;
        GmSample x = sample(20, 800, 0.3, 0.95, 0.5, 1.05); x.thermal_status = -1;
        gm_policy_step(&h, &x, GMC_CPU, th, 0.9); th += 0.5;
        CHECK(h.cap == GM_BALANCE, "partial: thermal cap initially active");
        x.headroom = 0.5; x.thermal_status = GM_THERMAL_PARTIAL;
        for (int i = 0; i < 160; i++, th += 0.5) gm_policy_step(&h, &x, GMC_CPU, th, 0.9);
        CHECK(h.cap == GM_BALANCE && h.level <= h.cap, "partial: missing sensor must prevent thermal recovery");
        x.thermal_status = -1;
        for (int i = 0; i < 58; i++, th += 0.5) gm_policy_step(&h, &x, GMC_CPU, th, 0.9);
        CHECK(h.cap == GM_BALANCE, "partial: recovery needs 30s complete cool telemetry");
        for (int i = 0; i < 6; i++, th += 0.5) gm_policy_step(&h, &x, GMC_CPU, th, 0.9);
        CHECK(h.cap == GM_PERF, "partial: recovery resumes with complete cool telemetry");
        GmPolicy q; gm_policy_init(&q, GM_MAX, 1); x.thermal_status = GM_THERMAL_PARTIAL;
        for (int i = 0; i < 40; i++) gm_policy_step(&q, &x, GMC_CPU, i * 0.5, 0.9);
        CHECK(q.aniso == 1, "partial: missing sensor prevents aniso increase");
        x.headroom = 1.05;
        gm_policy_step(&q, &x, GMC_CPU, 21, 0.9);
        CHECK(q.cap == GM_BALANCE, "partial: known hot sensor still enforces cap");
    }
    /* Reproducible stress of the thermal cap invariant across mixed scenes. */
    {
        unsigned rng = 0x9e3779b9u; int bad = 0;
        for (int trial = 0; trial < 100; trial++) {
            GmPolicy z; gm_policy_init(&z, GM_MAX, 1);
            for (int i = 0; i < 2000; i++) {
                rng = rng * 1664525u + 1013904223u; unsigned r = rng;
                GmSample x = sample((r % 40) + 1, 100 + (r % 1000), ((r >> 8) % 101) / 100.0, ((r >> 16) % 101) / 100.0, 0.5, 0.3 + ((r >> 24) % 80) / 100.0);
                x.fps_limit = 30;
                if ((r & 31) == 0) { x.headroom = -1; x.thermal_status = -1; }
                else if ((r & 127) == 1) x.thermal_status = 4;
                int cls = (r >> 12) % 7;
                gm_policy_step(&z, &x, cls, i * 0.5, 0.9);
                if (z.level < GM_ECO || z.level > z.cap || z.cap > GM_MAX) bad++;
            }
        }
        CHECK(!bad, "thermal cap invariant over 200000 mixed synthetic samples: %d violations", bad);
    }
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

static void test_session_stats(void) {
    char dir[] = "/tmp/me1-gm-stats-XXXXXX", path[256], buf[4096];
    CHECK(mkdtemp(dir) != NULL, "temporary stats directory");
    snprintf(path, sizeof path, "%s/me1_gearmass_last.txt", dir);
    setenv("ME1_DOCS", dir, 1); setenv("ME1_GM_RENDER_RES", "1280x720", 1);
    gm_session_save(); FILE* f = fopen(path, "r"); size_t n = f ? fread(buf, 1, sizeof buf-1, f) : 0; if (f) fclose(f); buf[n] = 0;
    CHECK(strstr(buf, "run_id=") && strstr(buf, "active_seconds=") && !strstr(buf, "resolution="), "no resolution until backbuffer observed");
    gm_frame(0, 0, 30); gm_frame_size(1280, 720); gm_session_save(); f = fopen(path, "r"); n = f ? fread(buf, 1, sizeof buf-1, f) : 0; if (f) fclose(f); buf[n] = 0;
    CHECK(strstr(buf, "resolution=1280x720") != NULL, "matching backbuffer recorded");
    CHECK(strstr(buf, "fps_limit_mixed=0") != NULL, "stable FPS cap recorded");
    CHECK(strstr(buf, "active_heat_known=") != NULL, "active thermal coverage recorded");
    gm_frame(0, 0, 45); gm_frame(0, 0, 30); gm_session_save(); f = fopen(path, "r"); n = f ? fread(buf, 1, sizeof buf-1, f) : 0; if (f) fclose(f); buf[n] = 0;
    CHECK(strstr(buf, "fps_limit_mixed=1") != NULL, "FPS cap change remains recorded even if changed back");
    gm_frame_size(640, 360); gm_session_save(); f = fopen(path, "r"); n = f ? fread(buf, 1, sizeof buf-1, f) : 0; if (f) fclose(f); buf[n] = 0;
    CHECK(strstr(buf, "resolution=") == NULL, "mixed backbuffers cannot drive resolution adaptation");
    f = fopen(path, "w"); if (f) { fputs("sentinel", f); fclose(f); }
    setenv("ME1_GM_RES_SKIP", "1", 1); gm_session_save();
    f = fopen(path, "r"); n = f ? fread(buf, 1, sizeof buf-1, f) : 0; if (f) fclose(f); buf[n] = 0;
    CHECK(!strcmp(buf, "sentinel"), "headless test preserves last real session");
    unsetenv("ME1_GM_RES_SKIP"); unsetenv("ME1_DOCS"); unsetenv("ME1_GM_RENDER_RES"); unlink(path); rmdir(dir);
}
int main(void) {
    test_env();
    setenv("ME1_GM_WORKERS", "8", 1); setenv("ME1_GM_LOG", "0", 1); setenv("ME1_GEARMASS", "custom", 1);
    gm_init();
    CHECK(gm_pool_max() == 8, "explicit ME1_GM_WORKERS=8 allows 8 workers (pool %d)", gm_pool_max());
    test_pool();
    test_policy();
    test_session_stats();
    char hud[160]; gm_hud(hud, sizeof hud);
    printf("test_gm: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail != 0;
}
