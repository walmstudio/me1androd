/* Test of PhoneModify engine modifiers (hle_mod.c, v0.10.4): parsing, draw/prim caps, ini substitution into
 * BIOEngine.ini and guest-memory patches. usage: test_mod GAME_DIR DOCS_DIR DRIVE_C_DIR */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/stat.h>

static int g_fail, g_ok;
#define CHECK(cond, ...) do { if (cond) g_ok++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static uint32_t api(const char* dll, const char* name, int n, ...) {
    uint32_t t = hle_resolve_impl(dll, name), a[16]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    if (!t) { fprintf(stderr, "FAIL: %s!%s not implemented\n", dll, name); g_fail++; return 0; }
    uint32_t sp = g_cpu.esp; uint32_t r = rt_call(&g_cpu, t, n, a);
    if (g_cpu.esp != sp) { fprintf(stderr, "FAIL: %s esp\n", name); g_fail++; }
    return r;
}
#define K(name, n, ...) api("kernel32.dll", name, n, __VA_ARGS__)
static uint32_t W(const char* s) {   /* ASCII/UTF-8 (BMP) -> guest UTF-16 */
    uint32_t p = ghalloc(2 * strlen(s) + 2, 1), k = 0;
    for (const unsigned char* q = (const unsigned char*)s; *q;) {
        unsigned cp = *q++;
        if (cp >= 0xC0) { int e = cp >= 0xE0 ? 2 : 1; cp &= e == 2 ? 15 : 31; while (e--) cp = cp << 6 | (*q++ & 63); }
        wr16(p + 2 * k++, cp);
    }
    return p;
}
static char* read_guest_file(const char* wpath, uint32_t* size) {
    uint32_t got = ghalloc(4, 1);
    uint32_t h = K("CreateFileW", 7, W(wpath), 0x80000000u, 1u, 0u, 3u, 0x80u, 0u);
    if (h == 0xFFFFFFFFu) return NULL;
    *size = K("GetFileSize", 2, h, 0u);
    uint32_t buf = ghalloc(*size + 1, 1);
    K("SetFilePointer", 4, h, 0u, 0u, 0u);
    K("ReadFile", 5, h, buf, *size, got, 0u);
    K("CloseHandle", 1, h);
    uint8_t* b = malloc(*size + 1); memcpy(b, M + buf, *size); b[*size] = 0;
    return (char*)b;
}
static void put_file(const char* host, const char* body) {
    FILE* f = fopen(host, "wb"); if (!f) { fprintf(stderr, "cannot write %s\n", host); exit(1); }
    fputs(body, f); fclose(f);
}
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: test_mod GAME_DIR DOCS_DIR DRIVE_C_DIR\n"); return 2; }
    const char* game = argv[1]; const char* docs = argv[2];
    setenv("ME1_PHONE_MOD", "1", 1);
    setenv("ME1_SHADER_LITE", "0", 1);   /* modifiers work independently of lite */
    char moddir[1200], modfile[1300], engini[1200], engwin[1600];
    snprintf(moddir, sizeof moddir, "%s/PhoneModify", game);
    mkdir(moddir, 0777);
    snprintf(modfile, sizeof modfile, "%s/phone_tune.txt", moddir);
    put_file(modfile,
             "# test modifier\n"
             "maxdraws 1234\n"
             "maxprims 456000\n"
             "ini SystemSettings ParticleLODBias 2\n"
             "ini SystemSettings DynamicShadows False\n");
    /* user's BIOEngine.ini as the game would have created it */
    snprintf(engini, sizeof engini, "%s/BioWare/Mass Effect/Config", docs);
    mkdir(engini, 0777);
    snprintf(engini, sizeof engini, "%s/BioWare/Mass Effect/Config/BIOEngine.ini", docs);
    put_file(engini, "[SystemSettings]\r\nDynamicShadows=True\r\nParticleLODBias=0\r\n");
    snprintf(engwin, sizeof engwin, "C:\\Users\\Shepard\\Documents\\BioWare\\Mass Effect\\Config\\BIOEngine.ini");

    rt_init();
    hle_fs_init(game, docs, argv[3]);
    CHECK(hle_mod_on() == 1, "modifiers on");
    CHECK(hle_mod_maxdraws() == 1234 && hle_mod_maxprims() == 456000, "caps parsed (%u/%u)", hle_mod_maxdraws(), hle_mod_maxprims());
    CHECK(hle_fs_mod_key_count() == 2, "ini keys registered (%u)", hle_fs_mod_key_count());
    CHECK(strstr(hle_mod_dir(), "PhoneModify") != NULL, "mod dir %s", hle_mod_dir());
    /* the engine's view of BIOEngine.ini: mod keys applied in memory, file untouched */
    uint32_t sz = 0; char* b = read_guest_file(engwin, &sz);
    CHECK(b && strstr(b, "DynamicShadows=False") && strstr(b, "ParticleLODBias=2"), "mod ini applied in memory:\n%s", b ? b : "(null)");
    struct stat st; stat(engini, &st);
    CHECK(b && sz > 0 && st.st_size == (off_t)strlen("[SystemSettings]\r\nDynamicShadows=True\r\nParticleLODBias=0\r\n"), "BIOEngine.ini on disk untouched (%ld)", (long)st.st_size);
    free(b);
    /* the game writes the ini: mod values restored to what the user had */
    const char* gamerite = "[SystemSettings]\r\nDynamicShadows=False\r\nParticleLODBias=2\r\n[WinDrv.WindowsClient]\r\nStartupResolutionX=1280\r\n";
    uint32_t g = gstrdup(gamerite), wr = ghalloc(4, 1);
    uint32_t h = K("CreateFileW", 7, W(engwin), 0x40000000u, 0u, 0u, 2u, 0x80u, 0u);
    CHECK(h != 0xFFFFFFFFu && K("WriteFile", 5, h, g, (uint32_t)strlen(gamerite), wr, 0u) == 1, "game writes BIOEngine.ini");
    K("CloseHandle", 1, h);
    { FILE* f = fopen(engini, "rb"); char disk[2000] = {0}; size_t dn = f ? fread(disk, 1, sizeof disk - 1, f) : 0; if (f) fclose(f); disk[dn] = 0;
      CHECK(strstr(disk, "DynamicShadows=True") && strstr(disk, "ParticleLODBias=0") && strstr(disk, "StartupResolutionX=1280"), "mod values restored on write:\n%s", disk); }
    /* guest patch: allocate guest memory, patch it, verify */
    uint32_t pad_ = hle_vmem_alloc(0x18000000, 4096, 0x3000u);   /* MEM_COMMIT|MEM_RESERVE: tracked by hle_vmem_query */
    snprintf(modfile, sizeof modfile, "%s/patchme.txt", moddir);
    char patchline[96]; snprintf(patchline, sizeof patchline, "patch %08x 11 22 33 44\n", pad_ + 4);
    put_file(modfile, patchline);
    hle_fs_init(game, docs, argv[3]);   /* re-init re-parses the folder */
    wr32(pad_ + 4, 0);
    hle_mod_apply();
    CHECK(rd32(pad_ + 4) == 0x44332211, "patch applied (%08x)", rd32(pad_ + 4));
    hle_mod_apply();   /* second call is a no-op */
    CHECK(rd32(pad_ + 4) == 0x44332211, "patch applied once");
    /* off switch: ME1_PHONE_MOD=0 ignores the folder */
    setenv("ME1_PHONE_MOD", "0", 1);
    hle_fs_init(game, docs, argv[3]);
    CHECK(hle_mod_on() == 0 && hle_mod_maxdraws() == 0 && hle_fs_mod_key_count() == 0, "modifiers off");
    unsetenv("ME1_PHONE_MOD"); unsetenv("ME1_SHADER_LITE");
    fprintf(stderr, "test_mod: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail != 0;
}
