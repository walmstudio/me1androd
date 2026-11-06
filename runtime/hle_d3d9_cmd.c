/* hle_d3d9_cmd.c - the ring buffer and the render thread of the D3D9 command stream (see hle_d3d9_cmd.h).
 * Record format, state deltas and the replay itself are D3D9-specific and live in hle_d3d9.c. */
#include "hle_d3d9_cmd.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <sched.h>

static struct {
    int mode;
    uint8_t* buf; uint32_t guest, cap, maxrec;
    cmd_exec_fn exec; cmd_idle_fn idle;
    uint64_t w;                 /* producer: end of the last committed record (published, seq_cst) */
    uint64_t r;                 /* consumer: end of the last executed record (published, seq_cst) */
    uint32_t resn;              /* producer: bytes reserved by cmd_begin */
    int sleeping, waiting, idle_ms, busy, spin_us;
    uint64_t waitpos;
    pthread_mutex_t m; pthread_cond_t cv_work, cv_done;
    uint64_t nrec, bytes, nwait, nfull, nkick; double wait_s;
} Q;
static __thread int t_consumer;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static inline void relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

/* consumer: run the record at Q.r */
static void consume_one(void) {
    CmdHdr* h = (CmdHdr*)(Q.buf + Q.r % Q.cap);
    uint32_t sz = h->size;
    if (h->type) Q.exec(h);
    __atomic_store_n(&Q.r, Q.r + sz, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&Q.waiting, __ATOMIC_SEQ_CST) && Q.r >= __atomic_load_n(&Q.waitpos, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&Q.m); pthread_cond_broadcast(&Q.cv_done); pthread_mutex_unlock(&Q.m);
    }
}
static void kick(void) {
    if (Q.mode != 2 || !__atomic_load_n(&Q.sleeping, __ATOMIC_SEQ_CST)) return;
    pthread_mutex_lock(&Q.m); pthread_cond_signal(&Q.cv_work); pthread_mutex_unlock(&Q.m);
    Q.nkick++;
}
void cmd_kick(void) { kick(); }
/* producer: until the consumer has passed pos */
static void wait_done(uint64_t pos) {
    if (Q.mode == 1) {
        if (Q.busy) { static int n; if (!n++) fprintf(stderr, "[d3d] BUG: command stream re-entered during the replay\n"); return; }
        Q.busy = 1; while (Q.r < pos) consume_one(); Q.busy = 0;
        return;
    }
    if (__atomic_load_n(&Q.r, __ATOMIC_SEQ_CST) >= pos) return;
    double t0 = now_s();
    kick();
    for (int i = 0; i < 2000 && __atomic_load_n(&Q.r, __ATOMIC_SEQ_CST) < pos; i++) relax();   /* short calls answer in < 5 us */
    if (__atomic_load_n(&Q.r, __ATOMIC_SEQ_CST) < pos) {
        pthread_mutex_lock(&Q.m);
        __atomic_store_n(&Q.waitpos, pos, __ATOMIC_RELAXED);
        __atomic_store_n(&Q.waiting, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&Q.r, __ATOMIC_SEQ_CST) < pos) {
            if (__atomic_load_n(&Q.sleeping, __ATOMIC_SEQ_CST)) pthread_cond_signal(&Q.cv_work);
            pthread_cond_wait(&Q.cv_done, &Q.m);
        }
        __atomic_store_n(&Q.waiting, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&Q.m);
    }
    Q.nwait++; Q.wait_s += now_s() - t0;
}
void cmd_wait(uint64_t pos) { if (Q.mode && !t_consumer && pos) wait_done(pos); }

static void* worker(void* a) {
    (void)a;
    t_consumer = 1;
    Q.idle(0);
    for (;;) {
        uint64_t w = __atomic_load_n(&Q.w, __ATOMIC_SEQ_CST);
        if (Q.r != w) { while (Q.r != w) consume_one(); continue; }
        Q.idle(1);
        /* the producer usually records the next call within microseconds: spin before sleeping (ME1_D3D_CMD_SPIN us) */
        double t0 = now_s(); int got = 0;
        for (int i = 0; ; i++) {
            if (__atomic_load_n(&Q.w, __ATOMIC_SEQ_CST) != Q.r) { got = 1; break; }
            relax();
            if ((i & 63) == 63 && (now_s() - t0) * 1e6 >= Q.spin_us) break;
        }
        if (got) continue;
        pthread_mutex_lock(&Q.m);
        __atomic_store_n(&Q.sleeping, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&Q.w, __ATOMIC_SEQ_CST) == Q.r) {
            int ms = __atomic_load_n(&Q.idle_ms, __ATOMIC_RELAXED);
            if (ms > 0) {
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += (long)ms * 1000000L; ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L;
                if (pthread_cond_timedwait(&Q.cv_work, &Q.m, &ts) == ETIMEDOUT && __atomic_load_n(&Q.w, __ATOMIC_SEQ_CST) == Q.r) {
                    pthread_mutex_unlock(&Q.m); Q.idle(2); pthread_mutex_lock(&Q.m);
                }
            } else pthread_cond_wait(&Q.cv_work, &Q.m);
        }
        __atomic_store_n(&Q.sleeping, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&Q.m);
    }
    return NULL;
}

int cmd_init(int mode, uint8_t* host, uint32_t guest, uint32_t bytes, cmd_exec_fn exec, cmd_idle_fn idle) {
    if (Q.mode || (mode != 1 && mode != 2) || !host || bytes < (1u << 20)) return 0;
    Q.buf = host; Q.guest = guest; Q.cap = bytes & ~15u; Q.maxrec = Q.cap / 4; Q.exec = exec; Q.idle = idle;
    const char* e = getenv("ME1_D3D_CMD_SPIN"); Q.spin_us = e ? atoi(e) : 50; if (Q.spin_us < 0) Q.spin_us = 0;
    pthread_mutex_init(&Q.m, NULL); pthread_cond_init(&Q.cv_work, NULL); pthread_cond_init(&Q.cv_done, NULL);
    Q.mode = mode;
    if (mode == 2) {
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 4u << 20);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        int r = pthread_create(&t, &at, worker, NULL); pthread_attr_destroy(&at);
        if (r) { fprintf(stderr, "[d3d] command stream: no render thread (%s) - replay on the D3D thread\n", strerror(r)); Q.mode = 1; }
    }
    return Q.mode;
}
int cmd_mode(void) { return Q.mode; }
uint32_t cmd_max(void) { return Q.maxrec; }
uint64_t cmd_pos(void) { return Q.w; }
uint64_t cmd_done(void) { return __atomic_load_n(&Q.r, __ATOMIC_SEQ_CST); }
int cmd_on_consumer(void) { return t_consumer || (Q.mode == 1 && Q.busy); }
uint32_t cmd_guest(const void* p) { return Q.guest + (uint32_t)((const uint8_t*)p - Q.buf); }
void cmd_set_idle_ms(int ms) { __atomic_store_n(&Q.idle_ms, ms, __ATOMIC_RELAXED); }

void* cmd_begin(uint32_t n) {
    n = (n + 15) & ~15u;
    if (n > Q.maxrec) { fprintf(stderr, "[d3d] BUG: command record of %u bytes (max %u)\n", n, Q.maxrec); return NULL; }
    uint32_t phys = (uint32_t)(Q.w % Q.cap), pad = phys + n > Q.cap ? Q.cap - phys : 0;
    if (Q.w + pad + n - cmd_done() > Q.cap) { Q.nfull++; wait_done(Q.w + pad + n - Q.cap); }
    if (pad) {
        CmdHdr* h = (CmdHdr*)(Q.buf + phys); h->size = pad; h->type = 0; h->flags = 0; h->name = NULL;
        __atomic_store_n(&Q.w, Q.w + pad, __ATOMIC_SEQ_CST);
    }
    Q.resn = n;
    return Q.buf + Q.w % Q.cap;
}
uint64_t cmd_end(void* rec, uint32_t n, uint16_t type, const char* name) {
    n = (n + 15) & ~15u;
    if (n > Q.resn) { fprintf(stderr, "[d3d] BUG: command record grew from %u to %u bytes\n", Q.resn, n); n = Q.resn; }
    CmdHdr* h = rec; h->size = n; h->type = type; h->flags = 0; h->name = name;
    __atomic_store_n(&Q.w, Q.w + n, __ATOMIC_SEQ_CST);
    Q.resn = 0; Q.nrec++; Q.bytes += n;
    kick();
    return Q.w;
}
void cmd_cancel(void) { Q.resn = 0; }
void cmd_stats(char* o, size_t cap) {
    snprintf(o, cap, "%llu records, %.1f MB, %llu waits (%.1f ms), %llu ring full, %llu wakeups",
             (unsigned long long)Q.nrec, Q.bytes / 1048576.0, (unsigned long long)Q.nwait, Q.wait_s * 1e3,
             (unsigned long long)Q.nfull, (unsigned long long)Q.nkick);
}
void cmd_counters(uint64_t* bytes, uint64_t* waits, double* wait_s) { *bytes = Q.bytes; *waits = Q.nwait; *wait_s = Q.wait_s; }
