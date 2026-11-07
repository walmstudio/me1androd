#pragma once
/* GCC's gcov atexit handler is bypassed by _exit(). Enabled only for the
 * isolated Linux PGO training build. Never flush from a signal handler. */
#if defined(ME1_PGO_GCC) && defined(__GNUC__) && !defined(__clang__)
extern void __gcov_dump(void);
static inline void me1_pgo_flush(void) { __gcov_dump(); }
#else
static inline void me1_pgo_flush(void) { }
#endif
