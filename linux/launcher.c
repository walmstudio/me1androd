/* me1-launcher: the desktop counterpart of the Android launcher (v1.1) - game folder, automatic preset, renderer,
 * every setting of MassEffectConfig.exe (settings/me1settings.c, the same code as me1cfg and the Android launcher),
 * controls and mods, extra environment, the PhoneModify generator, "what the game will see", the log of the last run;
 * starts me1-linux and comes back when the game exits. SDL2 + an immediate-mode UI (linux/ui.c), no other dependencies.
 *
 *   me1-launcher [--game DIR] [--docs DIR] [--lang CODE] [--play]
 *
 * Its own options: $XDG_CONFIG_HOME/me1port/launcher.ini; the game folder: $XDG_CONFIG_HOME/me1port/game_dir (shared
 * with me1-linux and linux/install.sh --game). */
#define _GNU_SOURCE
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "ui.h"
#include "i18n.h"
#include "gpuprobe.h"
#include "me1settings.h"
#include "ini.h"

#define VERSION "1.1.5"
extern const unsigned char me1a_loading[], me1a_icon[]; extern const unsigned int me1a_loading_size, me1a_icon_size;

/* --------------------------------------------------------------------------------------------- utils --- */
static int is_dir(const char* p) { struct stat st; return p && *p && stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char* p) { struct stat st; return p && *p && stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static void mkdirs(const char* path) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s", path);
    for (char* s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
static int ci_child(const char* dir, const char* name, int want_dir, char* out, size_t cap) {
    DIR* d = opendir(dir); struct dirent* de; int ok = 0; char p[PATH_MAX];
    if (!d) return 0;
    while (!ok && (de = readdir(d))) if (!strcasecmp(de->d_name, name)) {
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name); ok = want_dir ? is_dir(p) : is_file(p);
        if (ok && out) snprintf(out, cap, "%s", p);
    }
    closedir(d); return ok;
}
/* base + "A/B/c.ini", every component case-insensitive (Windows names) */
static int ci_path(const char* base, const char* rel, int want_dir, char* out, size_t cap) {
    char cur[PATH_MAX], comp[256], nx[PATH_MAX]; snprintf(cur, sizeof cur, "%s", base);
    const char* p = rel;
    while (*p) {
        const char* e = strchr(p, '/'); size_t n = e ? (size_t)(e - p) : strlen(p);
        snprintf(comp, sizeof comp, "%.*s", (int)n, p);
        if (!ci_child(cur, comp, e ? 1 : want_dir, nx, sizeof nx)) return 0;
        snprintf(cur, sizeof cur, "%s", nx);
        p += n; if (*p == '/') p++;
    }
    if (out) snprintf(out, cap, "%s", cur);
    return 1;
}
static const char* home(void) { const char* h = getenv("HOME"); return h && *h ? h : "/tmp"; }
static const char* pretty(const char* p) {   /* $HOME -> ~ (rotating buffers) */
    static char b[4][PATH_MAX]; static int k; char* o = b[k++ & 3];
    size_t hl = strlen(home());
    if (p && !strncmp(p, home(), hl) && (p[hl] == '/' || !p[hl])) snprintf(o, PATH_MAX, "~%s", p + hl); else snprintf(o, PATH_MAX, "%s", p ? p : "");
    return o;
}
static char* read_file(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (64 << 20)) { fclose(f); return NULL; }
    char* b = malloc((size_t)sz + 1); size_t r = fread(b, 1, (size_t)sz, f); fclose(f); b[r] = 0; if (n) *n = r; return b;
}
static void spawn_detached(char* const argv[]) {   /* xdg-open & co. */
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) { setsid(); int fd = open("/dev/null", O_RDWR); if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); } execvp(argv[0], argv); _exit(127); }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}
static void open_path(const char* p) { mkdirs(p); char* a[] = { "xdg-open", (char*)p, NULL }; spawn_detached(a); }
static int in_path(const char* prog) {
    const char* path = getenv("PATH"); if (!path) return 0;
    char buf[PATH_MAX]; const char* s = path;
    while (*s) { const char* e = strchr(s, ':'); size_t n = e ? (size_t)(e - s) : strlen(s);
        snprintf(buf, sizeof buf, "%.*s/%s", (int)n, s, prog); if (access(buf, X_OK) == 0) return 1; s += n; if (*s) s++; }
    return 0;
}
static char g_exedir[PATH_MAX];
static int near_exe(const char* name, char* out, size_t cap) {
    static const char* const sub[] = { "", "/../lib/me1port", "/../bin" };
    for (unsigned i = 0; i < sizeof sub / sizeof *sub; i++) { snprintf(out, cap, "%s%s/%s", g_exedir, sub[i], name); if (access(out, X_OK) == 0) return 1; }
    return 0;
}
/* growing string */
typedef struct { char* s; size_t n, cap; } Str;
static void sadd(Str* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void sadd(Str* b, const char* fmt, ...) {
    va_list ap;
    for (;;) {
        if (!b->cap) { b->cap = 1024; b->s = malloc(b->cap); b->s[0] = 0; }
        va_start(ap, fmt); int k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap); va_end(ap);
        if (k >= 0 && (size_t)k < b->cap - b->n) { b->n += (size_t)k; return; }
        b->cap = b->cap * 2 + (k > 0 ? (size_t)k : 0); b->s = realloc(b->s, b->cap);
    }
}

/* ------------------------------------------------------------------------------------------- state --- */
static char g_cfgdir[PATH_MAX], g_game[PATH_MAX], g_docs[PATH_MAX], g_me1linux[PATH_MAX];
static ME1Config C; static int g_dirty;
static struct {
    int lang; char renderer[16], fexact[8], window[24], mods[4096], env[4096]; int grab, vkdev, auto_done; float zoom;
} P = { .grab = 1, .vkdev = -1, .zoom = 1.0f };
static SDL_Window* g_win; static SDL_Renderer* g_r;
static SDL_Texture* g_banner; static int g_bw, g_bh;
static char g_autorep[1024];

static void prefs_path(char* out, size_t cap) { snprintf(out, cap, "%s/launcher.ini", g_cfgdir); }
static void prefs_load(void) {
    char p[PATH_MAX + 16]; prefs_path(p, sizeof p);
    strcpy(P.renderer, "auto"); strcpy(P.fexact, "auto"); strcpy(P.window, "auto");
    P.lang = -1;
    FILE* f = fopen(p, "r"); if (!f) return;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0; char* eq = strchr(line, '='); if (!eq) continue; *eq = 0; const char* v = eq + 1;
        if (!strcmp(line, "lang")) P.lang = i18n_find(v);
        else if (!strcmp(line, "renderer")) snprintf(P.renderer, sizeof P.renderer, "%s", v);
        else if (!strcmp(line, "float_emu")) snprintf(P.fexact, sizeof P.fexact, "%s", v);
        else if (!strcmp(line, "window")) snprintf(P.window, sizeof P.window, "%s", v);
        else if (!strcmp(line, "grab")) P.grab = atoi(v);
        else if (!strcmp(line, "vk_device")) P.vkdev = atoi(v);
        else if (!strcmp(line, "mods")) snprintf(P.mods, sizeof P.mods, "%s", v);
        else if (!strcmp(line, "env")) snprintf(P.env, sizeof P.env, "%s", v);
        else if (!strcmp(line, "zoom")) { P.zoom = (float)atof(v); if (P.zoom < 0.6f || P.zoom > 3) P.zoom = 1; }
        else if (!strcmp(line, "auto_done")) P.auto_done = atoi(v);
    }
    fclose(f);
}
static void prefs_save(void) {
    char p[PATH_MAX + 16], t[PATH_MAX + 32]; prefs_path(p, sizeof p); snprintf(t, sizeof t, "%s.tmp", p);
    mkdirs(g_cfgdir);
    FILE* f = fopen(t, "w"); if (!f) return;
    fprintf(f, "# me1-launcher %s\nlang=%s\nrenderer=%s\nfloat_emu=%s\nwindow=%s\ngrab=%d\nvk_device=%d\nmods=%s\nenv=%s\nzoom=%.2f\nauto_done=%d\n",
            VERSION, I18N_CODES[i18n_cur()], P.renderer, P.fexact, P.window, P.grab, P.vkdev, P.mods, P.env, P.zoom, P.auto_done);
    fclose(f); rename(t, p);
}
static int mod_on(const char* name) {
    size_t l = strlen(name); const char* s = P.mods;
    while (*s) { const char* e = strchr(s, '|'); size_t n = e ? (size_t)(e - s) : strlen(s); if (n == l && !strncmp(s, name, l)) return 1; s += n; if (*s) s++; }
    return 0;
}
static void mod_set(const char* name, int on) {
    char o[sizeof P.mods] = ""; size_t l = strlen(name), k = 0; const char* s = P.mods;
    while (*s) { const char* e = strchr(s, '|'); size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!(n == l && !strncmp(s, name, l)) && k + n + 2 < sizeof o) { if (k) o[k++] = '|'; memcpy(o + k, s, n); k += n; o[k] = 0; }
        s += n; if (*s) s++; }
    if (on && k + l + 2 < sizeof o) { if (k) o[k++] = '|'; memcpy(o + k, name, l); k += l; o[k] = 0; }
    snprintf(P.mods, sizeof P.mods, "%s", o); prefs_save();
}

/* ---------------------------------------------------------------------------------------- game files --- */
static struct { int ini, exe, cooked, physx, unc, vegas, found; } GS;
static int is_game(const char* d) { return d && *d && ci_path(d, "Binaries/MassEffect.exe", 0, NULL, 0); }
static void game_status(void) {
    memset(&GS, 0, sizeof GS);
    if (!*g_game) return;
    GS.ini = ci_path(g_game, "BioGame/Config/DefaultEngine.ini", 0, NULL, 0);
    GS.exe = ci_path(g_game, "Binaries/MassEffect.exe", 0, NULL, 0);
    GS.cooked = ci_path(g_game, "BioGame/CookedPC", 1, NULL, 0);
    GS.physx = ci_path(g_game, "Binaries/PhysXCore.dll", 0, NULL, 0);
    GS.unc = ci_path(g_game, "DLC/DLC_UNC", 1, NULL, 0);
    GS.vegas = ci_path(g_game, "DLC/DLC_Vegas", 1, NULL, 0);
    GS.found = GS.exe && GS.ini;
}
static void config_reload(void) { me1cfg_load(&C, *g_game ? g_game : ".", g_docs); g_dirty = 0; }
static void game_dir_save(void) {
    char p[PATH_MAX + 16]; snprintf(p, sizeof p, "%s/game_dir", g_cfgdir); mkdirs(g_cfgdir);
    FILE* f = fopen(p, "w"); if (f) { fprintf(f, "%s\n", g_game); fclose(f); }
}
static void game_dir_set(const char* d) {
    char r[PATH_MAX];
    if (!realpath(d, r)) snprintf(r, sizeof r, "%s", d);
    size_t n = strlen(r); while (n > 1 && r[n - 1] == '/') r[--n] = 0;
    snprintf(g_game, sizeof g_game, "%s", r);
    game_dir_save(); game_status(); config_reload();
}
/* candidates without scanning: Steam libraries, Wine/Lutris/Heroic/Bottles prefixes */
static int known_places(char out[][PATH_MAX], int max) {
    static const char* const steam[] = { "/.steam/steam", "/.local/share/Steam", "/.var/app/com.valvesoftware.Steam/.local/share/Steam", "/snap/steam/common/.local/share/Steam" };
    static const char* const pf[] = { "Program Files (x86)", "Program Files" };
    static const char* const sub[] = { "Mass Effect", "Steam/steamapps/common/Mass Effect", "Origin Games/Mass Effect", "EA Games/Mass Effect", "BioWare/Mass Effect", "GOG Galaxy/Games/Mass Effect" };
    int n = 0; char p[PATH_MAX], lib[PATH_MAX];
    for (unsigned i = 0; i < sizeof steam / sizeof *steam && n < max; i++) {
        snprintf(p, sizeof p, "%s%s/steamapps/common/Mass Effect", home(), steam[i]);
        if (is_game(p)) { if (!realpath(p, out[n])) snprintf(out[n], PATH_MAX, "%s", p); int dup = 0; for (int k = 0; k < n; k++) if (!strcmp(out[k], out[n])) dup = 1; if (!dup) n++; }
        snprintf(p, sizeof p, "%s%s/steamapps/libraryfolders.vdf", home(), steam[i]);
        char* v = read_file(p, NULL);
        for (char* q = v; q && (q = strstr(q, "\"path\"")); ) {
            q += 6; char* a = strchr(q, '"'); if (!a) break; char* b = strchr(a + 1, '"'); if (!b) break;
            snprintf(lib, sizeof lib, "%.*s/steamapps/common/Mass Effect", (int)(b - a - 1), a + 1); q = b + 1;
            if (is_game(lib) && n < max) { if (!realpath(lib, out[n])) snprintf(out[n], PATH_MAX, "%s", lib); int dup = 0; for (int k = 0; k < n; k++) if (!strcmp(out[k], out[n])) dup = 1; if (!dup) n++; }
        }
        free(v);
    }
    const char* prefixes[] = { "/.wine", "/Games", "/.local/share/bottles/bottles", "/.var/app/com.usebottles.bottles/data/bottles/bottles", "/Games/Heroic/Prefixes" };
    for (unsigned i = 0; i < sizeof prefixes / sizeof *prefixes; i++) {
        char base[PATH_MAX]; snprintf(base, sizeof base, "%s%s", home(), prefixes[i]);
        DIR* d = i == 0 ? NULL : opendir(base); struct dirent* de;
        for (int k = 0; k < (i == 0 ? 1 : 4096); k++) {
            char pre[PATH_MAX];
            if (i == 0) snprintf(pre, sizeof pre, "%s", base);
            else { if (!d || !(de = readdir(d))) break; if (de->d_name[0] == '.') continue; snprintf(pre, sizeof pre, "%s/%s", base, de->d_name);
                char t[PATH_MAX]; if (!ci_child(pre, "drive_c", 1, t, sizeof t)) { snprintf(t, sizeof t, "%s/pfx/drive_c", pre); if (!is_dir(t)) continue; } snprintf(pre, sizeof pre, "%s", t); }
            for (unsigned a = 0; a < 2; a++) for (unsigned s = 0; s < sizeof sub / sizeof *sub && n < max; s++) {
                snprintf(p, sizeof p, "%s%s%s/%s", pre, i == 0 ? "/drive_c/" : "/", pf[a], sub[s]);
                if (is_game(p)) { snprintf(out[n], PATH_MAX, "%s", p); n++; }
            }
        }
        if (d) closedir(d);
    }
    return n;
}
/* background search: known places, then the home folder and mounted disks (depth-limited, 15 s at most) */
static struct { pthread_t th; volatile int running, done, cancel; volatile long scanned; int n; char found[16][PATH_MAX]; } S;
static int skip_name(const char* n) {
    static const char* const keep[] = { ".steam", ".local", ".wine", ".var", ".PlayOnLinux", ".lutris", ".share" };
    static const char* const skip[] = { "proc", "sys", "dev", "node_modules", "__pycache__", "BioGame", "Binaries", "CookedPC", "Movies", "windows",
        "snap", "go", "Pictures", "Music", "Изображения", "Музыка", "site-packages", "dosdevices" };
    if (n[0] == '.') { for (unsigned i = 0; i < sizeof keep / sizeof *keep; i++) if (!strcmp(n, keep[i])) return 0; return 1; }
    for (unsigned i = 0; i < sizeof skip / sizeof *skip; i++) if (!strcasecmp(n, skip[i])) return 1;
    return 0;
}
static void* search_main(void* u) {
    (void)u;
    char (*kp)[PATH_MAX] = malloc(16 * PATH_MAX); int nk = known_places(kp, 16);
    for (int i = 0; i < nk && S.n < 16; i++) snprintf(S.found[S.n++], PATH_MAX, "%s", kp[i]);
    free(kp);
    /* BFS */
    typedef struct { char* p; int depth; } Q;
    size_t cap = 4096, qh = 0, qt = 0; Q* q = malloc(cap * sizeof *q);
    char roots[6][PATH_MAX]; int maxd[6], nr = 0;
    snprintf(roots[nr], PATH_MAX, "%s", home()); maxd[nr++] = 7;
    snprintf(roots[nr], PATH_MAX, "/mnt"); maxd[nr++] = 5;
    snprintf(roots[nr], PATH_MAX, "/media"); maxd[nr++] = 6;
    snprintf(roots[nr], PATH_MAX, "/run/media"); maxd[nr++] = 6;
    snprintf(roots[nr], PATH_MAX, "/opt"); maxd[nr++] = 4;
    snprintf(roots[nr], PATH_MAX, "/games"); maxd[nr++] = 4;
    for (int i = 0; i < nr; i++) if (is_dir(roots[i])) { q[qt].p = strdup(roots[i]); q[qt].depth = maxd[i]; qt++; }
    uint32_t t0 = SDL_GetTicks();
    while (qh < qt && !S.cancel && S.n < 16 && SDL_GetTicks() - t0 < 15000) {
        Q it = q[qh++];
        DIR* d = opendir(it.p); struct dirent* de;
        if (d) {
            S.scanned++;
            int game = 0; char bin[PATH_MAX];
            if (ci_child(it.p, "Binaries", 1, bin, sizeof bin) && ci_child(bin, "MassEffect.exe", 0, NULL, 0)) game = 1;
            if (game) {
                int dup = 0; for (int k = 0; k < S.n; k++) { char a[PATH_MAX], b[PATH_MAX]; if (realpath(S.found[k], a) && realpath(it.p, b) && !strcmp(a, b)) dup = 1; }
                if (!dup) snprintf(S.found[S.n++], PATH_MAX, "%s", it.p);
            } else if (it.depth > 0) while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || skip_name(de->d_name)) continue;
                if (de->d_type != DT_DIR && de->d_type != DT_UNKNOWN) continue;   /* no symlinks: no loops */
                if (qt == cap) { cap *= 2; q = realloc(q, cap * sizeof *q); }
                size_t l = strlen(it.p) + strlen(de->d_name) + 2; char* np = malloc(l); snprintf(np, l, "%s/%s", it.p, de->d_name);
                if (de->d_type == DT_UNKNOWN && !is_dir(np)) { free(np); continue; }
                q[qt].p = np; q[qt].depth = it.depth - 1; qt++;
            }
            closedir(d);
        }
        free(it.p);
    }
    while (qh < qt) free(q[qh++].p);
    free(q);
    S.done = 1; S.running = 0;
    SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_USEREVENT; SDL_PushEvent(&e);
    return NULL;
}
static void search_start(void) {
    if (S.running) return;
    memset((void*)&S, 0, sizeof S); S.running = 1;
    pthread_create(&S.th, NULL, search_main, NULL); pthread_detach(S.th);
}

/* --------------------------------------------------------------------------------------- the machine --- */
static GpuDev g_gpu[8]; static int g_ngpu = -1, g_bgpu = -1; static char g_glr[256], g_glv[256];
static int g_ram_mb, g_threads, g_mhz, g_dw, g_dh; static char g_cpu[160];
static void detect_machine(void) {
    g_ngpu = gpu_probe_vulkan(g_gpu, 8); g_bgpu = g_ngpu > 0 ? gpu_best(g_gpu, g_ngpu) : -1;
    if (g_bgpu < 0) gpu_probe_gl(g_glr, sizeof g_glr, g_glv, sizeof g_glv);
    FILE* f = fopen("/proc/meminfo", "r"); char l[256];
    if (f) { while (fgets(l, sizeof l, f)) if (!strncmp(l, "MemTotal:", 9)) g_ram_mb = (int)(atol(l + 9) / 1024); fclose(f); }
    g_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
    if (f) { if (fgets(l, sizeof l, f)) g_mhz = atoi(l) / 1000; fclose(f); }
    f = fopen("/proc/cpuinfo", "r");
    if (f) { while (fgets(l, sizeof l, f)) if (!strncmp(l, "model name", 10) || !strncmp(l, "Model", 5)) { char* c = strchr(l, ':'); if (c) { c++; while (*c == ' ') c++; snprintf(g_cpu, sizeof g_cpu, "%s", c); g_cpu[strcspn(g_cpu, "\n")] = 0; break; } } fclose(f); }
    SDL_DisplayMode dm; if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { g_dw = dm.w; g_dh = dm.h; }
}
static int cur_gpu(void) { return P.vkdev >= 0 && P.vkdev < g_ngpu ? P.vkdev : g_bgpu; }   /* the device the game will use */
static const char* gpu_name(void) { return cur_gpu() >= 0 ? g_gpu[cur_gpu()].name : g_glr[0] ? g_glr : "?"; }
static int has_vk_gpu(void) { return cur_gpu() >= 0 && g_gpu[cur_gpu()].type != GPU_CPU; }
static int spv_ok(void) { char p[PATH_MAX]; return near_exe("libme1spv.so", p, sizeof p) || is_file("/usr/lib/me1port/libme1spv.so"); }
static int auto_vulkan(void) { return has_vk_gpu() && spv_ok(); }
static int contains(const char* h, const char* n) { return h && strcasestr(h, n) != NULL; }
/* desktop classes: llvmpipe -> minimal; old Intel iGPU -> low; Intel Xe / AMD Vega iGPU -> medium; RDNA iGPU,
 * small discrete -> high; discrete with >= 6 GB -> ultra. Resolution: the desktop (capped per class), 60 FPS */
static int classify(char* rep, size_t cap, char* res, size_t rcap, int* fps) {
    const char* g = gpu_name(); int cg = cur_gpu(), type = cg >= 0 ? g_gpu[cg].type : -1; uint64_t vram = cg >= 0 ? g_gpu[cg].vram : 0;
    int t = 2;
    if (type == GPU_CPU || contains(g, "llvmpipe") || contains(g, "softpipe") || contains(g, "swiftshader") || contains(g, "lavapipe")) t = 0;
    else if (type == GPU_DISCRETE) t = vram >= (6ull << 30) ? 4 : vram >= (3ull << 30) ? 3 : 2;
    else if (contains(g, "Radeon 7") && (contains(g, "80M") || contains(g, "60M")) ) t = 3;
    else if (contains(g, "Radeon 8") && contains(g, "0M")) t = 3;
    else if (contains(g, "Vega") || contains(g, "Radeon Graphics") || contains(g, "RENOIR") || contains(g, "RAVEN") || contains(g, "CEZANNE") || contains(g, "LUCIENNE") || contains(g, "PICASSO")) t = 2;
    else if (contains(g, "REMBRANDT") || contains(g, "PHOENIX") || contains(g, "STRIX") || contains(g, "VANGOGH") || contains(g, "Steam Deck")) t = 3;
    else if (contains(g, "Iris") || contains(g, "Xe ") || contains(g, "Arc")) t = 2;
    else if (contains(g, "Intel") && (contains(g, "UHD") || contains(g, "HD Graphics 6") || contains(g, "HD 6") || contains(g, "(KBL") || contains(g, "(CML") || contains(g, "(CFL") || contains(g, "(WHL") || contains(g, "(ADL") || contains(g, "(TGL"))) t = 1;
    else if (contains(g, "Intel") && (contains(g, "HD Graphics") || contains(g, "Haswell") || contains(g, "Ivy") || contains(g, "Bay Trail") || contains(g, "(HSW") || contains(g, "(IVB") || contains(g, "(BYT") || contains(g, "(BDW"))) t = 0;
    else if (contains(g, "Mali") || contains(g, "Adreno") || contains(g, "V3D") || contains(g, "VideoCore")) t = 0;
    else if (type == GPU_INTEGRATED) t = 2;
    char why[160] = "";
    if (g_ram_mb > 0 && g_ram_mb < 6000 && t > 1) { t = 1; snprintf(why, sizeof why, "; мало ОЗУ → «%s»", T(me1_presets[t].label)); }
    else if (g_ram_mb > 0 && g_ram_mb < 7500 && t > 2) { t = 2; snprintf(why, sizeof why, "; мало ОЗУ → «%s»", T(me1_presets[t].label)); }
    /* resolution: the desktop, at most this many pixels per class */
    static const long long MAXPX[5] = { 1280LL * 720, 1600LL * 900, 1920LL * 1200, 2560LL * 1600, 1LL << 40 };
    int w = g_dw > 0 ? g_dw : 1920, h = g_dh > 0 ? g_dh : 1080;
    if ((long long)w * h > MAXPX[t]) { double k = sqrt((double)MAXPX[t] / ((double)w * h)); w = (int)(w * k) & ~7; h = (int)(h * k) & ~1; }
    snprintf(res, rcap, "%dx%d", w, h);
    *fps = t == 0 ? 30 : 0;
    size_t k = 0;
    k += (size_t)snprintf(rep + k, cap - k, "GPU %s", g);
    if (type >= 0) k += (size_t)snprintf(rep + k, cap - k, " (%s%s", gpu_type_name(type), vram && type == GPU_DISCRETE ? ", видеопамять " : "");
    if (type >= 0 && vram && type == GPU_DISCRETE) k += (size_t)snprintf(rep + k, cap - k, "%.0f ГБ", vram / 1073741824.0);
    if (type >= 0) k += (size_t)snprintf(rep + k, cap - k, ")");
    k += (size_t)snprintf(rep + k, cap - k, " → класс «%s»%s. ", T(me1_presets[t].label), why);
    if (g_ram_mb > 0) k += (size_t)snprintf(rep + k, cap - k, "ОЗУ %.1f ГБ, ", g_ram_mb / 1024.0);
    if (g_threads > 0) k += (size_t)snprintf(rep + k, cap - k, "CPU %d потоков%s", g_threads, g_mhz > 0 ? " " : ", ");
    if (g_mhz > 0) k += (size_t)snprintf(rep + k, cap - k, "до %.1f ГГц, ", g_mhz / 1000.0);
    if (g_dw > 0) k += (size_t)snprintf(rep + k, cap - k, "экран %d×%d, ", g_dw, g_dh);
    snprintf(rep + k, cap - k, "рендер %d×%d", w, h);
    return t;
}
static int apply_auto(char* rep, size_t cap) {
    char res[32], err[256], fs[8]; int fps, t = classify(rep, cap, res, sizeof res, &fps);
    snprintf(fs, sizeof fs, "%d", fps);
    if (me1cfg_preset(&C, me1_presets[t].id, err, sizeof err) || me1cfg_set(&C, "screen.resolution", res, err, sizeof err)
        || me1cfg_set(&C, "port.fps_limit", fs, err, sizeof err)) { snprintf(rep, cap, "ошибка: %s", err); return -1; }
    me1cfg_set(&C, "screen.vsync", t >= 1 ? "1" : "0", err, sizeof err);
    g_dirty = 1;
    return t;
}

/* ------------------------------------------------------------------------------------------ modals --- */
/* a stack of dialogs drawn over the page: a message, monospaced text (logs, reports), a text field, a list */
enum { MK_MSG, MK_MONO, MK_INPUT, MK_LIST };
typedef struct Modal Modal;
typedef void (*ModalCb)(Modal* m, int btn, int item);   /* btn: button index, -1 = Esc / closed; item: list row */
struct Modal {
    int kind; char title[256]; char* body; char btn[3][64]; int nbtn; ModalCb cb;
    char input[PATH_MAX]; int focus_done;
    char* items[160]; int nitems;   /* MK_LIST: "\x01" prefix = section header; "\n" splits a title and its description */
    char** lines; int nlines;       /* MK_MONO */
    UiScroll sc; int to_end, tag, mono_items, fresh;
};
static Modal g_m[4]; static int g_nm;
static Modal* modal(int kind, const char* title, const char* body, ModalCb cb, const char* b0, const char* b1, const char* b2) {
    if (g_nm == 4) return NULL;
    Modal* m = &g_m[g_nm++]; memset(m, 0, sizeof *m);
    m->kind = kind; snprintf(m->title, sizeof m->title, "%s", title ? title : ""); m->body = strdup(body ? body : ""); m->cb = cb;
    const char* b[3] = { b0, b1, b2 };
    for (int i = 0; i < 3; i++) if (b[i]) snprintf(m->btn[m->nbtn++], 64, "%s", b[i]);
    if (!m->nbtn) snprintf(m->btn[m->nbtn++], 64, "OK");
    if (kind == MK_MONO) {
        int cap = 256; m->lines = malloc(sizeof(char*) * (size_t)cap);
        for (char* s = m->body; s; ) {
            char* e = strchr(s, '\n'); if (e) *e = 0;
            if (m->nlines == cap) { cap *= 2; m->lines = realloc(m->lines, sizeof(char*) * (size_t)cap); }
            m->lines[m->nlines++] = s; s = e ? e + 1 : NULL;
        }
        while (m->nlines > 1 && !*m->lines[m->nlines - 1]) m->nlines--;
    }
    ui_popup_close(); ui.focus = 0; ui.redraw = 1; m->fresh = 1;
    return m;
}
static void modal_item(Modal* m, const char* s) { if (m && m->nitems < 160) m->items[m->nitems++] = strdup(s); }
static void modal_free(Modal* m) { free(m->body); free(m->lines); for (int i = 0; i < m->nitems; i++) free(m->items[i]); }
static void msg(const char* title, const char* body) { modal(MK_MSG, title, body, NULL, "OK", NULL, NULL); }
static void modal_close(int btn, int item) {
    Modal m = g_m[--g_nm];
    if (ui.focus) { ui.focus = 0; SDL_StopTextInput(); }
    if (m.kind == MK_MONO && btn >= 0 && !strcmp(m.btn[btn], T("Копировать"))) {   /* copy keeps the dialog */
        Str s = { 0 }; for (int i = 0; i < m.nlines; i++) sadd(&s, "%s\n", m.lines[i]);
        SDL_SetClipboardText(s.s ? s.s : ""); free(s.s); ui_toast(T("Скопировано в буфер обмена"));
        g_m[g_nm++] = m; return;
    }
    if (m.cb) m.cb(&m, btn, item);
    modal_free(&m); ui.redraw = 1;
}
static int item_h(const char* it, int w) {
    if (it[0] == '\x01') return ui_lineh(UF_BOLD, D(14)) + D(14);
    const char* nl = strchr(it, '\n');
    return ui_lineh(UF_MONO, D(14)) + (nl ? ui_textbox_h(UF_REG, D(12), w - D(28), nl + 1) + D(2) : 0) + D(12);
}
static void modal_draw(int k) {
    Modal* m = &g_m[k]; int top = k == g_nm - 1;
    ui.layer = top ? UI_LAYER_MODAL : 9;
    SDL_Rect all = { 0, 0, ui.w, ui.h }; ui_fill(all, UICA(0x02050a, top ? 170 : 60));
    int mono = m->kind == MK_MONO, list = m->kind == MK_LIST;
    int w = mono || list ? ui.w - D(80) : ui.w - D(120), maxw = mono ? D(1180) : list ? D(860) : D(640);
    if (w > maxw) w = maxw;
    int pad = D(24), iw = w - 2 * pad, tl = ui_lineh(UF_BOLD, D(19));
    int bh = D(38), maxh = ui.h - D(60);
    /* content height */
    int ch = 0, lh = ui_lineh(mono ? UF_MONO : UF_REG, mono ? D(13) : D(14));
    if (m->kind == MK_MSG || m->kind == MK_INPUT) ch = *m->body ? ui_textbox_h(UF_REG, D(14), iw, m->body) : 0;
    if (mono) ch = m->nlines * lh;
    if (list) { if (*m->body) ch = ui_textbox_h(UF_REG, D(13), iw, m->body) + D(10); for (int i = 0; i < m->nitems; i++) ch += item_h(m->items[i], iw); }
    int extra = m->kind == MK_INPUT ? D(52) : 0;
    int avail = maxh - (pad + tl + D(14) + extra + D(18) + bh + pad), vh = ch < avail ? ch : avail;
    if (vh < D(40)) vh = D(40) < ch ? D(40) : ch;
    int h = pad + tl + D(14) + vh + extra + D(18) + bh + pad;
    SDL_Rect box = { (ui.w - w) / 2, (ui.h - h) / 2, w, h };
    SDL_Rect sh = { box.x + D(4), box.y + D(8), box.w, box.h }; ui_rrect(sh, D(12), UICA(0, 110));
    ui_rrect(box, D(12), UIC(0x0f1824)); ui_rframe(box, D(12), 1, UIC(top ? C_ACC : C_LINE));
    SDL_Rect hb = { box.x, box.y, box.w, D(4) }; (void)hb;
    int x = box.x + pad, y = box.y + pad;
    ui_text_fit(UF_BOLD, D(19), x, y, iw, UIC(0xffffff), m->title); y += tl + D(14);
    SDL_Rect view = { x - D(6), y, iw + D(12), vh };
    if (mono) { SDL_Rect bg = { view.x - D(4), view.y - D(6), view.w + D(8), view.h + D(12) }; ui_rrect(bg, D(6), UIC(0x0a1018)); }
    m->sc.id = ui_id("modal-scroll", k);
    if (m->to_end) { ui_scroll_to_end(&m->sc); m->sc.content = ch; m->to_end = 0; }
    ui_scroll_begin(&m->sc, view);
    int cy = y - m->sc.pos;
    int clicked_item = -1;
    if (m->kind == MK_MSG || m->kind == MK_INPUT) ui_textbox(UF_REG, D(14), x, cy, iw, UIC(C_TEXT), m->body);
    else if (mono) {
        int first = m->sc.pos / (lh > 0 ? lh : 1); if (first < 0) first = 0;
        for (int i = first; i < m->nlines && cy + i * lh < y + vh; i++) {
            const char* s = m->lines[i]; UiCol c = UIC(0xc9d6e4);
            if (strstr(s, "FAULT") || strstr(s, "CRASH") || strstr(s, "✗") || strstr(s, "error") || strstr(s, "ошибка")) c = UIC(C_ERR);
            else if (strstr(s, "✓")) c = UIC(C_OK);
            else if (s[0] == '[') c = UIC(0xa9bbd0);
            ui_text(UF_MONO, D(13), x, cy + i * lh, c, s, -1);
        }
    } else {
        if (*m->body) cy += ui_textbox(UF_REG, D(13), x, cy, iw, UIC(C_DIM), m->body) + D(10);
        for (int i = 0; i < m->nitems; i++) {
            const char* it = m->items[i]; int ih = item_h(it, iw);
            if (cy + ih >= y && cy < y + vh) {
                if (it[0] == '\x01') { ui_text(UF_BOLD, D(14), x, cy + D(8), UIC(C_ACC2), it + 1, -1); }
                else {
                    SDL_Rect r = { x - D(4), cy, iw + D(8), ih - D(4) };
                    uint32_t id = ui_id(it, k * 1000 + i);
                    int hov = ui_hover(r);
                    if (hov) { ui_rrect(r, D(6), UICA(C_ACC, 36)); ui.cursor = 1; if (ui.pressed) ui.active = id; }
                    if (hov && ui.released && ui.active == id) clicked_item = i;
                    const char* nl = strchr(it, '\n');
                    ui_text(m->mono_items ? UF_MONO : UF_REG, m->mono_items ? D(13) : D(14), x + D(8), cy + D(6), UIC(hov ? 0xffffff : C_TEXT), it, nl ? (int)(nl - it) : -1);
                    if (nl) ui_textbox(UF_REG, D(12), x + D(20), cy + D(6) + ui_lineh(UF_MONO, D(14)) + D(2), iw - D(28), UIC(C_DIM), nl + 1);
                }
            }
            cy += ih;
        }
    }
    ui_scroll_end(&m->sc, ch);
    y += vh + D(18);
    int enter = 0;
    if (m->kind == MK_INPUT) {
        SDL_Rect ir = { x, y - D(4), iw, D(38) }; uint32_t id = ui_id("modal-input", k);
        if (top && !m->focus_done) { ui.focus = id; SDL_StartTextInput(); m->focus_done = 1; }
        if (ui_input(id, ir, m->input, sizeof m->input, "") == 2) enter = 1;
        y += extra;
    }
    /* buttons: the first one is the main action (right) */
    int bx = box.x + box.w - pad, res = -2;
    for (int i = 0; i < m->nbtn; i++) {
        int bw = ui_textw(i == 0 ? UF_BOLD : UF_REG, i == 0 ? D(17) : D(14), m->btn[i], -1) + D(40); if (bw < D(110)) bw = D(110);
        bx -= bw; SDL_Rect br = { bx, box.y + box.h - pad - bh, bw, bh }; bx -= D(10);
        if (ui_button(ui_id(m->btn[i], k * 10 + i), br, m->btn[i], i == 0 ? UB_PRIMARY : UB_NORMAL)) res = i;
    }
    if (!top) return;
    if (m->fresh) { m->fresh = 0; return; }   /* the click that opened it is not for it */
    if (clicked_item >= 0) { modal_close(0, clicked_item); return; }
    if (res >= 0) { modal_close(res, -1); return; }
    if (enter || (m->kind != MK_INPUT && (ui_key(SDLK_RETURN) || ui_key(SDLK_KP_ENTER)))) { modal_close(0, -1); return; }
    if (ui_key(SDLK_ESCAPE)) { modal_close(-1, -1); return; }
    if (ui.pressed && !ui_hover(box) && m->kind != MK_INPUT) { ui.pressed = 0; modal_close(-1, -1); return; }
}

/* ------------------------------------------------------------------------------------------ layout --- */
static struct { int x, y, w; } L;
static SDL_Rect lrow(int h) { SDL_Rect r = { L.x, L.y, L.w, h }; L.y += h; return r; }
static int para(int px, UiCol c, const char* s) { int h = ui_textbox(UF_REG, D(px), L.x, L.y, L.w, c, s); L.y += h + D(6); return h; }
static void heading(const char* s) {
    L.y += D(10);
    ui_text(UF_BOLD, D(18), L.x, L.y, UIC(0xffffff), s, -1); L.y += ui_lineh(UF_BOLD, D(18)) + D(4);
    SDL_Rect ln = { L.x, L.y, D(48), D(2) }; ui_fill(ln, UIC(C_ACC)); L.y += D(14);
}
#define NCARD 96
static struct { uint32_t id; int h; } g_card[NCARD]; static int g_ncard;
static struct { int* h; int y0; } g_cs[4]; static int g_ncs;
static int* card_slot(uint32_t id) {
    for (int i = 0; i < g_ncard; i++) if (g_card[i].id == id) return &g_card[i].h;
    static int dummy; if (g_ncard == NCARD) return &dummy;
    g_card[g_ncard].id = id; g_card[g_ncard].h = 0; return &g_card[g_ncard++].h;
}
static void card_begin(const char* key, const char* title, UiCol accent) {
    int* h = card_slot(ui_id(key, 0));
    SDL_Rect r = { L.x, L.y, L.w, *h };
    if (*h > 0) {
        ui_rrect(r, D(10), UIC(C_PANEL)); ui_rframe(r, D(10), 1, UICA(C_LINE, 190));
        SDL_Rect a = { r.x + D(1), r.y + D(12), D(3), D(22) }; ui_fill(a, accent);
    }
    g_cs[g_ncs].h = h; g_cs[g_ncs].y0 = L.y; g_ncs++;
    L.x += D(22); L.w -= D(44); L.y += D(16);
    if (title) { ui_text(UF_BOLD, D(16), L.x, L.y, UIC(0xffffff), title, -1); L.y += ui_lineh(UF_BOLD, D(16)) + D(10); }
}
static void card_end(void) {
    g_ncs--; L.y += D(10); L.x -= D(22); L.w += D(44);
    int h = L.y - g_cs[g_ncs].y0; if (*g_cs[g_ncs].h != h) { *g_cs[g_ncs].h = h; ui.redraw = 1; }
    L.y += D(16);
}
/* a row of buttons that wraps */
static struct { int x, y, h; } F;
static void flow_begin(void) { F.x = L.x; F.y = L.y; F.h = D(36); }
static SDL_Rect flow_rect(const char* label, int style) {
    int pri = (style & 0xFF) == UB_PRIMARY, w = ui_textw(pri ? UF_BOLD : UF_REG, pri ? D(17) : D(14), label, -1) + D(36);
    if (w < D(96)) w = D(96);
    if (F.x + w > L.x + L.w && F.x > L.x) { F.x = L.x; F.y += F.h + D(8); }
    SDL_Rect r = { F.x, F.y, w, F.h }; F.x += w + D(8); return r;
}
static int flow_btn(const char* label, int style) { return ui_button(ui_id(label, 77), flow_rect(label, style), label, style); }
static void flow_end(void) { L.y = F.y + F.h + D(12); }
static void status_line(const char* s, UiCol c) { int h = ui_textbox(UF_REG, D(14), L.x, L.y, L.w, c, s); L.y += h + D(6); }

/* ---------------------------------------------------------------------------------------- settings --- */
static void cfg_set(const char* id, const char* v) {
    char err[256] = "";
    if (me1cfg_set(&C, id, v, err, sizeof err)) { char* t = i18n_tx(err); ui_toast(t); free(t); }
    else g_dirty = 1;
}
static const char* cfg_get(const char* id) { int i; return me1cfg_find(id, &i) ? C.value[i] : ""; }
/* the desktop wording of a few Android texts */
static const char* set_help(const SettingDef* d) {
    if (!strcmp(d->id, "screen.resolution")) return "Разрешение рендера игры. Окно открывается такого же размера (если помещается на экране), в полноэкранном режиме картинка масштабируется на весь экран";
    if (!strcmp(d->id, "screen.fullscreen")) return "Окно на весь экран (в игре — Alt+Enter или F11)";
    if (!strcmp(d->id, "port.fps_limit")) return "Меньше нагрев и шум вентиляторов; «Без ограничения» — движок сам держит до ~62 кадров/с";
    if (!strcmp(d->id, "input.mouse_sensitivity")) return "MouseSensitivity: скорость обзора мышью";
    if (!strcmp(d->id, "snd.device")) return "Устройство ISACT; в порте звук микширует свой OpenAL, вывод — через SDL (PipeWire/PulseAudio/ALSA)";
    if (!strcmp(d->id, "port.phone_mod")) return "Применять модификаторы из папки PhoneModify в папке игры (файлы .txt): лимиты вызовов отрисовки и примитивов на кадр, "
        "подмена ключей движка, запись байтов в память игры. Заготовку создаёт «Генератор PhoneModify» (вкладка «Дополнительно»). Осторожно: лимиты и патчи меняют картинку";
    return d->help ? d->help : "";
}
static const char* group_name(const char* g) { return !strcmp(g, "Android") ? "Порт" : g; }
static int set_hidden(const SettingDef* d) { return !strncmp(d->id, "port.touch_", 11); }
static const char* preset_help(int i) {
    static const char* const H[] = {
        "Программный рендер (llvmpipe) и очень старые GPU: упрощённый расчёт, без теней и постобработки",
        "Старые встроенные GPU (Intel HD 4000–620): без теней и постобработки, текстуры попроще",
        "Встроенные AMD Vega / Intel Iris Xe и начальные дискретные GPU",
        "Встроенные RDNA (Radeon 680M/780M), дискретные GPU с 3–6 ГБ: тени, полная детализация",
        "Дискретные GPU от 6 ГБ: всё на максимум, текстуры до 4096",
    };
    return i >= 0 && i < 5 ? H[i] : "";
}
static void desk_res(char* out, size_t cap) { snprintf(out, cap, "%dx%d", g_dw > 0 ? g_dw : 1920, g_dh > 0 ? g_dh : 1080); }
static void setting_row(int i) {
    const SettingDef* d = &me1_settings[i];
    if (set_hidden(d)) return;
    const char* help = T(set_help(d));
    int cw = L.w * 2 / 5; if (cw > D(330)) cw = D(330); if (cw < D(190)) cw = D(190);
    int tw = L.w - cw - D(28), lh = ui_lineh(UF_REG, D(15));
    int hh = *help ? ui_textbox_h(UF_REG, D(12), tw, help) : 0;
    int h = lh + (hh ? D(3) + hh : 0); if (h < D(34)) h = D(34);
    SDL_Rect row = { L.x, L.y, L.w, h + D(14) };
    if (C.dirty[i]) { SDL_Rect dot = { L.x - D(12), L.y + D(7) + lh / 2 - D(3), D(6), D(6) }; ui_rrect(dot, D(3), UIC(C_WARN)); }
    ui_text_fit(UF_REG, D(15), L.x, L.y + D(7), tw, UIC(C_TEXT), T(d->label));
    if (hh) ui_textbox(UF_REG, D(12), L.x, L.y + D(7) + lh + D(3), tw, UIC(C_DIM), help);
    SDL_Rect cr = { L.x + L.w - cw, L.y + D(7), cw, D(34) };
    uint32_t id = ui_id(d->id, 1);
    const char* v = C.value[i];
    if (d->type == ST_BOOL || d->type == ST_BOOL01) {
        SDL_Rect sr = { L.x + L.w - D(60), L.y + D(7), D(60), D(34) };
        int on = strcmp(v, "0") != 0;
        if (ui_switch(id, sr, "", on, 0)) cfg_set(d->id, on ? "0" : "1");
    } else if (d->type == ST_ENUM || d->type == ST_RES) {
        static char lb[72][96]; const char* labs[72]; const char* vals[72]; int n = 0, sel = -1;
        char desk[32]; desk_res(desk, sizeof desk); int have_desk = 0;
        for (const SetChoice* ch = d->choices; ch && ch->value && n < 68; ch++) {
            vals[n] = ch->value;
            if (d->type == ST_RES && !strcmp(ch->value, desk)) { snprintf(lb[n], sizeof lb[n], "%s%s", T(ch->label), T(" (рабочий стол)")); have_desk = 1; }
            else snprintf(lb[n], sizeof lb[n], "%s", T(ch->label));
            labs[n] = lb[n]; n++;
        }
        if (d->type == ST_RES && !have_desk) { vals[n] = desk; snprintf(lb[n], sizeof lb[n], "%s%s", desk, T(" (рабочий стол)")); for (char* q = lb[n]; *q; q++) if (*q == 'x') { memmove(q + 2, q + 1, strlen(q + 1) + 1); q[0] = (char)0xC3; q[1] = (char)0x97; break; } labs[n] = lb[n]; n++; }
        for (int k = 0; k < n; k++) if (!strcmp(vals[k], v)) sel = k;
        if (sel < 0) { vals[n] = v; snprintf(lb[n], sizeof lb[n], "%s%s", v, T(" (свой)")); labs[n] = lb[n]; sel = n++; }
        int s2 = sel;
        if (ui_combo(id, cr, labs, n, &s2) && s2 != sel) { char nv[96]; snprintf(nv, sizeof nv, "%s", vals[s2]); cfg_set(d->id, nv); }
    } else if (d->type == ST_INT || d->type == ST_FLOAT) {
        double x = atof(v), step = d->type == ST_FLOAT ? (d->max - d->min > 10 ? 0.5 : 0.1) : 1;
        SDL_Rect sl = { cr.x, cr.y, cr.w - D(64), cr.h };
        if (ui_slider(id, sl, &x, d->min, d->max, step)) {
            char nv[32]; if (d->type == ST_FLOAT) { snprintf(nv, sizeof nv, "%.1f", x); size_t l = strlen(nv); if (l > 2 && !strcmp(nv + l - 2, ".0")) nv[l - 2] = 0; }
            else snprintf(nv, sizeof nv, "%d", (int)(x + 0.5));
            cfg_set(d->id, nv);
        }
        SDL_Rect vr = { cr.x + cr.w - D(58), cr.y, D(58), cr.h }; ui_text_c(UF_BOLD, D(15), vr, UIC(C_ACC2), C.value[i]);
    } else {
        static char sb[ME1_MAXSET][96]; snprintf(sb[i], sizeof sb[i], "%s", v);
        if (ui_input(id, cr, sb[i], sizeof sb[i], "") == 2) cfg_set(d->id, sb[i]);
    }
    L.y += h + D(14);
    (void)row;
    SDL_Rect sep = { L.x, L.y - D(1), L.w, 1 }; ui_fill(sep, UICA(C_LINE, 90));
}
static void settings_group(const char* group, const char* title) {
    int any = 0;
    for (int i = 0; i < me1_nsettings; i++) if (!strcmp(group_name(me1_settings[i].group), group) && !set_hidden(&me1_settings[i])) any = 1;
    if (!any) return;
    card_begin(group, T(title ? title : group), UIC(C_ACC));
    for (int i = 0; i < me1_nsettings; i++) if (!strcmp(group_name(me1_settings[i].group), group)) setting_row(i);
    card_end();
}

/* -------------------------------------------------------------------------------------------- mods --- */
/* <game>/Mods/<name> with "Mass Effect"/BioGame/DLC and/or Config inside (archives often add one more level) */
static const char* const MOD_DIRS[] = { "Mass Effect", "MassEffect", "Mass Effect 1", "BioGame", "DLC", "Binaries", "Config" };
static int has_mod_dir(const char* f) { for (unsigned i = 0; i < sizeof MOD_DIRS / sizeof *MOD_DIRS; i++) if (ci_child(f, MOD_DIRS[i], 1, NULL, 0)) return 1; return 0; }
static void mod_root(const char* f, char* out, size_t cap) {
    snprintf(out, cap, "%s", f);
    if (has_mod_dir(f)) return;
    DIR* d = opendir(f); struct dirent* de; char p[PATH_MAX];
    if (!d) return;
    while ((de = readdir(d))) { if (de->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s", f, de->d_name); if (is_dir(p) && has_mod_dir(p)) { snprintf(out, cap, "%s", p); break; } }
    closedir(d);
}
static int is_pad_mod(const char* mod) {
    char r[PATH_MAX], ini[PATH_MAX]; mod_root(mod, r, sizeof r);
    if (!ci_path(r, "Config/BIOInput.ini", 0, ini, sizeof ini)) return 0;
    char* t = read_file(ini, NULL); int y = t && strstr(t, "XboxTypeS_"); free(t); return y;
}
typedef struct { char name[256], path[PATH_MAX], root[PATH_MAX]; int pad; } Mod;
static Mod g_mods[64]; static int g_nmods; static char g_modsdir[PATH_MAX];
static int mod_cmp(const void* a, const void* b) { return strcasecmp(((const Mod*)a)->name, ((const Mod*)b)->name); }
static void mods_scan(void) {
    g_nmods = 0;
    if (!*g_game) { g_modsdir[0] = 0; return; }
    if (!ci_child(g_game, "Mods", 1, g_modsdir, sizeof g_modsdir)) snprintf(g_modsdir, sizeof g_modsdir, "%s/Mods", g_game);
    DIR* d = opendir(g_modsdir); struct dirent* de;
    if (!d) return;
    while ((de = readdir(d)) && g_nmods < 64) {
        if (de->d_name[0] == '.') continue;
        Mod* m = &g_mods[g_nmods]; snprintf(m->path, sizeof m->path, "%s/%s", g_modsdir, de->d_name);
        if (!is_dir(m->path)) continue;
        mod_root(m->path, m->root, sizeof m->root);
        if (!has_mod_dir(m->root)) continue;
        snprintf(m->name, sizeof m->name, "%s", de->d_name); m->pad = is_pad_mod(m->path); g_nmods++;
    }
    closedir(d);
    qsort(g_mods, (size_t)g_nmods, sizeof *g_mods, mod_cmp);
}
static int pad_mod_on(void) { for (int i = 0; i < g_nmods; i++) if (g_mods[i].pad && mod_on(g_mods[i].name)) return 1; return 0; }
/* DLC */
static const char* const DLCS[][2] = { { "DLC_UNC", "Bring Down the Sky" }, { "DLC_Vegas", "Pinnacle Station" } };
static const char* dlc_name(const char* dir) { for (int i = 0; i < 2; i++) if (!strcasecmp(DLCS[i][0], dir)) return DLCS[i][1]; return dir; }
static void dlc_status(char* o, size_t cap) {
    size_t k = (size_t)snprintf(o, cap, "%s", T("DLC в папке игры:"));
    int have[2] = { GS.unc, GS.vegas };
    for (int i = 0; i < 2 && k < cap; i++) k += (size_t)snprintf(o + k, cap - k, "%s%s%s", i ? " · " : " ", DLCS[i][1], have[i] ? " ✓" : T(" — нет"));
}
/* a mod's DLC files are used only when that DLC is installed - say so instead of failing silently */
static void mod_dlc_note(const Mod* m, char* o, size_t cap) {
    o[0] = 0; char g[PATH_MAX] = "", md[PATH_MAX], base[PATH_MAX];
    static const char* const GD[] = { "Mass Effect", "MassEffect", "Mass Effect 1" };
    for (int i = 0; i < 3 && !*g; i++) ci_child(m->root, GD[i], 1, g, sizeof g);
    if (!ci_child(*g ? g : m->root, "DLC", 1, md, sizeof md)) return;
    int hb = ci_child(g_game, "DLC", 1, base, sizeof base);
    Str have = { 0 }, wait = { 0 };
    DIR* d = opendir(md); struct dirent* de; char p[PATH_MAX];
    if (!d) return;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s", md, de->d_name); if (!is_dir(p)) continue;
        Str* s = hb && ci_child(base, de->d_name, 1, NULL, 0) ? &have : &wait; sadd(s, "%s%s", s->n ? ", " : "", dlc_name(de->d_name));
    }
    closedir(d);
    size_t k = 0;
    if (have.n) k += (size_t)snprintf(o + k, cap - k, "%s%s ✓", T("файлы для "), have.s);
    if (wait.n && k < cap) { char dd[PATH_MAX + 8]; snprintf(dd, sizeof dd, "%s/DLC", g_game);
        snprintf(o + k, cap - k, "%s%s%s%s%s)", k ? "; " : "", T("файлы для "), wait.s, T(" подключатся сами, когда DLC появится в папке игры ("), pretty(dd)); }
    free(have.s); free(wait.s);
}
static void pads(char* o, size_t cap) {
    o[0] = 0; size_t k = 0;
    for (int i = 0; i < SDL_NumJoysticks() && k < cap; i++) {
        const char* n = SDL_IsGameController(i) ? SDL_GameControllerNameForIndex(i) : SDL_JoystickNameForIndex(i);
        if (n) k += (size_t)snprintf(o + k, cap - k, "%s%s", k ? ", " : "", n);
    }
}

/* ------------------------------------------------------------------------------- environment list --- */
/* every environment variable of the runtime: { "NAME=example", description } ("" name: a section header). The texts
 * are those of the Android launcher (MainActivity.ENV, translations shared); Android-only entries dropped */
static const char* const ENV[][2] = {
    { "", "Ролики и звук" },
    { "ME1_BINK=0", "не загружать binkw32.dll: все ролики пропускаются" },
    { "ME1_DSOUND=0", "без DirectSound: ролики без звука" },
    { "ME1_DSOUND_LOG=1", "лог DirectSound (буферы роликов, микшер)" },
    { "ME1_AL=0", "выключить весь звук игры (OpenAL)" },
    { "ME1_AL_GAIN=0.7", "общая громкость (1 = 100 %, можно до 2)" },
    { "ME1_AL_RATE=48000", "частота вывода звука, Гц (8000–192000; по умолчанию — родная частота устройства)" },
    { "ME1_AL_LOG=2", "подробный лог звука (0 — без лога, 1 — по умолчанию)" },
    { "ME1_AL_IMA4=1", "сообщать игре о поддержке сжатия IMA4 ADPCM (по умолчанию нет)" },
    { "ME1_AL_SINK=null", "не выводить звук на устройство (вместе с ME1_AL_WAV — запись микса в файл)" },
    { "ME1_AL_WAV=/tmp/me1_audio.wav", "записывать микс в WAV (только с ME1_AL_SINK=null)" },
    { "", "Графика" },
    { "ME1_FLOAT_EMU=1", "точность float D3D9: 2 — точная эмуляция (0·∞ = 0) и в mul/mad/dp, без артефактов (по умолчанию на ПК); 1 — частичная; 0 — выкл., быстрее" },
    { "ME1_SHADER_LITE=1", "упрощённый расчёт (то же, что переключатель «Упрощённый расчёт» в «Графике»): без динамических теней и теней персонажей, простые карты освещения, без размытия/глубины резкости/бликов/декалей, билинейная фильтрация, LOD персонажей ниже; FP16 там, где игра допускает half, таймер 5 мс, звук 32 кГц; 0 — выкл." },
    { "ME1_LITE_INI=0", "упрощённый расчёт без настроек движка (только шейдеры, таймер и звук)" },
    { "ME1_LITE_SKIP=DirectionalLightmaps,DetailMode", "упрощённый расчёт: не трогать перечисленные параметры движка (DynamicShadows, LightEnvironmentShadows, DirectionalLightmaps, MotionBlur, DepthOfField, QualityBloom, LensFlares, DynamicDecals, Trilinear, MaxAnisotropy, SkeletalMeshLODBias, DetailMode, EnableDynamicShadows)" },
    { "ME1_CPU_PIN=0", "не распределять потоки по ядрам (по умолчанию игровой поток и поток рендера — на быстрых ядрах, остальные — на медленных)" },
    { "ME1_PHONE_MOD=1", "применять модификаторы из <папка игры>/PhoneModify (то же, что переключатель «Модификаторы PhoneModify»): лимиты вызовов/примитивов на кадр, ключи INI, патчи кода. Создать модификатор: me1cfg modgen balanced|aggressive" },
    { "ME1_SHOW_FPS=1", "счётчик FPS в правом верхнем углу (то же, что «Показывать FPS» в настройках порта)" },
    { "ME1_CURSOR=2", "курсор мыши игры: 1 — по умолчанию, виден, когда игра его показывает (меню); 2 — всегда; 0 — никогда" },
    { "ME1_FPS_LIMIT=60", "ограничить частоту кадров (10–240): меньше нагрев и расход батареи; 0 — без ограничения" },
    { "ME1_RENDERER=gles", "рендерер: vulkan — свой D3D9→Vulkan, gles — OpenGL, zink — OpenGL через Mesa Zink (обычно задаёт лаунчер)" },
    { "ME1_VK_DEVICE=1", "Vulkan: номер устройства (0, 1, …; по умолчанию — дискретная GPU, затем встроенная)" },
    { "ME1_VK_VALIDATE=1", "Vulkan: слой проверки VK_LAYER_KHRONOS_validation, если он есть (медленно, для отладки)" },
    { "ME1_VK_STRICT=1", "Vulkan: вести себя как на самом слабом драйвере (без BC-сжатия, без float-фильтрации) — для проверки" },
    { "ME1_VK_SPLIT=2048", "Vulkan: отправлять кадр частями по N вызовов отрисовки — обход падения драйвера Mali (DEVICE_LOST на первом тяжёлом кадре). По умолчанию включено (2048); 0 — одним сабмитом, как в v0.10.2" },
    { "ME1_SPV_LIB=/путь/libme1spv.so", "компилятор GLSL→SPIR-V для Vulkan (по умолчанию — рядом с me1-linux)" },
    { "ME1_GPU_TIMER=0", "не замерять время GPU на кадр (строка «[d3d] frame … GPU … ms/frame» в логе)" },
    { "ME1_OCCLUSION=2", "запросы видимости (occlusion queries): 1 — по умолчанию, как в D3D9: невидимое отсекается, блики не светят сквозь стены; 2 — без ожидания GPU (результат с опозданием на кадр — если с 1 подтормаживает); 0 — выкл. (всё считается видимым, как до v1.1.3)" },
    { "ME1_INVARIANT=0", "выключить invariant gl_Position: чуть быстрее, но возможны полосы/мерцание на освещённых поверхностях (1 — по умолчанию)" },
    { "ME1_TEX_16=0", "DXT-текстуры (распаковка на CPU) в RGBA8: лучше качество, больше памяти; 1 — по умолчанию (DXT1 → RGB5A1); 2 — ещё DXT3/5 → RGBA4" },
    { "ME1_TEX_MAXSIZE=1024", "ограничить размер текстур (512, 1024, 2048): меньше памяти и загрузок, ниже чёткость" },
    { "ME1_GL_NOS3TC=1", "не использовать аппаратное сжатие S3TC/DXT, даже если GPU умеет (распаковка на CPU)" },
    { "ME1_GL_NORGTC=1", "не использовать аппаратное сжатие RGTC (ATI1/ATI2)" },
    { "ME1_VRAM_MB=1024", "сколько видеопамяти сообщать игре, МБ (по умолчанию 512)" },
    { "ME1_SCREEN=1920x1080", "разрешение «рабочего стола» для игры (по умолчанию — экран устройства)" },
    { "ME1_GL=0", "без рендера (чёрный экран) — только для поиска падений" },
    { "ME1_NO_LOADING=1", "без экрана загрузки (окно остаётся чёрным до первого кадра)" },
    { "SDL_VIDEODRIVER=x11", "оконная система: x11 или wayland (то же, что me1-linux --x11 / --wayland)" },
    { "ZINK_DEBUG=compact", "отладочные флаги Mesa Zink (compact, nobgc, …); MESA_LOG/MESA_DEBUG тоже работают" },
    { "", "Кэши" },
    { "ME1_GL_CACHE=off", "не сохранять собранные GL-программы (кэш в памяти приложения, ускоряет повторные запуски)" },
    { "ME1_HLSL_CACHE=off", "не использовать кэш скомпилированных HLSL-шейдеров (Documents/me1_hlsl_cache): компиляция при каждом запуске" },
    { "ME1_VFS_CACHE=0", "без кэша каталогов (файловый слой Win32)" },
    { "", "Время, потоки, ввод" },
    { "ME1_MM_PERIOD=1", "период мультимедиа-таймера, мс (1–100, по умолчанию 2)" },
    { "ME1_MM=0", "выключить мультимедиа-таймеры (timeSetEvent)" },
    { "ME1_SPIN_SLEEP_US=0", "пауза в циклах ожидания Sleep(0), мкс (по умолчанию 100; 0 — только yield, больше нагрузка на CPU)" },
    { "ME1_XINPUT=0", "геймпад XInput: 0 — нет, 1 — подключён всегда (ставит лаунчер в профиле «Геймпад»), по умолчанию — после первого нажатия" },
    { "", "Снимки кадров и логи" },
    { "ME1_FRAME_DUMP=/tmp/me1_frames", "сохранять снимки кадров (PNG) в эту папку: первый кадр и каждый N-й (ME1_FRAME_DUMP_EVERY)" },
    { "ME1_FRAME_DUMP_EVERY=300", "сохранять каждый N-й кадр в Documents/me1_frames (по умолчанию 1800)" },
    { "ME1_D3D_LOG=1", "лог вызовов Direct3D (2 — очень подробно и медленно)" },
    { "ME1_GL_DEBUG=1", "проверка ошибок OpenGL после каждого вызова (медленно)" },
    { "ME1_GLSL_DUMP=/tmp/me1_glsl", "сохранять исходники GLSL программ, которые не собрались" },
    { "ME1_D3D_DUMP=/tmp/me1_shaders", "сохранять байткод всех шейдеров (shaders.pak)" },
    { "ME1_HB=5", "период строки состояния [hb] в логе, с (0 — выкл., по умолчанию 10)" },
    { "ME1_PROF_HZ=200", "частота сэмплирующего профилировщика в строке [hb], Гц (по умолчанию 100)" },
    { "ME1_ECHO_LOG=0", "не копировать лог движка (Logs/*.log) в me1exe_run.log" },
    { "ME1_VFS_TRACE=1", "лог обращений к файлам (2 — подробнее)" },
    { "ME1_TRACE_INPUT=1", "лог ввода (мышь, клавиатура)" },
    { "ME1_TRACE_UI=1", "лог оконных сообщений и курсора" },
    { "ME1_TRACE_THREADS=1", "лог создания потоков и синхронизации" },
    { "ME1_TRACE_EH=1", "лог исключений (SEH / C++)" },
    { "ME1_TRACE_REG=1", "лог обращений к реестру" },
    { "ME1_LIST_MISSING=1", "перечислить все нереализованные функции Windows" },
    { "ME1_HEAP_NOREUSE=1", "не переиспользовать освобождённую память (поиск ошибок; память растёт!)" },
    { "ME1_WATCH=1", "включить сторож памяти PhysX (следит за 12283ce0; по умолчанию выключен с v1.1)" },
    { "ME1_MAX_SECONDS=120", "закрыть игру через N секунд после первого кадра" },
    { "ME1_MAX_FRAMES=5000", "закрыть игру после N кадров" },
    { "", "Задаёт лаунчер (можно переопределить)" },
    { "ME1_D3DX=hle", "не загружать d3dx9_31.dll — без компилятора HLSL шейдеры не соберутся (all — все функции D3DX из dll)" },
    { "ME1_MODS=/путь/Mods/ME1Controller", "папки модов через «:» (поверх игры и Documents/…/Config; лаунчер ставит включённые моды)" },
};
static const int NENV = (int)(sizeof ENV / sizeof ENV[0]);
static void env_add(const char* kv) {   /* replaces the same NAME */
    const char* eq = strchr(kv, '='); size_t nl = eq ? (size_t)(eq - kv) + 1 : strlen(kv);
    char o[sizeof P.env] = ""; size_t k = 0; char tmp[sizeof P.env]; snprintf(tmp, sizeof tmp, "%s", P.env);
    for (char* t = strtok(tmp, " \t\n"); t; t = strtok(NULL, " \t\n")) if (strncmp(t, kv, nl)) k += (size_t)snprintf(o + k, sizeof o - k, "%s%s", k ? " " : "", t);
    if (k < sizeof o) snprintf(o + k, sizeof o - k, "%s%s", k ? " " : "", kv);
    snprintf(P.env, sizeof P.env, "%s", o); prefs_save();
}
static void env_pick_cb(Modal* m, int btn, int item) {
    (void)m; (void)btn;
    if (item < 0 || item >= NENV || !*ENV[item][0]) return;
    env_add(T(ENV[item][0]));   /* T: example paths ("/путь/…") in the interface language */
    char t[600]; snprintf(t, sizeof t, "%s%s", T(ENV[item][0]), T(" — поправьте значение при необходимости")); ui_toast(t);
}
static void env_list(void) {
    Modal* m = modal(MK_LIST, T("Переменные (касание — добавить)"), T("Щелчок по строке добавляет переменную в поле «Доп. переменные» (та же переменная заменяется)."), env_pick_cb, T("Закрыть"), NULL, NULL);
    if (m) m->mono_items = 1;
    for (int i = 0; i < NENV; i++) {
        char b[1200];
        if (!*ENV[i][0]) snprintf(b, sizeof b, "\x01%s", T(ENV[i][1])); else snprintf(b, sizeof b, "%s\n%s", T(ENV[i][0]), T(ENV[i][1]));
        modal_item(m, b);
    }
}

/* -------------------------------------------------------------------------------- PhoneModify generator --- */
/* texts mirror me1cfg modgen (settings/me1cfg_cli.c) and the Android launcher (translations shared) */
static void modgen_text(Str* b, const char* lvl) {
    sadd(b, "# PhoneModify: %s\n%s%s", lvl, T("# Включить: переключатель \"Модификаторы PhoneModify\" в лаунчере (или ME1_PHONE_MOD=1).\n"),
         T("# Проверка в логе: [mod] PhoneModify active: ... и ', capped N draws' в строке кадра.\n\n"));
    if (!strcmp(lvl, "off")) return;
    int light = !strcmp(lvl, "light"), bal = !strcmp(lvl, "balanced"), agg = !strcmp(lvl, "aggressive"), ext = !strcmp(lvl, "extreme"), cpu = !strcmp(lvl, "cpu");
    if (!light) sadd(b, "ini SystemSettings ParticleLODBias %s%s", bal ? "1" : "2", T("          # частицы беднее: снегопад PeakDrift и прочий жор\n"));
    sadd(b, "%s", T("ini SystemSettings DynamicShadows False\nini SystemSettings LightEnvironmentShadows False    # проходы теней\n"));
    if (!light) sadd(b, "%s", T("ini SystemSettings DynamicDecals False        # кровь/следы: каждая перерисовывает геометрию\n"));
    if (agg || ext) sadd(b, "ini SystemSettings MotionBlur False\nini SystemSettings DepthOfField False\nini SystemSettings LensFlares False\nini SystemSettings QualityBloom False\n");
    if (ext) sadd(b, "%s", T("ini SystemSettings DirectionalLightmaps False   # 1 карта освещения вместо 3\n"));
    if (agg || ext) sadd(b, "%s", T("ini SystemSettings DetailMode 1              # мелкие детали окружения\n"));
    if (!light) sadd(b, "ini SystemSettings SkeletalMeshLODBias %s%s", ext ? "2" : "1", T("            # персонажи на LOD ниже\n"));
    if (cpu) sadd(b, "maxdraws 1200\nmaxprims 300000\n");
    if (ext) sadd(b, "%s", T("ini SystemSettings MaxAnisotropy 1                 # анизотропия\n"));
    if (agg) sadd(b, "%s", T("ini SystemSettings ScreenPercentage 70            # сцена в 70% разрешения, интерфейс полный\n"));
    if (ext) sadd(b, "%s", T("ini SystemSettings ScreenPercentage 60            # сцена в 60% разрешения, интерфейс полный\n"));
    if (bal) sadd(b, "maxdraws 6000\nmaxprims 3000000\n");
    if (agg) sadd(b, "maxdraws 3500\nmaxprims 1800000\n");
    if (ext) sadd(b, "maxdraws 2500\nmaxprims 1200000\n");
    sadd(b, "%s", T("\n# Тонкая настройка вручную: раскомментируй и поправь.\n# maxdraws 2000\n# maxprims 800000\n# ini SystemSettings ScreenPercentage 50\n# patch 10e7dc00 90 90 90        # пример: байты по адресу из лога (m_10e7dc00)\n"));
}
static void save_quiet(void) { char m[4096]; me1cfg_save(&C, m, sizeof m); config_reload(); }
static void modgen_write(const char* lvl) {
    char dir[PATH_MAX], f[PATH_MAX + 32];
    if (!ci_child(g_game, "PhoneModify", 1, dir, sizeof dir)) snprintf(dir, sizeof dir, "%s/PhoneModify", g_game);
    snprintf(f, sizeof f, "%s/phone_tune.txt", dir);
    char err[256];
    if (!strcmp(lvl, "off")) {
        unlink(f); me1cfg_set(&C, "port.phone_mod", "0", err, sizeof err); save_quiet();
        ui_toast(T("phone_tune.txt удалён, модификаторы выключены")); return;
    }
    mkdirs(dir);
    FILE* o = fopen(f, "w");
    if (!o) { char e[PATH_MAX + 128]; snprintf(e, sizeof e, "%s: %s", f, strerror(errno)); msg(T("Ошибка"), e); return; }
    Str b = { 0 }; modgen_text(&b, lvl); fputs(b.s, o); fclose(o); free(b.s);
    me1cfg_set(&C, "port.phone_mod", "1", err, sizeof err); save_quiet();
    char t[256]; snprintf(t, sizeof t, "%s%s%s", T("Создан PhoneModify/phone_tune.txt ("), lvl, T("), модификаторы включены")); ui_toast(t);
}
static int g_tier = -1;
static const char* modgen_auto(void) { return g_tier <= 0 ? "aggressive" : g_tier == 1 ? "balanced" : "light"; }
static void modgen_cb(Modal* m, int btn, int item) {
    (void)m; (void)btn;
    static const char* const LV[] = { "", "light", "balanced", "aggressive", "extreme", "cpu", "off" };
    if (item < 0 || item > 6) return;
    modgen_write(item == 0 ? modgen_auto() : LV[item]);
}
static void modgen_show(void) {
    if (!GS.found) { msg(T("Генератор PhoneModify"), T("Сначала укажите папку игры.")); return; }
    char b[512];
    Modal* m = modal(MK_LIST, T("Генератор PhoneModify"), T("Создаёт <папка игры>/PhoneModify/phone_tune.txt и включает «Модификаторы PhoneModify». На ПК обычно не нужен — только для очень слабых GPU."), modgen_cb, T("Отмена"), NULL, NULL);
    snprintf(b, sizeof b, "%s%s → %s%s", T("Авто — по компьютеру (GPU: "), gpu_name(), modgen_auto(), T(")  [рекомендуется]")); modal_item(m, b);
    modal_item(m, T("light — только тени (почти без потерь)"));
    modal_item(m, T("balanced — частицы/тени/декали/LOD, лимит 6000 вызовов и 3 млн примитивов"));
    modal_item(m, T("aggressive — + MotionBlur/DoF/блум выкл, сцена 70%, лимит 3500 и 1,8 млн"));
    modal_item(m, T("extreme — всё по минимуму, сцена 60%, лимит 2500 и 1,2 млн"));
    modal_item(m, T("cpu — для мощных GPU с медленным CPU: без снижения разрешения, лимит 1200 и 300 тыс."));
    modal_item(m, T("off — убрать модификатор и выключить режим"));
}

/* ------------------------------------------------------------------------- "what the game will see" --- */
static void check_report(void) {
    Str o = { 0 };
    sadd(&o, "Папка игры: %s\n", *g_game ? g_game : "—");
    sadd(&o, "  %s BioGame\\Config\\DefaultEngine.ini\n  %s Binaries\\MassEffect.exe\n  %s BioGame\\CookedPC\n  %s Binaries\\PhysXCore.dll\n",
         GS.ini ? "✓" : "✗", GS.exe ? "✓" : "✗", GS.cooked ? "✓" : "✗", GS.physx ? "✓" : "✗");
    sadd(&o, "Мои документы (для игры): C:\\Users\\Shepard\\Documents -> %s\n", g_docs);
    char p[PATH_MAX]; size_t n = 0; char* txt = NULL; const char* src = "BIOEngine.ini (создан игрой)";
    if (ci_path(g_docs, "BioWare/Mass Effect/Config/BIOEngine.ini", 0, p, sizeof p)) txt = read_file(p, &n);
    if (!txt && ci_path(g_game, "BioGame/Config/DefaultEngine.ini", 0, p, sizeof p)) {
        txt = read_file(p, &n); src = "DefaultEngine.ini + настройки порта";
        void* pt; size_t pn;
        if (txt && me1cfg_patch_default(g_docs, "Engine", txt, n, &pt, &pn)) { free(txt); txt = malloc(pn + 1); memcpy(txt, pt, pn); txt[pn] = 0; n = pn; free(pt); }
    }
    if (!txt) sadd(&o, "Конфигурация игры не найдена — скопируйте игру в папку выше.\n");
    else {
        Ini ini; ini_init(&ini); ini_load_mem(&ini, txt, n); free(txt);
        sadd(&o, "Игра прочитает (%s):\n", src);
        static const char* const Q[][3] = {
            { "WinDrv.WindowsClient", "StartupResolutionX", "Разрешение X" }, { "WinDrv.WindowsClient", "StartupResolutionY", "Разрешение Y" },
            { "WinDrv.WindowsClient", "StartupFullscreen", "Полный экран" },
            { "WinDrv.WindowsClient", "TextureLODLevel", "Детализация текстур" }, { "WinDrv.WindowsClient", "FilterLevel", "Фильтрация" },
            { "WinDrv.WindowsClient", "EnableDynamicShadows", "Динамические тени" }, { "WinDrv.WindowsClient", "UseVSync", "V-Sync" },
            { "SystemSettings", "ParticleLODBias", "ParticleLODBias" }, { "Engine.GameEngine", "bUsePostProcessEffects", "Постобработка" },
            { "D3DDrv.D3DRenderDevice", "MaxTextureSize", "MaxTextureSize" }, { "ISACTAudio.ISACTAudioDevice", "MaxChannels", "Каналов звука" },
            { "Engine.Client", "DisplayGamma", "Гамма" }, { "Engine.Engine", "Language", "Язык" } };
        for (unsigned i = 0; i < sizeof Q / sizeof Q[0]; i++) {
            char v[128]; if (!ini_get(&ini, Q[i][0], Q[i][1], v, sizeof v)) strcpy(v, "—");
            int w = 0; for (const unsigned char* q = (const unsigned char*)Q[i][2]; *q; q++) w += (*q & 0xC0) != 0x80;
            sadd(&o, "  %s%*s %s\n", Q[i][2], w < 22 ? 22 - w : 0, "", v);
        }
        ini_free(&ini);
    }
    /* the port's own options and what me1-linux will be started with */
    char v[64];
    sadd(&o, "\nme1port.ini:\n");
    static const char* const PO[] = { "port.fps_limit", "port.show_fps", "port.simple_shaders", "port.phone_mod" };
    for (unsigned i = 0; i < 4; i++) { if (!me1cfg_port_get(g_docs, PO[i], v, sizeof v)) strcpy(v, "—"); sadd(&o, "  %-22s %s\n", PO[i], v); }
    sadd(&o, "\nme1-linux: %s\n", *g_me1linux ? g_me1linux : "✗");
    sadd(&o, "  %s libme1spv.so (Vulkan)\n", spv_ok() ? "✓" : "✗");
    char* t = i18n_tx(o.s); free(o.s);
    modal(MK_MONO, T("Что увидит игра"), t, NULL, "OK", T("Копировать"), NULL); free(t);
}

/* ------------------------------------------------------------------------------------------ the log --- */
static int interesting(const char* l) {
    static const char* const pre[] = { "[rt]", "[exe]", "[d3d", "[gl]", "[vk]", "[bink]", "[hb]", "[log]", "[dinput]", "[ui]", "[al]", "[mm]",
        "[hle] MessageBox", "[linux]", "[spv]", "[xinput]", "[mod]", "  ", "[hle] unimplemented", "[hle] missing", "me1-linux" };
    for (unsigned i = 0; i < sizeof pre / sizeof *pre; i++) if (!strncmp(l, pre[i], strlen(pre[i]))) return 1;
    return strstr(l, "distinct APIs called") || strstr(l, "FAULT") || strstr(l, "CRASH");
}
static void log_path(char* o, size_t cap) { snprintf(o, cap, "%s/me1exe_run.log", g_docs); }
static void log_cb(Modal* m, int btn, int item) { (void)m; (void)item; if (btn == 1) { char p[PATH_MAX]; log_path(p, sizeof p); char* a[] = { "xdg-open", p, NULL }; spawn_detached(a); } }
static void log_show(void) {
    char p[PATH_MAX]; log_path(p, sizeof p);
    char* t = read_file(p, NULL);
    if (!t) { char e[PATH_MAX + 64]; snprintf(e, sizeof e, "%s%s", T("Нет лога: "), pretty(p)); msg(T("Лог последнего запуска"), e); return; }
    enum { MAXL = 400 }; static char* ring[MAXL]; int n = 0, head = 0;
    for (char* s = t; s && *s; ) {
        char* e = strchr(s, '\n'); if (e) *e = 0;
        size_t l = strlen(s); if (l && s[l - 1] == '\r') s[--l] = 0;
        if (interesting(s)) { if (l > 300) strcpy(s + 297, " …"); if (n < MAXL) ring[n++] = s; else { ring[head] = s; head = (head + 1) % MAXL; } }
        s = e ? e + 1 : NULL;
    }
    Str b = { 0 };
    for (int i = 0; i < n; i++) sadd(&b, "%s\n", ring[(head + i) % (n < MAXL ? MAXL : n)]);
    if (!n) sadd(&b, "(пусто)\n");
    char title[PATH_MAX + 8]; snprintf(title, sizeof title, "%s", pretty(p));
    Modal* m = modal(MK_MONO, title, b.s, log_cb, "OK", T("Открыть файл"), T("Копировать"));
    if (m) m->to_end = 1;
    free(b.s); free(t);
}

/* ------------------------------------------------------------------------------------------ caches --- */
#include <ftw.h>
static int rm_one(const char* p, const struct stat* st, int fl, struct FTW* f) { (void)st; (void)fl; (void)f; return remove(p); }
static long dir_size(const char* d) {
    long sz = 0; DIR* dd = opendir(d); struct dirent* de; char p[PATH_MAX]; struct stat st;
    if (!dd) return 0;
    while ((de = readdir(dd))) { if (de->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s", d, de->d_name); if (stat(p, &st) == 0) sz += S_ISDIR(st.st_mode) ? dir_size(p) : (long)st.st_size; }
    closedir(dd); return sz;
}
static void cache_dirs(char* gl, char* hlsl, size_t cap) {
    const char* xc = getenv("XDG_CACHE_HOME");
    if (xc && *xc) snprintf(gl, cap, "%s/me1port/gl_cache", xc); else snprintf(gl, cap, "%s/.cache/me1port/gl_cache", home());
    snprintf(hlsl, cap, "%s/me1_hlsl_cache", g_docs);
}
static void cache_cb(Modal* m, int btn, int item) {
    (void)m; (void)item; if (btn < 0 || btn > 1) return;
    char gl[PATH_MAX], hl[PATH_MAX]; cache_dirs(gl, hl, sizeof gl);
    nftw(gl, rm_one, 16, FTW_DEPTH | FTW_PHYS); mkdirs(gl);
    if (btn == 1) nftw(hl, rm_one, 16, FTW_DEPTH | FTW_PHYS);
    ui_toast(T("Кэш очищен"));
}
static void cache_ask(void) {
    char gl[PATH_MAX], hl[PATH_MAX], b[PATH_MAX * 2 + 512]; cache_dirs(gl, hl, sizeof gl);
    snprintf(b, sizeof b, T("Собранные программы GL/Vulkan: %s (%.1f МБ) — пересоберутся за секунды.\nСкомпилированные HLSL-шейдеры: %s (%.1f МБ) — компиляция заново займёт несколько минут при первом запуске.\n\nОчищать стоит после обновления драйвера, если появились артефакты."),
             pretty(gl), dir_size(gl) / 1048576.0, pretty(hl), dir_size(hl) / 1048576.0);
    modal(MK_MSG, T("Очистить кэш шейдеров"), b, cache_cb, T("Только GL/Vulkan"), T("Всё, включая HLSL"), T("Отмена"));
}

/* ------------------------------------------------------------------------------------ game folder --- */
static void expand_home(const char* in, char* out, size_t cap) {
    while (*in == ' ') in++;
    if (in[0] == '~' && (in[1] == '/' || !in[1])) snprintf(out, cap, "%s%s", home(), in + 1); else snprintf(out, cap, "%s", in);
    size_t n = strlen(out); while (n > 1 && (out[n - 1] == ' ' || out[n - 1] == '\n' || out[n - 1] == '/')) out[--n] = 0;
}
/* the folder itself or a game folder up to two levels inside it */
static int find_game_in(const char* d, char* out, size_t cap) {
    if (is_game(d)) { snprintf(out, cap, "%s", d); return 1; }
    DIR* dd = opendir(d); struct dirent* de; char p[PATH_MAX]; int ok = 0;
    if (!dd) return 0;
    while (!ok && (de = readdir(dd))) {
        if (de->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s", d, de->d_name);
        if (is_game(p)) { snprintf(out, cap, "%s", p); ok = 1; break; }
        if (is_dir(p)) { DIR* d2 = opendir(p); struct dirent* e2; char q[PATH_MAX];
            while (d2 && (e2 = readdir(d2))) { if (e2->d_name[0] == '.') continue; snprintf(q, sizeof q, "%s/%s", p, e2->d_name); if (is_game(q)) { snprintf(out, cap, "%s", q); ok = 1; break; } }
            if (d2) closedir(d2); }
    }
    closedir(dd); return ok;
}
static void mods_scan(void);
static void choose_game(const char* raw) {
    char p[PATH_MAX], g[PATH_MAX]; expand_home(raw, p, sizeof p);
    if (!*p) return;
    if (!is_dir(p)) { char t[PATH_MAX + 64]; snprintf(t, sizeof t, "%s%s", T("Нет такой папки: "), p); ui_toast(t); return; }
    if (!find_game_in(p, g, sizeof g)) { ui_toast(T("В этой папке нет Binaries/MassEffect.exe — выберите папку игры (в ней Binaries и BioGame)")); return; }
    game_dir_set(g); mods_scan();
    char t[PATH_MAX + 32]; snprintf(t, sizeof t, "%s%s", T("Игра: "), pretty(g_game)); ui_toast(t);
}
static void path_cb(Modal* m, int btn, int item) { (void)item; if (btn == 0) choose_game(m->input); }
static void path_modal(void) {
    Modal* m = modal(MK_INPUT, T("Папка игры"), T("Путь к папке с Binaries и BioGame (можно ~/…). Подойдёт и папка выше — игра найдётся внутри (до двух уровней)."), path_cb, T("Выбрать"), T("Отмена"), NULL);
    if (m) snprintf(m->input, sizeof m->input, "%s", *g_game ? pretty(g_game) : "~/");
}
/* the desktop's folder dialog (zenity / kdialog / yad) in a thread; without one - the path field */
static struct { pthread_t th; volatile int running, done; char out[PATH_MAX]; } PK;
static void* picker_main(void* u) {
    (void)u; char cmd[PATH_MAX * 2 + 256], start[PATH_MAX]; snprintf(start, sizeof start, "%s/", *g_game ? g_game : home());
    for (char* q = start; *q; q++) if (*q == '\'') *q = '_';
    if (in_path("zenity")) snprintf(cmd, sizeof cmd, "zenity --file-selection --directory --title='%s' --filename='%s' 2>/dev/null", T("Папка Mass Effect (с Binaries и BioGame)"), start);
    else if (in_path("kdialog")) snprintf(cmd, sizeof cmd, "kdialog --getexistingdirectory '%s' --title '%s' 2>/dev/null", start, T("Папка Mass Effect (с Binaries и BioGame)"));
    else snprintf(cmd, sizeof cmd, "yad --file --directory --filename='%s' 2>/dev/null", start);
    PK.out[0] = 0;
    FILE* f = popen(cmd, "r");
    if (f) { if (!fgets(PK.out, sizeof PK.out, f)) PK.out[0] = 0; pclose(f); }
    PK.out[strcspn(PK.out, "\n")] = 0;
    PK.done = 1; PK.running = 0;
    SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_USEREVENT; e.user.code = 2; SDL_PushEvent(&e);
    return NULL;
}
static void pick_folder(void) {
    if (PK.running) return;
    if (!in_path("zenity") && !in_path("kdialog") && !in_path("yad")) { path_modal(); return; }
    PK.running = 1; PK.done = 0;
    pthread_create(&PK.th, NULL, picker_main, NULL); pthread_detach(PK.th);
}

/* ---------------------------------------------------------------------------------------- launching --- */
static pid_t g_child = -1; static uint32_t g_child_t0; static int g_play, g_quit;
static void enabled_mod_roots(Str* s) { for (int i = 0; i < g_nmods; i++) if (mod_on(g_mods[i].name)) sadd(s, "%s%s", s->n ? ":" : "", g_mods[i].root); }
static int do_save(int quiet) {
    char m[4096]; int rc = me1cfg_save(&C, m, sizeof m);
    char* t = i18n_tx(m);
    if (rc) msg(T("Ошибка"), t);
    else if (!quiet) msg(T("Сохранено"), t);
    free(t);
    if (!rc) config_reload();
    return rc;
}
static void launch(void) {
    if (g_child > 0) return;
    if (!GS.found) { msg(T("Игра не найдена"), T("Укажите папку игры на вкладке «Главная» (в ней Binaries/MassEffect.exe и BioGame).")); return; }
    if (!*g_me1linux) { msg(T("Нет me1-linux"), T("Рядом с лаунчером нет программы me1-linux. Соберите порт (make linux) и установите: linux/install.sh.")); return; }
    if (g_dirty && do_save(1)) return;
    prefs_save();
    char* av[40]; int ac = 0; Str mods = { 0 };
    av[ac++] = g_me1linux; av[ac++] = "--game"; av[ac++] = g_game; av[ac++] = "--docs"; av[ac++] = g_docs;
    av[ac++] = "--lang"; av[ac++] = (char*)I18N_CODES[i18n_cur()];
    if (strcmp(P.renderer, "auto")) { av[ac++] = "--renderer"; av[ac++] = P.renderer; }
    if (strcmp(P.window, "auto")) { av[ac++] = "--window"; av[ac++] = P.window; }
    if (!P.grab) av[ac++] = "--no-grab";
    enabled_mod_roots(&mods);
    if (mods.n) { av[ac++] = "--mods"; av[ac++] = mods.s; }
    av[ac] = NULL;
    fflush(stdout); fflush(stderr);
    pid_t p = fork();
    if (p == 0) {
        for (int fd = 3; fd < 1024; fd++) close(fd);
        if (strcmp(P.fexact, "auto")) setenv("ME1_FLOAT_EMU", P.fexact, 1);
        if (P.vkdev >= 0) { char v[16]; snprintf(v, sizeof v, "%d", P.vkdev); setenv("ME1_VK_DEVICE", v, 1); }
        if (pad_mod_on()) setenv("ME1_XINPUT", "1", 0);
        char env[sizeof P.env]; snprintf(env, sizeof env, "%s", P.env);
        for (char* t = strtok(env, " \t\n"); t; t = strtok(NULL, " \t\n")) { char* eq = strchr(t, '='); if (eq && eq > t) { *eq = 0; setenv(t, eq + 1, 1); } }
        execv(av[0], av);
        fprintf(stderr, "me1-launcher: exec %s: %s\n", av[0], strerror(errno)); _exit(127);
    }
    free(mods.s);
    if (p < 0) { char e[256]; snprintf(e, sizeof e, "fork: %s", strerror(errno)); msg(T("Ошибка"), e); return; }
    g_child = p; g_child_t0 = SDL_GetTicks();
    fprintf(stderr, "me1-launcher: started %s (pid %d)\n", g_me1linux, (int)p);
    SDL_HideWindow(g_win);
}
static void result_cb(Modal* m, int btn, int item) { (void)m; (void)item; if (btn == 1) log_show(); }
static void child_done(int st) {
    uint32_t secs = (SDL_GetTicks() - g_child_t0) / 1000; g_child = -1;
    game_status(); config_reload(); mods_scan();
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (ok && g_play) { g_quit = 1; return; }
    SDL_ShowWindow(g_win); SDL_RaiseWindow(g_win);
    char b[512], tm[64];
    if (secs >= 3600) snprintf(tm, sizeof tm, T("%u ч %u мин"), secs / 3600, secs / 60 % 60); else snprintf(tm, sizeof tm, T("%u мин %u с"), secs / 60, secs % 60);
    if (ok) { snprintf(b, sizeof b, "%s%s", T("Игра закрыта, время: "), tm); ui_toast(b); return; }
    if (WIFSIGNALED(st)) snprintf(b, sizeof b, T("Игра аварийно завершилась (сигнал %d, %s) через %s. Подробности — в логе последнего запуска."), WTERMSIG(st), strsignal(WTERMSIG(st)), tm);
    else snprintf(b, sizeof b, T("Игра завершилась с кодом %d через %s. Подробности — в логе последнего запуска."), WEXITSTATUS(st), tm);
    modal(MK_MSG, T("Игра завершилась с ошибкой"), b, result_cb, "OK", T("Показать лог"), NULL);
}

/* -------------------------------------------------------------------------------------- custom rows --- */
static int row_layout(const char* label, const char* help, SDL_Rect* ctrl, int cwide) {
    int cw = L.w * 2 / 5; if (cw > D(330)) cw = D(330); if (cw < D(190)) cw = D(190); if (!cwide) cw = D(60);
    int tw = L.w - cw - D(28), lh = ui_lineh(UF_REG, D(15));
    int hh = help && *help ? ui_textbox_h(UF_REG, D(12), tw, help) : 0;
    int h = lh + (hh ? D(3) + hh : 0); if (h < D(34)) h = D(34);
    ui_text_fit(UF_REG, D(15), L.x, L.y + D(7), tw, UIC(C_TEXT), label);
    if (hh) ui_textbox(UF_REG, D(12), L.x, L.y + D(7) + lh + D(3), tw, UIC(C_DIM), help);
    *ctrl = (SDL_Rect){ L.x + L.w - cw, L.y + D(7), cw, D(34) };
    return h + D(14);
}
static void row_done(int h) { L.y += h; SDL_Rect sep = { L.x, L.y - D(1), L.w, 1 }; ui_fill(sep, UICA(C_LINE, 90)); }
static int row_combo(const char* key, const char* label, const char* help, const char* const* labs, int n, int* sel) {
    SDL_Rect c; int h = row_layout(label, help, &c, 1);
    int r = ui_combo(ui_id(key, 2), c, labs, n, sel); row_done(h); return r;
}
static int row_switch(const char* key, const char* label, const char* help, int on) {
    SDL_Rect c; int h = row_layout(label, help, &c, 0);
    int r = ui_switch(ui_id(key, 3), c, "", on, 0); row_done(h); return r;
}

/* ------------------------------------------------------------------------------------------- pages --- */
enum { PG_HOME, PG_GFX, PG_GAME, PG_CTRL, PG_MORE, PG_N };
static const char* const PAGES[PG_N] = { "Главная", "Графика", "Игра и звук", "Управление и моды", "Дополнительно" };
static int g_page; static UiScroll g_sc[PG_N]; static SDL_Rect g_area;
static SDL_Texture* g_icon; static int g_iw, g_ih;
static float g_k = 1;   /* base scale of this display */

static void refresh_auto_report(void) { char res[32]; int fps; g_tier = classify(g_autorep, sizeof g_autorep, res, sizeof res, &fps); }
static void first_run_auto(void) {
    if (P.auto_done || !GS.found) return;
    P.auto_done = 1; prefs_save();
    char p[PATH_MAX + 64]; snprintf(p, sizeof p, "%s/BioWare/Mass Effect/me1port.ini", g_docs);
    if (is_file(p)) return;   /* the user already has settings */
    char rep[1024]; if (apply_auto(rep, sizeof rep) < 0) return;
    char m[4096]; int rc = me1cfg_save(&C, m, sizeof m); config_reload();
    Str b = { 0 }; char* t = i18n_tx(rep); sadd(&b, "%s%s", t, T("\n\nИзменить можно на вкладке «Графика» (пресеты и отдельные настройки).")); free(t);
    if (rc) { t = i18n_tx(m); sadd(&b, "\n\n%s", t); free(t); }
    msg(T("Настройки подобраны автоматически"), b.s); free(b.s);
}
static void auto_now(void) {
    char rep[1024]; int t = apply_auto(rep, sizeof rep);
    char* tx = i18n_tx(rep); char b[1400];
    if (t < 0) snprintf(b, sizeof b, "%s", tx); else snprintf(b, sizeof b, "%s%s%s", T("Автопресет: "), tx, T(" — нажмите «Сохранить»"));
    free(tx); ui_toast(b);
}
static void preset_apply(int k) {
    char res[96], fps[16], err[256];
    snprintf(res, sizeof res, "%s", cfg_get("screen.resolution")); snprintf(fps, sizeof fps, "%s", cfg_get("port.fps_limit"));
    if (me1cfg_preset(&C, me1_presets[k].id, err, sizeof err)) { char* t = i18n_tx(err); ui_toast(t); free(t); return; }
    me1cfg_set(&C, "screen.resolution", res, err, sizeof err); me1cfg_set(&C, "port.fps_limit", fps, err, sizeof err);   /* desktop: quality only */
    g_dirty = 1; ui_toast(T("Пресет применён — нажмите «Сохранить»"));
}

static void banner(void) {
    SDL_Rect bn = { g_area.x, L.y - D(24), g_area.w, D(230) };
    if (g_banner) ui_image_cover(g_banner, g_bw, g_bh, bn, 0.38f); else ui_fill(bn, UIC(C_PANEL));
    SDL_Rect lo = { bn.x, bn.y + bn.h / 3, bn.w, bn.h - bn.h / 3 + 1 }; ui_vgrad(lo, UICA(C_BG, 0), UICA(C_BG, 255));
    SDL_Rect le = { bn.x, bn.y, bn.w * 2 / 3, bn.h }; ui_hgrad(le, UICA(C_BG, 200), UICA(C_BG, 0));
    ui_text(UF_REG, D(15), L.x, bn.y + bn.h - D(36), UIC(C_ACC2), T("Порт ПК-версии 2007 года для Linux · статическая рекомпиляция"), -1);
    L.y = bn.y + bn.h + D(8);
}
static void page_home(void) {
    banner();
    /* files */
    UiCol acc = UIC(GS.found ? C_OK : C_WARN);
    card_begin("files", T("Файлы игры"), acc);
    const char* st = GS.found ? T("✓ Игра найдена") : *g_game && GS.ini ? T("✓ Конфиги найдены, ✗ нет Binaries\\MassEffect.exe") : T("✗ Игра не найдена");
    char b[PATH_MAX * 2];
    snprintf(b, sizeof b, "%s%s", st, GS.found && !GS.cooked ? T(" (нет BioGame\\CookedPC)") : "");
    status_line(b, UIC(GS.found ? C_OK : C_WARN));
    snprintf(b, sizeof b, "%s: %s", T("Папка игры"), *g_game ? pretty(g_game) : "—"); status_line(b, UIC(C_TEXT));
    if (!GS.found) para(13, UIC(C_DIM), T("Нужна установленная ПК-версия Mass Effect (Steam, EA/Origin, GOG или диск, версия 1.02): папка, в которой лежат Binaries и BioGame. Её можно выбрать вручную или найти автоматически — поиск просматривает домашнюю папку, библиотеки Steam, префиксы Wine/Lutris/Bottles/Heroic и подключённые диски."));
    L.y += D(4);
    flow_begin();
    if (flow_btn(T("Выбрать папку…"), PK.running ? UB_NORMAL | UB_DISABLED : UB_NORMAL)) pick_folder();
    if (flow_btn(T("Ввести путь…"), UB_NORMAL)) path_modal();
    if (flow_btn(S.running ? T("Поиск…") : T("Найти автоматически"), S.running ? UB_NORMAL | UB_DISABLED : UB_NORMAL)) search_start();
    if (GS.found && flow_btn(T("Открыть папку"), UB_NORMAL)) open_path(g_game);
    if (flow_btn(T("Проверить"), UB_NORMAL)) check_report();
    flow_end();
    if (S.running) { snprintf(b, sizeof b, T("Поиск… просмотрено папок: %ld"), S.scanned); status_line(b, UIC(C_ACC2)); ui.redraw = 1; }
    if (S.done && !S.n) status_line(T("Игра не найдена в домашней папке, библиотеках Steam и на подключённых дисках — выберите папку вручную."), UIC(C_WARN));
    if (S.done && S.n) {
        status_line(T("Найдено (щелчок — выбрать):"), UIC(C_DIM));
        for (int i = 0; i < S.n; i++) {
            char r[PATH_MAX]; int cur = realpath(S.found[i], r) && !strcmp(r, g_game);
            snprintf(b, sizeof b, "%s %s%s", cur ? "●" : "○", pretty(S.found[i]), cur ? T("  (выбрана)") : "");
            SDL_Rect rr = lrow(D(32));
            if (ui_button(ui_id(S.found[i], 5), rr, "", UB_FLAT) && !cur) { game_dir_set(S.found[i]); mods_scan(); first_run_auto(); }
            ui_text_fit(UF_REG, D(14), rr.x + D(10), rr.y + (rr.h - ui_lineh(UF_REG, D(14))) / 2, rr.w - D(20), UIC(cur ? C_OK : C_TEXT), b);
        }
        L.y += D(6);
    }
    SDL_Rect sep = { L.x, L.y, L.w, 1 }; ui_fill(sep, UICA(C_LINE, 120)); L.y += D(12);
    char sd[PATH_MAX + 32]; snprintf(sd, sizeof sd, "%s/BioWare/Mass Effect", g_docs);
    snprintf(b, sizeof b, "%s%s", T("Сохранения и настройки: "), pretty(sd));
    { SDL_Rect r = lrow(D(34)); int bw = D(110); ui_text_fit(UF_REG, D(13), r.x, r.y + D(8), r.w - bw - D(10), UIC(C_DIM), b);
      SDL_Rect br = { r.x + r.w - bw, r.y, bw, D(32) }; if (ui_button(ui_id("open-docs", 0), br, T("Открыть"), UB_FLAT)) open_path(sd); }
    if (GS.found) { dlc_status(b, sizeof b); status_line(b, UIC(C_DIM)); }
    card_end();
    if (!*g_me1linux) {
        card_begin("nolinux", T("Нет me1-linux"), UIC(C_ERR));
        para(14, UIC(C_WARN), T("Рядом с лаунчером нет программы me1-linux. Соберите порт (make linux) и установите: linux/install.sh."));
        card_end();
    }
    /* the machine */
    card_begin("pc", T("Компьютер"), UIC(C_ACC));
    if (*g_cpu) { snprintf(b, sizeof b, "CPU: %s · %d%s · %s %.1f %s", g_cpu, g_threads, T(" потоков"), T("ОЗУ"), g_ram_mb / 1024.0, T("ГБ")); status_line(b, UIC(C_TEXT)); }
    if (g_ngpu > 0) for (int i = 0; i < g_ngpu; i++) {
        GpuDev* d = &g_gpu[i];
        snprintf(b, sizeof b, "%s GPU %d: %s — %s, Vulkan %u.%u%s", i == cur_gpu() ? "★" : "  ", i, d->name, T(gpu_type_name(d->type)), d->api >> 22, (d->api >> 12) & 0x3FF,
                 i == cur_gpu() ? T("  (для игры)") : "");
        status_line(b, UIC(i == cur_gpu() ? C_ACC2 : C_DIM));
    } else { snprintf(b, sizeof b, "%s · OpenGL: %s %s", T("Vulkan нет"), *g_glr ? g_glr : "?", g_glv); status_line(b, UIC(C_WARN)); }
    { char* t = i18n_tx(g_autorep); snprintf(b, sizeof b, "%s%s", T("Автопресет: "), t); free(t); para(13, UIC(C_DIM), b); }
    flow_begin();
    if (flow_btn(T("Подобрать настройки автоматически"), GS.found ? UB_NORMAL : UB_NORMAL | UB_DISABLED)) auto_now();
    if (flow_btn(T("Настройки графики →"), UB_FLAT)) g_page = PG_GFX;
    flow_end();
    card_end();
}
static const char* const REN_ID[] = { "auto", "vulkan", "gl", "zink" };
static const char* const FE_ID[] = { "auto", "2", "1", "0" };
static void page_gfx(void) {
    char b[1024];
    card_begin("renderer", T("Рендерер"), UIC(C_ACC));
    {
        const char* labs[4] = { T("Авто (Vulkan, если есть)"), T("Vulkan — свой D3D9 → Vulkan"), T("OpenGL"), T("Zink — OpenGL через Vulkan (Mesa)") };
        int sel = 0; for (int i = 0; i < 4; i++) if (!strcmp(P.renderer, REN_ID[i])) sel = i;
        if (!strcmp(P.renderer, "auto")) {
            if (auto_vulkan()) snprintf(b, sizeof b, "%s%s", T("Сейчас: Vulkan — "), gpu_name());
            else snprintf(b, sizeof b, "%s%s", T("Сейчас: OpenGL — "), !spv_ok() ? T("нет libme1spv.so (компилятор шейдеров для Vulkan)") : g_ngpu <= 0 ? T("нет Vulkan") : T("Vulkan только программный (llvmpipe)"));
        } else if (!strcmp(P.renderer, "vulkan") && !spv_ok()) snprintf(b, sizeof b, "%s", T("⚠ Нет libme1spv.so — без него Vulkan не запустится"));
        else snprintf(b, sizeof b, "%s", T("Vulkan обычно быстрее и без артефактов; OpenGL — запасной вариант"));
        if (row_combo("ren", T("Рендерер"), b, labs, 4, &sel)) { snprintf(P.renderer, sizeof P.renderer, "%s", REN_ID[sel]); prefs_save(); }
    }
    if (g_ngpu > 0) {
        static char lb[9][300]; const char* labs[9]; int n = 0;
        snprintf(lb[n], sizeof lb[n], "%s%s)", T("Авто ("), g_bgpu >= 0 ? g_gpu[g_bgpu].name : "?"); labs[n] = lb[n]; n++;
        for (int i = 0; i < g_ngpu && n < 9; i++) { snprintf(lb[n], sizeof lb[n], "%d: %s (%s)", i, g_gpu[i].name, T(gpu_type_name(g_gpu[i].type))); labs[n] = lb[n]; n++; }
        int sel = P.vkdev >= 0 && P.vkdev < g_ngpu ? P.vkdev + 1 : 0;
        if (row_combo("vkdev", T("Видеокарта (Vulkan)"), T("По умолчанию — дискретная, затем встроенная"), labs, n, &sel)) { P.vkdev = sel - 1; prefs_save(); refresh_auto_report(); }
    }
    {
        const char* labs[4] = { T("Авто (точная)"), T("Точная — без артефактов"), T("Частичная"), T("Выключена — быстрее") };
        int sel = 0; for (int i = 0; i < 4; i++) if (!strcmp(P.fexact, FE_ID[i])) sel = i;
        if (row_combo("fexact", T("Точность float (D3D9)"), T("Эмуляция правил float D3D9 (0·∞ = 0) в шейдерах: лечит чёрные/белые точки и мерцание. ME1_FLOAT_EMU"), labs, 4, &sel)) { snprintf(P.fexact, sizeof P.fexact, "%s", FE_ID[sel]); prefs_save(); }
    }
    {
        static const char* const WS[] = { "1280x720", "1366x768", "1600x900", "1920x1080", "2560x1440", "3840x2160" };
        static char lb[10][64]; const char* labs[10]; const char* vals[10]; int n = 0, sel = 0;
        vals[n] = "auto"; snprintf(lb[n], sizeof lb[n], "%s", T("Авто — по разрешению игры")); labs[n] = lb[n]; n++;
        for (int i = 0; i < 6; i++) { int w, h; sscanf(WS[i], "%dx%d", &w, &h); if (g_dw > 0 && (w > g_dw || h > g_dh)) continue; vals[n] = WS[i]; snprintf(lb[n], sizeof lb[n], "%d×%d", w, h); labs[n] = lb[n]; n++; }
        for (int i = 0; i < n; i++) if (!strcmp(vals[i], P.window)) sel = i;
        if (sel == 0 && strcmp(P.window, "auto")) { vals[n] = P.window; snprintf(lb[n], sizeof lb[n], "%s%s", P.window, T(" (свой)")); labs[n] = lb[n]; sel = n++; }
        if (row_combo("win", T("Размер окна"), T("В оконном режиме; картинка масштабируется в окно"), labs, n, &sel)) { snprintf(P.window, sizeof P.window, "%s", vals[sel]); prefs_save(); }
    }
    if (row_switch("grab", T("Захватывать мышь"), T("Курсор не уходит из окна игры; в игре Ctrl+Alt+M — отпустить/захватить"), P.grab)) { P.grab = !P.grab; prefs_save(); }
    card_end();
    /* presets */
    card_begin("presets", T("Пресеты"), UIC(C_ACC));
    para(13, UIC(C_DIM), T("Пресет меняет качество графики; разрешение и ограничение FPS остаются вашими. Изменения вступают в силу после «Сохранить»."));
    flow_begin(); int hov = -1;
    { SDL_Rect r = flow_rect(T("Авто (рекомендуется)"), UB_PRIMARY); if (ui_button(ui_id("preset-auto", 0), r, T("Авто (рекомендуется)"), UB_PRIMARY)) auto_now(); if (ui_hover(r)) hov = 99; }
    for (int i = 0; i < me1_npresets; i++) { SDL_Rect r = flow_rect(T(me1_presets[i].label), UB_NORMAL); if (ui_button(ui_id(me1_presets[i].id, 9), r, T(me1_presets[i].label), UB_NORMAL)) preset_apply(i); if (ui_hover(r)) hov = i; }
    flow_end();
    { char* t = NULL;
      if (hov == 99) { t = i18n_tx(g_autorep); snprintf(b, sizeof b, "%s", t); }
      else snprintf(b, sizeof b, "%s", hov >= 0 ? T(preset_help(hov)) : T("Наведите на пресет — здесь появится описание."));
      free(t); para(13, UIC(hov >= 0 ? C_ACC2 : C_DIM), b); }
    card_end();
    settings_group("Экран", NULL);
    settings_group("Графика", NULL);
    settings_group("Порт", NULL);
}
static void page_game(void) {
    settings_group("Игра", NULL);
    settings_group("Звук", NULL);
}
static void page_ctrl(void) {
    char b[2048];
    card_begin("pad", T("Геймпад"), UIC(C_ACC));
    pads(b, sizeof b);
    if (*b) { char t[2100]; snprintf(t, sizeof t, "%s%s", T("Геймпад: "), b); status_line(t, UIC(C_OK)); }
    else status_line(T("Геймпад не подключён (USB/Bluetooth-геймпады определяются автоматически)."), UIC(C_DIM));
    snprintf(b, sizeof b, "%s%s", T("Игра видит геймпад как контроллер Xbox 360 (XInput) — Xbox, PlayStation, Switch Pro и другие, которые знает SDL. В оригинальной игре нет "),
             T("привязок для геймпада — нужен мод ME1Controller (включите его ниже). Мод рассчитан на 16:9 и английский текст подсказок."));
    para(13, UIC(C_DIM), b);
    if (SDL_NumJoysticks() > 0 && !pad_mod_on()) para(13, UIC(C_WARN), T("⚠ Не включён мод с привязками геймпада (Config/BIOInput.ini с XboxTypeS_…) — в игре геймпад работать не будет."));
    card_end();
    card_begin("kbd", T("Клавиатура и мышь"), UIC(C_ACC));
    para(13, UIC(C_DIM), T("Как в оригинальной игре на ПК. В игре: Alt+Enter или F11 — окно ↔ полный экран, Ctrl+Alt+M — отпустить/захватить мышь."));
    for (int i = 0; i < me1_nsettings; i++) if (!strcmp(me1_settings[i].group, "Управление")) setting_row(i);
    card_end();
    card_begin("mods", T("Моды"), UIC(C_ACC));
    if (!GS.found) para(13, UIC(C_WARN), T("Сначала укажите папку игры."));
    else {
        snprintf(b, sizeof b, "%s%s%s%s%s", T("Моды: "), pretty(g_modsdir), T("/<имя мода>/ (внутри — Config и «Mass Effect»; файлы подменяются при запуске, "), T("оригинал не меняется)"), g_nmods ? "" : T(" — модов нет"));
        para(13, UIC(C_DIM), b);
        for (int i = 0; i < g_nmods; i++) {
            Mod* m = &g_mods[i]; char lab[300]; snprintf(lab, sizeof lab, "%s%s", m->name, m->pad ? T(" (геймпад)") : "");
            int on = mod_on(m->name);
            if (ui_check(ui_id(m->name, 11), lrow(D(36)), lab, on)) {
                mod_set(m->name, !on);
                if (m->pad) ui_toast(!on ? T("Профиль управления: геймпад XInput") : T("Профиль управления: клавиатура и мышь"));
            }
            char note[1024]; mod_dlc_note(m, note, sizeof note);
            if (*note) { L.x += D(34); L.w -= D(34); para(12, UIC(C_DIM), note); L.x -= D(34); L.w += D(34); }
        }
        L.y += D(4);
        flow_begin();
        if (flow_btn(T("Открыть папку модов"), UB_NORMAL)) open_path(g_modsdir);
        if (flow_btn(T("Обновить список"), UB_NORMAL)) { mods_scan(); ui_toast(T("Список модов обновлён")); }
        flow_end();
        dlc_status(b, sizeof b); status_line(b, UIC(C_DIM));
    }
    card_end();
}
static void page_more(void) {
    char b[PATH_MAX * 2];
    card_begin("env", T("Доп. переменные (для опытов)"), UIC(C_ACC));
    para(13, UIC(C_DIM), T("Пары ИМЯ=значение через пробел — добавляются к окружению игры. Пустое поле — всё по умолчанию. Полный список с описаниями — кнопка ниже."));
    { SDL_Rect r = lrow(D(38)); if (ui_input(ui_id("env", 0), r, P.env, sizeof P.env, "ME1_SHOW_FPS=1 ME1_HB=5")) prefs_save(); L.y += D(10); }
    flow_begin();
    if (flow_btn(T("Список переменных…"), UB_NORMAL)) env_list();
    if (flow_btn(T("Очистить"), *P.env ? UB_NORMAL : UB_NORMAL | UB_DISABLED)) { P.env[0] = 0; prefs_save(); }
    flow_end();
    card_end();
    card_begin("phonemod", "PhoneModify", UIC(C_ACC));
    { int i; if (me1cfg_find("port.phone_mod", &i)) setting_row(i); }
    flow_begin();
    if (flow_btn(T("Генератор PhoneModify…"), UB_NORMAL)) modgen_show();
    if (GS.found && flow_btn(T("Открыть папку PhoneModify"), UB_NORMAL)) { char d[PATH_MAX + 16]; if (!ci_child(g_game, "PhoneModify", 1, d, sizeof d)) snprintf(d, sizeof d, "%s/PhoneModify", g_game); open_path(d); }
    flow_end();
    card_end();
    card_begin("diag", T("Проверка и журналы"), UIC(C_ACC));
    { char lp[PATH_MAX]; log_path(lp, sizeof lp); snprintf(b, sizeof b, "%s%s", T("Лог игры: "), pretty(lp)); para(13, UIC(C_DIM), b); }
    flow_begin();
    if (flow_btn(T("Что увидит игра"), UB_NORMAL)) check_report();
    if (flow_btn(T("Лог последнего запуска"), UB_NORMAL)) log_show();
    if (flow_btn(T("Открыть папку документов"), UB_NORMAL)) open_path(g_docs);
    if (flow_btn(T("Очистить кэш шейдеров…"), UB_NORMAL)) cache_ask();
    flow_end();
    card_end();
    card_begin("reset", T("Сброс настроек"), UIC(C_WARN));
    para(13, UIC(C_DIM), T("Вернуть значения из Default*.ini игры. Записываются только после «Сохранить»."));
    flow_begin();
    if (flow_btn(T("По умолчанию"), UB_DANGER)) { me1cfg_reset(&C); g_dirty = 1; ui_toast(T("Значения из Default*.ini — нажмите «Сохранить»")); }
    flow_end();
    card_end();
    card_begin("uiscale", T("Интерфейс"), UIC(C_ACC));
    { SDL_Rect r = lrow(D(36)); char z[32]; snprintf(z, sizeof z, "%d%%", (int)(P.zoom * 100 + 0.5f));
      ui_text(UF_REG, D(15), r.x, r.y + D(8), UIC(C_TEXT), T("Масштаб"), -1);
      int x = r.x + D(160);
      SDL_Rect m1 = { x, r.y, D(40), D(34) }, vv = { x + D(46), r.y, D(70), D(34) }, p1 = { x + D(122), r.y, D(40), D(34) }, rs = { x + D(170), r.y, D(110), D(34) };
      if (ui_button(ui_id("zoom-", 0), m1, "−", UB_NORMAL)) { P.zoom = P.zoom > 0.7f ? P.zoom - 0.1f : 0.6f; prefs_save(); }
      ui_text_c(UF_BOLD, D(15), vv, UIC(C_ACC2), z);
      if (ui_button(ui_id("zoom+", 0), p1, "+", UB_NORMAL)) { P.zoom = P.zoom < 2.9f ? P.zoom + 0.1f : 3.0f; prefs_save(); }
      if (ui_button(ui_id("zoom0", 0), rs, "100%", UB_FLAT)) { P.zoom = 1; prefs_save(); } }
    para(12, UIC(C_DIM), T("Также Ctrl + колесо мыши, Ctrl+= / Ctrl+− / Ctrl+0."));
    card_end();
    card_begin("about", T("О программе"), UIC(C_ACC));
    snprintf(b, sizeof b, "me1-launcher %s — %s", VERSION, T("лаунчер порта Mass Effect (ПК, 2007) для Linux. Игру запускает me1-linux: статически рекомпилированный MassEffect.exe, Direct3D 9 → Vulkan / OpenGL, звук через SDL."));
    para(13, UIC(C_TEXT), b);
    char lp[PATH_MAX + 32]; prefs_path(lp, sizeof lp);
    snprintf(b, sizeof b, "%s%s\nme1-linux: %s", T("Настройки лаунчера: "), pretty(lp), *g_me1linux ? pretty(g_me1linux) : "✗");
    para(12, UIC(C_DIM), b);
    para(12, UIC(C_DIM), T("Шрифты: Exo 2 (SIL Open Font License 1.1), DejaVu (лицензия Bitstream Vera). stb_truetype и stb_image — public domain. Mass Effect © BioWare / Electronic Arts: нужна своя купленная копия игры."));
    card_end();
}

/* ------------------------------------------------------------------------------------------ sidebar --- */
static void lang_cb(Modal* m, int btn, int item) { (void)item; if (btn == 0) cfg_set("game.language", I18N_GAME[m->tag]); }
static void set_lang(int idx) {
    i18n_set(idx); P.lang = idx; prefs_save(); refresh_auto_report();
    SDL_SetWindowTitle(g_win, "Mass Effect");   /* v1.1.3: ASCII only (GNOME/XWayland showed the UTF-8 title as Latin-1) */
    int si; const SettingDef* d = me1cfg_find("game.language", &si);
    if (!d || !GS.found || !strcmp(C.value[si], I18N_GAME[idx])) return;
    for (const SetChoice* ch = d->choices; ch && ch->value; ch++) if (!strcmp(ch->value, I18N_GAME[idx])) {
        char b[512]; snprintf(b, sizeof b, T("Переключить и язык текста игры на «%s»? Нужны файлы локализации этого языка в папке игры."), T(ch->label));
        Modal* m = modal(MK_MSG, T("Язык игры"), b, lang_cb, T("Да"), T("Нет"), NULL); if (m) m->tag = idx;
        return;
    }
}
static void sidebar(int sw) {
    SDL_Rect sb = { 0, 0, sw, ui.h }; ui_fill(sb, UIC(C_SIDE));
    SDL_Rect ln = { sw - 1, 0, 1, ui.h }; ui_fill(ln, UICA(C_LINE, 160));
    int x = D(20), y = D(22), is = D(46);
    if (g_icon) { SDL_Rect ir = { x, y, is, is }; SDL_RenderCopy(SDL_GetRenderer(g_win), g_icon, NULL, &ir); }
    ui_text(UF_BOLD, D(18), x + is + D(12), y + D(2), UIC(0xffffff), "MASS EFFECT", -1);
    { char vb[96]; snprintf(vb, sizeof vb, "%s · v%s", T("порт для Linux"), VERSION); ui_text_fit(UF_REG, D(12), x + is + D(12), y + D(26), sw - x - is - D(20), UIC(C_DIM), vb); }
    y += is + D(30);
    for (int i = 0; i < PG_N; i++) {
        SDL_Rect r = { 0, y, sw - 1, D(46) };
        if (ui_button(ui_id(PAGES[i], 20), r, T(PAGES[i]), i == g_page ? UB_TABSEL : UB_TAB)) { g_page = i; ui.focus = 0; }
        y += D(46);
    }
    /* bottom: language, save, play */
    int by = ui.h - D(18) - D(56);
    SDL_Rect play = { D(16), by, sw - D(32), D(56) };
    int can = GS.found && *g_me1linux;
    if (ui_button(ui_id("play", 0), play, T("▶  Запустить игру"), can ? UB_PRIMARY : UB_PRIMARY | UB_DISABLED)) launch();
    by -= D(12) + D(36);
    SDL_Rect sv = { D(16), by, sw - D(32), D(36) };
    if (ui_button(ui_id("save", 0), sv, g_dirty ? T("Сохранить изменения") : T("Сохранено ✓"), g_dirty ? UB_NORMAL : UB_FLAT | UB_DISABLED)) do_save(0);
    if (g_dirty) { by -= D(22); ui_text_fit(UF_REG, D(12), D(18), by, sw - D(36), UIC(C_WARN), T("● есть несохранённые изменения")); }
    by -= D(12) + D(36);
    SDL_Rect lr = { D(16), by, sw - D(32), D(36) };
    int sel = i18n_cur();
    if (ui_combo(ui_id("lang", 0), lr, I18N_NAMES, I18N_N, &sel) && sel != i18n_cur()) set_lang(sel);
    ui_text(UF_REG, D(12), D(18), by - D(20), UIC(C_DIM), "Язык · Language", -1);
}

/* ---------------------------------------------------------------------------------------------- frame --- */
static void zoom_by(float d) { float z = d == 0 ? 1.0f : P.zoom + d; if (z < 0.6f) z = 0.6f; if (z > 3.0f) z = 3.0f; if (z != P.zoom) { P.zoom = z; prefs_save(); ui.redraw = 1; } }
static void draw(void) {
    ui.scale = ui.ptscale * g_k * P.zoom;
    ui.top = ui_popup_open() ? UI_LAYER_POPUP : g_nm ? UI_LAYER_MODAL : UI_LAYER_MAIN;
    SDL_SetRenderDrawColor(g_r, 8, 12, 18, 255); SDL_RenderClear(g_r);
    /* global keys */
    if (ui.wheel && (SDL_GetModState() & KMOD_CTRL)) { zoom_by(ui.wheel > 0 ? 0.1f : -0.1f); ui.wheel = 0; }
    if (ui_key(SDLK_EQUALS | UI_CTRL) || ui_key(SDLK_PLUS | UI_CTRL) || ui_key(SDLK_KP_PLUS | UI_CTRL)) zoom_by(0.1f);
    if (ui_key(SDLK_MINUS | UI_CTRL) || ui_key(SDLK_KP_MINUS | UI_CTRL)) zoom_by(-0.1f);
    if (ui_key(SDLK_0 | UI_CTRL)) zoom_by(0);
    if (!g_nm && ui_key(SDLK_s | UI_CTRL) && g_dirty) do_save(0);
    if (!g_nm && (ui_key(SDLK_RETURN | UI_CTRL) || ui_key(SDLK_F5)) && GS.found && *g_me1linux) launch();
    int sw = D(262); if (sw > ui.w / 3) sw = ui.w / 3;
    ui.layer = UI_LAYER_MAIN;
    g_area = (SDL_Rect){ sw, 0, ui.w - sw, ui.h };
    UiScroll* sc = &g_sc[g_page]; sc->id = ui_id("page", g_page);
    ui_scroll_begin(sc, g_area);
    int cw = g_area.w - D(64); if (cw > D(980)) cw = D(980);
    L.x = g_area.x + (g_area.w - cw) / 2; L.w = cw; L.y = g_area.y + D(24) - sc->pos; int y0 = L.y;
    if (g_page != PG_HOME) {
        ui_text(UF_BOLD, D(26), L.x, L.y, UIC(0xffffff), T(PAGES[g_page]), -1); L.y += ui_lineh(UF_BOLD, D(26)) + D(18);
    }
    switch (g_page) { case PG_HOME: page_home(); break; case PG_GFX: page_gfx(); break; case PG_GAME: page_game(); break; case PG_CTRL: page_ctrl(); break; default: page_more(); }
    ui_scroll_end(sc, L.y - y0 + D(40));
    sidebar(sw);
    for (int i = 0; i < g_nm; i++) modal_draw(i);
    ui_overlay();
}
static char g_shot[PATH_MAX];   /* ME1L_SCRIPT "shot": the next frame goes to this BMP */
static void frame(void) {
    for (int pass = 0; pass < 3; pass++) {
        ui.redraw = 0;
        ui_begin(g_win); draw(); ui_end();
        if (!ui.redraw) break;
    }
    if (*g_shot) {
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, ui.w, ui.h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (s && SDL_RenderReadPixels(g_r, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) SDL_SaveBMP(s, g_shot);
        if (s) SDL_FreeSurface(s);
        g_shot[0] = 0;
    }
    SDL_RenderPresent(g_r);
}
/* ME1L_SCRIPT="page 1; wait 5; shot /tmp/a.bmp; click 400 300; modal check; quit" - screenshots and clicks for tests
 * (coordinates in window points) */
static char* g_script; static int g_wait;
static void script_step(void) {
    if (!g_script) return;
    if (g_wait > 0) { g_wait--; return; }
    while (*g_script == ' ' || *g_script == ';') g_script++;
    if (!*g_script) { g_script = NULL; return; }
    char cmd[PATH_MAX + 64]; size_t n = strcspn(g_script, ";"); snprintf(cmd, sizeof cmd, "%.*s", (int)n, g_script); g_script += n;
    char a[PATH_MAX]; int x, y;
    if (sscanf(cmd, "page %d", &x) == 1) { g_page = x >= 0 && x < PG_N ? x : 0; }
    else if (sscanf(cmd, "wait %d", &x) == 1) g_wait = x;
    else if (sscanf(cmd, "shot %4095s", a) == 1) { snprintf(g_shot, sizeof g_shot, "%s", a); g_wait = 1; }
    else if (sscanf(cmd, "click %d %d", &x, &y) == 2) {
        SDL_Event e; memset(&e, 0, sizeof e);
        e.type = SDL_MOUSEMOTION; e.motion.x = x; e.motion.y = y; SDL_PushEvent(&e);
        memset(&e, 0, sizeof e); e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_LEFT; e.button.x = x; e.button.y = y; e.button.state = SDL_PRESSED; SDL_PushEvent(&e);
        memset(&e, 0, sizeof e); e.type = SDL_MOUSEBUTTONUP; e.button.button = SDL_BUTTON_LEFT; e.button.x = x; e.button.y = y; SDL_PushEvent(&e);
        g_wait = 2;
    }
    else if (sscanf(cmd, "move %d %d", &x, &y) == 2) { SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_MOUSEMOTION; e.motion.x = x; e.motion.y = y; SDL_PushEvent(&e); g_wait = 1; }
    else if (sscanf(cmd, "scroll %d", &x) == 1) { SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_MOUSEWHEEL; e.wheel.y = x; SDL_PushEvent(&e); g_wait = 1; }
    else if (sscanf(cmd, "modal %63s", a) == 1) {
        if (!strcmp(a, "check")) check_report(); else if (!strcmp(a, "log")) log_show(); else if (!strcmp(a, "env")) env_list();
        else if (!strcmp(a, "modgen")) modgen_show(); else if (!strcmp(a, "cache")) cache_ask(); else if (!strcmp(a, "path")) path_modal();
        else if (!strcmp(a, "auto")) auto_now(); else if (!strcmp(a, "close") && g_nm) modal_close(-1, -1);
        g_wait = 2;
    }
    else if (sscanf(cmd, "lang %63s", a) == 1) { int k = i18n_find(a); if (k >= 0) { i18n_set(k); refresh_auto_report(); } }
    else if (!strcmp(cmd, "launch")) launch();
    else if (!strcmp(cmd, "quit")) g_quit = 1;
    else fprintf(stderr, "me1-launcher: ME1L_SCRIPT: %s?\n", cmd);
}

/* ----------------------------------------------------------------------------------------------- main --- */
static void usage(void) {
    printf("me1-launcher %s - %s\n\n  me1-launcher [--game DIR] [--docs DIR] [--lang CODE] [--play]\n\n"
           "  --game DIR   %s\n  --docs DIR   %s\n  --lang CODE  ru en de fr it es pl cs hu\n  --play       %s\n",
           VERSION, "Mass Effect (PC) for Linux: settings and start", "game folder (Binaries, BioGame)",
           "documents: saves, BIO*.ini, logs (default $XDG_DATA_HOME/me1port/Documents)", "start the game at once, quit when it exits");
}
static int which(const char* prog, char* out, size_t cap) {
    const char* path = getenv("PATH"); if (!path) return 0;
    const char* s = path;
    while (*s) { const char* e = strchr(s, ':'); size_t n = e ? (size_t)(e - s) : strlen(s);
        snprintf(out, cap, "%.*s/%s", (int)n, s, prog); if (access(out, X_OK) == 0) return 1; s += n; if (*s) s++; }
    return 0;
}
int main(int argc, char** argv) {
    const char* a_game = NULL, *a_docs = NULL, *a_lang = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) a_game = argv[++i];
        else if (!strcmp(argv[i], "--docs") && i + 1 < argc) a_docs = argv[++i];
        else if (!strcmp(argv[i], "--lang") && i + 1 < argc) a_lang = argv[++i];
        else if (!strcmp(argv[i], "--play")) g_play = 1;
        else if (!strcmp(argv[i], "--version")) { printf("me1-launcher %s\n", VERSION); return 0; }
        else { usage(); return strcmp(argv[i], "-h") && strcmp(argv[i], "--help") ? 2 : 0; }
    }
    { ssize_t n = readlink("/proc/self/exe", g_exedir, sizeof g_exedir - 1); if (n > 0) { g_exedir[n] = 0; char* s = strrchr(g_exedir, '/'); if (s) *s = 0; } else strcpy(g_exedir, "."); }
    { const char* xc = getenv("XDG_CONFIG_HOME"); if (xc && *xc) snprintf(g_cfgdir, sizeof g_cfgdir, "%s/me1port", xc); else snprintf(g_cfgdir, sizeof g_cfgdir, "%s/.config/me1port", home()); }
    prefs_load();
    { int li = a_lang ? i18n_find(a_lang) : -1; if (li < 0) li = P.lang; if (li < 0) li = i18n_system(); i18n_set(li); }
    if (a_docs) snprintf(g_docs, sizeof g_docs, "%s", a_docs);
    else { const char* xd = getenv("XDG_DATA_HOME"); if (xd && *xd) snprintf(g_docs, sizeof g_docs, "%s/me1port/Documents", xd); else snprintf(g_docs, sizeof g_docs, "%s/.local/share/me1port/Documents", home()); }
    mkdirs(g_docs);
    if (!near_exe("me1-linux", g_me1linux, sizeof g_me1linux) && !which("me1-linux", g_me1linux, sizeof g_me1linux)) g_me1linux[0] = 0;
    /* the game folder: --game, the remembered one, a known place; else a background search */
    char gd[PATH_MAX] = "";
    if (a_game) expand_home(a_game, gd, sizeof gd);
    else { char p[PATH_MAX + 16]; snprintf(p, sizeof p, "%s/game_dir", g_cfgdir); FILE* f = fopen(p, "r"); if (f) { if (fgets(gd, sizeof gd, f)) gd[strcspn(gd, "\r\n")] = 0; fclose(f); } }
    int auto_search = 0;
    if (!*gd || !is_dir(gd)) { char (*kp)[PATH_MAX] = malloc(4 * PATH_MAX); if (known_places(kp, 4) > 0) snprintf(gd, sizeof gd, "%s", kp[0]); else auto_search = 1; free(kp); }
    if (*gd) { char g2[PATH_MAX]; if (find_game_in(gd, g2, sizeof g2)) snprintf(gd, sizeof gd, "%s", g2); game_dir_set(gd); }
    else { game_status(); config_reload(); }
    mods_scan();

    SDL_SetHint(SDL_HINT_APP_NAME, "Mass Effect");
    /* WM_CLASS / xdg app_id = the .desktop file name: the dock shows one "Mass Effect" icon for the launcher and the game */
    setenv("SDL_VIDEO_X11_WMCLASS", "me1port", 0); setenv("SDL_VIDEO_WAYLAND_WMCLASS", "me1port", 0);
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "me1-launcher: SDL_Init: %s\n", SDL_GetError()); return 1; }
    detect_machine(); refresh_auto_report();
    g_k = g_dh >= 2000 ? 1.5f : g_dh >= 1350 ? 1.25f : g_dh > 0 && g_dh < 800 ? 0.85f : 1.0f;
    int ww = (int)(1200 * g_k * P.zoom), wh = (int)(800 * g_k * P.zoom);
    if (g_dw > 0 && ww > g_dw * 92 / 100) ww = g_dw * 92 / 100;
    if (g_dh > 0 && wh > g_dh * 88 / 100) wh = g_dh * 88 / 100;
    int play_now = g_play && GS.found && *g_me1linux;
    g_win = SDL_CreateWindow("Mass Effect", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (play_now ? SDL_WINDOW_HIDDEN : 0));
    if (!g_win) { fprintf(stderr, "me1-launcher: SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(g_win, 760, 520);
    g_r = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_r) g_r = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_r || !ui_init(g_r)) { fprintf(stderr, "me1-launcher: renderer: %s\n", SDL_GetError()); return 1; }
    { SDL_Surface* ic = ui_image_surface(me1a_icon, (int)me1a_icon_size); if (ic) { SDL_SetWindowIcon(g_win, ic); SDL_FreeSurface(ic); } }
    g_icon = ui_image(me1a_icon, (int)me1a_icon_size, &g_iw, &g_ih);
    g_banner = ui_image(me1a_loading, (int)me1a_loading_size, &g_bw, &g_bh);
    first_run_auto();
    if (getenv("ME1L_SCRIPT")) g_script = strdup(getenv("ME1L_SCRIPT"));
    if (auto_search && !*g_game) search_start();
    if (play_now) launch();
    else if (g_play) SDL_ShowWindow(g_win);

    while (!g_quit) {
        SDL_Event e;
        int anim = ui.redraw || ui.active || S.running || PK.running || g_script;
        int have = SDL_WaitEventTimeout(&e, g_child > 0 ? 250 : anim ? 16 : 500);
        while (have) {
            if (e.type == SDL_QUIT) g_quit = 1;
            else if (e.type == SDL_USEREVENT && e.user.code == 2 && PK.done) { PK.done = 0; if (*PK.out) { choose_game(PK.out); first_run_auto(); } }
            else if (e.type == SDL_USEREVENT && S.done) {
                if (!*g_game && S.n == 1) { game_dir_set(S.found[0]); mods_scan(); first_run_auto(); char t[PATH_MAX + 32]; snprintf(t, sizeof t, "%s%s", T("Игра: "), pretty(g_game)); ui_toast(t); }
            }
            ui_event(&e);
            have = SDL_PollEvent(&e);
        }
        if (g_child > 0) {
            int st; pid_t r = waitpid(g_child, &st, WNOHANG);
            if (r == g_child) child_done(st);
            if (g_child > 0 || g_quit) continue;
        }
        frame();
        script_step();
    }
    S.cancel = 1;
    ui_shutdown();
    if (g_banner) SDL_DestroyTexture(g_banner);
    if (g_icon) SDL_DestroyTexture(g_icon);
    SDL_DestroyRenderer(g_r); SDL_DestroyWindow(g_win); SDL_Quit();
    return 0;
}
