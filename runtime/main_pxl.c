/* Test harness: load recompiled PhysXLoader.dll, run DllMain, then call exported API. */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <unistd.h>
extern const FuncEntry pxl_funcs[]; extern const unsigned pxl_nfuncs;
extern const ImportEntry pxl_imports[]; extern const unsigned pxl_nimports;
extern const ExportEntry pxl_exports[]; extern const uint32_t pxl_base, pxl_entry, pxl_image_size;
#ifdef WITH_PXC   /* recompiled PhysXCore.dll (rebased to 0x12000000), loaded via PhysXLoader's LoadLibraryA */
extern const FuncEntry pxc_funcs[]; extern const unsigned pxc_nfuncs;
extern const ImportEntry pxc_imports[]; extern const unsigned pxc_nimports;
extern const ExportEntry pxc_exports[]; extern const uint32_t pxc_base, pxc_entry, pxc_image_size;
#endif
int main(int argc, char** argv) {
    rt_init();
    static Module m;
    m.name = "PhysXLoader.dll"; m.base = pxl_base; m.size = pxl_image_size; m.entry = pxl_entry;
    m.funcs = pxl_funcs; m.nfuncs = pxl_nfuncs; m.imports = pxl_imports; m.nimports = pxl_nimports; m.exports = pxl_exports;
    hle_register_module(&m, "C:\\MassEffect\\Binaries\\PhysXLoader.dll");
    rt_load_module(&m, argc > 1 ? argv[1] : "image.bin");
#ifdef WITH_PXC
    static Module x;
    x.name = "PhysXCore.dll"; x.base = pxc_base; x.size = pxc_image_size; x.entry = pxc_entry;
    x.funcs = pxc_funcs; x.nfuncs = pxc_nfuncs; x.imports = pxc_imports; x.nimports = pxc_nimports; x.exports = pxc_exports;
    hle_register_module(&x, "C:\\MassEffect\\Binaries\\PhysXCore.dll");
    rt_load_module(&x, argc > 2 ? argv[2] : "physxcore.bin");
#endif
    CPU* c = &g_cpu;
    uint32_t r = hle_dll_attach(&m);
    fprintf(stderr, "[test] DllMain(PROCESS_ATTACH) = %u\n", r);
    uint32_t err = ghalloc(4, 1);
    uint32_t fn = rt_find_export(&m, "NxCreatePhysicsSDK");
    uint32_t b[5] = { 0x02070200u /* NX_PHYSICS_SDK_VERSION 2.7.2 */, 0, 0, 0, err };
    /* desc must be a valid reference: build a default NxPhysicsSDKDesc-like zeroed block */
    b[3] = ghalloc(64, 1);
    /* NxPhysicsSDKDesc::setToDefault() (PhysX 2.7): hwPageSize, hwPageMax, hwConvexMax, cookerThreadMask, flags */
    wr32(b[3] + 0, 65536); wr32(b[3] + 4, 256); wr32(b[3] + 8, 2048); wr32(b[3] + 12, 0); wr32(b[3] + 16, 0);
    r = rt_call(c, fn, 5, b);
    fprintf(stderr, "[test] NxCreatePhysicsSDK(...) = %08x, errorCode = %u\n", r, rd32(err));
#ifdef WITH_PXC
    if (r) {   /* NxPhysicsSDK vtable (MSVC): 0 ~dtor, 1 release, 2 setParameter, 3 getParameter, 4 createScene, 5 releaseScene, 6 getNbScenes */
        uint32_t sdk = r, vt = rd32(sdk);
        uint32_t p1 = 1;                                   /* NX_SKIN_WIDTH */
        c->ecx = sdk; rt_call(c, rd32(vt + 12), 1, &p1);
        fprintf(stderr, "[test] sdk->getParameter(NX_SKIN_WIDTH) = %g\n", FST(0)); fpop(c);
        float f = 0.05f; uint32_t a2[2] = { 1, 0 }; memcpy(&a2[1], &f, 4);
        c->ecx = sdk; uint32_t ok = rt_call(c, rd32(vt + 8), 2, a2) & 0xff;
        c->ecx = sdk; rt_call(c, rd32(vt + 12), 1, &p1);
        fprintf(stderr, "[test] sdk->setParameter(NX_SKIN_WIDTH, 0.05) = %u; getParameter = %g\n", ok, FST(0)); fpop(c);
        c->ecx = sdk; fprintf(stderr, "[test] sdk->getNbScenes() = %u\n", rt_call(c, rd32(vt + 24), 0, NULL));
        usleep(200000);   /* let the SDK's worker thread reach its idle wait */
        fprintf(stderr, "[test] guest threads started: %u\n", hle_threads_started());
        c->ecx = sdk; rt_call(c, rd32(vt + 4), 0, NULL);  /* release(): stops and joins the worker thread */
        fprintf(stderr, "[test] sdk->release() returned\n");
    }
#endif
    hle_report();
    return 0;
}
