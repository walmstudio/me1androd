/* Exception test: recompiled msvcr80.dll (the game's own CRT: _except_handler3/4, __CxxFrameHandler3,
 * _CxxThrowException) + recompiled eh_test.dll (tests/eh/mk_eh_test.py). Checks SEH __except/__finally, EH4 cookies,
 * hardware divide-by-zero, C++ throw/catch/rethrow/nested catch with destructors, loops and several threads. */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
extern const FuncEntry crt_funcs[]; extern const unsigned crt_nfuncs;
extern const ImportEntry crt_imports[]; extern const unsigned crt_nimports;
extern const ExportEntry crt_exports[]; extern const uint32_t crt_base, crt_entry, crt_image_size;
extern const FuncEntry eht_funcs[]; extern const unsigned eht_nfuncs;
extern const ImportEntry eht_imports[]; extern const unsigned eht_nimports;
extern const ExportEntry eht_exports[]; extern const uint32_t eht_base, eht_entry, eht_image_size;
static int fails;
static Module crt, eht;
static void check(int ok, const char* what, uint64_t got, uint64_t want) {
    printf("%s %-52s %llu%s", ok ? "ok  " : "FAIL", what, (unsigned long long)got, ok ? "\n" : "");
    if (!ok) { printf(" (expected %llu)\n", (unsigned long long)want); fails++; }
}
static uint32_t call0(const char* name) { return rt_call(&g_cpu, rt_find_export(&eht, name), 0, NULL); }
int main(int argc, char** argv) {
    const char* crt_img = argc > 1 ? argv[1] : "out/msvcr80/image.bin";
    const char* eht_img = argc > 2 ? argv[2] : "out/eh_test/image.bin";
    uint32_t loops = argc > 3 ? (uint32_t)atoi(argv[3]) : 10000, nthr = argc > 4 ? (uint32_t)atoi(argv[4]) : 4;
    rt_init();
    crt.name = "msvcr80.dll"; crt.base = crt_base; crt.size = crt_image_size; crt.entry = crt_entry;
    crt.funcs = crt_funcs; crt.nfuncs = crt_nfuncs; crt.imports = crt_imports; crt.nimports = crt_nimports; crt.exports = crt_exports;
    hle_register_module(&crt, "C:\\MassEffect\\Binaries\\msvcr80.dll");
    rt_load_module(&crt, crt_img);
    eht.name = "eh_test.dll"; eht.base = eht_base; eht.size = eht_image_size; eht.entry = eht_entry;
    eht.funcs = eht_funcs; eht.nfuncs = eht_nfuncs; eht.imports = eht_imports; eht.nimports = eht_nimports; eht.exports = eht_exports;
    hle_register_module(&eht, "C:\\MassEffect\\Binaries\\eh_test.dll");
    rt_load_module(&eht, eht_img);
    check(hle_dll_attach(&crt) == 1, "msvcr80 DllMain(PROCESS_ATTACH)", 1, 1);
    check(hle_dll_attach(&eht) == 1, "eh_test DllMain(PROCESS_ATTACH)", 1, 1);
    uint32_t D = eht_base + 0x5000;
    uint32_t r;
    r = call0("t_seh3");        check(r == 42, "SEH3 __except(RaiseException), filter GetExceptionCode", r, 42);
    r = call0("t_div0");        check(r == 0x94, "SEH3 __except(integer divide by zero)", r, 0x94);
    r = call0("t_finally");     check(r == 7 && rd32(D + 8) == 1, "SEH3 __finally by local unwind + __except", r, 7);
    r = call0("t_seh4");        check(r == 0x44, "SEH4 frame (_except_handler4_common, cookies)", r, 0x44);
    r = call0("t_seh_nested");  check(r == 0x55 && rd32(D + 8) == 2, "nested SEH frames, global unwind runs __finally", r, 0x55);
    r = call0("t_cxx_int");     check(r == 71 && rd32(D + 4) == 1, "C++ throw 7 / catch(int e), destructor unwound", r, 71);
    r = call0("t_cxx_multi");   check(r == 55, "C++ catch(Foo&) skipped, catch(...) taken", r, 55);
    r = call0("t_cxx_seh");     check(r == 77, "C++ catch(...) of an SEH exception", r, 77);
    r = call0("t_cxx_rethrow"); check(r == 1005 && rd32(D + 12) == 1, "C++ rethrow (throw;) into the caller's catch", r, 1005);
    r = call0("t_cxx_nested");  check(r == 201, "C++ try/catch nested inside a catch block", r, 201);
    uint32_t per = 71 + 0x44 + 1005 + 7 + 201;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t esp0 = g_cpu.esp, depth0 = rt_eh_depth;
    r = rt_call(&g_cpu, rt_find_export(&eht, "t_loop"), 1, &loops);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    char what[96]; snprintf(what, sizeof what, "loop x%u (5 throws each), %.0f ms = %.1f us/throw", loops, ms, ms * 1e3 / (5.0 * loops));
    check(r == per * loops, what, r, (uint64_t)per * loops);
    check(g_cpu.esp == esp0 && rt_eh_depth == depth0, "guest esp / eh frame depth balanced after loop", g_cpu.esp - esp0, 0);
    check(rd32(D + 4) == 1 + loops, "destructor count", rd32(D + 4), 1 + loops);
    check(rd32(D + 12) == 1 + loops, "rethrow count", rd32(D + 12), 1 + loops);
    uint32_t iters = loops / 10 ? loops / 10 : 1, a[2] = { nthr, iters };
    clock_gettime(CLOCK_MONOTONIC, &t0);
    r = rt_call(&g_cpu, rt_find_export(&eht, "t_mt"), 2, a);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    snprintf(what, sizeof what, "%u threads x %u rounds throwing concurrently, %.0f ms", nthr, iters, ms);
    check(r == 0 && rd32(D + 0x10) == per * iters * nthr, what, rd32(D + 0x10), (uint64_t)per * iters * nthr);
    check(rd32(D + 4) == 1 + loops + iters * nthr, "destructor count after threads", rd32(D + 4), 1 + loops + iters * nthr);
    printf(fails ? "test_eh: %d FAILED\n" : "test_eh: all ok\n", fails);
    return fails ? 1 : 0;
}
