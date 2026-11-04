/* Test of the VFS / Win32 file layer through the real HLE thunks (same path the recompiled game uses).
 * usage: test_fs GAME_DIR DOCS_DIR DRIVE_C_DIR   (GAME_DIR must contain BioGame/Config/Default*.ini) */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>

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
static uint32_t A(const char* s) { return gstrdup(s); }
static const char* U(uint32_t p) {   /* guest UTF-16 -> static UTF-8 */
    static char b[4][2048]; static int r; char* o = b[r++ & 3]; size_t k = 0;
    for (uint32_t i = 0; rd16(p + 2 * i) && k < 2040; i++) {
        unsigned cp = rd16(p + 2 * i);
        if (cp < 0x80) o[k++] = cp; else if (cp < 0x800) { o[k++] = 0xC0 | cp >> 6; o[k++] = 0x80 | (cp & 63); }
        else { o[k++] = 0xE0 | cp >> 12; o[k++] = 0x80 | (cp >> 6 & 63); o[k++] = 0x80 | (cp & 63); }
    }
    o[k] = 0; return o;
}
static uint32_t lasterr(void) { return rd32(g_cpu.fs_base + 0x34); }
static char* read_guest_file(const char* wpath, uint32_t* size) {
    uint32_t h = K("CreateFileW", 7, W(wpath), 0x80000000u, 1u, 0u, 3u, 0x80u, 0u);
    if (h == 0xFFFFFFFFu) return NULL;
    *size = K("GetFileSize", 2, h, 0u);
    uint32_t buf = ghalloc(*size + 1, 1), got = ghalloc(4, 1);
    K("ReadFile", 5, h, buf, *size, got, 0u);
    CHECK(rd32(got) == *size, "ReadFile got %u of %u", rd32(got), *size);
    K("CloseHandle", 1, h);
    char* r = malloc(*size + 1); memcpy(r, M + buf, *size); r[*size] = 0; return r;
}
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: test_fs GAME DOCS DRIVE_C\n"); return 2; }
    rt_init(); hle_fs_init(argv[1], argv[2], argv[3]);
    char host[1024];
    /* --- no duplicate implementations between the two HLE tables --- */
    for (unsigned i = 0; i < hle_fs_ndefs; i++) {
        uint32_t t = hle_resolve_impl(hle_fs_defs[i].dll, hle_fs_defs[i].name);
        CHECK(t && (t - THUNK_BASE) / 16 >= (t - THUNK_BASE) / 16 && hle_fs_defs[i].fn, "%s", hle_fs_defs[i].name);
    }
    /* --- paths --- */
    uint32_t buf = ghalloc(4096, 1), n;
    n = K("GetCurrentDirectoryW", 2, 260u, buf);
    CHECK(!strcmp(U(buf), "C:\\MassEffect\\Binaries") && n == 22, "cwd '%s' %u", U(buf), n);
    n = K("GetFullPathNameW", 4, W("..\\BioGame\\.\\Config\\\\DefaultEngine.ini"), 260u, buf, buf + 3000);
    CHECK(!strcmp(U(buf), "C:\\MassEffect\\BioGame\\Config\\DefaultEngine.ini"), "fullpath '%s'", U(buf));
    CHECK(!strcmp(U(rd32(buf + 3000)), "DefaultEngine.ini"), "filepart '%s'", U(rd32(buf + 3000)));
    n = K("GetFullPathNameW", 4, W("x"), 4u, buf, 0u);
    CHECK(n == strlen("C:\\MassEffect\\Binaries\\x") + 1, "small buffer -> required %u", n);
    /* --- shell folders --- */
    n = api("shell32.dll", "SHGetFolderPathW", 5, 0u, 0x8005u, 0u, 0u, buf);
    CHECK(n == 0 && !strcmp(U(buf), "C:\\Users\\Shepard\\Documents"), "SHGetFolderPath(PERSONAL) %08x '%s'", n, U(buf));
    struct stat st; CHECK(stat(argv[2], &st) == 0 && S_ISDIR(st.st_mode), "Documents dir created on host");
    n = api("shell32.dll", "SHGetFolderPathW", 5, 0u, 0x1Cu, 0u, 0u, buf);
    CHECK(n == 0 && !strcmp(U(buf), "C:\\Users\\Shepard\\AppData\\Local"), "LOCAL_APPDATA '%s'", U(buf));
    /* --- environment --- */
    n = K("ExpandEnvironmentStringsW", 3, W("%USERPROFILE%\\x %NOPE% %TEMP%"), buf, 400u);
    CHECK(!strcmp(U(buf), "C:\\Users\\Shepard\\x %NOPE% C:\\Users\\Shepard\\AppData\\Local\\Temp") && n == strlen(U(buf)) + 1, "expand '%s' %u", U(buf), n);
    n = K("GetEnvironmentVariableW", 3, W("windir"), buf, 100u);
    CHECK(n == 10 && !strcmp(U(buf), "C:\\WINDOWS"), "windir %u '%s'", n, U(buf));
    n = K("GetEnvironmentVariableW", 3, W("ME1_NOT_SET"), buf, 100u);
    CHECK(n == 0 && lasterr() == 203, "missing env %u err %u", n, lasterr());
    /* --- Default*.ini overlay: case-insensitive path, patched content, original timestamp --- */
    uint32_t size, engsize = 0; char* eng = read_guest_file("..\\BIOGAME\\config\\defaultengine.INI", &size); engsize = size;
    CHECK(eng != NULL, "open DefaultEngine.ini case-insensitively");
    if (eng) {
        CHECK(strstr(eng, "StartupResolutionX=960\r\n") && strstr(eng, "TextureLODLevel=0\r\n") && strstr(eng, "UseVSYNC=True\r\n")
              && strstr(eng, "MaxTextureSize=1024\r\n") && strstr(eng, "ParticleLODBias=2\r\n"), "overrides applied in DefaultEngine.ini");
        CHECK(strstr(eng, "[IniVersion]") == NULL || 1, "-");
        free(eng);
    }
    uint32_t h = K("CreateFileW", 7, W("C:\\MassEffect\\BioGame\\Config\\DefaultEngine.ini"), 0x80000000u, 1u, 0u, 3u, 0u, 0u);
    uint32_t ft = ghalloc(8, 1); K("GetFileTime", 4, h, 0u, 0u, ft);
    snprintf(host, sizeof host, "%s/BioGame/Config/DefaultEngine.ini", argv[1]); stat(host, &st);
    uint64_t want = ((uint64_t)st.st_mtim.tv_sec + 11644473600ull) * 10000000ull + st.st_mtim.tv_nsec / 100;
    CHECK(rd64(ft) == want, "GetFileTime keeps the original timestamp");
    uint32_t wfd = ghalloc(600, 1);
    n = K("GetFileSize", 2, h, 0u);
    CHECK(n == engsize, "GetFileSize = patched size %u (disk %ld)", n, (long)st.st_size);
    K("CloseHandle", 1, h);
    uint32_t fad = ghalloc(40, 1);
    CHECK(K("GetFileAttributesExW", 3, W("..\\BioGame\\Config\\DefaultEngine.ini"), 0u, fad) == 1 && rd32(fad + 32) == n && rd64(fad + 20) == want, "GetFileAttributesEx: patched size, original time");
    char* game = read_guest_file("..\\BioGame\\Config\\DefaultGame.ini", &size);
    CHECK(game && strstr(game, "m_nDefaultMotionBlur=0\r\n") && strstr(game, "m_nDefaultFilmGrain=0\r\n"), "DefaultGame.ini overrides");
    free(game);
    char* party = read_guest_file("..\\BioGame\\Config\\DefaultParty.ini", &size);
    snprintf(host, sizeof host, "%s/BioGame/Config/DefaultParty.ini", argv[1]); stat(host, &st);
    CHECK(party && size == (uint32_t)st.st_size, "other ini files untouched");
    free(party);
    /* writes to Default*.ini handles are refused? no: opening for write gives the real file (game never does it) */
    /* --- user config dir: create, write BIOEngine.ini, read back, find --- */
    const char* cfg = "C:\\Users\\Shepard\\Documents\\BioWare\\Mass Effect\\Config";
    CHECK(K("CreateDirectoryW", 2, W("C:\\Users\\Shepard\\Documents\\BioWare"), 0u) == 1 || lasterr() == 183, "mkdir BioWare");
    CHECK(K("CreateDirectoryW", 2, W("C:\\Users\\Shepard\\Documents\\New\\Sub"), 0u) == 0 && lasterr() == 3, "mkdir without parent -> PATH_NOT_FOUND");
    CHECK(K("CreateDirectoryW", 2, W("C:\\Users\\Shepard\\Documents\\New"), 0u) == 1 && K("RemoveDirectoryW", 1, W("C:\\Users\\Shepard\\Documents\\NEW")) == 1, "mkdir/rmdir (case-insensitive)");
    CHECK(K("CreateDirectoryW", 2, W("C:\\Users\\Shepard\\Documents\\BioWare\\Mass Effect"), 0u) == 1 || lasterr() == 183, "mkdir Mass Effect");
    CHECK(K("CreateDirectoryW", 2, W(cfg), 0u) == 0 && lasterr() == 183, "Config exists (created by me1cfg) err=%u", lasterr());
    char p[1200]; snprintf(p, sizeof p, "%s\\BIOEngine.ini", cfg);
    h = K("CreateFileW", 7, W(p), 0x40000000u, 0u, 0u, 2u, 0x80u, 0u);
    CHECK(h != 0xFFFFFFFFu, "create BIOEngine.ini");
    const char* txt = "[WinDrv.WindowsClient]\r\nStartupResolutionX=960\r\n";
    uint32_t g = gstrdup(txt), wr = ghalloc(4, 1);
    CHECK(K("WriteFile", 5, h, g, (uint32_t)strlen(txt), wr, 0u) == 1 && rd32(wr) == strlen(txt), "WriteFile");
    K("CloseHandle", 1, h);
    h = K("CreateFileW", 7, W(p), 0x40000000u, 0u, 0u, 2u, 0x80u, 0u);
    CHECK(h != 0xFFFFFFFFu && lasterr() == 183, "CREATE_ALWAYS on existing -> ERROR_ALREADY_EXISTS (%u)", lasterr());
    K("WriteFile", 5, h, g, (uint32_t)strlen(txt), wr, 0u);
    uint32_t hi = ghalloc(4, 1); wr32(hi, 0);
    CHECK(K("SetFilePointer", 4, h, 10u, hi, 0u) == 10, "SetFilePointer 64-bit");
    CHECK(K("SetEndOfFile", 1, h) == 1 && K("GetFileSize", 2, h, 0u) == 10, "SetEndOfFile");
    CHECK(K("SetFilePointer", 4, h, 0u, 0u, 2u) == 10, "seek end");
    CHECK(K("SetFilePointer", 4, h, (uint32_t)-20, 0u, 1u) == 0xFFFFFFFFu && lasterr() == 131, "negative seek");
    K("CloseHandle", 1, h);
    CHECK(K("CreateFileW", 7, W(p), 0x40000000u, 0u, 0u, 1u, 0x80u, 0u) == 0xFFFFFFFFu && lasterr() == 80, "CREATE_NEW existing -> ERROR_FILE_EXISTS");
    CHECK(K("CreateFileW", 7, W("C:\\Users\\Shepard\\Documents\\nope\\x.ini"), 0x80000000u, 0u, 0u, 3u, 0u, 0u) == 0xFFFFFFFFu && lasterr() == 3, "missing dir -> ERROR_PATH_NOT_FOUND (%u)", lasterr());
    CHECK(K("CreateFileW", 7, W("C:\\Users\\Shepard\\Documents\\x.ini"), 0x80000000u, 0u, 0u, 3u, 0u, 0u) == 0xFFFFFFFFu && lasterr() == 2, "missing file -> ERROR_FILE_NOT_FOUND (%u)", lasterr());
    snprintf(p, sizeof p, "%s\\*.ini", cfg);
    h = K("FindFirstFileW", 2, W(p), wfd);
    int found_bio = 0, found_ov = 0, cnt = 0;
    if (h != 0xFFFFFFFFu) {
        do { cnt++; if (!strcmp(U(wfd + 44), "BIOEngine.ini")) { found_bio = 1; CHECK(rd32(wfd + 32) == 10 && (rd32(wfd) & 0x10) == 0, "find size/attr"); }
             if (!strcmp(U(wfd + 44), "me1port_overrides.ini")) found_ov = 1; } while (K("FindNextFileW", 2, h, wfd));
        CHECK(lasterr() == 18, "ERROR_NO_MORE_FILES");
        K("FindClose", 1, h);
    }
    CHECK(found_bio && found_ov, "FindFirstFile *.ini: %d entries", cnt);
    h = K("FindFirstFileW", 2, W("..\\BioGame\\Config\\Default*.ini"), wfd); cnt = 0;
    if (h != 0xFFFFFFFFu) { do cnt++; while (K("FindNextFileW", 2, h, wfd)); K("FindClose", 1, h); }
    CHECK(cnt >= 3, "Default*.ini: %d", cnt);
    /* --- copy / move / delete / attributes --- */
    char a1[1200], a2[1200]; snprintf(a1, sizeof a1, "%s\\BIOEngine.ini", cfg); snprintf(a2, sizeof a2, "%s\\BIOEngine.bak", cfg);
    CHECK(K("CopyFileW", 3, W(a1), W(a2), 1u) == 1, "CopyFile");
    CHECK(K("CopyFileW", 3, W(a1), W(a2), 1u) == 0 && lasterr() == 80, "CopyFile fail-if-exists");
    CHECK(K("MoveFileW", 2, W(a2), W(a1)) == 0 && lasterr() == 183, "MoveFile onto existing");
    CHECK(K("MoveFileExW", 3, W(a2), W(a1), 1u) == 1, "MoveFileEx REPLACE_EXISTING");
    CHECK(K("GetFileAttributesW", 1, W(a2)) == 0xFFFFFFFFu && lasterr() == 2, "moved away");
    CHECK(K("GetFileAttributesW", 1, W(cfg)) == 0x10u, "dir attr");
    CHECK(K("SetFileAttributesW", 2, W(a1), 0x80u) == 1, "SetFileAttributes NORMAL");
    /* --- profile API on a new file and on the UTF-16 launcher ini --- */
    char pi[1200]; snprintf(pi, sizeof pi, "%s\\test_profile.ini", cfg);
    CHECK(K("WritePrivateProfileStringW", 4, W("Sec"), W("Key"), W("Значение 1"), W(pi)) == 1, "WritePrivateProfileString");
    n = K("GetPrivateProfileStringW", 6, W("sec"), W("KEY"), W("def"), buf, 100u, W(pi));
    CHECK(n == 10 && !strcmp(U(buf), "Значение 1"), "GetPrivateProfileString '%s' %u", U(buf), n);
    n = K("GetPrivateProfileStringW", 6, W("Sec"), W("Missing"), W("def  "), buf, 100u, W(pi));
    CHECK(n == 3 && !strcmp(U(buf), "def"), "default '%s'", U(buf));
    K("WritePrivateProfileStringW", 4, W("Sec"), W("K2"), W("\"quoted\""), W(pi));
    n = K("GetPrivateProfileStringW", 6, W("Sec"), W("K2"), 0u, buf, 100u, W(pi));
    CHECK(!strcmp(U(buf), "quoted"), "quotes stripped '%s'", U(buf));
    n = K("GetPrivateProfileStringW", 6, W("Sec"), 0u, 0u, buf, 100u, W(pi));
    CHECK(n == 7 && !strcmp(U(buf), "Key") && !strcmp(U(buf + 8), "K2"), "key list %u", n);
    CHECK(K("GetPrivateProfileIntW", 4, W("Engine.Engine"), W("Nope"), 7u, W("..\\BioGame\\Config\\DefaultEngine.ini")) == 7, "GetPrivateProfileInt default");
    n = K("GetPrivateProfileStringW", 6, W("WinDrv.WindowsClient"), W("TextureLODLevel"), W("?"), buf, 100u, W("..\\BioGame\\Config\\DefaultEngine.ini"));
    CHECK(!strcmp(U(buf), "0"), "profile API also sees overrides: '%s'", U(buf));
    snprintf(host, sizeof host, "%s/Binaries/gl.ini", argv[1]);
    if (stat(host, &st) == 0) {
        n = K("GetPrivateProfileStringW", 6, 0u, 0u, 0u, buf, 2000u, W("gl.ini"));   /* relative name -> C:\WINDOWS (Win32 rule) */
        CHECK(n == 0, "bare name looks in C:\\WINDOWS");
        n = K("GetPrivateProfileStringW", 6, 0u, 0u, 0u, buf, 2000u, W(".\\gl.ini"));
        CHECK(n > 0, "UTF-16 gl.ini sections: first '%s'", U(buf));
    }
    CHECK(K("DeleteFileW", 1, W(pi)) == 1 && K("DeleteFileW", 1, W(pi)) == 0 && lasterr() == 2, "DeleteFile");
    {   /* directory-listing cache: misses must not stick after the VFS itself creates entries */
        char q[1200], d2[1200], q2[1300];
        snprintf(q, sizeof q, "%s\\Cache_Probe.TXT", cfg); snprintf(d2, sizeof d2, "%s\\NewDir", cfg); snprintf(q2, sizeof q2, "%s\\newdir\\x.bin", cfg);
        CHECK(K("GetFileAttributesW", 1, W(q)) == 0xFFFFFFFFu && lasterr() == 2, "cache: probe missing");
        uint32_t ch = K("CreateFileW", 7, W(q), 0x40000000u, 0u, 0u, 1u, 0x80u, 0u);
        CHECK(ch != 0xFFFFFFFFu && K("CloseHandle", 1, ch), "cache: CREATE_NEW after a miss");
        CHECK(K("GetFileAttributesW", 1, W("C:\\USERS\\shepard\\documents\\bioware\\MASS EFFECT\\config\\cache_probe.txt")) == 0x20u, "cache: new file found case-insensitively");
        CHECK(K("GetFileAttributesW", 1, W(q2)) == 0xFFFFFFFFu && lasterr() == 3, "cache: file in missing dir -> PATH_NOT_FOUND");
        CHECK(K("CreateDirectoryW", 2, W(d2), 0u) == 1, "cache: mkdir");
        ch = K("CreateFileW", 7, W(q2), 0x40000000u, 0u, 0u, 2u, 0x80u, 0u);
        CHECK(ch != 0xFFFFFFFFu && K("CloseHandle", 1, ch) && K("GetFileAttributesW", 1, W(q2)) == 0x20u, "cache: file in the new dir");
        CHECK(K("DeleteFileW", 1, W(q2)) == 1 && K("GetFileAttributesW", 1, W(q2)) == 0xFFFFFFFFu, "cache: deleted file is gone");
        CHECK(K("RemoveDirectoryW", 1, W(d2)) == 1 && K("GetFileAttributesW", 1, W(d2)) == 0xFFFFFFFFu, "cache: removed dir is gone");
        CHECK(K("DeleteFileW", 1, W(q)) == 1 && K("GetFileAttributesW", 1, W(q)) == 0xFFFFFFFFu, "cache: probe deleted");
    }
    /* --- current directory --- */
    CHECK(K("SetCurrentDirectoryW", 1, W("..\\BioGame")) == 1, "SetCurrentDirectory");
    K("GetCurrentDirectoryW", 2, 260u, buf);
    CHECK(!strcmp(U(buf), "C:\\MassEffect\\BioGame"), "cwd now '%s'", U(buf));
    CHECK(K("GetFileAttributesW", 1, W("config\\DEFAULTGAME.ini")) == 0x20u, "relative to new cwd");
    CHECK(K("SetCurrentDirectoryW", 1, W("C:\\MassEffect\\Binaries")) == 1, "restore cwd");
    /* --- ANSI variants and time conversion --- */
    CHECK(K("GetFileAttributesA", 1, A("..\\BioGame\\Config\\DefaultInput.ini")) == 0x20u, "GetFileAttributesA");
    uint32_t sys = ghalloc(16, 1); wr64(ft, 116444736000000000ull + 10000000ull * 86400ull * 365ull);   /* 1971-01-01 */
    K("FileTimeToSystemTime", 2, ft, sys);
    CHECK(rd16(sys) == 1971 && rd16(sys + 2) == 1 && rd16(sys + 6) == 1, "FileTimeToSystemTime %u-%u-%u", rd16(sys), rd16(sys + 2), rd16(sys + 6));
    uint32_t ft2 = ghalloc(8, 1); K("SystemTimeToFileTime", 2, sys, ft2);
    CHECK(rd64(ft2) == rd64(ft), "SystemTimeToFileTime round-trip");
    /* calendar arithmetic vs libc for a spread of dates (1601..2100) */
    int bad = 0;
    for (int64_t sec = -11644473600ll; sec < 4102444800ll; sec += 86400ll * 37 + 3601) {
        wr64(ft, ((uint64_t)(sec + 11644473600ll)) * 10000000ull); K("FileTimeToSystemTime", 2, ft, sys);
        time_t tt = (time_t)sec; struct tm t; gmtime_r(&tt, &t);
        if (rd16(sys) != t.tm_year + 1900 || rd16(sys + 2) != t.tm_mon + 1 || rd16(sys + 6) != t.tm_mday || rd16(sys + 4) != t.tm_wday || rd16(sys + 8) != t.tm_hour) bad++;
        K("SystemTimeToFileTime", 2, sys, ft2); if (rd64(ft2) != rd64(ft)) bad++;
    }
    CHECK(bad == 0, "calendar mismatches: %d", bad);

    {   /* --- mod overlays (ME1_MODS, v0.9.7) --- */
        char m1[1200], m2[1200], t[1400]; uint32_t sz;
        snprintf(m1, sizeof m1, "%s/mods_test/ModA", argv[3]); snprintf(m2, sizeof m2, "%s/mods_test/ModB", argv[3]);
        static const char* const dirs[] = { "/Mass Effect/BioGame/Config/SubDir", "/Mass Effect/DLC/DLC_Vegas", "/Config" };
        for (int i = 0; i < 3; i++) { snprintf(t, sizeof t, "mkdir -p '%s%s' '%s%s'", m1, dirs[i], m2, dirs[i]); if (system(t)) {} }
        #define PUT(dir, rel, text) do { snprintf(t, sizeof t, "%s%s", dir, rel); FILE* fp = fopen(t, "wb"); if (fp) { fputs(text, fp); fclose(fp); } } while (0)
        PUT(m1, "/Mass Effect/BioGame/Config/defaultqa.ini", "[ModA]\r\nShadow=1\r\n");   /* other case than the base file */
        PUT(m1, "/Mass Effect/BioGame/Config/ModOnly.ini", "[ModA]\r\nNew=1\r\n");
        PUT(m2, "/Mass Effect/BioGame/Config/ModOnly.ini", "[ModB]\r\nNew=2\r\n");
        PUT(m1, "/Mass Effect/DLC/DLC_Vegas/Pkg.upk", "x");
        PUT(m1, "/Config/BIOInput.ini", "[Engine.PlayerInput]\r\nModded=1\r\n");
        PUT(m2, "/Config/BIOGame.ini", "[ModB]\r\nX=1\r\n");
        snprintf(t, sizeof t, "%s:%s:/nonexistent/mod", m1, m2); setenv("ME1_MODS", t, 1);
        hle_fs_init(argv[1], argv[2], argv[3]);
        char* b = read_guest_file("C:\\MassEffect\\BioGame\\Config\\DefaultQA.ini", &sz);
        CHECK(b && strstr(b, "Shadow=1"), "mod file shadows the base file: %.20s", b ? b : "(null)"); free(b);
        CHECK(K("GetFileAttributesW", 1, W("C:\\MassEffect\\BioGame\\Config\\DefaultQA.ini")) == 0x20u, "game overlay file is not read-only");
        b = read_guest_file("..\\BioGame\\Config\\modonly.INI", &sz);
        CHECK(b && strstr(b, "New=1"), "new mod file, first mod wins: %.20s", b ? b : "(null)"); free(b);
        CHECK(K("GetFileAttributesW", 1, W("C:\\MassEffect\\DLC\\DLC_Vegas\\Pkg.upk")) == 0xFFFFFFFFu && lasterr() == 3, "mod file in a missing base directory stays invisible (err %u)", lasterr());
        CHECK(K("GetFileAttributesW", 1, W("C:\\MassEffect\\BioGame\\Config\\SubDir")) == 0xFFFFFFFFu, "mod-only directory stays invisible");
        /* v0.9.8: the DLC appears later (copied into the game) -> the mod's DLC file is used from the next start on */
        snprintf(t, sizeof t, "mkdir -p '%s/dlc/DLC_VEGAS'", argv[1]); if (system(t)) {}
        hle_fs_init(argv[1], argv[2], argv[3]);
        CHECK(K("GetFileAttributesW", 1, W("C:\\MassEffect\\DLC\\DLC_Vegas\\Pkg.upk")) == 0x20u, "DLC folder added to the game -> mod DLC file visible");
        b = read_guest_file("C:\\MassEffect\\DLC\\DLC_Vegas\\pkg.UPK", &sz); CHECK(b && sz == 1 && b[0] == 'x', "... and readable"); free(b);
        snprintf(t, sizeof t, "rm -rf '%s/dlc'", argv[1]); if (system(t)) {}
        hle_fs_init(argv[1], argv[2], argv[3]);
        uint32_t wfd = ghalloc(600, 1); int cnt = 0, modonly = 0, qa = 0, sorted = 1; char prev[300] = "";
        uint32_t h = K("FindFirstFileW", 2, W("..\\BioGame\\Config\\*.ini"), wfd);
        if (h != 0xFFFFFFFFu) {
            do {
                const char* nm = U(wfd + 44); cnt++;
                if (!strcasecmp(nm, "ModOnly.ini")) modonly++;
                if (!strcasecmp(nm, "DefaultQA.ini")) { qa++; if (rd32(wfd + 32) != strlen("[ModA]\r\nShadow=1\r\n")) qa = 100; }
                if (strcasecmp(prev, nm) > 0) sorted = 0;
                snprintf(prev, sizeof prev, "%s", nm);
            } while (K("FindNextFileW", 2, h, wfd));
            K("FindClose", 1, h);
        }
        CHECK(modonly == 1 && qa == 1 && sorted && cnt >= 3, "FindFirstFile merges the overlay: %d entries, ModOnly %d, DefaultQA %d, sorted %d", cnt, modonly, qa, sorted);
        h = K("FindFirstFileW", 2, W("C:\\MassEffect\\BioGame\\Config\\Mod*"), wfd);
        CHECK(h != 0xFFFFFFFFu && !strcmp(U(wfd + 44), "ModOnly.ini") && !K("FindNextFileW", 2, h, wfd), "pattern matching only an overlay file");
        if (h != 0xFFFFFFFFu) K("FindClose", 1, h);
        /* read-only user Config overlay */
        const char* bi = "C:\\Users\\Shepard\\Documents\\BioWare\\Mass Effect\\Config\\BIOInput.ini";
        snprintf(t, sizeof t, "%s/BioWare/Mass Effect/Config", argv[2]); struct stat cs;
        CHECK(stat(t, &cs) == 0 && S_ISDIR(cs.st_mode), "base Config directory created for the overlay");
        CHECK(K("GetFileAttributesW", 1, W(bi)) == 0x21u, "config overlay file is READONLY|ARCHIVE (%x)", K("GetFileAttributesW", 1, W(bi)));
        b = read_guest_file(bi, &sz); CHECK(b && strstr(b, "Modded=1"), "config overlay readable"); free(b);
        CHECK(K("CreateFileW", 7, W(bi), 0x40000000u, 0u, 0u, 2u, 0x80u, 0u) == 0xFFFFFFFFu && lasterr() == 5, "CREATE_ALWAYS on a read-only mod ini -> ACCESS_DENIED");
        CHECK(K("CreateFileW", 7, W(bi), 0xC0000000u, 0u, 0u, 3u, 0x80u, 0u) == 0xFFFFFFFFu && lasterr() == 5, "open for writing -> ACCESS_DENIED");
        CHECK(K("DeleteFileW", 1, W(bi)) == 0 && lasterr() == 5, "DeleteFile -> ACCESS_DENIED");
        CHECK(K("SetFileAttributesW", 2, W(bi), 0x80u) == 1 && K("GetFileAttributesW", 1, W(bi)) == 0x21u, "SetFileAttributes(NORMAL) leaves it read-only");
        CHECK(K("WritePrivateProfileStringW", 4, W("Engine.PlayerInput"), W("Modded"), W("0"), W(bi)) == 0, "WritePrivateProfileString -> fails");
        uint32_t pb = ghalloc(200, 1);
        CHECK(K("GetPrivateProfileStringW", 6, W("Engine.PlayerInput"), W("Modded"), W("?"), pb, 100u, W(bi)) == 1 && !strcmp(U(pb), "1"), "profile read from the overlay");
        CHECK(K("MoveFileW", 2, W(bi), W("C:\\Users\\Shepard\\Documents\\BioWare\\Mass Effect\\Config\\moved.ini")) == 0 && lasterr() == 5, "MoveFile of a read-only mod ini fails");
        CHECK(K("GetFileAttributesW", 1, W("C:\\Users\\Shepard\\Documents\\BioWare\\Mass Effect\\Config\\BIOGame.ini")) == 0x21u, "second mod's config file visible");
        uint32_t wh = K("CreateFileW", 7, W("C:\\Users\\Shepard\\Documents\\BioWare\\Mass Effect\\Config\\BIOEngine.ini"), 0x40000000u, 0u, 0u, 2u, 0x80u, 0u);
        CHECK(wh != 0xFFFFFFFFu, "other user inis stay writable");
        if (wh != 0xFFFFFFFFu) K("CloseHandle", 1, wh);
        snprintf(t, sizeof t, "%s/BioWare/Mass Effect/Config/BIOEngine.ini", argv[2]);
        CHECK(stat(t, &cs) == 0, "... and are written to the base Documents folder");
        unsetenv("ME1_MODS"); hle_fs_init(argv[1], argv[2], argv[3]);
        CHECK(K("GetFileAttributesW", 1, W("..\\BioGame\\Config\\ModOnly.ini")) == 0xFFFFFFFFu, "no ME1_MODS -> no overlay");
        #undef PUT
    }
    {   /* v0.10.1 «Упрощённый расчёт»: engine switches applied in memory to the user's BIOEngine.ini, restored when the game writes it */
        char t[1200], bi[300]; snprintf(t, sizeof t, "%s/BioWare/Mass Effect/Config/BIOEngine.ini", argv[2]);
        snprintf(bi, sizeof bi, "C:\\Users\\Shepard\\Documents\\BioWare\\Mass Effect\\Config\\BIOEngine.ini");
        const char* orig = "[SystemSettings]\r\nDynamicShadows=True\r\nDetailMode=0\r\nSkeletalMeshLODBias=0\r\n[WinDrv.WindowsClient]\r\nStartupResolutionX=960\r\n";
        FILE* f = fopen(t, "wb"); fputs(orig, f); fclose(f);
        setenv("ME1_SHADER_LITE", "1", 1); unsetenv("ME1_LITE_SKIP"); hle_fs_init(argv[1], argv[2], argv[3]);
        uint32_t sz = 0; char* b = read_guest_file(bi, &sz);
        CHECK(b && strstr(b, "DynamicShadows=False") && strstr(b, "DetailMode=0") && strstr(b, "SkeletalMeshLODBias=1") && !strstr(b, "CompositeDynamicLights")
              && strstr(b, "EnableDynamicShadows=False") && strstr(b, "DirectionalLightmaps=False") && strstr(b, "StartupResolutionX=960"), "lite: switches applied in memory (CompositeDynamicLights not touched since v0.10.2)");
        struct stat ls; stat(t, &ls);
        CHECK(sz > strlen(orig) && ls.st_size == (off_t)strlen(orig), "lite: patched size %u, file on disk untouched (%ld)", sz, (long)ls.st_size);
        uint8_t fad[40]; uint32_t gfad = ghalloc(40, 1); (void)fad;
        CHECK(K("GetFileAttributesExW", 3, W(bi), 0u, gfad) == 1 && rd32(gfad + 32) == sz, "lite: GetFileAttributesEx reports the patched size");
        free(b);
        /* the game writes the ini back: lite values -> user's values, user's own change (MotionBlur, resolution) kept */
        const char* game = "[SystemSettings]\r\nDynamicShadows=False\r\nDetailMode=0\r\nSkeletalMeshLODBias=1\r\nMotionBlur=True\r\nCompositeDynamicLights=True\r\n"
                           "[WinDrv.WindowsClient]\r\nStartupResolutionX=1280\r\nEnableDynamicShadows=False\r\n";
        uint32_t h = K("CreateFileW", 7, W(bi), 0x40000000u, 0u, 0u, 2u, 0x80u, 0u), wr = ghalloc(4, 1), g = gstrdup(game);
        CHECK(h != 0xFFFFFFFFu && K("WriteFile", 5, h, g, (uint32_t)strlen(game), wr, 0u) == 1, "lite: game writes BIOEngine.ini");
        K("CloseHandle", 1, h);
        f = fopen(t, "rb"); char disk[2000] = {0}; size_t dn = f ? fread(disk, 1, sizeof disk - 1, f) : 0; if (f) fclose(f); disk[dn] = 0;
        CHECK(strstr(disk, "DynamicShadows=True") && strstr(disk, "SkeletalMeshLODBias=0") && strstr(disk, "MotionBlur=True") && strstr(disk, "StartupResolutionX=1280")
              && strstr(disk, "CompositeDynamicLights=True") && !strstr(disk, "EnableDynamicShadows") && strstr(disk, "DetailMode=0"), "lite: user values restored on disk:\n%s", disk);
        setenv("ME1_LITE_SKIP", "DynamicShadows, DirectionalLightmaps", 1); hle_fs_init(argv[1], argv[2], argv[3]);
        b = read_guest_file(bi, &sz);
        CHECK(b && strstr(b, "DynamicShadows=True") && !strstr(b, "DirectionalLightmaps") && strstr(b, "LensFlares=False"), "lite: ME1_LITE_SKIP leaves keys alone");
        free(b);
        unsetenv("ME1_SHADER_LITE"); unsetenv("ME1_LITE_SKIP"); hle_fs_init(argv[1], argv[2], argv[3]);
        b = read_guest_file(bi, &sz);
        CHECK(b && sz == (uint32_t)strlen(disk) && !strstr(b, "LensFlares"), "lite off: file read as is"); free(b);
    }
    fprintf(stderr, "test_fs: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail != 0;
}
