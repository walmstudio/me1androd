/* JNI bridge for the Android launcher (com.me1port.launcher.Native).
 * Settings use the same code as the me1cfg CLI; check() reads the game config through the runtime's
 * Win32 file layer (HLE thunks + VFS), i.e. exactly the way the recompiled MassEffect.exe will. */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "me1settings.h"
#include "ini.h"
#include "rt.h"
#include "hle.h"

static ME1Config g_cfg;
static int g_loaded, g_rt_ready;

static jstring js(JNIEnv* e, const char* s) { return (*e)->NewStringUTF(e, s ? s : ""); }

static const char* jget(JNIEnv* e, jstring j) { return j ? (*e)->GetStringUTFChars(e, j, NULL) : ""; }
static void jrel(JNIEnv* e, jstring j, const char* s) { if (j) (*e)->ReleaseStringUTFChars(e, j, s); }

#define FN(name) Java_com_me1port_launcher_Native_##name

JNIEXPORT jstring JNICALL FN(schema)(JNIEnv* e, jclass cl) {
    char* s = me1cfg_schema_json(); jstring r = js(e, s); free(s); return r;
}
JNIEXPORT jstring JNICALL FN(load)(JNIEnv* e, jclass cl, jstring jg, jstring jd) {
    const char* g = jget(e, jg); const char* d = jget(e, jd);
    me1cfg_load(&g_cfg, g, d); g_loaded = 1;
    jrel(e, jg, g); jrel(e, jd, d);
    char* s = me1cfg_dump(&g_cfg); jstring r = js(e, s); free(s); return r;
}
JNIEXPORT jstring JNICALL FN(dump)(JNIEnv* e, jclass cl) {
    if (!g_loaded) return js(e, "");
    char* s = me1cfg_dump(&g_cfg); jstring r = js(e, s); free(s); return r;
}
/* returns "" on success or an error message */
JNIEXPORT jstring JNICALL FN(set)(JNIEnv* e, jclass cl, jstring jid, jstring jv) {
    if (!g_loaded) return js(e, "настройки не загружены");
    const char* id = jget(e, jid); const char* v = jget(e, jv); char err[512] = "";
    me1cfg_set(&g_cfg, id, v, err, sizeof err);
    jrel(e, jid, id); jrel(e, jv, v);
    return js(e, err);
}
JNIEXPORT jstring JNICALL FN(preset)(JNIEnv* e, jclass cl, jstring jid) {
    if (!g_loaded) return js(e, "настройки не загружены");
    const char* id = jget(e, jid); char err[512] = "";
    me1cfg_preset(&g_cfg, id, err, sizeof err);
    jrel(e, jid, id);
    return js(e, err);
}
/* automatic preset: "<tier>\n<report>" (tier -1 = error); apply = 0 only classifies */
JNIEXPORT jstring JNICALL FN(autoPreset)(JNIEnv* e, jclass cl, jstring jgpu, jstring jsoc, jint ram, jint cores, jint mhz, jint sw, jint sh, jint apply) {
    const char* g = jget(e, jgpu); const char* so = jget(e, jsoc);
    ME1Device d = { g, so, ram, cores, mhz, sw, sh };
    char rep[600], out[640], res[32]; int fps, t;
    if (apply && !g_loaded) { t = -1; snprintf(rep, sizeof rep, "настройки не загружены"); }
    else if (apply) t = me1cfg_auto(&g_cfg, &d, rep, sizeof rep);
    else t = me1cfg_classify(&d, rep, sizeof rep, &fps, res, sizeof res);
    jrel(e, jgpu, g); jrel(e, jsoc, so);
    snprintf(out, sizeof out, "%d\n%s", t, rep);
    return js(e, out);
}
JNIEXPORT void JNICALL FN(reset)(JNIEnv* e, jclass cl) { if (g_loaded) me1cfg_reset(&g_cfg); }
/* "OK\n<details>" or "ERR\n<message>" */
JNIEXPORT jstring JNICALL FN(save)(JNIEnv* e, jclass cl) {
    if (!g_loaded) return js(e, "ERR\nнастройки не загружены");
    char msg[4096], out[4200];
    int rc = me1cfg_save(&g_cfg, msg, sizeof msg);
    snprintf(out, sizeof out, "%s\n%s", rc ? "ERR" : "OK", msg);
    return js(e, out);
}
/* bit 0: BioGame/Config/DefaultEngine.ini, 1: Binaries/MassEffect.exe, 2: BioGame/CookedPC, 3: Binaries/PhysXCore.dll */
static int exists_ci(const char* win) { char h[1024]; return hle_fs_host_path(win, h, sizeof h); }
static int game_status(void) {
    return exists_ci("C:\\MassEffect\\BioGame\\Config\\DefaultEngine.ini") | exists_ci("C:\\MassEffect\\Binaries\\MassEffect.exe") << 1
         | exists_ci("C:\\MassEffect\\BioGame\\CookedPC") << 2 | exists_ci("C:\\MassEffect\\Binaries\\PhysXCore.dll") << 3;
}
JNIEXPORT jint JNICALL FN(gameStatus)(JNIEnv* e, jclass cl, jstring jg, jstring jd) {
    const char* g = jget(e, jg); const char* d = jget(e, jd);
    hle_fs_init(g, d, NULL); int r = game_status();
    jrel(e, jg, g); jrel(e, jd, d); return r;
}

/* ---- self-check: read the config through CreateFileW/ReadFile thunks like the game ---- */
static size_t cat(char* o, size_t k, size_t cap, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static size_t cat(char* o, size_t k, size_t cap, const char* fmt, ...) {
    if (k >= cap) return k;
    va_list ap; va_start(ap, fmt); int n = vsnprintf(o + k, cap - k, fmt, ap); va_end(ap);
    return n < 0 ? k : (k + n < cap ? k + n : cap - 1);
}
static char* read_via_thunks(const char* winpath, size_t* n) {
    uint32_t a[7] = { hle_wstr(winpath), 0x80000000u, 1u, 0u, 3u, 0x80u, 0u };
    uint32_t h = hle_call("kernel32.dll", "CreateFileW", 7, a);
    if (h == 0xFFFFFFFFu) return NULL;
    uint32_t b2[2] = { h, 0 }; uint32_t size = hle_call("kernel32.dll", "GetFileSize", 2, b2);
    uint32_t buf = ghalloc(size + 1, 1), got = ghalloc(4, 1);
    uint32_t r[5] = { h, buf, size, got, 0 }; hle_call("kernel32.dll", "ReadFile", 5, r);
    uint32_t cl[1] = { h }; hle_call("kernel32.dll", "CloseHandle", 1, cl);
    char* out = malloc(rd32(got) + 1); memcpy(out, M + buf, rd32(got)); out[rd32(got)] = 0;
    *n = rd32(got); ghfree(buf); ghfree(got);
    return out;
}
char* me1port_check(const char* game, const char* docs) {
    size_t cap = 16384, k = 0; char* o = malloc(cap); o[0] = 0;
    if (!g_rt_ready) { rt_init(); g_rt_ready = 1; }
    hle_fs_init(game, docs, NULL);
    int st = game_status();
    k = cat(o, k, cap, "Папка игры: %s\n", game);
    k = cat(o, k, cap, "  %s BioGame\\Config\\DefaultEngine.ini\n  %s Binaries\\MassEffect.exe\n  %s BioGame\\CookedPC\n  %s Binaries\\PhysXCore.dll\n",
            st & 1 ? "✓" : "✗", st & 2 ? "✓" : "✗", st & 4 ? "✓" : "✗", st & 8 ? "✓" : "✗");
    uint32_t path = ghalloc(520, 1); uint32_t sh[5] = { 0, 0x8005u, 0, 0, path };
    uint32_t hr = hle_call("shell32.dll", "SHGetFolderPathW", 5, sh);
    char wdocs[600], hdocs[1024]; hle_wstr_utf8(path, wdocs, sizeof wdocs); hle_fs_host_path(wdocs, hdocs, sizeof hdocs);
    k = cat(o, k, cap, "Мои документы (для игры): %s -> %s%s\n", wdocs, hdocs, hr ? " (ошибка)" : "");
    /* which file wins: user BIOEngine.ini (if the game already generated it) or Default*.ini + overrides */
    char ubio[700]; snprintf(ubio, sizeof ubio, "%s\\BioWare\\Mass Effect\\Config\\BIOEngine.ini", wdocs);
    size_t n; char* txt = read_via_thunks(ubio, &n); const char* src = "BIOEngine.ini (создан игрой)";
    if (!txt) { txt = read_via_thunks("..\\BioGame\\Config\\DefaultEngine.ini", &n); src = "DefaultEngine.ini + настройки порта"; }
    if (!txt) { k = cat(o, k, cap, "Конфигурация игры не найдена — скопируйте игру в папку выше.\n"); return o; }
    Ini ini; ini_load_mem(&ini, txt, n); free(txt);
    k = cat(o, k, cap, "Игра прочитает (%s):\n", src);
    static const char* const Q[][3] = {
        { "WinDrv.WindowsClient", "StartupResolutionX", "Разрешение X" }, { "WinDrv.WindowsClient", "StartupResolutionY", "Разрешение Y" },
        { "WinDrv.WindowsClient", "TextureLODLevel", "Детализация текстур" }, { "WinDrv.WindowsClient", "FilterLevel", "Фильтрация" },
        { "WinDrv.WindowsClient", "EnableDynamicShadows", "Динамические тени" }, { "WinDrv.WindowsClient", "UseVSync", "V-Sync" },
        { "SystemSettings", "ParticleLODBias", "ParticleLODBias" }, { "Engine.GameEngine", "bUsePostProcessEffects", "Постобработка" },
        { "D3DDrv.D3DRenderDevice", "MaxTextureSize", "MaxTextureSize" }, { "ISACTAudio.ISACTAudioDevice", "MaxChannels", "Каналов звука" },
        { "Engine.Client", "DisplayGamma", "Гамма" }, { "Engine.Engine", "Language", "Язык" } };
    for (unsigned i = 0; i < sizeof Q / sizeof Q[0]; i++) {
        char v[128]; if (!ini_get(&ini, Q[i][0], Q[i][1], v, sizeof v)) strcpy(v, "—");
        int w = 0; for (const unsigned char* q = (const unsigned char*)Q[i][2]; *q; q++) w += (*q & 0xC0) != 0x80;   /* UTF-8 aware column */
        k = cat(o, k, cap, "  %s%*s %s\n", Q[i][2], w < 22 ? 22 - w : 0, "", v);
    }
    ini_free(&ini);
    return o;
}
JNIEXPORT jstring JNICALL FN(check)(JNIEnv* e, jclass cl, jstring jg, jstring jd) {
    const char* g = jget(e, jg); const char* d = jget(e, jd);
    char* s = me1port_check(g, d); jstring r = js(e, s); free(s);
    jrel(e, jg, g); jrel(e, jd, d); return r;
}
JNIEXPORT jstring JNICALL FN(portOption)(JNIEnv* e, jclass cl, jstring jd, jstring jid) {
    const char* d = jget(e, jd); const char* id = jget(e, jid); char v[128] = "";
    me1cfg_port_get(d, id, v, sizeof v);
    jrel(e, jd, d); jrel(e, jid, id); return js(e, v);
}
