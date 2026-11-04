/* Multithreading test: recompiled mt_test.dll (tests/mt/mk_mt_test.py) runs N guest threads hammering locked
 * instructions, CRITICAL_SECTIONs, mutexes, semaphores, events, TLS; the counters must come out exact. */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
extern const FuncEntry mtt_funcs[]; extern const unsigned mtt_nfuncs;
extern const ImportEntry mtt_imports[]; extern const unsigned mtt_nimports;
extern const ExportEntry mtt_exports[]; extern const uint32_t mtt_base, mtt_entry, mtt_image_size;
static int fails;
static void check(int ok, const char* what, uint64_t got, uint64_t want) {
    printf("%s %-44s %llu%s", ok ? "ok  " : "FAIL", what, (unsigned long long)got, ok ? "\n" : "");
    if (!ok) { printf(" (expected %llu)\n", (unsigned long long)want); fails++; }
}
int main(int argc, char** argv) {
    uint32_t n = argc > 2 ? (uint32_t)atoi(argv[2]) : 8, iters = argc > 3 ? (uint32_t)atoi(argv[3]) : 20000;
    iters &= ~15u; if (n > 31) n = 31;
    rt_init();
    static Module m;
    m.name = "mt_test.dll"; m.base = mtt_base; m.size = mtt_image_size; m.entry = mtt_entry;
    m.funcs = mtt_funcs; m.nfuncs = mtt_nfuncs; m.imports = mtt_imports; m.nimports = mtt_nimports; m.exports = mtt_exports;
    hle_register_module(&m, "C:\\MassEffect\\Binaries\\mt_test.dll");
    rt_load_module(&m, argc > 1 ? argv[1] : "out/mt_test/image.bin");
    hle_dll_attach(&m);
    uint32_t a[2] = { n, iters };
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t r = rt_call(&g_cpu, rt_find_export(&m, "mt_run"), 2, a);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t D = mtt_base + 0x4000; uint64_t total = (uint64_t)n * iters;
    printf("mt_run(%u threads, %u iterations) = %u, %.0f ms\n", n, iters, r, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    check(r == 0, "mt_run returned", r, 0);
    check(rd32(D + 0x00) == total, "lock xadd counter", rd32(D + 0x00), total);
    check(rd32(D + 0x04) == total, "lock inc counter", rd32(D + 0x04), total);
    check(rd32(D + 0x71) == total, "misaligned lock add counter", rd32(D + 0x71), total);
    check(rd32(D + 0x08) == total, "CRITICAL_SECTION counter", rd32(D + 0x08), total);
    check(rd32(D + 0x10) == total, "lock cmpxchg spinlock (plain-store release)", rd32(D + 0x10), total);
    check(rd32(D + 0x18) == total, "xchg spinlock counter", rd32(D + 0x18), total);
    check(rd32(D + 0x20) == total, "InterlockedIncrement counter", rd32(D + 0x20), total);
    check(rd64(D + 0x28) == total, "lock cmpxchg8b 64-bit counter", rd64(D + 0x28), total);
    check(rd32(D + 0x0C) == total / 16, "mutex counter (every 16th round)", rd32(D + 0x0C), total / 16);
    check(rd32(D + 0x24) == (1u << n) - 1, "lock bts bitmap", rd32(D + 0x24), (1u << n) - 1);
    check(rd32(D + 0x34) == 0, "TLS mismatches", rd32(D + 0x34), 0);
    check(rd32(D + 0x50) == 0, "WaitForMultipleObjects(all threads)", rd32(D + 0x50), 0);
    check(rd32(D + 0x6C) == n, "exit codes (ExitThread / return)", rd32(D + 0x6C), n);
    check(rd32(D + 0x54) == n, "semaphore: N successful waits", rd32(D + 0x54), n);
    check(rd32(D + 0x58) == 0x102, "semaphore: then WAIT_TIMEOUT", rd32(D + 0x58), 0x102);
    check(rd32(D + 0x5C) == 0x102, "Wait(30 ms) on unsignalled event", rd32(D + 0x5C), 0x102);
    check(rd32(D + 0x60) == 0 && rd32(D + 0x64) == 0x102, "auto-reset event consumed once", rd32(D + 0x64), 0x102);
    check(rd32(D + 0x68) == 3, "mutex recursion: Release x3 = 1,1,0", rd32(D + 0x68), 3);
    check(rd32(D + 0x78) == n, "DllMain(DLL_THREAD_ATTACH) calls", rd32(D + 0x78), n);
    check(rd32(D + 0x7C) == n, "DllMain(DLL_THREAD_DETACH) calls", rd32(D + 0x7C), n);
    int uniq = 1; uint32_t main_tid = rd32(g_cpu.fs_base + 0x24);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = rd32(D + 0x100 + 4 * i); if (!t || t == main_tid) uniq = 0;
        for (uint32_t j = 0; j < i; j++) if (rd32(D + 0x100 + 4 * j) == t) uniq = 0;
    }
    check(uniq, "unique GetCurrentThreadId per thread", uniq, 1);
    printf("info unprotected 'add [mem], 1' counter: %u of %llu (lost updates are expected here)\n", rd32(D + 0x4C), (unsigned long long)total);
    printf(fails ? "test_mt: %d FAILED\n" : "test_mt: all ok\n", fails);
    return fails ? 1 : 0;
}
