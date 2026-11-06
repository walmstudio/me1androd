#ifndef GEARMASS_H
#define GEARMASS_H
/* gearmass.h - GEARMASS, the load governor of the port (v1.2.1).
 *
 * One switch, ME1_GEARMASS=auto|eco|balance|perf|max|custom|off (launcher: "Нагрузка (GEARMASS)"), decides how hard
 * the CPU and the GPU work: how far the D3D9 replay thread may run behind the game (ahead), how many worker threads
 * take parallel jobs (texture decoding, SPIR-V compilation), active waits of the heavy threads (spin), the prime core,
 * the ADPF target and the utilization floor (uclamp), the frame limit and the anisotropy floor. Nothing is tied to a
 * device: presets are scaled by the core topology read at run time (core count, frequency tiers), and "auto" steers
 * by measurements - per-thread CPU time (/proc/self/task), GPU time per frame (timer queries), draws per frame, file
 * reads (loading screens), the thermal headroom (Android thermal API, Linux /sys/class/thermal).
 * Every knob has its own ME1_GM_* variable; an explicit value pins that knob in every mode (see gearmass.c).
 * Log prefix: [gearmass]. */
#include <stdint.h>
#include <stddef.h>

enum { GM_ECO = 0, GM_BALANCE = 1, GM_PERF = 2, GM_MAX = 3, GM_CUSTOM = 4, GM_AUTO = 5, GM_OFF = 6 };

typedef struct {
    int level;      /* the applied preset level GM_ECO..GM_MAX */
    int ahead;      /* frames the D3D9 replay thread may run behind the game (0..2; ME1_D3D_CMD=2 only) */
    int workers;    /* active threads of the worker pool (0 = parallel jobs run serially on the caller) */
    int spin;       /* heavy threads: 0 = as before, 1 = Sleep(0) only yields, 2 = + short spins instead of blocking */
    int prime;      /* 1 = the UE3 game thread alone on the prime core(s) when the CPU has a separate prime tier */
    int adpf_ms10;  /* ADPF target work duration, 0.1 ms units (0 = from the frame limit, else 60 fps) */
    int uclamp;     /* utilization floor of the heavy threads, 0..1024 (0 = none) */
    int fps_limit;  /* frame limit, 0 = off */
    int aniso;      /* anisotropy floor of mipmapped linear samplers: 1 = the game's setting .. 16 */
    int quality;    /* quality governor: raises the anisotropy floor while the GPU has headroom */
} GmKnobs;
extern volatile GmKnobs gm_k;

void gm_init(void);                     /* once (d3d_init): reads ME1_GEARMASS / ME1_GM_*, starts the monitor thread */
int  gm_mode(void);                     /* the selected mode GM_ECO..GM_OFF */
const char* gm_mode_name(int mode);
int  gm_cmd_default(void);              /* default of ME1_D3D_CMD for this machine and mode */
void gm_frame(uint64_t draws_total, double slept_s, int fps_limit);   /* every Present, the game's rendering thread */
void gm_gpu_ns(uint64_t ns, int eof);   /* renderer: GPU time of a finished frame / submission (eof = last of a frame) */
int  gm_hud(char* out, size_t cap);     /* one-line indicator "GM AUTO:MAX | cpu-bound main | CPU 5/8 | GPU 61% | heat 62%" */
int  gm_pool_max(void);                 /* worker threads the pool may start */
void gm_report(void);                   /* totals at exit (hle_d3d9_report) */

/* fork-join: fn(ctx, i0, i1) over disjoint ranges of [0, n), the caller takes part; returns when all are done.
 * Runs serially on the caller when there are no active workers, n < 2 * min_chunk or another job is running:
 * the result never depends on the number of workers (outputs must be disjoint). */
typedef void (*gm_job_fn)(void* ctx, int i0, int i1);
void gm_parallel_for(int n, gm_job_fn fn, void* ctx, int min_chunk);

/* ---- the pure parts of the policy (build/test_gm) ---- */
typedef struct {
    double fps, draws;        /* frames/s, draws per frame */
    double gpu_busy;          /* GPU time / wall time, < 0 = unknown */
    double heavy[3];          /* CPU share (minus active waits) of main, render, D3D9 replay; < 0 = no such thread */
    double fs_mbps;           /* game file reads, MB/s */
    double headroom;          /* thermal headroom (1.0 = the system throttles), < 0 = unknown */
    int thermal_status;       /* Android AThermal status (0 none .. 6 shutdown), 0 if unknown */
    int fps_limit, frames;    /* the active frame limit; frames in the window */
} GmSample;
enum { GMC_IDLE, GMC_LOADING, GMC_CAPPED, GMC_GPU, GMC_CPU, GMC_WAIT };
const char* gm_class_name(int cls);
int  gm_classify(const GmSample* s, int* which);   /* which: the busiest heavy thread (0 main, 1 render, 2 d3d) */
typedef struct {
    int level, cap, cand, cand_n, ahead, ahead_cand, ahead_n, aniso, quality;
    double t_change, t_cool, t_hot, t_q;
} GmPolicy;
void gm_policy_init(GmPolicy* p, int start_level, int quality);
void gm_policy_step(GmPolicy* p, const GmSample* s, int cls, double now, double thermal_thr);
void gm_preset(int level, int pool_max, GmKnobs* k);   /* the knobs of a preset level, scaled to the pool size */
#endif
