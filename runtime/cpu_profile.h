/* CPU profiling infrastructure for me1-token Stage 8 optimization.
 * Lightweight instrumentation for hot function detection.
 * Compiles to no-op when RT_CPU_PROFILE is not defined.
 */

#ifndef RT_CPU_PROFILE_H
#define RT_CPU_PROFILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration: define RT_CPU_PROFILE to enable profiling.
 * When disabled, all macros expand to nothing (zero overhead). */
#ifndef RT_CPU_PROFILE
#define RT_CPU_PROFILE 0
#endif

/* Maximum number of profiled functions (adjust as needed) */
#define CPU_PROF_MAX_FUNCTIONS 256

/* Profiling data structure */
typedef struct CpuProfEntry {
    const char* name;          /* Function name (static string) */
    uint64_t total_cycles;     /* Accumulated cycle count */
    uint64_t call_count;       /* Number of calls */
    uint64_t min_cycles;       /* Minimum cycles in single call */
    uint64_t max_cycles;       /* Maximum cycles in single call */
} CpuProfEntry;

/* Profiling indices - must match cpu_prof_register calls in cpu_prof_init() */
enum {
    CPU_PROF_FSIN = 0,
    CPU_PROF_FCOS,
    CPU_PROF_FSINCOS,
    CPU_PROF_FPTAN,
    CPU_PROF_FPATAN,
    CPU_PROF_F2XM1,
    CPU_PROF_FYL2X,
    CPU_PROF_FYL2XP1,
    CPU_PROF_FRNDINT,
    CPU_PROF_FSCALE,
    CPU_PROF_FXTRACT,
    CPU_PROF_FPREM,
    CPU_PROF_FPREM1,
    CPU_PROF_FXAM,
    CPU_PROF_FSQRT,
    CPU_PROF_FCHS,
    CPU_PROF_FABS,
    CPU_PROF_FIST16,
    CPU_PROF_FIST32,
    CPU_PROF_FIST64,
    CPU_PROF_COUNT
};

/* Global profiling state */
extern CpuProfEntry cpu_prof_table[CPU_PROF_MAX_FUNCTIONS];
extern uint32_t cpu_prof_count;

/* Cycle counter - uses rdtsc on x86, PMCCNTR on ARM64 */
static inline uint64_t cpu_prof_cycles(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ __volatile__("mrs %0, pmccntr_el0" : "=r"(val));
    return val;
#else
    return 0;
#endif
}

/* Profiling macros - only active when RT_CPU_PROFILE=1 */
#if RT_CPU_PROFILE

/* Register a function for profiling (call once at startup) */
#define CPU_PROF_REGISTER(name) cpu_prof_register(name)

/* Start profiling a function */
#define CPU_PROF_START(idx) \
    uint64_t cpu_prof_start_##idx = cpu_prof_cycles()

/* End profiling and accumulate */
#define CPU_PROF_END(idx) do { \
    uint64_t cpu_prof_end_##idx = cpu_prof_cycles(); \
    uint64_t cpu_prof_delta_##idx = cpu_prof_end_##idx - cpu_prof_start_##idx; \
    if (idx < CPU_PROF_MAX_FUNCTIONS) { \
        cpu_prof_table[idx].total_cycles += cpu_prof_delta_##idx; \
        cpu_prof_table[idx].call_count++; \
        if (cpu_prof_table[idx].call_count == 1 || cpu_prof_delta_##idx < cpu_prof_table[idx].min_cycles) \
            cpu_prof_table[idx].min_cycles = cpu_prof_delta_##idx; \
        if (cpu_prof_delta_##idx > cpu_prof_table[idx].max_cycles) \
            cpu_prof_table[idx].max_cycles = cpu_prof_delta_##idx; \
    } \
} while (0)

/* Combined macro for simple functions */
#define CPU_PROF_SCOPE(idx) \
    uint64_t cpu_prof_start_##idx = cpu_prof_cycles(); \
    do { } while (0); \
    uint64_t cpu_prof_end_##idx = cpu_prof_cycles(); \
    uint64_t cpu_prof_delta_##idx = cpu_prof_end_##idx - cpu_prof_start_##idx; \
    if (idx < CPU_PROF_MAX_FUNCTIONS) { \
        cpu_prof_table[idx].total_cycles += cpu_prof_delta_##idx; \
        cpu_prof_table[idx].call_count++; \
        if (cpu_prof_table[idx].call_count == 1 || cpu_prof_delta_##idx < cpu_prof_table[idx].min_cycles) \
            cpu_prof_table[idx].min_cycles = cpu_prof_delta_##idx; \
        if (cpu_prof_delta_##idx > cpu_prof_table[idx].max_cycles) \
            cpu_prof_table[idx].max_cycles = cpu_prof_delta_##idx; \
    }

#else

#define CPU_PROF_REGISTER(name) ((void)0)
#define CPU_PROF_START(idx) ((void)0)
#define CPU_PROF_END(idx) ((void)0)
#define CPU_PROF_SCOPE(idx) ((void)0)

#endif

/* Public API */
void cpu_prof_init(void);
void cpu_prof_register(const char* name);
void cpu_prof_dump(const char* filepath);
void cpu_prof_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RT_CPU_PROFILE_H */