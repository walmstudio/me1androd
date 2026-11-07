/* Guest threads and Win32 synchronisation on top of pthreads.
 *  - CreateThread: host pthread (16 MiB host stack) with its own CPU, guest stack (guest heap) and TEB (unique tid,
 *    TLS slots, SEH chain end, stack bounds); CREATE_SUSPENDED/ResumeThread, ExitThread (longjmp to the thread's
 *    root), exit codes, thread handle signalled on exit, DllMain(DLL_THREAD_ATTACH/DETACH) for loaded DLLs.
 *  - kernel objects: events (manual/auto reset, PulseEvent), mutexes (recursive, abandoned on owner exit),
 *    semaphores, threads; named objects + Open*; WaitForSingleObject/WaitForMultipleObjects (any/all, timeouts).
 *  - CRITICAL_SECTION: one host mutex per guest address (lock-free lookup); RecursionCount/OwningThread/LockCount
 *    are mirrored into the guest struct for code that inspects them.
 *  - Interlocked*: host atomics (seq_cst). TLS slots live in each TEB; TlsAlloc/TlsFree clear the slot everywhere.
 * Kernel-object state is guarded by one mutex + condition variable (few objects, coarse waits). None of these APIs
 * take the big HLE lock (HLE_NOLOCK): several of them block. */
#define _GNU_SOURCE
#include "hle.h"
#include "gearmass.h"
#include <pthread.h>
#include <setjmp.h>
#include <dirent.h>
#include <sched.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define PEB_VA 0x7FFDF000u
#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED 0x80u
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE_MS 0xFFFFFFFFu
#define STILL_ACTIVE 259u
#define PSEUDO_THREAD 0xFFFFFFFEu
#define PSEUDO_PROCESS 0xFFFFFFFFu
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_NOT_OWNER 288u
#define ERROR_TOO_MANY_POSTS 298u

enum { KO_FREE, KO_EVENT, KO_MUTEX, KO_SEM, KO_THREAD };
typedef struct GThread GThread;
typedef struct KObj {
    int type, refs;
    int manual, signaled;                  /* event */
    uint32_t pulse_gen; int pulse_left, waiters;
    uint32_t owner; int rec, abandoned;    /* mutex (owner = guest tid) */
    int32_t count, max;                    /* semaphore */
    GThread* th;                           /* thread */
    char name[96];
} KObj;
struct GThread {
    CPU cpu;                               /* unused for the main thread (g_cpu) */
    uint32_t tid, teb, teb_mem, stack_mem, start, param, handle, exit_code;
    int suspend, done, terminated, is_main;
    jmp_buf exit_jb;
    GThread* next;                         /* live threads */
};

#define KOBJ_BASE 0x30000u
#define KOBJ_MAX  8192
static KObj g_obj[KOBJ_MAX];
static unsigned g_obj_hint;
static pthread_mutex_t g_kmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_kcv;
static GThread g_main;
static GThread* g_threads = &g_main;
static uint32_t g_next_tid = 0x1008;
static _Thread_local GThread* t_self;
static int g_tls_used[64];
static int g_thr_trace = 1;
static unsigned g_nthreads_started;

static GThread* self_thread(void) { return t_self ? t_self : &g_main; }
static uint32_t cur_tid(CPU* c) { return rd32(c->fs_base + 0x24); }
static unsigned g_kgen;   /* v1.2.1: bumped by every kwake (g_kmx held): hle_wait's spin (GEARMASS spin 2) sees a wake-up */
static void kwake(void) { __atomic_add_fetch(&g_kgen, 1, __ATOMIC_RELEASE); pthread_cond_broadcast(&g_kcv); }

/* v0.10.1 CPU placement on big.LITTLE phones (ME1_CPU_PIN, default 1): the game thread (main) and the rendering thread
 * (the one calling Present: UE3 render thread, also Bink decoding) get the fast cores, every other game thread (audio
 * timer, PhysX workers, streaming, ...) the slow ones, so the two threads that bound the frame rate never share a big
 * core with background work. Cores are told apart by cpufreq/cpuinfo_max_freq; nothing happens on symmetric CPUs
 * or without sysfs (desktop tests). ME1_CPU_PIN=0 leaves placement to the kernel.
 * v0.10.13:
 *  - the guest main thread is pinned by run_entry (main_exe.c) itself. Before, hle_thread_init pinned the LOADER thread
 *    (which only waits in pthread_join); the real game thread inherited the mask, was not on the list, and the first
 *    hle_cpu_tidy reset it to all cores -> EAS parked it on the efficiency cores (A520 at 1.2 GHz on Snapdragon 8 Gen 3).
 *  - with <= 2 slow cores (8 Gen 3: 2x A520) the other game threads are NOT squeezed onto them (full mask instead):
 *    the main thread waits for the streaming thread, which starved there.
 *  - ME1_CPU_PRIME=1: main alone on the fastest core(s) when that tier is separate (X2/X4), render on the other big ones.
 *  - every heavy thread (main, render) is registered for ADPF (performance hint session, see hle_adpf_frame) and,
 *    with ME1_UCLAMP=n (1..1024), gets a utilization floor; ME1_HEAVY_NICE=n sets its nice value. */
static int g_heavy_tid[3];        /* host tids: [0] main, [1] render, [2] D3D9 replay thread (v1.2.1, ME1_D3D_CMD=2) */
static __thread int t_heavy;      /* 1 on main/render threads: sleep0 may spin there (ME1_SPIN_HEAVY) */
static __thread int t_role = -1;  /* v1.2.1: 0 main, 1 render, 2 D3D9 replay (index of g_heavy_tid / g_heavy_wait) */
static uint64_t g_heavy_wait[3];  /* v1.2.1: ns the heavy threads spent in active waits (GEARMASS spin): not real load */
static void heavy_wait_add(int64_t ns) { if (t_role >= 0 && ns > 0) __atomic_add_fetch(&g_heavy_wait[t_role], (uint64_t)ns, __ATOMIC_RELAXED); }
uint64_t hle_heavy_wait_ns(int which) { return which >= 0 && which < 3 ? __atomic_load_n(&g_heavy_wait[which], __ATOMIC_RELAXED) : 0; }
static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static int gm_spin_us(void);
#ifdef __linux__
#include <sys/syscall.h>
#include <sys/resource.h>
static cpu_set_t g_cpu_big, g_cpu_little, g_cpu_light, g_cpu_prime, g_cpu_bignp, g_cpu_all; static int g_cpu_pin = -1, g_cpu_prime_on;
static void cpu_list(const cpu_set_t* s, int n, char* o, size_t cap) {
    size_t k = 0; o[0] = 0;
    for (int i = 0; i < n && k + 12 < cap; i++) if (CPU_ISSET(i, s)) {
        int j = i; while (j + 1 < n && CPU_ISSET(j + 1, s)) j++;
        k += (size_t)snprintf(o + k, cap - k, "%s%d", k ? "," : "", i);
        if (j > i) k += (size_t)snprintf(o + k, cap - k, "-%d", j);
        i = j;
    }
}
static int g_cpu_avail;   /* v1.2.2: CPUs of the process mask BEFORE any placement (GEARMASS topology, hle_cpu_avail) */
static void cpu_avail_init(void) {
    if (g_cpu_avail) return;
    /* On Android, gm_init() may run on an already-pinned thread whose affinity mask only covers the fast cores.
       Prefer the system's online CPU count over the (possibly restricted) thread mask. */
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) g_cpu_avail = (int)n;
    else {
        cpu_set_t c0; CPU_ZERO(&c0);
        if (sched_getaffinity(0, sizeof c0, &c0) == 0 && CPU_COUNT(&c0) > 0) g_cpu_avail = CPU_COUNT(&c0);
        else g_cpu_avail = 1;
    }
}
static int cpu_pin_init(void) {
    if (g_cpu_pin >= 0) return g_cpu_pin;
    cpu_avail_init();   /* first: the calling thread is not pinned yet */
    g_cpu_pin = 0;
    const char* e = getenv("ME1_CPU_PIN"); if (e && *e == '0') { fprintf(stderr, "[hle] CPU placement off (ME1_CPU_PIN=0)\n"); return 0; }
    long n = sysconf(_SC_NPROCESSORS_CONF); if (n < 2 || n > CPU_SETSIZE) return 0;
    long f[CPU_SETSIZE]; long fmin = 0, fmax = 0; int nf = 0;
    for (int i = 0; i < n; i++) {
        char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE* fp = fopen(p, "r"); f[i] = 0;
        if (fp) { if (fscanf(fp, "%ld", &f[i]) != 1) f[i] = 0; fclose(fp); }
        if (f[i] > 0) { if (!nf || f[i] < fmin) fmin = f[i]; if (!nf || f[i] > fmax) fmax = f[i]; nf++; }
    }
    if (nf < n || fmax <= fmin) return 0;   /* unknown or symmetric */
    CPU_ZERO(&g_cpu_big); CPU_ZERO(&g_cpu_little); CPU_ZERO(&g_cpu_prime);
    for (int i = 0; i < n; i++) {
        if (f[i] == fmin) CPU_SET(i, &g_cpu_little); else CPU_SET(i, &g_cpu_big);
        if (f[i] == fmax) CPU_SET(i, &g_cpu_prime);
    }
    cpu_set_t cur; CPU_ZERO(&cur);
    if (sched_getaffinity(0, sizeof cur, &cur) == 0) {   /* stay inside the app's cpuset */
        cpu_set_t a, b, p; CPU_AND(&a, &g_cpu_big, &cur); CPU_AND(&b, &g_cpu_little, &cur); CPU_AND(&p, &g_cpu_prime, &cur);
        if (CPU_COUNT(&a) && CPU_COUNT(&b)) { g_cpu_big = a; g_cpu_little = b; g_cpu_prime = p; }
    }
    int nb = CPU_COUNT(&g_cpu_big), nl = CPU_COUNT(&g_cpu_little), np = CPU_COUNT(&g_cpu_prime);
    if (nb < 1 || nl < 1) return 0;
    CPU_OR(&g_cpu_light, &g_cpu_big, &g_cpu_little); g_cpu_all = g_cpu_light;
    if (nl >= 3) g_cpu_light = g_cpu_little;   /* enough slow cores for the background threads */
    CPU_ZERO(&g_cpu_bignp);
    for (int i = 0; i < n; i++) if (CPU_ISSET(i, &g_cpu_big) && !CPU_ISSET(i, &g_cpu_prime)) CPU_SET(i, &g_cpu_bignp);
    e = getenv("ME1_CPU_PRIME");
    g_cpu_prime_on = e && *e == '1' && np >= 1 && CPU_COUNT(&g_cpu_bignp) >= 1;
    char bl[64], ll[64], gl[64], pl[64]; cpu_list(&g_cpu_big, (int)n, bl, sizeof bl); cpu_list(&g_cpu_little, (int)n, ll, sizeof ll);
    cpu_list(&g_cpu_light, (int)n, gl, sizeof gl); cpu_list(&g_cpu_prime, (int)n, pl, sizeof pl);
    fprintf(stderr, "[hle] CPU: %ld cores, fast %s (%ld MHz), slow %s (%ld MHz), prime %s: main and render threads -> %s%s, other game threads -> %s "
            "(ME1_CPU_PIN=0 off, ME1_CPU_PRIME=1 main alone on prime)\n",
            n, bl, fmax / 1000, ll, fmin / 1000, pl, g_cpu_prime_on ? "main " : "", g_cpu_prime_on ? pl : bl, gl);
    g_cpu_pin = 1; return 1;
}
/* v0.10.2: the tids hle_cpu_place has pinned. Threads created by an already-pinned thread (Mali driver workers, the GL
 * presenter, the AL mixer, AAudio) inherit its mask and would crowd the fast cores with non-critical work. */
#define CPUPIN_MAX 96
static struct { unsigned tid; int8_t heavy; } g_cpu_pinned[CPUPIN_MAX];
static unsigned g_cpu_pinned_n;
static pthread_mutex_t g_cpumx = PTHREAD_MUTEX_INITIALIZER;
static void cpu_pin_note(unsigned tid, int heavy) {
    pthread_mutex_lock(&g_cpumx);
    unsigned i; for (i = 0; i < g_cpu_pinned_n; i++) if (g_cpu_pinned[i].tid == tid) break;
    if (i == g_cpu_pinned_n && g_cpu_pinned_n < CPUPIN_MAX) i = g_cpu_pinned_n++;
    if (i < CPUPIN_MAX) { g_cpu_pinned[i].tid = tid; g_cpu_pinned[i].heavy = (int8_t)heavy; }
    pthread_mutex_unlock(&g_cpumx);
}
/* ME1_UCLAMP / ME1_HEAVY_NICE for main and render (no-ops when unset) */
struct me1_sched_attr { uint32_t size, sched_policy; uint64_t sched_flags; int32_t sched_nice; uint32_t sched_priority;
                        uint64_t sched_runtime, sched_deadline, sched_period; uint32_t sched_util_min, sched_util_max; };
static void heavy_tune(void) {
    const char* e;
    if ((e = getenv("ME1_HEAVY_NICE")) && *e) {
        int v = atoi(e); if (v < -19) v = -19; if (v > 19) v = 19;
        int r = setpriority(PRIO_PROCESS, (id_t)gettid(), v);
        fprintf(stderr, "[hle] tid %d: nice %d (ME1_HEAVY_NICE) -> %s\n", (int)gettid(), v, r ? strerror(errno) : "ok");
    }
#ifdef SYS_sched_setattr
    if ((e = getenv("ME1_UCLAMP")) && atoi(e) > 0) {
        struct me1_sched_attr a; memset(&a, 0, sizeof a); a.size = sizeof a;
        a.sched_flags = 0x08 | 0x10 | 0x20;   /* KEEP_POLICY | KEEP_PARAMS | UTIL_CLAMP_MIN */
        int v = atoi(e); a.sched_util_min = (uint32_t)(v > 1024 ? 1024 : v);
        long r = syscall(SYS_sched_setattr, 0, &a, 0);
        fprintf(stderr, "[hle] tid %d: uclamp.min %u (ME1_UCLAMP) -> %s\n", (int)gettid(), a.sched_util_min, r ? strerror(errno) : "ok");
    }
#endif
}
#endif
/* heavy: 0 = other game thread, 1 = render thread (first Present), 2 = guest main thread (run_entry),
 * 3 = the D3D9 replay thread (v1.2.1, ME1_D3D_CMD=2: placed like the render thread, part of the ADPF session),
 * 4 = a GEARMASS worker (v1.2.1: like the other game threads) */
#ifdef __linux__
static const cpu_set_t* heavy_mask(int heavy) {
    if (heavy == 0 || heavy == 4) return &g_cpu_light;
    if (heavy == 3 && CPU_COUNT(&g_cpu_big) < 3) return &g_cpu_all;   /* fewer fast cores than heavy threads: the replay floats */
    if (!g_cpu_prime_on) return &g_cpu_big;
    return heavy == 2 ? &g_cpu_prime : &g_cpu_bignp;
}
#endif
/* v1.2.2: the number of CPUs the process may use, read before hle_cpu_place pinned any thread (0 = not known yet).
 * gm_init runs on an already pinned thread: its own mask (2 fast cores) said nothing about the machine. */
int hle_cpu_avail(void) {
#ifdef __linux__
    return g_cpu_avail;
#else
    return 0;
#endif
}
void hle_cpu_place(int heavy) {
    static __thread int placed = -1;
    if (placed == heavy) return;
    placed = heavy;
    int tid = (int)gettid();
    if (heavy && heavy != 4) {
        t_heavy = 1; t_role = heavy == 2 ? 0 : heavy == 3 ? 2 : 1;
        __atomic_store_n(&g_heavy_tid[t_role], tid, __ATOMIC_RELEASE);
    }
#ifdef __linux__
    if (heavy && heavy != 4) heavy_tune();
    if (!cpu_pin_init()) return;
    const cpu_set_t* m = heavy_mask(heavy);
    if (sched_setaffinity(0, sizeof(cpu_set_t), m) == 0) {
        cpu_pin_note((unsigned)tid, heavy);
        static int logs;
        if ((heavy && heavy != 4) || logs < 3) {
            if (!heavy || heavy == 4) logs++;
            char l[64]; cpu_list(m, (int)sysconf(_SC_NPROCESSORS_CONF), l, sizeof l);
            fprintf(stderr, "[hle] pinned tid %d (%s) -> %s\n", tid, heavy == 2 ? "main" : heavy == 3 ? "d3d replay" : heavy == 4 ? "worker" : heavy ? "render" : "game thread", l);
        }
    }
    else { static int k; if (k++ < 3) fprintf(stderr, "[hle] sched_setaffinity: %s\n", strerror(errno)); }
#endif
}
/* v1.2.1 (GEARMASS): the prime core switched on/off at run time - the heavy threads are moved at once */
void hle_cpu_replace(int prime) {
#ifdef __linux__
    if (!cpu_pin_init()) return;
    int on = prime && CPU_COUNT(&g_cpu_prime) >= 1 && CPU_COUNT(&g_cpu_bignp) >= 1;
    static int last = -1;
    int changed = on != g_cpu_prime_on || last < 0;
    g_cpu_prime_on = on; last = on;
    static const int role_heavy[3] = { 2, 1, 3 };
    char msg[160]; size_t k = 0; msg[0] = 0;
    for (int r = 0; r < 3; r++) {
        int tid = hle_heavy_tid(r); if (!tid) continue;
        const cpu_set_t* m = heavy_mask(role_heavy[r]);
        if (sched_setaffinity((pid_t)tid, sizeof(cpu_set_t), m) == 0 && k + 40 < sizeof msg) {
            char l[48]; cpu_list(m, (int)sysconf(_SC_NPROCESSORS_CONF), l, sizeof l);
            k += (size_t)snprintf(msg + k, sizeof msg - k, "%s%s %d -> %s", k ? ", " : "", r == 0 ? "main" : r == 1 ? "render" : "d3d", tid, l);
        }
    }
    if (changed && msg[0]) fprintf(stderr, "[gearmass] placement: prime %s: %s\n", on ? "on" : "off", msg);
#else
    (void)prime;
#endif
}
/* v1.2.1 (GEARMASS): utilization floor of the heavy threads (uclamp.min; 0 removes it) */
void hle_cpu_uclamp(int v) {
#if defined(__linux__) && defined(SYS_sched_setattr)
    if (v < 0) v = 0;
    if (v > 1024) v = 1024;
    char msg[120]; size_t k = 0; msg[0] = 0; static int fails, dead;
    if (dead) return;
    for (int r = 0; r < 3 && !dead; r++) {
        int tid = hle_heavy_tid(r); if (!tid) continue;
        struct me1_sched_attr a; memset(&a, 0, sizeof a); a.size = sizeof a;
        a.sched_flags = 0x08 | 0x10 | 0x20;   /* KEEP_POLICY | KEEP_PARAMS | UTIL_CLAMP_MIN */
        a.sched_util_min = (uint32_t)v;
        long rc = syscall(SYS_sched_setattr, tid, &a, 0);
        if (rc) {
            int er = errno;
            if (fails++ < 3) fprintf(stderr, "[gearmass] uclamp.min %d for tid %d: %s\n", v, tid, strerror(er));
            if (er == EOPNOTSUPP || er == ENOSYS || er == E2BIG) { dead = 1; fprintf(stderr, "[gearmass] the kernel has no uclamp: utilization floor off\n"); }
        }
        else if (k + 16 < sizeof msg) k += (size_t)snprintf(msg + k, sizeof msg - k, "%s%d", k ? "," : "", tid);
    }
    if (msg[0]) fprintf(stderr, "[gearmass] uclamp.min %d: tids %s\n", v, msg);
#else
    (void)v;
#endif
}
int hle_heavy_tid(int which) { return __atomic_load_n(&g_heavy_tid[which == 2 ? 2 : which & 1], __ATOMIC_ACQUIRE); }
#ifdef __linux__
/* v0.10.2: give the inherited masks back. Scans /proc/self/task for threads hle_cpu_place did NOT pin whose mask is
 * exactly the fast set or exactly the slow set and restores the full set (still inside the app's cpuset); called from
 * present() every 2 s. Returns how many threads were unpinned. v0.10.13: guest threads are always on the list
 * (main included), so tidy can no longer unpin the game thread. */
int hle_cpu_tidy(void) {
    if (!cpu_pin_init()) return 0;
    static int logs; int changed = 0;
    char list[192]; size_t lk = 0; list[0] = 0;
    cpu_set_t all; CPU_ZERO(&all); CPU_OR(&all, &g_cpu_big, &g_cpu_little);
    DIR* dp = opendir("/proc/self/task"); if (!dp) return 0;
    struct dirent* de;
    while ((de = readdir(dp))) {
        char* end; unsigned long tid = strtoul(de->d_name, &end, 10);
        if (end == de->d_name || end[0] || !tid) continue;
        int ours = 0;
        pthread_mutex_lock(&g_cpumx);
        for (unsigned i = 0; i < g_cpu_pinned_n; i++) if (g_cpu_pinned[i].tid == tid) { ours = 1; break; }
        pthread_mutex_unlock(&g_cpumx);
        if (ours || (int)tid == hle_heavy_tid(0) || (int)tid == hle_heavy_tid(1) || (int)tid == hle_heavy_tid(2)) continue;
        cpu_set_t m; CPU_ZERO(&m);
        if (sched_getaffinity((pid_t)tid, sizeof m, &m) != 0) continue;
        if (!CPU_EQUAL(&m, &g_cpu_big) && !CPU_EQUAL(&m, &g_cpu_little) && !CPU_EQUAL(&m, &g_cpu_prime) && !CPU_EQUAL(&m, &g_cpu_bignp)) continue;
        if (CPU_EQUAL(&m, &all)) continue;
        if (sched_setaffinity((pid_t)tid, sizeof all, &all) != 0) continue;
        changed++;
        if (lk + 12 < sizeof list) lk += (size_t)snprintf(list + lk, sizeof list - lk, "%s%lu", lk ? "," : "", tid);
    }
    closedir(dp);
    if (changed && logs < 6) {
        logs++;
        long n = sysconf(_SC_NPROCESSORS_CONF); char al[80]; cpu_list(&all, (int)n, al, sizeof al);
        fprintf(stderr, "[hle] CPU tidy: %d thread%s with an inherited pinned mask -> %s: %s\n",
                changed, changed == 1 ? "" : "s", al, list);
    }
    return changed;
}
#else
int hle_cpu_tidy(void) { return 0; }
#endif

/* ---- v0.10.13: Android Dynamic Performance Framework (ADPF) performance hint session ----
 * The frame is a chain main -> render -> GPU in which every thread waits for the other one by polling (Sleep), so each
 * one looks 65-90 % busy at ANY clock and schedutil/EAS settle at a low frequency (the efficiency cores at 1.2 GHz on
 * the 8 Gen 3 tester's phone). A hint session tells the system the target frame time and the actual one, and the
 * kernel raises the clocks/placement of the session's threads. NDK API 33 (Android 13), looked up at run time
 * (minSdk 29). ME1_ADPF=0 disables it. Called from present() on the render thread only. */
#if defined(__ANDROID__)
#include <dlfcn.h>
typedef void* (*adpf_mgr_fn)(void);
typedef void* (*adpf_create_fn)(void*, const int32_t*, size_t, int64_t);
typedef int (*adpf_i64_fn)(void*, int64_t);
typedef int (*adpf_threads_fn)(void*, const int32_t*, size_t);
static struct { int state; void* mgr; void* s; adpf_create_fn create; adpf_i64_fn target, report; adpf_threads_fn threads;
                int tids[3]; int64_t tgt, last; } A;
static int64_t adpf_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
void hle_adpf_frame(int fps_limit, double slept_s) {
    if (A.state < 0) return;
    if (A.state == 0) {
        const char* e = getenv("ME1_ADPF"); if (e && *e == '0') { A.state = -1; fprintf(stderr, "[adpf] off (ME1_ADPF=0)\n"); return; }
        void* h = dlopen("libandroid.so", RTLD_NOW);
        adpf_mgr_fn mg = h ? (adpf_mgr_fn)dlsym(h, "APerformanceHint_getManager") : NULL;
        A.create = h ? (adpf_create_fn)dlsym(h, "APerformanceHint_createSession") : NULL;
        A.target = h ? (adpf_i64_fn)dlsym(h, "APerformanceHint_updateTargetWorkDuration") : NULL;
        A.report = h ? (adpf_i64_fn)dlsym(h, "APerformanceHint_reportActualWorkDuration") : NULL;
        A.threads = h ? (adpf_threads_fn)dlsym(h, "APerformanceHint_setThreads") : NULL;   /* API 34 */
        A.mgr = mg ? mg() : NULL;
        if (!A.mgr || !A.create || !A.target || !A.report) { A.state = -1; fprintf(stderr, "[adpf] performance hint API unavailable (needs Android 13+)\n"); return; }
        A.state = 1;
    }
    int t0 = hle_heavy_tid(0), t1 = hle_heavy_tid(1), t2 = hle_heavy_tid(2);
    int64_t tgt = gm_k.adpf_ms10 > 0 ? (int64_t)gm_k.adpf_ms10 * 100000LL : 1000000000LL / (fps_limit > 0 ? fps_limit : 60), now = adpf_ns();   /* v1.2.1: GEARMASS target */
    if (!A.s || t0 != A.tids[0] || t1 != A.tids[1] || t2 != A.tids[2]) {
        int32_t ids[3]; size_t n = 0;
        if (t0) ids[n++] = t0;
        if (t1 && t1 != t0) ids[n++] = t1;
        if (t2 && t2 != t0 && t2 != t1) ids[n++] = t2;
        if (!n) return;
        if (A.s && A.threads && A.threads(A.s, ids, n) == 0) { /* updated in place */ }
        else {
            if (A.s) return;   /* no setThreads (Android 13): keep the first session */
            A.s = A.create(A.mgr, ids, n, tgt);
            if (!A.s) { A.state = -1; fprintf(stderr, "[adpf] createSession failed - no performance hints\n"); return; }
        }
        A.tids[0] = t0; A.tids[1] = t1; A.tids[2] = t2; A.tgt = tgt; A.last = now;
        fprintf(stderr, "[adpf] session: main tid %d, render tid %d, d3d replay tid %d, target %.1f ms (ME1_ADPF=0 off)\n", t0, t1, t2, tgt / 1e6);
        return;
    }
    if (tgt != A.tgt) { A.target(A.s, tgt); A.tgt = tgt; }
    int64_t work = now - A.last - (int64_t)(slept_s * 1e9);
    A.last = now;
    if (work < 1000000) work = 1000000;
    if (work > 1000000000) work = 1000000000;   /* loading hitches: still "late", but bounded */
    A.report(A.s, work);
}
#else
void hle_adpf_frame(int fps_limit, double slept_s) { (void)fps_limit; (void)slept_s; }
#endif
void hle_thread_init(void) {
    static int done; if (done) return; done = 1;
    /* v0.10.13: no placement here - this runs on the loader thread; run_entry pins the real game thread */
    pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&g_kcv, &a); pthread_condattr_destroy(&a);
    g_main.is_main = 1; g_main.teb = g_cpu.fs_base; g_main.tid = rd32(g_cpu.fs_base + 0x24);
    if (getenv("ME1_TRACE_THREADS")) g_thr_trace = atoi(getenv("ME1_TRACE_THREADS"));
}
unsigned hle_threads_started(void) { return __atomic_load_n(&g_nthreads_started, __ATOMIC_RELAXED); }

/* ---------------- kernel object table (g_kmx held) ---------------- */
static uint32_t ko_alloc(int type, const char* name) {
    for (unsigned k = 0; k < KOBJ_MAX; k++) {
        unsigned i = (g_obj_hint + k) % KOBJ_MAX;
        if (g_obj[i].type != KO_FREE) continue;
        KObj* o = &g_obj[i]; memset(o, 0, sizeof *o); o->type = type; o->refs = 1;
        if (name) snprintf(o->name, sizeof o->name, "%s", name);
        g_obj_hint = i + 1; return KOBJ_BASE + 4 * i;
    }
    return 0;
}
static KObj* ko_get(uint32_t h) {
    if (h < KOBJ_BASE || h >= KOBJ_BASE + 4 * KOBJ_MAX || (h & 3)) return NULL;
    KObj* o = &g_obj[(h - KOBJ_BASE) / 4]; return o->type != KO_FREE ? o : NULL;
}
static uint32_t ko_handle(KObj* o) { return KOBJ_BASE + 4 * (uint32_t)(o - g_obj); }
static KObj* ko_named(const char* name) {
    for (unsigned i = 0; i < KOBJ_MAX; i++) if (g_obj[i].type != KO_FREE && g_obj[i].name[0] && !strcmp(g_obj[i].name, name)) return &g_obj[i];
    return NULL;
}
static void ko_release(KObj* o) {
    if (--o->refs > 0) return;
    if (o->type == KO_THREAD && o->th && !o->th->is_main) free(o->th);   /* only after the thread finished: it holds a ref while alive */
    o->type = KO_FREE;
}
static uint32_t thread_handle_of(GThread* t) {   /* main thread: created on demand, never freed */
    if (!t->handle) { t->handle = ko_alloc(KO_THREAD, NULL); KObj* o = ko_get(t->handle); if (o) o->th = t; }
    return t->handle;
}
static GThread* thread_of(uint32_t h) {
    if (h == PSEUDO_THREAD) return self_thread();
    KObj* o = ko_get(h); return o && o->type == KO_THREAD ? o->th : NULL;
}
int hle_kobj_close(uint32_t h) {
    pthread_mutex_lock(&g_kmx);
    KObj* o = ko_get(h); if (o) ko_release(o);
    pthread_mutex_unlock(&g_kmx);
    return o != NULL;
}

/* ---------------- create / open / signal ---------------- */
static void get_name(uint32_t p, int wide, char* out, size_t cap) {
    out[0] = 0; if (!p) return;
    if (wide) hle_wstr_utf8(p, out, cap); else snprintf(out, cap, "%s", GSTR(p));
}
static void obj_create(CPU* c, int type, uint32_t name_p, int wide, int manual, int initial, int32_t max) {
    char name[96]; get_name(name_p, wide, name, sizeof name);
    pthread_mutex_lock(&g_kmx);
    KObj* o = name[0] ? ko_named(name) : NULL;
    if (o) {
        uint32_t h = o->type == type ? (o->refs++, ko_handle(o)) : 0;
        pthread_mutex_unlock(&g_kmx);
        hle_set_last_error(c, h ? ERROR_ALREADY_EXISTS : ERROR_INVALID_HANDLE); RET(h); return;
    }
    uint32_t h = ko_alloc(type, name[0] ? name : NULL); o = ko_get(h);
    if (o) switch (type) {
        case KO_EVENT: o->manual = manual; o->signaled = initial; break;
        case KO_MUTEX: if (initial) { o->owner = cur_tid(c); o->rec = 1; } break;
        case KO_SEM: o->count = initial; o->max = max; break;
    }
    pthread_mutex_unlock(&g_kmx);
    if (g_thr_trace > 1) fprintf(stderr, "[hle] create %s%s%s -> %08x\n", type == KO_EVENT ? "event" : type == KO_MUTEX ? "mutex" : "semaphore",
                                 name[0] ? " " : "", name, h);
    hle_set_last_error(c, h ? 0 : ERROR_NOT_ENOUGH_MEMORY); RET(h);
}
static void obj_open(CPU* c, int type, uint32_t name_p, int wide) {
    char name[96]; get_name(name_p, wide, name, sizeof name);
    pthread_mutex_lock(&g_kmx);
    KObj* o = name[0] ? ko_named(name) : NULL; uint32_t h = 0;
    if (o && o->type == type) { o->refs++; h = ko_handle(o); }
    pthread_mutex_unlock(&g_kmx);
    hle_set_last_error(c, h ? 0 : ERROR_FILE_NOT_FOUND); RET(h);
}
static void k_CreateEventA(CPU* c) { obj_create(c, KO_EVENT, ARG(3), 0, ARG(1) != 0, ARG(2) != 0, 0); }
static void k_CreateEventW(CPU* c) { obj_create(c, KO_EVENT, ARG(3), 1, ARG(1) != 0, ARG(2) != 0, 0); }
static void k_CreateMutexA(CPU* c) { obj_create(c, KO_MUTEX, ARG(2), 0, 0, ARG(1) != 0, 0); }
static void k_CreateMutexW(CPU* c) { obj_create(c, KO_MUTEX, ARG(2), 1, 0, ARG(1) != 0, 0); }
static void k_CreateSemaphoreA(CPU* c) { obj_create(c, KO_SEM, ARG(3), 0, 0, (int32_t)ARG(1), (int32_t)ARG(2)); }
static void k_CreateSemaphoreW(CPU* c) { obj_create(c, KO_SEM, ARG(3), 1, 0, (int32_t)ARG(1), (int32_t)ARG(2)); }
static void k_OpenEventA(CPU* c) { obj_open(c, KO_EVENT, ARG(2), 0); }
static void k_OpenEventW(CPU* c) { obj_open(c, KO_EVENT, ARG(2), 1); }
static void k_OpenMutexA(CPU* c) { obj_open(c, KO_MUTEX, ARG(2), 0); }
static void k_OpenMutexW(CPU* c) { obj_open(c, KO_MUTEX, ARG(2), 1); }
static void k_OpenSemaphoreA(CPU* c) { obj_open(c, KO_SEM, ARG(2), 0); }
static void k_OpenSemaphoreW(CPU* c) { obj_open(c, KO_SEM, ARG(2), 1); }

static KObj* get_typed(CPU* c, uint32_t h, int type) {   /* g_kmx held */
    KObj* o = ko_get(h);
    if (!o || o->type != type) { hle_set_last_error(c, ERROR_INVALID_HANDLE); return NULL; }
    return o;
}
static void k_SetEvent(CPU* c) {
    pthread_mutex_lock(&g_kmx); KObj* o = get_typed(c, ARG(0), KO_EVENT);
    if (o) { o->signaled = 1; kwake(); }
    pthread_mutex_unlock(&g_kmx); RET(o != NULL);
}
static void k_ResetEvent(CPU* c) {
    pthread_mutex_lock(&g_kmx); KObj* o = get_typed(c, ARG(0), KO_EVENT);
    if (o) o->signaled = 0;
    pthread_mutex_unlock(&g_kmx); RET(o != NULL);
}
/* releases the threads waiting right now (all for manual-reset, one for auto-reset); the event ends up reset */
static void k_PulseEvent(CPU* c) {
    pthread_mutex_lock(&g_kmx); KObj* o = get_typed(c, ARG(0), KO_EVENT);
    if (o) { o->signaled = 0; if (o->waiters > 0) { o->pulse_gen++; o->pulse_left = o->manual ? o->waiters : 1; kwake(); } }
    pthread_mutex_unlock(&g_kmx); RET(o != NULL);
}
static void k_ReleaseMutex(CPU* c) {
    uint32_t me = cur_tid(c), ok = 0;
    pthread_mutex_lock(&g_kmx); KObj* o = get_typed(c, ARG(0), KO_MUTEX);
    if (o && o->owner == me && o->rec > 0) { ok = 1; if (--o->rec == 0) { o->owner = 0; kwake(); } }
    else if (o) hle_set_last_error(c, ERROR_NOT_OWNER);
    pthread_mutex_unlock(&g_kmx); RET(ok);
}
static void k_ReleaseSemaphore(CPU* c) {
    int32_t n = (int32_t)ARG(1); uint32_t ok = 0;
    pthread_mutex_lock(&g_kmx); KObj* o = get_typed(c, ARG(0), KO_SEM);
    if (o) {
        if (n <= 0 || (int64_t)o->count + n > o->max) hle_set_last_error(c, n <= 0 ? ERROR_INVALID_PARAMETER : ERROR_TOO_MANY_POSTS);
        else { if (ARG(2)) wr32(ARG(2), (uint32_t)o->count); o->count += n; ok = 1; kwake(); }
    }
    pthread_mutex_unlock(&g_kmx); RET(ok);
}

/* ---------------- waiting ---------------- */
static int ko_ready(KObj* o, uint32_t me, uint32_t gen) {
    switch (o->type) {
    case KO_EVENT: return o->signaled || (o->pulse_left > 0 && o->pulse_gen != gen);
    case KO_MUTEX: return o->owner == 0 || o->owner == me;
    case KO_SEM: return o->count > 0;
    case KO_THREAD: return !o->th || o->th->done;
    }
    return 1;
}
static int ko_take(KObj* o, uint32_t me) {   /* returns 1 if an abandoned mutex was acquired */
    switch (o->type) {
    case KO_EVENT: if (!o->signaled) o->pulse_left--; else if (!o->manual) o->signaled = 0; return 0;
    case KO_MUTEX: { int ab = o->abandoned; o->abandoned = 0; o->owner = me; o->rec++; return ab; }
    case KO_SEM: o->count--; return 0;
    }
    return 0;
}
static void exit_current_thread(uint32_t code) __attribute__((noreturn));
uint32_t hle_wait(CPU* c, uint32_t n, const uint32_t* hs, int all, uint32_t ms) {
    if (n == 0 || n > 64) { hle_set_last_error(c, ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    uint32_t me = cur_tid(c); GThread* self = self_thread();
    struct timespec dl = { 0, 0 };
    if (ms && ms != INFINITE_MS) {
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    }
    KObj* os[64]; uint32_t gen[64]; uint32_t r = WAIT_TIMEOUT; int timed_out = 0, term = 0, spun = 0;
    pthread_mutex_lock(&g_kmx);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h = hs[i] == PSEUDO_THREAD ? thread_handle_of(self) : hs[i];
        os[i] = ko_get(h);
        if (os[i]) { os[i]->refs++; os[i]->waiters++; gen[i] = os[i]->pulse_gen; }
        else if (g_thr_trace) fprintf(stderr, "[hle] wait on non-waitable handle %08x -> treated as signalled\n", hs[i]);
    }
    for (;;) {
        if (all) {
            int ok = 1;
            for (uint32_t i = 0; i < n && ok; i++) if (os[i] && !ko_ready(os[i], me, gen[i])) ok = 0;
            if (ok) { int ab = 0; for (uint32_t i = 0; i < n; i++) if (os[i]) ab |= ko_take(os[i], me); r = ab ? WAIT_ABANDONED : WAIT_OBJECT_0; break; }
        } else {
            uint32_t hit = n;
            for (uint32_t i = 0; i < n && hit == n; i++) if (!os[i] || ko_ready(os[i], me, gen[i])) hit = i;
            if (hit < n) { int ab = os[hit] ? ko_take(os[hit], me) : 0; r = (ab ? WAIT_ABANDONED : WAIT_OBJECT_0) + hit; break; }
        }
        if (self->terminated) { term = 1; break; }
        if (ms == 0 || timed_out) { r = WAIT_TIMEOUT; break; }
        if (t_heavy && gm_k.spin >= 2 && !spun && gm_spin_us() > 0) {   /* v1.2.1: GEARMASS spin 2 - a short spin before blocking */
            unsigned g0 = __atomic_load_n(&g_kgen, __ATOMIC_RELAXED); spun = 1;
            pthread_mutex_unlock(&g_kmx);
            int64_t t0 = now_ns(), end = t0 + (int64_t)gm_spin_us() * 1000;
            while (__atomic_load_n(&g_kgen, __ATOMIC_ACQUIRE) == g0 && now_ns() < end) sched_yield();
            heavy_wait_add(now_ns() - t0);
            pthread_mutex_lock(&g_kmx);
            if (__atomic_load_n(&g_kgen, __ATOMIC_RELAXED) != g0) continue;   /* something was signalled: check again */
        }
        if (ms == INFINITE_MS) pthread_cond_wait(&g_kcv, &g_kmx);
        else if (pthread_cond_timedwait(&g_kcv, &g_kmx, &dl) == ETIMEDOUT) timed_out = 1;
    }
    for (uint32_t i = 0; i < n; i++) if (os[i]) { os[i]->waiters--; ko_release(os[i]); }
    pthread_mutex_unlock(&g_kmx);
    if (term) exit_current_thread(self->exit_code);
    return r;
}
static void k_WaitForSingleObject(CPU* c) { uint32_t h = ARG(0); RET(hle_wait(c, 1, &h, 0, ARG(1))); }
static void k_WaitForSingleObjectEx(CPU* c) { uint32_t h = ARG(0); RET(hle_wait(c, 1, &h, 0, ARG(1))); }
static void k_WaitForMultipleObjects(CPU* c) {
    uint32_t n = ARG(0), hs[64]; if (n > 64) n = 0;
    for (uint32_t i = 0; i < n; i++) hs[i] = rd32(ARG(1) + 4 * i);
    RET(hle_wait(c, n, hs, ARG(2) != 0, ARG(3)));
}
static void k_WaitForMultipleObjectsEx(CPU* c) { k_WaitForMultipleObjects(c); }
static void k_SignalObjectAndWait(CPU* c) {   /* (signal, wait, ms, alertable): not atomic, good enough for events/mutexes */
    uint32_t s = ARG(0), w = ARG(1), ms = ARG(2);
    pthread_mutex_lock(&g_kmx); KObj* o = ko_get(s);
    if (o && o->type == KO_EVENT) o->signaled = 1;
    else if (o && o->type == KO_MUTEX && o->owner == cur_tid(c) && --o->rec == 0) o->owner = 0;
    else if (o && o->type == KO_SEM && o->count < o->max) o->count++;
    kwake(); pthread_mutex_unlock(&g_kmx);
    RET(hle_wait(c, 1, &w, 0, ms));
}

/* ---------------- threads ---------------- */
static void thread_finish(GThread* t) {
    pthread_mutex_lock(&g_kmx);
    t->done = 1;
    for (unsigned i = 0; i < KOBJ_MAX; i++)
        if (g_obj[i].type == KO_MUTEX && g_obj[i].owner == t->tid) { g_obj[i].owner = 0; g_obj[i].rec = 0; g_obj[i].abandoned = 1; }
    for (GThread** pp = &g_threads; *pp; pp = &(*pp)->next) if (*pp == t) { *pp = t->next; break; }
    ghfree(t->stack_mem); ghfree(t->teb_mem);
    kwake();
    KObj* o = ko_get(t->handle);
    if (o) ko_release(o);   /* the thread's own reference; frees t if the handle is already closed */
    pthread_mutex_unlock(&g_kmx);
}
static void* thread_main(void* p) {
    GThread* volatile t = p;
    t_self = t; rt_tcpu = &t->cpu;
    hle_cpu_place(0);   /* until it turns out to be the render thread (first Present) */
    pthread_mutex_lock(&g_kmx);
    while (t->suspend > 0 && !t->terminated) pthread_cond_wait(&g_kcv, &g_kmx);
    pthread_mutex_unlock(&g_kmx);
    uint32_t sp0 = t->cpu.esp;
    if (!setjmp(t->exit_jb)) {
        if (!t->terminated) {
            hle_dll_notify(&t->cpu, 2);                                   /* DLL_THREAD_ATTACH */
            t->exit_code = rt_call(&t->cpu, t->start, 1, (uint32_t*)&t->param);
        }
    }
    t->cpu.esp = sp0; rt_eh_reset();
    if (!t->terminated) hle_dll_notify(&t->cpu, 3);                       /* DLL_THREAD_DETACH */
    if (g_thr_trace > 1) fprintf(stderr, "[hle] thread %u exit(%u)\n", t->tid, t->exit_code);
    hle_hb_thread_exit();
    thread_finish(t);
    return NULL;
}
static void exit_current_thread(uint32_t code) {
    GThread* t = self_thread();
    t->exit_code = code;
    if (!t->is_main) longjmp(t->exit_jb, 1);
    /* main thread: Windows keeps the process alive while other threads run */
    pthread_mutex_lock(&g_kmx); thread_handle_of(t); t->done = 1; kwake(); pthread_mutex_unlock(&g_kmx);
    fprintf(stderr, "[hle] ExitThread(%u) on the main thread\n", code);
    pthread_exit(NULL);
}
void hle_thread_check(void) {   /* called on every HLE entry: TerminateThread takes effect here */
    GThread* t = t_self;
    if (t && __atomic_load_n(&t->terminated, __ATOMIC_RELAXED) && !t->is_main) exit_current_thread(t->exit_code);
}
static void k_CreateThread(CPU* c) {   /* (sa, stack, start, param, flags, *tid) */
    uint32_t ssz = ARG(1), start = ARG(2), param = ARG(3), flags = ARG(4), ptid = ARG(5);
    if (ssz < 0x100000) ssz = 0x100000;               /* PE default reserve: 1 MiB */
    ssz = (ssz + 0xFFFFu) & ~0xFFFFu;
    GThread* t = calloc(1, sizeof *t);
    t->stack_mem = ghalloc(ssz, 0); t->teb_mem = ghalloc(0x2000, 1);
    if (!t->stack_mem || !t->teb_mem) { ghfree(t->stack_mem); ghfree(t->teb_mem); free(t); hle_set_last_error(c, ERROR_NOT_ENOUGH_MEMORY); RET(0); return; }
    t->teb = (t->teb_mem + 0xFFFu) & ~0xFFFu;
    uint32_t top = (t->stack_mem + ssz) & ~15u, T = t->teb;
    t->start = start; t->param = param; t->suspend = (flags & 4) ? 1 : 0;   /* CREATE_SUSPENDED */
    pthread_mutex_lock(&g_kmx);
    t->tid = g_next_tid; g_next_tid += 4;
    t->handle = ko_alloc(KO_THREAD, NULL);
    KObj* o = ko_get(t->handle);
    if (o) { o->th = t; o->refs = 2; t->next = g_threads; g_threads = t; }   /* returned handle + the running thread */
    pthread_mutex_unlock(&g_kmx);
    if (!o) { ghfree(t->stack_mem); ghfree(t->teb_mem); free(t); hle_set_last_error(c, ERROR_NOT_ENOUGH_MEMORY); RET(0); return; }
    wr32(T + 0x00, 0xFFFFFFFFu); wr32(T + 0x04, top); wr32(T + 0x08, t->stack_mem); wr32(T + 0x18, T);
    wr32(T + 0x20, rd32(c->fs_base + 0x20)); wr32(T + 0x24, t->tid); wr32(T + 0x2C, T + 0xE10); wr32(T + 0x30, PEB_VA);
    memset(&t->cpu, 0, sizeof t->cpu);                /* fresh FPU/SSE state, like a new Windows thread */
    t->cpu.fs_base = T; t->cpu.esp = top - 16;
    rt_fninit(&t->cpu); t->cpu.fcw = 0x27F; t->cpu.mxcsr = 0x1F80;
    pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);          /* recompiled code nests host frames like guest frames */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t pt; int e = pthread_create(&pt, &a, thread_main, t);
    pthread_attr_destroy(&a);
    if (e) {
        pthread_mutex_lock(&g_kmx);
        for (GThread** pp = &g_threads; *pp; pp = &(*pp)->next) if (*pp == t) { *pp = t->next; break; }
        o->type = KO_FREE; pthread_mutex_unlock(&g_kmx);
        ghfree(t->stack_mem); ghfree(t->teb_mem); free(t); hle_set_last_error(c, ERROR_NOT_ENOUGH_MEMORY); RET(0); return;
    }
    __atomic_add_fetch(&g_nthreads_started, 1, __ATOMIC_RELAXED);
    if (ptid) wr32(ptid, t->tid);
    if (g_thr_trace) fprintf(stderr, "[hle] CreateThread(start=%08x, param=%08x%s) -> %08x (tid %u)\n", start, param, (flags & 4) ? ", suspended" : "", t->handle, t->tid);
    hle_set_last_error(c, 0); RET(t->handle);
}
static void k_ExitThread(CPU* c) { exit_current_thread(ARG(0)); }
static void k_TerminateThread(CPU* c) {
    uint32_t code = ARG(1);
    pthread_mutex_lock(&g_kmx); GThread* t = thread_of(ARG(0));
    if (t && !t->is_main && t != self_thread() && !t->done) {
        /* no asynchronous kill: the thread unwinds at its next Win32 call / wake-up; waiters are released now */
        t->exit_code = code; __atomic_store_n(&t->terminated, 1, __ATOMIC_RELAXED); t->suspend = 0; kwake();
        fprintf(stderr, "[hle] TerminateThread(tid %u, %u): stops at its next Win32 call\n", t->tid, code);
    }
    pthread_mutex_unlock(&g_kmx);
    if (!t) { hle_set_last_error(c, ERROR_INVALID_HANDLE); RET(0); return; }
    if (t == self_thread()) exit_current_thread(code);
    RET(1);
}
static void k_GetExitCodeThread(CPU* c) {
    pthread_mutex_lock(&g_kmx); GThread* t = thread_of(ARG(0));
    if (t && ARG(1)) wr32(ARG(1), t->done || t->terminated ? t->exit_code : STILL_ACTIVE);
    pthread_mutex_unlock(&g_kmx);
    if (!t) hle_set_last_error(c, ERROR_INVALID_HANDLE);
    RET(t != NULL);
}
static void k_ResumeThread(CPU* c) {
    pthread_mutex_lock(&g_kmx); GThread* t = thread_of(ARG(0)); uint32_t prev = 0xFFFFFFFFu;
    if (t) { prev = (uint32_t)t->suspend; if (t->suspend > 0 && --t->suspend == 0) kwake(); }
    pthread_mutex_unlock(&g_kmx);
    if (!t) hle_set_last_error(c, ERROR_INVALID_HANDLE);
    RET(prev);
}
static void k_SuspendThread(CPU* c) {
    pthread_mutex_lock(&g_kmx); GThread* t = thread_of(ARG(0)); uint32_t prev = 0xFFFFFFFFu;
    if (t) {
        prev = (uint32_t)t->suspend++;
        if (t == self_thread() && !t->is_main) { while (t->suspend > 0 && !t->terminated) pthread_cond_wait(&g_kcv, &g_kmx); }
        else if (t != self_thread() && prev == 0 && !t->done && g_thr_trace)
            fprintf(stderr, "[hle] SuspendThread(tid %u): running threads are not stopped asynchronously (count only)\n", t->tid);
    }
    pthread_mutex_unlock(&g_kmx);
    if (!t) hle_set_last_error(c, ERROR_INVALID_HANDLE);
    hle_thread_check();
    RET(prev);
}
static void k_GetCurrentThread(CPU* c) { RET(PSEUDO_THREAD); }
static void k_SetThreadPriority(CPU* c) { RET(1); }
static void k_GetThreadPriority(CPU* c) { RET(0); }
static void k_SetThreadAffinityMask(CPU* c) { RET(1); }       /* previous mask */
static void k_SetThreadIdealProcessor(CPU* c) { RET(0); }     /* previous ideal processor */
static void k_DuplicateHandle(CPU* c) {   /* (srcProc, src, dstProc, *dst, access, inherit, options) */
    uint32_t h = ARG(1);
    pthread_mutex_lock(&g_kmx);
    if (h == PSEUDO_THREAD) { h = thread_handle_of(self_thread()); KObj* o = ko_get(h); if (o) o->refs++; }
    else { KObj* o = ko_get(h); if (o && !(ARG(6) & 1 /* DUPLICATE_CLOSE_SOURCE */)) o->refs++; }
    pthread_mutex_unlock(&g_kmx);
    if (ARG(3)) wr32(ARG(3), h);
    RET(1);
}
static void k_DisableThreadLibraryCalls(CPU* c) { hle_dll_disable_thread_calls(ARG(0)); RET(1); }
/* Sleep(0) spin loops (the PhysX worker waits with appSleep(0): ~200k calls/s, a whole core): after 64 back-to-back
 * Sleep(0) calls of a thread (less than 1 ms apart) each one sleeps ME1_SPIN_SLEEP_US (default 100 us) instead of
 * only yielding. */
static int g_spin_us = -1, g_gm_spin_us = -1;
static __thread uint32_t t_spin_n; static __thread int64_t t_spin_last;
static int gm_spin_us(void) {   /* v1.2.1: GEARMASS spin 2 - how long a heavy thread spins before it blocks */
    if (g_gm_spin_us < 0) { const char* e = getenv("ME1_GM_SPIN_US"); int v = e ? atoi(e) : 100; g_gm_spin_us = v < 0 ? 0 : v > 2000 ? 2000 : v; }
    return g_gm_spin_us;
}
static void sleep0(void) {
    if (g_spin_us < 0) { const char* e = getenv("ME1_SPIN_SLEEP_US"); g_spin_us = e ? atoi(e) : 100; if (g_spin_us > 5000) g_spin_us = 5000; }
    int64_t now = now_ns();
    int chained = now - t_spin_last < 1000000;
    t_spin_n = chained ? t_spin_n + 1 : 0;
    /* v0.10.13 ME1_SPIN_HEAVY=1, v1.2.1 GEARMASS spin >= 1: main/render/replay only yield (the time between back-to-back
     * calls is an active wait, reported to GEARMASS so it is not taken for load) */
    int hs = t_heavy && gm_k.spin >= 1;
    if (hs && chained) heavy_wait_add(now - t_spin_last);
    if (g_spin_us > 0 && t_spin_n >= 64 && !hs) {
        struct timespec ts = { 0, (long)g_spin_us * 1000L }; nanosleep(&ts, NULL);
        now = now_ns();
    } else sched_yield();
    t_spin_last = now;
}
/* v1.2.1: GEARMASS spin 2 on a heavy thread - Sleep(1..2) sleeps ms-1 and yields up to the deadline (no timer-slack
 * oversleep in the frame chain) */
static void spin_sleep(uint32_t ms) {
    int64_t t0 = now_ns(), end = t0 + (int64_t)ms * 1000000;
    if (ms > 1) { struct timespec ts = { 0, (long)(ms - 1) * 1000000L }; while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {} }
    int64_t t1 = now_ns();
    while (now_ns() < end) sched_yield();
    heavy_wait_add(now_ns() - t1);
}
static void k_Sleep(CPU* c) {
    uint32_t ms = ARG(0);
    if (ms == 0) sleep0();
    else if (ms <= 2 && t_heavy && gm_k.spin >= 2) spin_sleep(ms);
    else { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {} }
    hle_thread_check();
}
static void k_SleepEx(CPU* c) { k_Sleep(c); RET(0); }
static void k_SwitchToThread(CPU* c) { sched_yield(); RET(1); }

/* ---------------- TLS (slots at TEB+0xE10) ---------------- */
static void k_TlsAlloc(CPU* c) {
    pthread_mutex_lock(&g_kmx);
    for (int i = 0; i < 64; i++) if (!g_tls_used[i]) {
        g_tls_used[i] = 1;
        for (GThread* t = g_threads; t; t = t->next) wr32(t->teb + 0xE10 + 4 * i, 0);
        pthread_mutex_unlock(&g_kmx); RET(i); return;
    }
    pthread_mutex_unlock(&g_kmx);
    hle_set_last_error(c, ERROR_NOT_ENOUGH_MEMORY); RET(0xFFFFFFFFu);
}
static void k_TlsFree(CPU* c) {
    uint32_t i = ARG(0);
    if (i >= 64) { hle_set_last_error(c, ERROR_INVALID_PARAMETER); RET(0); return; }
    pthread_mutex_lock(&g_kmx);
    g_tls_used[i] = 0;
    for (GThread* t = g_threads; t; t = t->next) wr32(t->teb + 0xE10 + 4 * i, 0);
    pthread_mutex_unlock(&g_kmx); RET(1);
}
static void k_TlsGetValue(CPU* c) {
    uint32_t i = ARG(0);
    if (i >= 64) { hle_set_last_error(c, ERROR_INVALID_PARAMETER); RET(0); return; }
    hle_set_last_error(c, 0); RET(rd32(c->fs_base + 0xE10 + 4 * i));
}
static void k_TlsSetValue(CPU* c) {
    uint32_t i = ARG(0);
    if (i >= 64) { hle_set_last_error(c, ERROR_INVALID_PARAMETER); RET(0); return; }
    wr32(c->fs_base + 0xE10 + 4 * i, ARG(1)); RET(1);
}

/* ---------------- Interlocked* (kernel32 exports on XP) ---------------- */
static void k_InterlockedIncrement(CPU* c) { RET(lk_xadd32(ARG(0), 1) + 1); }
static void k_InterlockedDecrement(CPU* c) { RET(lk_xadd32(ARG(0), 0xFFFFFFFFu) - 1); }
static void k_InterlockedExchange(CPU* c) { RET(lk_xchg32(ARG(0), ARG(1))); }
static void k_InterlockedExchangeAdd(CPU* c) { RET(lk_xadd32(ARG(0), ARG(1))); }
static void k_InterlockedCompareExchange(CPU* c) { uint32_t e = ARG(2); lk_cas32(ARG(0), &e, ARG(1)); RET(e); }

/* ---------------- CRITICAL_SECTION ----------------
 * guest struct (24 bytes): DebugInfo, LockCount (-1 = free), RecursionCount, OwningThread, LockSemaphore, SpinCount */
typedef struct CsNode { uint32_t addr; struct CsNode* next; pthread_mutex_t mx; uint32_t owner; int rec; } CsNode;
#define CS_BUCKETS 65536
static CsNode* g_cs[CS_BUCKETS];
static pthread_mutex_t g_cs_ins = PTHREAD_MUTEX_INITIALIZER;
static CsNode* cs_get(uint32_t a) {   /* nodes are never freed: lock-free lookup */
    unsigned b = ((a >> 2) * 2654435761u) >> 16;
    for (CsNode* n = __atomic_load_n(&g_cs[b], __ATOMIC_ACQUIRE); n; n = n->next) if (n->addr == a) return n;
    pthread_mutex_lock(&g_cs_ins);
    CsNode* n;
    for (n = g_cs[b]; n; n = n->next) if (n->addr == a) break;
    if (!n) {
        n = calloc(1, sizeof *n); n->addr = a; pthread_mutex_init(&n->mx, NULL); n->next = g_cs[b];
        __atomic_store_n(&g_cs[b], n, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_cs_ins);
    return n;
}
static void cs_mirror(uint32_t a, CsNode* n) {
    wr32(a + 4, n->rec ? 0 : 0xFFFFFFFFu); wr32(a + 8, (uint32_t)n->rec); wr32(a + 12, n->owner);
}
static void cs_init(uint32_t a, uint32_t spin) {
    CsNode* n = cs_get(a);
    memset(M + a, 0, 24); wr32(a + 20, spin);
    if (__atomic_load_n(&n->owner, __ATOMIC_RELAXED) == 0) cs_mirror(a, n);
}
static void k_InitializeCriticalSection(CPU* c) { cs_init(ARG(0), 0); }
static void k_InitializeCriticalSectionAndSpinCount(CPU* c) { cs_init(ARG(0), ARG(1)); RET(1); }
static void k_SetCriticalSectionSpinCount(CPU* c) { uint32_t o = rd32(ARG(0) + 20); wr32(ARG(0) + 20, ARG(1)); RET(o); }
static void k_DeleteCriticalSection(CPU* c) { memset(M + ARG(0), 0, 24); }
static void k_EnterCriticalSection(CPU* c) {
    uint32_t a = ARG(0), me = cur_tid(c); CsNode* n = cs_get(a);
    if (__atomic_load_n(&n->owner, __ATOMIC_RELAXED) == me) n->rec++;
    else {
        if (t_heavy && gm_k.spin >= 2 && __atomic_load_n(&n->owner, __ATOMIC_RELAXED)) {   /* v1.2.1: GEARMASS spin 2 - wait for the owner briefly */
            int64_t t0 = now_ns(), end = t0 + (int64_t)gm_spin_us() * 1000;
            while (__atomic_load_n(&n->owner, __ATOMIC_RELAXED) && now_ns() < end) sched_yield();
            heavy_wait_add(now_ns() - t0);
        }
        pthread_mutex_lock(&n->mx); __atomic_store_n(&n->owner, me, __ATOMIC_RELAXED); n->rec = 1; }
    cs_mirror(a, n);
}
static void k_TryEnterCriticalSection(CPU* c) {
    uint32_t a = ARG(0), me = cur_tid(c); CsNode* n = cs_get(a);
    if (__atomic_load_n(&n->owner, __ATOMIC_RELAXED) == me) n->rec++;
    else if (pthread_mutex_trylock(&n->mx) == 0) { __atomic_store_n(&n->owner, me, __ATOMIC_RELAXED); n->rec = 1; }
    else { RET(0); return; }
    cs_mirror(a, n); RET(1);
}
static void k_LeaveCriticalSection(CPU* c) {
    uint32_t a = ARG(0), me = cur_tid(c); CsNode* n = cs_get(a);
    if (__atomic_load_n(&n->owner, __ATOMIC_RELAXED) != me || n->rec <= 0) {
        static int warned; if (!warned++) fprintf(stderr, "[hle] LeaveCriticalSection(%08x) by a non-owner (tid %u) ignored\n", a, me);
        return;
    }
    if (--n->rec == 0) { __atomic_store_n(&n->owner, 0, __ATOMIC_RELAXED); cs_mirror(a, n); pthread_mutex_unlock(&n->mx); }
    else cs_mirror(a, n);
}


/* ---------------- winmm multimedia timers (timeSetEvent/timeKillEvent) ----------------
 * One guest timer thread (created on first use, like winmm's) runs every callback serially without the big lock.
 * ISACT's OpenAL driver ticks its whole sound engine from a periodic timeSetEvent(wPeriodMin) callback: without it
 * sources are generated and buffers filled but nothing is ever played. Periodic timers do not catch up a backlog
 * (like Windows, a late tick is followed by the next one at now + period). ME1_MM_PERIOD sets timeGetDevCaps'
 * wPeriodMin (default 2 ms since v0.9.5, was 1; 5 ms with ME1_SHADER_LITE=1 since v0.10); ME1_MM=0 restores the old no-op stubs. */
#define MM_MAX 64
#define TIME_PERIODIC 1u
#define TIME_CALLBACK_EVENT_SET 0x10u
#define TIME_CALLBACK_EVENT_PULSE 0x20u
#define TIME_KILL_SYNCHRONOUS 0x100u
typedef struct { uint32_t id, delay, cb, user, flags; int64_t due; } MmTimer;
static MmTimer g_mmt[MM_MAX]; static int g_mm_n;
static pthread_mutex_t g_mmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mmcv;
static uint32_t g_mm_next_id = 1, g_mm_running, g_mm_tid; static int g_mm_state;   /* 0 off, 1 starting/running, -1 failed */
static uint64_t g_mm_ticks, g_mm_late, g_mm_cb_ns;
static int g_mm_enabled = -1, g_mm_period = 2;   /* v0.9.5: 2 ms (1 ms cost ~20% of a core for ~735 ticks/s) */
static int64_t mm_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static void mm_config(void) {
    if (g_mm_enabled >= 0) return;
    const char* e = getenv("ME1_MM"); g_mm_enabled = !(e && *e == '0');
    e = getenv("ME1_MM_PERIOD"); if (e && atoi(e) > 0) g_mm_period = atoi(e) > 100 ? 100 : atoi(e);
    else if ((e = getenv("ME1_SHADER_LITE")) && *e == '1') g_mm_period = 5;   /* v0.10 "simplified calculation": ISACT ticks 200x/s, not 500x */
    pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&g_mmcv, &a); pthread_condattr_destroy(&a);
}
int hle_mm_period(void) { pthread_mutex_lock(&g_mmx); mm_config(); pthread_mutex_unlock(&g_mmx); return g_mm_period; }
void hle_mm_brief(char* o, size_t n) {
    pthread_mutex_lock(&g_mmx);
    uint64_t t = g_mm_ticks;
    snprintf(o, n, "mm: %d timer%s, %llu ticks (%llu late), %.1f us/tick", g_mm_n, g_mm_n == 1 ? "" : "s", (unsigned long long)t,
             (unsigned long long)g_mm_late, t ? g_mm_cb_ns / 1000.0 / t : 0.0);
    pthread_mutex_unlock(&g_mmx);
}
static void mm_thread(CPU* c) {   /* guest thread start routine (never returns) */
    pthread_mutex_lock(&g_mmx);
    g_mm_tid = cur_tid(c);
    for (;;) {
        int best = -1;
        for (int i = 0; i < g_mm_n; i++) if (best < 0 || g_mmt[i].due < g_mmt[best].due) best = i;
        if (best < 0) { pthread_cond_wait(&g_mmcv, &g_mmx); continue; }
        int64_t now = mm_now(), due = g_mmt[best].due;
        if (due > now) {
            struct timespec dl = { (time_t)(due / 1000000000), (long)(due % 1000000000) };
            pthread_cond_timedwait(&g_mmcv, &g_mmx, &dl);
            continue;   /* re-scan: a timer may have been added or killed */
        }
        MmTimer t = g_mmt[best];
        if (t.flags & TIME_PERIODIC) {
            int64_t per = (int64_t)(t.delay ? t.delay : 1) * 1000000, nx = due + per;
            if (nx <= now) { nx = now + per; g_mm_late++; }
            g_mmt[best].due = nx;
        } else g_mmt[best] = g_mmt[--g_mm_n];
        g_mm_running = t.id;
        pthread_mutex_unlock(&g_mmx);
        /* v1.2 (ME1_AL_TICK_US, off by default): skip ticks of periodic timers that come faster than the set interval
         * (the host mixer runs at the device rate regardless). One-shot timers always fire. */
        if ((t.flags & TIME_PERIODIC) && !hle_al_tick_due((uint64_t)now)) {
            pthread_mutex_lock(&g_mmx);   /* the loop runs with g_mmx held */
            g_mm_running = 0; pthread_cond_broadcast(&g_mmcv);
            continue;
        }
        hle_hb_leave();   /* v0.10: the callback is guest code - sample it as such ([hb] hot list), the waits below as blocked */
        int64_t t0 = mm_now();
        if (t.flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE))
            hle_call("kernel32.dll", (t.flags & TIME_CALLBACK_EVENT_SET) ? "SetEvent" : "PulseEvent", 1, &t.cb);
        else {
            uint32_t a[5] = { t.id, 0, t.user, 0, 0 };   /* TIMECALLBACK(uTimerID, uMsg, dwUser, dw1, dw2), stdcall */
            rt_call(c, t.cb, 5, a);
        }
        int64_t dt = mm_now() - t0;
        pthread_mutex_lock(&g_mmx);
        g_mm_running = 0; g_mm_ticks++; g_mm_cb_ns += (uint64_t)dt;
        pthread_cond_broadcast(&g_mmcv);
        hle_hb_enter(c, "__mm_timer_thread");
    }
}
static void mm_timeSetEvent(CPU* c) {   /* (delay ms, resolution, callback, user, flags) -> id (0 = failed) */
    uint32_t delay = ARG(0), cb = ARG(2), user = ARG(3), flags = ARG(4);
    pthread_mutex_lock(&g_mmx); mm_config();
    if (!g_mm_enabled) { pthread_mutex_unlock(&g_mmx); RET(0); return; }
    if (!cb || delay > 1000000 || g_mm_n >= MM_MAX || g_mm_state < 0) { pthread_mutex_unlock(&g_mmx); RET(0); return; }
    int start = g_mm_state == 0; if (start) g_mm_state = 1;
    uint32_t id = g_mm_next_id++; if (!g_mm_next_id) g_mm_next_id = 1;
    MmTimer* t = &g_mmt[g_mm_n++];
    t->id = id; t->delay = delay ? delay : 1; t->cb = cb; t->user = user; t->flags = flags;
    t->due = mm_now() + (int64_t)t->delay * 1000000;
    pthread_cond_broadcast(&g_mmcv);
    pthread_mutex_unlock(&g_mmx);
    if (start) {
        uint32_t a[6] = { 0, 0x40000, hle_resolve_impl("rt", "__mm_timer_thread"), 0, 0, 0 };
        uint32_t h = a[2] ? hle_call("kernel32.dll", "CreateThread", 6, a) : 0;
        if (!h) {
            pthread_mutex_lock(&g_mmx); g_mm_state = -1; g_mm_n = 0; pthread_mutex_unlock(&g_mmx);
            fprintf(stderr, "[mm] timer thread could not be created\n"); RET(0); return;
        }
    }
    fprintf(stderr, "[mm] timeSetEvent(%u ms, %s, cb=%08x, user=%08x, flags=%x) -> %u\n", delay, (flags & TIME_PERIODIC) ? "periodic" : "one-shot",
            cb, user, flags, id);
    RET(id);
}
static void mm_timeKillEvent(CPU* c) {
    uint32_t id = ARG(0), me = cur_tid(c); int found = 0;
    pthread_mutex_lock(&g_mmx);
    for (int i = 0; i < g_mm_n; i++) if (g_mmt[i].id == id) {
        int sync = (g_mmt[i].flags & TIME_KILL_SYNCHRONOUS) != 0;
        g_mmt[i] = g_mmt[--g_mm_n]; found = 1;
        while (sync && g_mm_running == id && me != g_mm_tid) pthread_cond_wait(&g_mmcv, &g_mmx);
        break;
    }
    pthread_cond_broadcast(&g_mmcv);
    pthread_mutex_unlock(&g_mmx);
    if (found) fprintf(stderr, "[mm] timeKillEvent(%u)\n", id);
    RET(found ? 0 : 11);   /* TIMERR_NOERROR / MMSYSERR_INVALPARAM */
}

#define K(n, a, f) { "kernel32.dll", n, a, f, HLE_NOLOCK },
const HleDef hle_thread_defs[] = {
    K("CreateEventA", 4, k_CreateEventA) K("CreateEventW", 4, k_CreateEventW) K("CreateMutexA", 3, k_CreateMutexA)
    K("CreateMutexW", 3, k_CreateMutexW) K("CreateSemaphoreA", 4, k_CreateSemaphoreA) K("CreateSemaphoreW", 4, k_CreateSemaphoreW)
    K("OpenEventA", 3, k_OpenEventA) K("OpenEventW", 3, k_OpenEventW) K("OpenMutexA", 3, k_OpenMutexA) K("OpenMutexW", 3, k_OpenMutexW)
    K("OpenSemaphoreA", 3, k_OpenSemaphoreA) K("OpenSemaphoreW", 3, k_OpenSemaphoreW)
    K("SetEvent", 1, k_SetEvent) K("ResetEvent", 1, k_ResetEvent) K("PulseEvent", 1, k_PulseEvent)
    K("ReleaseMutex", 1, k_ReleaseMutex) K("ReleaseSemaphore", 3, k_ReleaseSemaphore)
    K("WaitForSingleObject", 2, k_WaitForSingleObject) K("WaitForSingleObjectEx", 3, k_WaitForSingleObjectEx)
    K("WaitForMultipleObjects", 4, k_WaitForMultipleObjects) K("WaitForMultipleObjectsEx", 5, k_WaitForMultipleObjectsEx)
    K("SignalObjectAndWait", 4, k_SignalObjectAndWait)
    K("CreateThread", 6, k_CreateThread) K("ExitThread", 1, k_ExitThread) K("TerminateThread", 2, k_TerminateThread)
    K("GetExitCodeThread", 2, k_GetExitCodeThread) K("ResumeThread", 1, k_ResumeThread) K("SuspendThread", 1, k_SuspendThread)
    K("GetCurrentThread", 0, k_GetCurrentThread) K("SetThreadPriority", 2, k_SetThreadPriority) K("GetThreadPriority", 1, k_GetThreadPriority)
    K("SetThreadAffinityMask", 2, k_SetThreadAffinityMask) K("SetThreadIdealProcessor", 2, k_SetThreadIdealProcessor)
    K("DuplicateHandle", 7, k_DuplicateHandle) K("DisableThreadLibraryCalls", 1, k_DisableThreadLibraryCalls)
    K("Sleep", 1, k_Sleep) K("SleepEx", 2, k_SleepEx) K("SwitchToThread", 0, k_SwitchToThread)
    K("TlsAlloc", 0, k_TlsAlloc) K("TlsFree", 1, k_TlsFree) K("TlsGetValue", 1, k_TlsGetValue) K("TlsSetValue", 2, k_TlsSetValue)
    K("FlsAlloc", 1, k_TlsAlloc) K("FlsFree", 1, k_TlsFree) K("FlsGetValue", 1, k_TlsGetValue) K("FlsSetValue", 2, k_TlsSetValue)
    K("InterlockedIncrement", 1, k_InterlockedIncrement) K("InterlockedDecrement", 1, k_InterlockedDecrement)
    K("InterlockedExchange", 2, k_InterlockedExchange) K("InterlockedExchangeAdd", 2, k_InterlockedExchangeAdd)
    K("InterlockedCompareExchange", 3, k_InterlockedCompareExchange)
    K("InitializeCriticalSection", 1, k_InitializeCriticalSection)
    K("InitializeCriticalSectionAndSpinCount", 2, k_InitializeCriticalSectionAndSpinCount)
    K("SetCriticalSectionSpinCount", 2, k_SetCriticalSectionSpinCount) K("DeleteCriticalSection", 1, k_DeleteCriticalSection)
    K("EnterCriticalSection", 1, k_EnterCriticalSection) K("TryEnterCriticalSection", 1, k_TryEnterCriticalSection)
    K("LeaveCriticalSection", 1, k_LeaveCriticalSection)
    { "winmm.dll", "timeSetEvent", 5, mm_timeSetEvent, HLE_NOLOCK }, { "winmm.dll", "timeKillEvent", 1, mm_timeKillEvent, HLE_NOLOCK },
    { "rt", "__mm_timer_thread", 1, mm_thread, HLE_NOLOCK },
};
#undef K
const unsigned hle_thread_ndefs = sizeof hle_thread_defs / sizeof hle_thread_defs[0];
