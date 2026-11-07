#include "pgo.h"
/* MassEffect.exe runner: loads the recompiled game modules into guest memory and runs the EXE entry point
 * (MSVC CRT startup -> static constructors -> WinMain).
 *   msvcr80.dll (crt), msvcp80.dll (cpp), PhysXLoader.dll (pxl), [PhysXCore.dll (pxc) with -DWITH_PXC], MassEffect.exe (m),
 *   [ogg.dll (ogg), vorbis.dll (vb), vorbisfile.dll (vf) with -DWITH_VORBIS: Ogg Vorbis decoding for ISACT audio, rebased to
 *    10100000/10200000/10300000, loaded and attached at startup so LoadLibraryA("vorbisfile.dll") finds them],
 *   [binkw32.dll (bk) with -DWITH_BINK: the Bink video codec (v0.9.5), rebased to 10400000; ME1_BINK=0 keeps the HLE stubs
 *    that skip every movie],
 *   [d3dx9_31.dll (dx) with -DWITH_D3DX: the HLSL compiler; found via ME1_D3DX_DLL, <image dir>/d3dx9/image.bin,
 *    <game>/Binaries/d3dx9_31.dll or <game>/d3dx9_31.dll; without it the D3DX HLE answers E_FAIL]
 * Usage: test_exe <game dir with Binaries/ and BioGame/> <docs dir> [image dir (default out)] [-- args for the game]
 * Module images: <image dir>/<module>/image.bin when present, otherwise the original files <game dir>/Binaries/<name>.dll|exe
 * (each image is CRC-checked against the one the code was recompiled from).
 * Environment: ME1_TRACE_EH=1 (exception dispatch), ME1_VFS_TRACE=1 (file access), D3D9: see hle_d3d9.c,
 *   ME1_D3DX=hle (do not load d3dx9_31.dll) | all (bind every D3DX import of the EXE to it; default: only the shader
 *   compiler/assembler/preprocessor/disassembler, through the HLE wrappers that log and time each call).
 *   ME1_HLSL_CACHE=<dir>|off (compiled-shader cache, default <docs dir>/me1_hlsl_cache), ME1_HB / ME1_PROF_HZ (heartbeat, hle_hb.c).
 * SIGTERM/SIGINT (launcher watchdog, Ctrl-C) print a heartbeat and the HLE/D3D report before exiting.
 * -DME1_LIB: built into libme1game.so (Android): main() becomes me1_exe_main(), called by the activity (game_jni.c) or by
 * the small launcher executable (run_main.c); no signal handlers there (ART owns the signals of an app process). */
#include "rt.h"
#include "hle.h"
#include "me1settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#define MODDECL(p) extern const FuncEntry p##_funcs[]; extern const unsigned p##_nfuncs; \
    extern const ImportEntry p##_imports[]; extern const unsigned p##_nimports; \
    extern const ExportEntry p##_exports[]; extern const uint32_t p##_base, p##_entry, p##_image_size, p##_image_crc;
MODDECL(crt) MODDECL(cpp) MODDECL(pxl) MODDECL(m)
#ifdef WITH_PXC
MODDECL(pxc)
#endif
#ifdef WITH_D3DX
MODDECL(dx)
#endif
#ifdef WITH_VORBIS
MODDECL(ogg) MODDECL(vb) MODDECL(vf)
static Module ogg, vb, vf;
#endif
#ifdef WITH_BINK
MODDECL(bk)
static Module bk;
#endif
#define MODINIT(mod, p, nm) do { (mod)->name = nm; (mod)->base = p##_base; (mod)->size = p##_image_size; (mod)->entry = p##_entry; \
    (mod)->funcs = p##_funcs; (mod)->nfuncs = p##_nfuncs; (mod)->imports = p##_imports; (mod)->nimports = p##_nimports; \
    (mod)->exports = p##_exports; rt_expect_image_crc(p##_image_crc); } while (0)
static Module crt, cpp, pxl, pxc, dx, exe;
static const char* g_game;
static void image_path(char* img, size_t n, const char* dir, const char* sub, const char* winpath) {
    snprintf(img, n, "%s/%s/image.bin", dir, sub);
    FILE* f = fopen(img, "rb");
    if (f) fclose(f);
    else {   /* no image.bin: map the original file from the game folder */
        const char* fn = strrchr(winpath, '\\') + 1;
        snprintf(img, n, "%s/Binaries/%s", g_game, fn);
        if (!(f = fopen(img, "rb"))) snprintf(img, n, "%s/%s", g_game, fn); else fclose(f);
    }
}
static void load(Module* m, const char* dir, const char* sub, const char* winpath) {
    char img[1024]; image_path(img, sizeof img, dir, sub, winpath);
    hle_register_module(m, winpath);
    rt_load_module(m, img);
}
/* optional module: missing file or another build -> not loaded (LoadLibrary of it then fails as before) */
static int try_load(Module* m, const char* dir, const char* sub, const char* winpath) {
    char img[1024]; image_path(img, sizeof img, dir, sub, winpath);
    if (!rt_try_load_module(m, img)) { fprintf(stderr, "[exe] %s: %s missing or not the expected build: not loaded\n", m->name, img); return 0; }
    hle_register_module(m, winpath);
    return 1;
}
#ifdef WITH_D3DX
static int g_dx_all;
static int dx_prefer_hle(const Module* imp, const char* dll, const char* name) {
    (void)imp; (void)name; return !g_dx_all && !strcasecmp(dll, "d3dx9_31.dll");
}
static int file_ok(const char* p) { FILE* f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }
static int find_d3dx(const char* imgdir, char* out, size_t n) {
    const char* e = getenv("ME1_D3DX_DLL");
    if (e && *e) { snprintf(out, n, "%s", e); if (file_ok(out)) return 1; fprintf(stderr, "[exe] ME1_D3DX_DLL=%s: not found\n", e); }
    snprintf(out, n, "%s/d3dx9/image.bin", imgdir); if (file_ok(out)) return 1;
    snprintf(out, n, "%s/Binaries/d3dx9_31.dll", g_game); if (file_ok(out)) return 1;
    snprintf(out, n, "%s/d3dx9_31.dll", g_game); if (file_ok(out)) return 1;
    return 0;
}
#endif
static uint32_t entry_ret;
static void* run_entry(void* p) { (void)p; hle_cpu_place(2); /* v0.10.13: pin the real game thread */ entry_ret = rt_call(&g_cpu, exe.entry, 0, NULL); return NULL; }
static void on_term(int sig) {
    fprintf(stderr, "\n[exe] signal %d: stopping\n", sig);
    hle_hb_now("(signal)");
    hle_report(); fflush(stdout); fflush(stderr); _exit(128 + sig);
}
#ifdef ME1_LIB
__attribute__((visibility("default"))) void me1_exe_on_signal(int sig) { on_term(sig); }
__attribute__((visibility("default"))) int me1_exe_main(int argc, char** argv) {
#else
int main(int argc, char** argv) {
    signal(SIGTERM, on_term); signal(SIGINT, on_term);
#endif
    if (argc < 3) { fprintf(stderr, "usage: %s <game dir> <docs dir> [image dir] [-- game args]\n", argv[0]); return 2; }
    const char* imgdir = argc > 3 && strcmp(argv[3], "--") ? argv[3] : "out";
    static char cmd[2048] = "\"C:\\MassEffect\\Binaries\\MassEffect.exe\"";
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--")) {
        for (int j = i + 1; j < argc; j++) { strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1); strncat(cmd, argv[j], sizeof cmd - strlen(cmd) - 1); }
        break;
    }
    g_game = argv[1];
    { ME1Config cfg; char msg[512]; int ri;
      me1cfg_load(&cfg, argv[1], argv[2]); int r=me1cfg_adapt_resolution(&cfg,msg,sizeof msg);
      if (r || *msg) fprintf(stderr,"[gearmass] %s%s\n",r < 0 ? "auto resolution: " : "",msg);
      if (me1cfg_find("screen.resolution",&ri)) setenv("ME1_GM_RENDER_RES",cfg.value[ri],1);
    }
    rt_init();
    hle_fs_init(argv[1], argv[2], NULL);
    hle_set_cmdline(cmd);
    MODINIT(&crt, crt, "msvcr80.dll"); load(&crt, imgdir, "msvcr80", "C:\\MassEffect\\Binaries\\msvcr80.dll");
    MODINIT(&cpp, cpp, "msvcp80.dll"); load(&cpp, imgdir, "msvcp80", "C:\\MassEffect\\Binaries\\msvcp80.dll");
    int have_dx = 0;
#ifdef WITH_D3DX
    {
        const char* mode = getenv("ME1_D3DX"); char path[1024];
        g_dx_all = mode && !strcmp(mode, "all");
        if (mode && !strcmp(mode, "hle")) fprintf(stderr, "[exe] ME1_D3DX=hle: d3dx9_31.dll not loaded, D3DX HLE only\n");
        else if (!find_d3dx(imgdir, path, sizeof path))
            fprintf(stderr, "[exe] d3dx9_31.dll not found (ME1_D3DX_DLL, Binaries/): no HLSL compiler, D3DXCompileShader -> E_FAIL\n");
        else {
            MODINIT(&dx, dx, "d3dx9_31.dll");
            if (rt_try_load_module(&dx, path)) {
                hle_register_module(&dx, "C:\\Windows\\System32\\d3dx9_31.dll");
                rt_import_prefer_hle = dx_prefer_hle;
                have_dx = 1;
                fprintf(stderr, "[exe] d3dx9_31.dll: %s (recompiled HLSL compiler)\n", path);
            } else fprintf(stderr, "[exe] d3dx9_31.dll: %s is not the expected 9.15.779.0000 build: D3DX HLE only\n", path);
        }
    }
#endif
    MODINIT(&pxl, pxl, "PhysXLoader.dll"); load(&pxl, imgdir, "physxloader", "C:\\MassEffect\\Binaries\\PhysXLoader.dll");
#ifdef WITH_PXC
    MODINIT(&pxc, pxc, "PhysXCore.dll"); load(&pxc, imgdir, "physxcore", "C:\\MassEffect\\Binaries\\PhysXCore.dll");
#endif
#ifdef WITH_VORBIS
    /* in dependency order: imports bind to the guest DLLs already loaded */
    int have_vorbis = 0;
    MODINIT(&ogg, ogg, "ogg.dll");
    if (try_load(&ogg, imgdir, "ogg", "C:\\MassEffect\\Binaries\\ogg.dll")) {
        MODINIT(&vb, vb, "vorbis.dll");
        if (try_load(&vb, imgdir, "vorbis", "C:\\MassEffect\\Binaries\\vorbis.dll")) {
            MODINIT(&vf, vf, "vorbisfile.dll");
            have_vorbis = try_load(&vf, imgdir, "vorbisfile", "C:\\MassEffect\\Binaries\\vorbisfile.dll");
        }
    }
    if (!have_vorbis) fprintf(stderr, "[exe] Ogg Vorbis DLLs not loaded: no ISACT music/speech\n");
#endif
#ifdef WITH_BINK
    int have_bink = 0;
    {
        const char* e = getenv("ME1_BINK");
        if (e && !strcmp(e, "0")) fprintf(stderr, "[exe] ME1_BINK=0: binkw32.dll not loaded, movies are skipped\n");
        else {
            MODINIT(&bk, bk, "binkw32.dll");
            have_bink = try_load(&bk, imgdir, "bink", "C:\\MassEffect\\Binaries\\binkw32.dll");
            if (!have_bink) fprintf(stderr, "[exe] binkw32.dll not loaded: movies are skipped\n");
        }
    }
#endif
    MODINIT(&exe, m, "MassEffect.exe"); load(&exe, imgdir, "exe", "C:\\MassEffect\\Binaries\\MassEffect.exe");
    uint32_t r = hle_dll_attach(&crt); fprintf(stderr, "[exe] msvcr80 DllMain = %u\n", r);
    r = hle_dll_attach(&cpp); fprintf(stderr, "[exe] msvcp80 DllMain = %u\n", r);
#ifdef WITH_D3DX
    if (have_dx) {
        r = hle_dll_attach(&dx); fprintf(stderr, "[exe] d3dx9_31 DllMain = %u\n", r);
        hle_d3dx_attach_module(&dx);
        if (!getenv("ME1_HLSL_CACHE")) {   /* compiled shaders survive runs that do not reach a clean exit */
            static char hc[1100]; snprintf(hc, sizeof hc, "%s/me1_hlsl_cache", argv[2]); setenv("ME1_HLSL_CACHE", hc, 0);
        }
    }
#endif
    (void)have_dx;
    r = hle_dll_attach(&pxl); fprintf(stderr, "[exe] PhysXLoader DllMain = %u\n", r);
#ifdef WITH_BINK
    if (have_bink) { r = hle_dll_attach(&bk); fprintf(stderr, "[exe] binkw32.dll (recompiled Bink codec) at %08x, DllMain = %u\n", bk.base, r); }
#endif
#ifdef WITH_VORBIS
    if (have_vorbis) {
        uint32_t r1 = hle_dll_attach(&ogg), r2 = hle_dll_attach(&vb), r3 = hle_dll_attach(&vf);
        fprintf(stderr, "[exe] Ogg Vorbis (recompiled): ogg.dll %08x, vorbis.dll %08x, vorbisfile.dll %08x, DllMain %u/%u/%u\n",
                ogg.base, vb.base, vf.base, r1, r2, r3);
    }
#endif
    fprintf(stderr, "[exe] command line: %s\n[exe] entry %08x\n", cmd, exe.entry);
    hle_hb_start();
    hle_watch_start();
    /* the guest main thread runs on a host thread with a big stack: recompiled code nests host frames like guest
     * frames, and Unreal's script VM recurses deeply (rt_tcpu stays NULL there, so it uses g_cpu) */
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256u << 20);
    pthread_t th; pthread_create(&th, &a, run_entry, NULL); pthread_join(th, NULL); r = entry_ret;
    fprintf(stderr, "[exe] entry returned %u\n", r);
    hle_report();
    me1_pgo_flush();   /* only normal completion, never the signal-handler path */
    return (int)r;
}
