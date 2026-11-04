/* ME1 recomp runtime: guest CPU state, guest memory, dispatch. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <setjmp.h>

/* 128-bit SSE register (also used for 64-bit MMX registers, low half only). */
typedef union XMM {
    float f[4]; double d[2];
    uint8_t b[16]; int8_t sb[16]; uint16_t w[8]; int16_t sw[8];
    uint32_t u[4]; int32_t i[4]; uint64_t q[2]; int64_t sq[2];
} XMM;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t fs_base;
    uint32_t df;
    uint32_t eflags_saved;
    /* x87: values are kept as IEEE double (Windows default control word is 53-bit precision). */
    double st[8];
    uint32_t ftop, fempty;   /* fempty: one bit per physical register (1 = empty) */
    uint32_t fcw, fsw;       /* fsw holds C0-C3 and exception bits; TOP lives in ftop */
    uint32_t mxcsr;
    XMM xmm[8];
    XMM mm[8];               /* MMX registers (not aliased onto the x87 stack) */
} CPU;

typedef void (*GuestFn)(CPU*);
typedef struct { uint32_t va; GuestFn fn; uint32_t owner; } FuncEntry;   /* owner != 0: exception landing target */
typedef struct { uint32_t slot; const char* dll; const char* name; } ImportEntry;
typedef struct { const char* name; uint32_t va; } ExportEntry;

extern uint8_t* M;   /* base of the 4 GiB guest arena */

static inline uint8_t  rd8 (uint32_t a) { return M[a]; }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return v; }
static inline void wr8 (uint32_t a, uint8_t  v) { M[a] = v; }
static inline void wr16(uint32_t a, uint16_t v) { memcpy(M + a, &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { memcpy(M + a, &v, 8); }
/* ---- memory ordering ----
 * x86 is TSO: every plain load is an acquire, every plain store a release. ARM64 is weakly ordered, so guest code
 * that synchronises through plain MOVs (spinlock release, volatile flags, lock-free queues) needs the same guarantees.
 * The recompiler emits rdoN/wroN ("ordered") for non-stack accesses and plain rdN/wrN for [esp+..] (and [ebp+..] in
 * functions with a standard frame): stack memory is thread-private. On x86-64 hosts acquire/release compile to plain
 * MOVs (free); on ARM64 they become LDAR/STLR. They also stop the C compiler from hoisting loads out of spin loops.
 * -DRT_NO_TSO: plain accesses everywhere (faster, only safe for single-threaded code). */
#ifndef RT_NO_TSO
#define RT_ORD_LD(N, T) static inline T rdo##N(uint32_t a) { \
    if (sizeof(T) == 1 || !(a & (sizeof(T) - 1))) return __atomic_load_n((T*)(M + a), __ATOMIC_ACQUIRE); \
    T v; memcpy(&v, M + a, sizeof(T)); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v; }
#define RT_ORD_ST(N, T) static inline void wro##N(uint32_t a, T v) { \
    if (sizeof(T) == 1 || !(a & (sizeof(T) - 1))) { __atomic_store_n((T*)(M + a), v, __ATOMIC_RELEASE); return; } \
    __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(M + a, &v, sizeof(T)); }
RT_ORD_LD(8, uint8_t) RT_ORD_LD(16, uint16_t) RT_ORD_LD(32, uint32_t)
RT_ORD_ST(8, uint8_t) RT_ORD_ST(16, uint16_t) RT_ORD_ST(32, uint32_t)
#else
#define rdo8 rd8
#define rdo16 rd16
#define rdo32 rd32
#define wro8 wr8
#define wro16 wr16
#define wro32 wr32
#endif
#if defined(__x86_64__) || defined(__i386__)
#define RT_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define RT_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#define RT_PAUSE() ((void)0)
#endif

/* ---- LOCK-prefixed instructions, XCHG with memory, Interlocked* ----
 * seq_cst = full barrier, like x86 locked operations. Misaligned operands (legal on x86, a bus error for ARM64
 * atomics) fall back to a global spinlock. */
void rt_lk_slow_lock(void);
void rt_lk_slow_unlock(void);
#define RT_LK(N, T) \
static inline int lk_cas##N(uint32_t a, T* e, T d) { \
    if (sizeof(T) == 1 || !(a & (sizeof(T) - 1))) return __atomic_compare_exchange_n((T*)(M + a), e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
    rt_lk_slow_lock(); T cur; memcpy(&cur, M + a, sizeof(T)); int ok = cur == *e; \
    if (ok) memcpy(M + a, &d, sizeof(T)); else *e = cur; rt_lk_slow_unlock(); return ok; } \
static inline T lk_xchg##N(uint32_t a, T v) { \
    if (sizeof(T) == 1 || !(a & (sizeof(T) - 1))) return __atomic_exchange_n((T*)(M + a), v, __ATOMIC_SEQ_CST); \
    T o; memcpy(&o, M + a, sizeof(T)); while (!lk_cas##N(a, &o, v)) {} return o; } \
static inline T lk_xadd##N(uint32_t a, T v) { \
    if (sizeof(T) == 1 || !(a & (sizeof(T) - 1))) return __atomic_fetch_add((T*)(M + a), v, __ATOMIC_SEQ_CST); \
    T o; memcpy(&o, M + a, sizeof(T)); while (!lk_cas##N(a, &o, (T)(o + v))) {} return o; } \
static inline T lk_or##N(uint32_t a, T v) { \
    if (sizeof(T) == 1 || !(a & (sizeof(T) - 1))) return __atomic_fetch_or((T*)(M + a), v, __ATOMIC_SEQ_CST); \
    T o; memcpy(&o, M + a, sizeof(T)); while (!lk_cas##N(a, &o, (T)(o | v))) {} return o; } \
static inline T lk_and##N(uint32_t a, T v) { \
    if (sizeof(T) == 1 || !(a & (sizeof(T) - 1))) return __atomic_fetch_and((T*)(M + a), v, __ATOMIC_SEQ_CST); \
    T o; memcpy(&o, M + a, sizeof(T)); while (!lk_cas##N(a, &o, (T)(o & v))) {} return o; } \
static inline T lk_xor##N(uint32_t a, T v) { \
    if (sizeof(T) == 1 || !(a & (sizeof(T) - 1))) return __atomic_fetch_xor((T*)(M + a), v, __ATOMIC_SEQ_CST); \
    T o; memcpy(&o, M + a, sizeof(T)); while (!lk_cas##N(a, &o, (T)(o ^ v))) {} return o; }
RT_LK(8, uint8_t) RT_LK(16, uint16_t) RT_LK(32, uint32_t) RT_LK(64, uint64_t)

/* ---- guest threads: every host thread running guest code has its own CPU (see hle_thread.c) ---- */
extern _Thread_local struct CPU* rt_tcpu;
static inline void push32(CPU* c, uint32_t v) { c->esp -= 4; wr32(c->esp, v); }
static inline uint32_t pop32(CPU* c) { uint32_t v = rd32(c->esp); c->esp += 4; return v; }

#define FLAGS_DECL unsigned cf = 0, zf = 0, sf = 0, of = 0, pf = 0; (void)cf; (void)zf; (void)sf; (void)of; (void)pf
#define PARITY(x) (!__builtin_parity((uint8_t)(x)))
#define MKFLAGS() ((cf) | 2u | ((pf) << 2) | ((zf) << 6) | ((sf) << 7) | (c->df << 10) | ((of) << 11) | 0x200u)
#define SETFLAGS(v) do { cf = (v) & 1; pf = ((v) >> 2) & 1; zf = ((v) >> 6) & 1; sf = ((v) >> 7) & 1; c->df = ((v) >> 10) & 1; of = ((v) >> 11) & 1; } while (0)
#define CHECK_RET(c, a) ((void)0)

/* ---- exceptions (runtime/hle_seh.c) ----
 * Guest SEH/C++ EH runs as on Windows: RaiseException walks FS:[0] and calls the guest handlers (msvcr80's
 * _except_handler3/4, __CxxFrameHandler3 - recompiled code). The only non-local step - the jump into an __except block
 * or a catch continuation in a function whose host frame is further up - is done with longjmp: functions owning
 * landing targets (found by tools/ehscan.py) push an RtEhFrame and land via setjmp; the landing code runs as their tail
 * call. Hardware exceptions (div by zero, int3, ud2) raised by translated code go through the same dispatcher. */
typedef struct RtEhFrame {
    struct RtEhFrame* prev;
    uint32_t owner, entry_esp, entry_ebp;
    volatile uint32_t target;
    int hle_depth, disp_depth;
    jmp_buf jb;
} RtEhFrame;
extern _Thread_local RtEhFrame* rt_eh_top;
extern _Thread_local int rt_eh_depth;       /* > 0 while RaiseException/RtlUnwind run guest handlers */
extern _Thread_local int rt_hle_depth;      /* big HLE lock recursion of this thread (released when unwinding past it) */
static inline void rt_eh_enter(CPU* c, RtEhFrame* f, uint32_t owner) {
    f->prev = rt_eh_top; f->owner = owner; f->entry_esp = c->esp; f->entry_ebp = c->ebp; f->target = 0;
    f->hle_depth = rt_hle_depth; f->disp_depth = rt_eh_depth; rt_eh_top = f;
}
static inline void rt_eh_leave(RtEhFrame* f) { rt_eh_top = f->prev; }
static inline uint32_t rt_eh_target(RtEhFrame* f) { return f->target; }
void rt_eh_land(CPU* c, const FuncEntry* e);                   /* longjmps to the owner's frame; returns if none */
void rt_eh_reset(void);                                        /* thread root: forget all frames */
void rt_raise_hw(CPU* c, uint32_t code, uint32_t eip) __attribute__((noreturn));

void dispatch_call(CPU* c, uint32_t target);
#define dispatch_jmp dispatch_call
void call_import(CPU* c, uint32_t iat_slot);
void rt_fault(CPU* c, uint32_t eip, const char* why) __attribute__((noreturn));
void UNIMPL(CPU* c, uint32_t eip, const char* what) __attribute__((noreturn));
void rt_cpuid(CPU* c);
void rt_rdtsc(CPU* c);

/* ---- NaN canonicalisation ----
 * x86 produces the negative "real indefinite" QNaN for invalid operations; ARM produces a positive
 * default NaN. On non-x86 hosts results of arithmetic are mapped to the x86 pattern. */
#if defined(__x86_64__) || defined(__i386__)
#define FN32(x) (x)
#define FN64(x) (x)
#else
static inline float fn32(float x) { uint32_t b; memcpy(&b, &x, 4); if (b == 0x7FC00000u) { b = 0xFFC00000u; memcpy(&x, &b, 4); } return x; }
static inline double fn64(double x) { uint64_t b; memcpy(&b, &x, 8); if (b == 0x7FF8000000000000ull) { b = 0xFFF8000000000000ull; memcpy(&x, &b, 8); } return x; }
#define FN32(x) fn32(x)
#define FN64(x) fn64(x)
#endif

/* ---- x87 FPU ---- */
#define FST(i) (c->st[(c->ftop + (i)) & 7])
static inline void fpush(CPU* c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; c->fempty &= ~(1u << c->ftop); }
static inline void fpop(CPU* c) { c->fempty |= 1u << c->ftop; c->ftop = (c->ftop + 1) & 7; }
static inline float  rdf32(uint32_t a) { float v;  memcpy(&v, M + a, 4); return v; }
static inline double rdf64(uint32_t a) { double v; memcpy(&v, M + a, 8); return v; }
static inline void wrf32(uint32_t a, float v)  { memcpy(M + a, &v, 4); }
static inline void wrf64(uint32_t a, double v) { memcpy(M + a, &v, 8); }
double rt_rdf80(uint32_t a);
void rt_wrf80(uint32_t a, double v);
/* FCOM family: C3 C2 C0 = 000 (>), 001 (<), 100 (=), 111 (unordered) */
static inline void fcom_sw(CPU* c, double a, double b) {
    c->fsw &= ~0x4700u;
    if (a != a || b != b) c->fsw |= 0x4500u; else if (a < b) c->fsw |= 0x0100u; else if (a == b) c->fsw |= 0x4000u;
}
/* FCOMI / COMISS family: ZF PF CF = 000 (>), 001 (<), 100 (=), 111 (unordered); OF SF cleared */
#define FCOMI(a, b) do { double a_ = (a), b_ = (b); of = sf = 0; \
        if (a_ != a_ || b_ != b_) { zf = pf = cf = 1; } else { zf = (a_ == b_); cf = (a_ < b_); pf = 0; } } while (0)
static inline uint32_t fnstsw_val(CPU* c) { return (c->fsw & ~0x3800u) | ((c->ftop & 7) << 11); }
double  rt_frnd(CPU* c, double x);                 /* round per FCW.RC */
int16_t rt_fist16(CPU* c, double x, int trunc);
int32_t rt_fist32(CPU* c, double x, int trunc);
int64_t rt_fist64(CPU* c, double x, int trunc);
void rt_fxam(CPU* c);
void rt_fprem(CPU* c, int ieee);
void rt_fxtract(CPU* c);
void rt_fscale(CPU* c);
void rt_fninit(CPU* c);
void rt_fnstenv(CPU* c, uint32_t a);
void rt_fldenv(CPU* c, uint32_t a);
void rt_fnsave(CPU* c, uint32_t a);
void rt_frstor(CPU* c, uint32_t a);
void rt_fxsave(CPU* c, uint32_t a);
void rt_fxrstor(CPU* c, uint32_t a);

/* ---- SSE / MMX ---- */
static inline XMM rdx(uint32_t a) { XMM v; memcpy(&v, M + a, 16); return v; }
static inline void wrx(uint32_t a, XMM v) { memcpy(M + a, &v, 16); }
static inline XMM rdx64(uint32_t a) { XMM v; memset(&v, 0, 16); memcpy(&v, M + a, 8); return v; }
static inline XMM rdx32(uint32_t a) { XMM v; memset(&v, 0, 16); memcpy(&v, M + a, 4); return v; }
static inline XMM xmm_u32(uint32_t x) { XMM v; memset(&v, 0, 16); v.u[0] = x; return v; }
static inline XMM xmm_u64(uint64_t x) { XMM v; memset(&v, 0, 16); v.q[0] = x; return v; }
int32_t rt_cvt_f2i32(CPU* c, double x, int trunc);  /* cvt(t)ss2si / cvt(t)sd2si, MXCSR.RC */
float   rt_sse_i2f(CPU* c, int32_t x);
static inline int8_t   sat8 (int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : (int8_t)v; }
static inline uint8_t  usat8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : (uint8_t)v; }
static inline int16_t  sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint16_t usat16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t)v; }

/* ---- string instructions (w = element width) ---- */
#define RDW(w, a) ((w) == 1 ? rd8(a) : (w) == 2 ? rd16(a) : rd32(a))
#define WRW(w, a, v) do { if ((w) == 1) wr8(a, (uint8_t)(v)); else if ((w) == 2) wr16(a, (uint16_t)(v)); else wr32(a, (uint32_t)(v)); } while (0)
#define STEP(w) (c->df ? -(int32_t)(w) : (int32_t)(w))
#define STR_MOVS(w) do { WRW(w, c->edi, RDW(w, c->esi)); c->esi += STEP(w); c->edi += STEP(w); } while (0)
#define STR_MOVS_REP(w) do { if (!c->df && c->ecx && (c->edi >= c->esi + (w) * c->ecx || c->edi + (w) * c->ecx <= c->esi)) { \
        __atomic_thread_fence(__ATOMIC_RELEASE);   /* v0.10.13: stores before the copy stay before it (TSO) */ \
        memcpy(M + c->edi, M + c->esi, (size_t)(w) * c->ecx); c->esi += (w) * c->ecx; c->edi += (w) * c->ecx; c->ecx = 0; } \
        else while (c->ecx) { STR_MOVS(w); c->ecx--; } } while (0)
#define STR_STOS(w) do { WRW(w, c->edi, c->eax); c->edi += STEP(w); } while (0)
/* v0.10.13 (Q6): rep stosw/stosd as a native fill (memset when all bytes are equal, else a vectorisable loop) */
static inline void rt_fill(uint32_t a, uint32_t v, uint32_t n, int w) {
    uint8_t* p = M + a;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (w == 1 || (w == 2 && (uint8_t)v == (uint8_t)(v >> 8)) || (w == 4 && v == (v & 0xFFu) * 0x01010101u)) { memset(p, (uint8_t)v, (size_t)n * (size_t)w); return; }
    if (w == 2) { uint16_t x = (uint16_t)v; for (uint32_t i = 0; i < n; i++) memcpy(p + 2 * (size_t)i, &x, 2); }
    else for (uint32_t i = 0; i < n; i++) memcpy(p + 4 * (size_t)i, &v, 4);
}
#define STR_STOS_REP(w) do { if (!c->df && c->ecx && (uint64_t)c->edi + (uint64_t)(w) * c->ecx <= 0x100000000ull) { \
        rt_fill(c->edi, c->eax, c->ecx, (w)); c->edi += (uint32_t)((w) * c->ecx); c->ecx = 0; } \
        else while (c->ecx) { STR_STOS(w); c->ecx--; } } while (0)
#define STR_LODS(w) do { uint32_t v_ = RDW(w, c->esi); if ((w) == 4) c->eax = v_; else if ((w) == 2) c->eax = (c->eax & 0xFFFF0000u) | v_; else c->eax = (c->eax & 0xFFFFFF00u) | v_; c->esi += STEP(w); } while (0)
#define CMPW(w, a, b) do { uint32_t m_ = (w) == 1 ? 0xFFu : (w) == 2 ? 0xFFFFu : 0xFFFFFFFFu, x_ = (a) & m_, y_ = (b) & m_, r_ = (x_ - y_) & m_; \
        cf = x_ < y_; zf = r_ == 0; sf = (r_ >> (8 * (w) - 1)) & 1; of = (((x_ ^ y_) & (x_ ^ r_)) >> (8 * (w) - 1)) & 1; pf = PARITY(r_); } while (0)
#define STR_SCAS(w) do { CMPW(w, c->eax, RDW(w, c->edi)); c->edi += STEP(w); } while (0)
#define STR_SCAS_REP(w) do { while (c->ecx) { c->ecx--; STR_SCAS(w); if (!zf) break; } } while (0)
#define STR_SCAS_REPNE(w) do { while (c->ecx) { c->ecx--; STR_SCAS(w); if (zf) break; } } while (0)
#define STR_CMPS(w) do { CMPW(w, RDW(w, c->esi), RDW(w, c->edi)); c->esi += STEP(w); c->edi += STEP(w); } while (0)
#define STR_CMPS_REP(w) do { while (c->ecx) { c->ecx--; STR_CMPS(w); if (!zf) break; } } while (0)
#define STR_CMPS_REPNE(w) do { while (c->ecx) { c->ecx--; STR_CMPS(w); if (zf) break; } } while (0)
#define STR_LODS_REP(w) do { while (c->ecx) { STR_LODS(w); c->ecx--; } } while (0)

/* ---- module registry ---- */
typedef struct Module {
    const char* name; uint32_t base, size, entry;
    const FuncEntry* funcs; unsigned nfuncs;
    const ImportEntry* imports; unsigned nimports;
    const ExportEntry* exports;
} Module;
void rt_init(void);
void rt_load_module(Module* m, const char* image_path);   /* imports bind to already loaded guest modules first, then HLE */
int rt_try_load_module(Module* m, const char* image_path);   /* same, but returns 0 (nothing loaded) for a missing file or a CRC mismatch */
Module* rt_module_at(uint32_t va);
Module* rt_module_by_name(const char* dll);
/* optional: called for each import that a loaded guest DLL exports; returning 1 binds the HLE version instead
 * (main_exe.c: only the shader functions of the recompiled d3dx9_31.dll replace the D3DX HLE) */
extern int (*rt_import_prefer_hle)(const Module* importer, const char* dll, const char* name);
uint32_t rt_call(CPU* c, uint32_t va, int nargs, const uint32_t* args);
uint32_t rt_find_export(Module* m, const char* name);
extern CPU g_cpu;
static inline CPU* rt_cur(void) { return rt_tcpu ? rt_tcpu : &g_cpu; }   /* CPU of the calling thread */
