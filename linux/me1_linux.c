/* me1-linux: desktop Linux front end of the Mass Effect port (v1.1).
 * The same recompiled game as the Android build (MassEffect.exe + msvcr80/msvcp80 + PhysX + D3DX + Vorbis + Bink as
 * portable C, runtime/ HLE of Win32/D3D9/OpenAL/DirectInput/XInput), linked into one native executable for x86-64 or
 * aarch64 Linux. This file is what the Android GameActivity + game_jni.c are there: it owns the window (SDL2: X11 or
 * Wayland), hands its native handles to the renderers (me1_platform.h: EGL/GLES or Vulkan draw straight into it), feeds
 * keyboard / mouse / gamepad into the Win32 + DirectInput + XInput HLE, opens the audio device for the OpenAL mixer and
 * runs me1_exe_main (runtime/main_exe.c, -DME1_LIB) on a game thread.
 *
 *   me1-linux --game DIR [options] [-- game arguments]      (me1-linux --help)
 *
 * Window system: SDL2 picks X11 or Wayland (--video x11|wayland or SDL_VIDEODRIVER). The runtime never links SDL, X11
 * or Wayland itself - libX11 / libwayland-egl are dlopen'ed. Build: make linux (see linux/README.md). */
#define _GNU_SOURCE
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <dlfcn.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include "hle.h"
#include "me1_platform.h"
#include "me1settings.h"
#include "ui.h"
#include "i18n.h"
#include "gpuprobe.h"

int  me1_exe_main(int argc, char** argv);
void me1_exe_on_signal(int sig);
void me1gl_set_window(void* win);
int  me1gl_get_view(int* x, int* y, int* w, int* h, int* fw, int* fh);
void me1_status(int64_t o[10]);
void me1_perf(int64_t o[4]);
int  gm_hud(char* out, size_t cap);   /* v1.2.1 GEARMASS indicator (runtime/gearmass.c) */
void hle_hb_now(const char* why);

#define ME1_LINUX_VERSION "1.2.1"
static const char* const RENDERER_NAMES[4] = { "—", "OpenGL ES", "OpenGL ES (zink)", "Vulkan" };

/* ------------------------------------------------------------------------------------------------ options --- */
static struct {
    const char *game, *docs, *images, *renderer, *log, *mods, *video, *lang;
    int fullscreen, win_w, win_h, scr_w, scr_h, grab, verbose, fps_limit, show_fps, no_loading;
    int gargc; char** gargv;   /* after "--" */
} O = { .grab = 1, .fps_limit = -1, .fullscreen = -1 };

static void usage(FILE* f) {
    fprintf(f,
        "me1-linux " ME1_LINUX_VERSION " — Mass Effect (2007) для Linux: статическая рекомпиляция x86 -> нативный код\n\n"
        "Использование: me1-linux --game ПАПКА_ИГРЫ [параметры] [-- аргументы игры]\n\n"
        "  --game DIR         папка установленной игры (Binaries/, BioGame/); по умолчанию $ME1_GAME,\n"
        "                     ~/.config/me1port/game_dir (пишет install.sh --game) или текущая\n"
        "  --docs DIR         «Мои документы» игры (сохранения, BIO*.ini); по умолчанию\n"
        "                     $XDG_DATA_HOME/me1port/Documents (~/.local/share/me1port/Documents)\n"
        "  --images DIR       папка с image.bin модулей (по умолчанию: оригинальные файлы из Binaries/)\n"
        "  --renderer R       vulkan | gl | zink  (по умолчанию vulkan, если есть GPU с Vulkan и libme1spv.so,\n"
        "                     иначе gl; при сбое Vulkan — автоматически gl)\n"
        "  --fullscreen, --windowed  во весь экран / в окне (по умолчанию — как «Полноэкранный режим» в\n"
        "                     настройках игры; переключение в игре: Alt+Enter или F11)\n"
        "  --window WxH       размер окна (по умолчанию — разрешение игры, если помещается на экран)\n"
        "  --no-loading-screen  без экрана загрузки (картинка и ход компиляции шейдеров до первого кадра)\n"
        "  --lang CODE        язык экрана загрузки: ru en de fr it es pl cs hu (по умолчанию — как в лаунчере)\n"
        "  --screen WxH       «рабочий стол», который видит игра (по умолчанию — режим монитора)\n"
        "  --fps-limit N      ограничение частоты кадров (0 — без ограничения)\n"
        "  --show-fps         FPS и рендерер в заголовке окна (или «Показывать FPS» в me1cfg: port.show_fps=1)\n"
        "  --gearmass РЕЖИМ   нагрузка: auto (по умолчанию), eco, balance, perf, max, custom, off (= ME1_GEARMASS, me1cfg: port.gearmass)\n"
        "  --mods A:B         моды (папки), как ME1_MODS\n"
        "  --no-grab          не захватывать мышь (курсор системы; обзор мышью хуже)\n"
        "  --video x11|wayland  оконная система (по умолчанию — выбор SDL)\n"
        "  --log FILE|-       журнал (по умолчанию <docs>/me1exe_run.log; «-» — в терминал)\n"
        "  -v, --verbose      журнал и в файл, и в терминал\n"
        "  --env K=V          переменная окружения рантайма (можно несколько раз), см. README\n"
        "  --version, --help\n\n"
        "Клавиши: Alt+Enter / F11 — полный экран, Ctrl+Alt+M — захват мыши вкл/выкл.\n"
        "Настройки графики/звука/управления: лаунчер me1-launcher или me1cfg --game DIR --docs DIR show|set|preset\n"
        "(замена MassEffectConfig.exe). Точная эмуляция float D3D9 на ПК включена: ME1_FLOAT_EMU=2 (1 — быстрее).\n");
}
static int parse_wh(const char* s, int* w, int* h) { return s && sscanf(s, "%dx%d", w, h) == 2 && *w >= 320 && *h >= 200; }
static int is_dir(const char* p) { struct stat st; return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char* p) { struct stat st; return p && stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static void mkdirs(const char* path) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s", path);
    for (char* s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
/* dir/name exists, any case (the runtime matches path components case-insensitively); out: the real path */
static int ci_find(const char* dir, const char* name, int want_dir, char* out, size_t cap) {
    DIR* d = opendir(dir); struct dirent* de; int ok = 0; char p[PATH_MAX];
    if (!d) return 0;
    while (!ok && (de = readdir(d))) if (!strcasecmp(de->d_name, name)) {
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name); ok = want_dir ? is_dir(p) : is_file(p);
        if (ok && out) snprintf(out, cap, "%s", p);
    }
    closedir(d); return ok;
}
static char g_exedir[PATH_MAX];
static void exe_dir(void) {
    ssize_t n = readlink("/proc/self/exe", g_exedir, sizeof g_exedir - 1);
    if (n <= 0) { strcpy(g_exedir, "."); return; }
    g_exedir[n] = 0; char* s = strrchr(g_exedir, '/'); if (s) *s = 0;
}
/* first of <exe dir>/name, <exe dir>/../lib/me1port/name, <exe dir>/../share/me1port/name */
static int near_exe(const char* name, char* out, size_t cap, int dir) {
    static const char* const sub[] = { "", "/../lib/me1port", "/../share/me1port", "/../lib64/me1port" };
    for (unsigned i = 0; i < sizeof sub / sizeof *sub; i++) {
        snprintf(out, cap, "%s%s/%s", g_exedir, sub[i], name);
        if (dir ? is_dir(out) : is_file(out)) return 1;
    }
    return 0;
}
static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "me1-linux: %s: нужно значение\n", a), exit(2), (char*)0))
        if (!strcmp(a, "--")) { O.gargc = argc - i - 1; O.gargv = argv + i + 1; break; }
        else if (!strcmp(a, "--game")) O.game = NEXT();
        else if (!strcmp(a, "--docs")) O.docs = NEXT();
        else if (!strcmp(a, "--images")) O.images = NEXT();
        else if (!strcmp(a, "--renderer")) O.renderer = NEXT();
        else if (!strcmp(a, "--fullscreen")) O.fullscreen = 1;
        else if (!strcmp(a, "--windowed")) O.fullscreen = 0;
        else if (!strcmp(a, "--no-loading-screen")) O.no_loading = 1;
        else if (!strcmp(a, "--lang")) O.lang = NEXT();
        else if (!strcmp(a, "--window")) { if (!parse_wh(NEXT(), &O.win_w, &O.win_h)) { fprintf(stderr, "me1-linux: --window WxH\n"); exit(2); } }
        else if (!strcmp(a, "--screen")) { if (!parse_wh(NEXT(), &O.scr_w, &O.scr_h)) { fprintf(stderr, "me1-linux: --screen WxH\n"); exit(2); } }
        else if (!strcmp(a, "--fps-limit")) O.fps_limit = atoi(NEXT());
        else if (!strcmp(a, "--mods")) O.mods = NEXT();
        else if (!strcmp(a, "--show-fps")) O.show_fps = 1;
        else if (!strcmp(a, "--gearmass")) setenv("ME1_GEARMASS", NEXT(), 1);   /* v1.2.1 */
        else if (!strcmp(a, "--no-grab")) O.grab = 0;
        else if (!strcmp(a, "--video")) O.video = NEXT();
        else if (!strcmp(a, "--x11")) O.video = "x11";
        else if (!strcmp(a, "--wayland")) O.video = "wayland";
        else if (!strcmp(a, "--log")) O.log = NEXT();
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) O.verbose = 1;
        else if (!strcmp(a, "--env")) {
            char* kv = strdup(NEXT()); char* eq = strchr(kv, '=');
            if (!eq) { fprintf(stderr, "me1-linux: --env K=V\n"); exit(2); }
            *eq = 0; setenv(kv, eq + 1, 1);
        }
        else if (!strcmp(a, "--version")) { printf("me1-linux " ME1_LINUX_VERSION "\n"); exit(0); }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); exit(0); }
        else { fprintf(stderr, "me1-linux: неизвестный параметр %s\n\n", a); usage(stderr); exit(2); }
#undef NEXT
    }
}

/* ------------------------------------------------------------------------------------------------ logging --- */
static int g_tty_fd = -1;   /* original stderr for --verbose */
static int g_log_fd = -1;
static void* tee_main(void* p) {
    int in = (int)(intptr_t)p; char b[4096]; ssize_t n;
    while ((n = read(in, b, sizeof b)) > 0) {
        if (g_log_fd >= 0 && write(g_log_fd, b, (size_t)n) < 0) {}
        if (g_tty_fd >= 0 && write(g_tty_fd, b, (size_t)n) < 0) {}
    }
    return NULL;
}
static void open_log(const char* docs) {
    if (O.log && !strcmp(O.log, "-")) return;
    char def[PATH_MAX]; snprintf(def, sizeof def, "%s/me1exe_run.log", docs);
    const char* path = O.log ? O.log : def;
    g_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (g_log_fd < 0) { fprintf(stderr, "me1-linux: не открыть журнал %s: %s\n", path, strerror(errno)); return; }
    fprintf(stderr, "me1-linux: журнал — %s\n", path);
    fflush(stdout); fflush(stderr);
    if (O.verbose) {   /* tee: pipe -> file + terminal */
        int pf[2];
        if (pipe(pf) == 0) {
            g_tty_fd = dup(2);
            dup2(pf[1], 1); dup2(pf[1], 2); close(pf[1]);
            pthread_t th; pthread_create(&th, NULL, tee_main, (void*)(intptr_t)pf[0]); pthread_detach(th);
            return;
        }
    }
    dup2(g_log_fd, 1); dup2(g_log_fd, 2);
}

/* ------------------------------------------------------------------------------------------ crash report --- */
static struct sigaction g_old[65];
static void wr(const char* s) { if (write(2, s, strlen(s)) < 0) {} }
static void wrhex(uint64_t v) { char b[19] = "0x"; for (int i = 0; i < 16; i++) b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15]; b[18] = 0; wr(b); }
static void wrsym(uint64_t a) {
    Dl_info di; memset(&di, 0, sizeof di);
    if (a && dladdr((void*)(uintptr_t)a, &di) && di.dli_fname) {
        wr(" ("); wr(di.dli_fname); wr(" +"); wrhex(a - (uint64_t)(uintptr_t)di.dli_fbase);
        if (di.dli_sname) { wr(" "); wr(di.dli_sname); }
        wr(")");
    }
}
static void on_crash(int sig, siginfo_t* si, void* uc) {
    static volatile int once;
    if (!once) {
        once = 1;
        uint64_t pc = 0, lr = 0, sp = 0; ucontext_t* u = uc;
#if defined(__aarch64__)
        pc = u->uc_mcontext.pc; lr = u->uc_mcontext.regs[30]; sp = u->uc_mcontext.sp;
#elif defined(__x86_64__)
        pc = (uint64_t)u->uc_mcontext.gregs[REG_RIP]; sp = (uint64_t)u->uc_mcontext.gregs[REG_RSP];
        lr = sp ? *(uint64_t*)(uintptr_t)sp : 0;   /* return address if the fault is at a function entry */
#endif
        char n[8]; snprintf(n, sizeof n, "%d", sig);
        wr("\n[exe] NATIVE CRASH: signal "); wr(n); wr(" addr "); wrhex((uint64_t)(uintptr_t)si->si_addr);
        wr(" pc "); wrhex(pc); wrsym(pc); wr(" lr "); wrhex(lr); wrsym(lr); wr(" sp "); wrhex(sp); wr("\n");
        hle_hb_now("(native crash)");
        fflush(stdout); fflush(stderr);
    }
    sigaction(sig, &g_old[sig], NULL);
    raise(sig);
}
static void on_term_sig(int sig) { me1_exe_on_signal(sig); }   /* Ctrl-C in the terminal: report + exit */
static void signal_handlers(void) {
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    stack_t ss; ss.ss_sp = malloc(1 << 16); ss.ss_size = 1 << 16; ss.ss_flags = 0; sigaltstack(&ss, NULL);
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) {
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(sigs[i], &sa, &g_old[sigs[i]]);
    }
    signal(SIGINT, on_term_sig); signal(SIGTERM, on_term_sig); signal(SIGPIPE, SIG_IGN);
}

/* ------------------------------------------------------------------------------------------------- window --- */
static SDL_Window* g_win;
static Me1Window g_mw;
static int g_fullscreen, g_grabbed, g_focus = 1;
static float g_ptscale = 1.0f;   /* drawable pixels per window point (HiDPI) */

static void win_size_update(void) {
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(g_win, &w, &h);
#if SDL_VERSION_ATLEAST(2, 26, 0)
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
#endif
    if (pw <= 0 || ph <= 0) { pw = w; ph = h; }
    g_ptscale = w > 0 ? (float)pw / (float)w : 1.0f;
    if (pw != g_mw.w || ph != g_mw.h) {
        if (g_mw.w) fprintf(stderr, "[linux] window %dx%d -> %dx%d pixels\n", g_mw.w, g_mw.h, pw, ph);
        g_mw.w = pw; g_mw.h = ph; __atomic_add_fetch(&g_mw.gen, 1, __ATOMIC_RELEASE);
    }
}
/* X11: the visual of SDL's window, so that EGL picks a matching config (libX11 through dlopen: no link dependency) */
static unsigned long x11_visual(void* dpy, unsigned long xw) {
#if defined(SDL_VIDEO_DRIVER_X11)
    void* x = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (!x) return 0;
    int (*gwa)(Display*, Window, XWindowAttributes*) = (int (*)(Display*, Window, XWindowAttributes*))dlsym(x, "XGetWindowAttributes");
    VisualID (*vid)(Visual*) = (VisualID (*)(Visual*))dlsym(x, "XVisualIDFromVisual");
    XWindowAttributes wa; memset(&wa, 0, sizeof wa);
    if (gwa && vid && gwa((Display*)dpy, (Window)xw, &wa) && wa.visual) return (unsigned long)vid(wa.visual);
#else
    (void)dpy; (void)xw;
#endif
    return 0;
}
static void x11_init_threads(void) {   /* the presenter thread draws into SDL's X11 connection */
    void* x = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    int (*it)(void) = x ? (int (*)(void))dlsym(x, "XInitThreads") : NULL;
    if (it) it();
}
static int g_game_w, g_game_h;   /* the game's render resolution (screen.resolution) */
extern const unsigned char me1a_icon[]; extern const unsigned int me1a_icon_size;
static void set_icon(SDL_Window* w) {
    SDL_Surface* ic = ui_image_surface(me1a_icon, (int)me1a_icon_size);
    if (ic) { SDL_SetWindowIcon(w, ic); SDL_FreeSurface(ic); }
}
/* native handles of g_win -> g_mw (kind, display, window/surface); visual: X11 visual id for EGL */
static int wm_fetch(unsigned long* visual) {
    SDL_SysWMinfo wi; SDL_VERSION(&wi.version);
    if (!SDL_GetWindowWMInfo(g_win, &wi)) { fprintf(stderr, "[linux] SDL_GetWindowWMInfo: %s\n", SDL_GetError()); return 0; }
    if (visual) *visual = 0;
    switch (wi.subsystem) {
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        g_mw.kind = ME1_WS_X11; g_mw.display = wi.info.x11.display; g_mw.xwindow = (unsigned long)wi.info.x11.window; g_mw.surface = NULL;
        if (visual) *visual = x11_visual(g_mw.display, g_mw.xwindow);
        return 1;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        g_mw.kind = ME1_WS_WAYLAND; g_mw.display = wi.info.wl.display; g_mw.surface = wi.info.wl.surface; g_mw.xwindow = 0;
        return 1;
#endif
    default:
        fprintf(stderr, "[linux] unsupported window system (SDL subsystem %d): need X11 or Wayland\n", (int)wi.subsystem);
        return 0;
    }
}
static int open_window(void) {
    int w = O.win_w ? O.win_w : 1280, h = O.win_h ? O.win_h : 720;
    SDL_DisplayMode dm;
    if (!O.win_w && SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0) {
        /* v1.1: the game's resolution 1:1 when it fits the desktop, else 85 % of the desktop with the game's aspect */
        int gw = g_game_w > 0 ? g_game_w : 1280, gh = g_game_h > 0 ? g_game_h : 720;
        if (gw <= dm.w - 80 && gh <= dm.h - 120) { w = gw; h = gh; }
        else { w = dm.w * 85 / 100; h = (int)((long long)w * gh / gw); if (h > dm.h * 85 / 100) { h = dm.h * 85 / 100; w = (int)((long long)h * gw / gh); } }
    }
    Uint32 fl = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (O.fullscreen == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    g_win = SDL_CreateWindow("Mass Effect", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, fl);
    if (!g_win) { fprintf(stderr, "[linux] SDL_CreateWindow: %s\n", SDL_GetError()); return 0; }
    g_fullscreen = O.fullscreen == 1;
    SDL_SetWindowMinimumSize(g_win, 320, 200);
    set_icon(g_win);
    memset(&g_mw, 0, sizeof g_mw);
    unsigned long visual = 0;
    if (!wm_fetch(&visual)) return 0;
    win_size_update();
    fprintf(stderr, "[linux] window: %s (SDL video driver %s), %dx%d pixels%s, visual 0x%lx\n",
            g_mw.kind == ME1_WS_X11 ? "X11" : "Wayland", SDL_GetCurrentVideoDriver(), g_mw.w, g_mw.h,
            g_fullscreen ? ", fullscreen" : "", visual);
    me1_platform_set(g_mw.kind, g_mw.display, visual);
    return 1;
}
static void set_fullscreen(int on) {
    if (SDL_SetWindowFullscreen(g_win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0) g_fullscreen = on;
    fprintf(stderr, "[linux] fullscreen %s\n", g_fullscreen ? "on" : "off");
    win_size_update();
}
/* v1.1.5: while captured, g_menu: the game shows its cursor (menus) - absolute pointer, system cursor visible and
 * confined to the window; otherwise relative mode (camera). See cursor_update. */
static int g_menu;
static void set_grab(int on) {
    on = on && g_focus;
    if (!on && g_menu) { SDL_SetWindowGrab(g_win, SDL_FALSE); g_menu = 0; g_grabbed = 0; return; }   /* relative mode is already off */
    if (on == g_grabbed) return;
    if (SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) == 0) g_grabbed = on;
}

/* -------------------------------------------------------------------------------------------------- input --- */
/* SDL scancode (USB HID usage = physical key position, like DirectInput) -> Windows virtual key + DirectInput code */
static void map_key(SDL_Scancode sc, int* vk, int* dik) {
    static const unsigned char dik_az[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    static const unsigned char dik_kp[10] = { 0x4F,0x50,0x51,0x4B,0x4C,0x4D,0x47,0x48,0x49,0x52 };   /* KP_1..KP_9, KP_0 */
    *vk = 0; *dik = 0;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { *vk = 'A' + (sc - SDL_SCANCODE_A); *dik = dik_az[sc - SDL_SCANCODE_A]; return; }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) { int k = sc - SDL_SCANCODE_1; *vk = k == 9 ? '0' : '1' + k; *dik = 0x02 + k; return; }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) { int k = sc - SDL_SCANCODE_F1; *vk = 0x70 + k; *dik = k < 10 ? 0x3B + k : 0x57 + (k - 10); return; }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_0) { int k = sc - SDL_SCANCODE_KP_1; *vk = k == 9 ? 0x60 : 0x61 + k; *dik = dik_kp[k]; return; }
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: *vk = 0x1B; *dik = 0x01; break;
    case SDL_SCANCODE_RETURN: *vk = 0x0D; *dik = 0x1C; break;
    case SDL_SCANCODE_KP_ENTER: *vk = 0x0D; *dik = 0x9C; break;
    case SDL_SCANCODE_SPACE: *vk = 0x20; *dik = 0x39; break;
    case SDL_SCANCODE_TAB: *vk = 0x09; *dik = 0x0F; break;
    case SDL_SCANCODE_BACKSPACE: *vk = 0x08; *dik = 0x0E; break;
    case SDL_SCANCODE_DELETE: *vk = 0x2E; *dik = 0xD3; break;
    case SDL_SCANCODE_INSERT: *vk = 0x2D; *dik = 0xD2; break;
    case SDL_SCANCODE_HOME: *vk = 0x24; *dik = 0xC7; break;
    case SDL_SCANCODE_END: *vk = 0x23; *dik = 0xCF; break;
    case SDL_SCANCODE_PAGEUP: *vk = 0x21; *dik = 0xC9; break;
    case SDL_SCANCODE_PAGEDOWN: *vk = 0x22; *dik = 0xD1; break;
    case SDL_SCANCODE_UP: *vk = 0x26; *dik = 0xC8; break;
    case SDL_SCANCODE_DOWN: *vk = 0x28; *dik = 0xD0; break;
    case SDL_SCANCODE_LEFT: *vk = 0x25; *dik = 0xCB; break;
    case SDL_SCANCODE_RIGHT: *vk = 0x27; *dik = 0xCD; break;
    case SDL_SCANCODE_LSHIFT: *vk = 0xA0; *dik = 0x2A; break;
    case SDL_SCANCODE_RSHIFT: *vk = 0xA1; *dik = 0x36; break;
    case SDL_SCANCODE_LCTRL: *vk = 0xA2; *dik = 0x1D; break;
    case SDL_SCANCODE_RCTRL: *vk = 0xA3; *dik = 0x9D; break;
    case SDL_SCANCODE_LALT: *vk = 0xA4; *dik = 0x38; break;
    case SDL_SCANCODE_RALT: *vk = 0xA5; *dik = 0xB8; break;
    case SDL_SCANCODE_LGUI: *vk = 0x5B; *dik = 0xDB; break;
    case SDL_SCANCODE_RGUI: *vk = 0x5C; *dik = 0xDC; break;
    case SDL_SCANCODE_APPLICATION: *vk = 0x5D; *dik = 0xDD; break;
    case SDL_SCANCODE_CAPSLOCK: *vk = 0x14; *dik = 0x3A; break;
    case SDL_SCANCODE_NUMLOCKCLEAR: *vk = 0x90; *dik = 0x45; break;
    case SDL_SCANCODE_SCROLLLOCK: *vk = 0x91; *dik = 0x46; break;
    case SDL_SCANCODE_PRINTSCREEN: *vk = 0x2C; *dik = 0xB7; break;
    case SDL_SCANCODE_PAUSE: *vk = 0x13; *dik = 0xC5; break;
    case SDL_SCANCODE_GRAVE: *vk = 0xC0; *dik = 0x29; break;      /* console */
    case SDL_SCANCODE_MINUS: *vk = 0xBD; *dik = 0x0C; break;
    case SDL_SCANCODE_EQUALS: *vk = 0xBB; *dik = 0x0D; break;
    case SDL_SCANCODE_LEFTBRACKET: *vk = 0xDB; *dik = 0x1A; break;
    case SDL_SCANCODE_RIGHTBRACKET: *vk = 0xDD; *dik = 0x1B; break;
    case SDL_SCANCODE_BACKSLASH: *vk = 0xDC; *dik = 0x2B; break;
    case SDL_SCANCODE_NONUSBACKSLASH: *vk = 0xE2; *dik = 0x56; break;
    case SDL_SCANCODE_SEMICOLON: *vk = 0xBA; *dik = 0x27; break;
    case SDL_SCANCODE_APOSTROPHE: *vk = 0xDE; *dik = 0x28; break;
    case SDL_SCANCODE_COMMA: *vk = 0xBC; *dik = 0x33; break;
    case SDL_SCANCODE_PERIOD: *vk = 0xBE; *dik = 0x34; break;
    case SDL_SCANCODE_SLASH: *vk = 0xBF; *dik = 0x35; break;
    case SDL_SCANCODE_KP_DIVIDE: *vk = 0x6F; *dik = 0xB5; break;
    case SDL_SCANCODE_KP_MULTIPLY: *vk = 0x6A; *dik = 0x37; break;
    case SDL_SCANCODE_KP_MINUS: *vk = 0x6D; *dik = 0x4A; break;
    case SDL_SCANCODE_KP_PLUS: *vk = 0x6B; *dik = 0x4E; break;
    case SDL_SCANCODE_KP_PERIOD: *vk = 0x6E; *dik = 0x53; break;
    default: break;
    }
}
static void key_event(const SDL_KeyboardEvent* k) {
    int down = k->type == SDL_KEYDOWN, vk, dik;
    SDL_Keymod mod = SDL_GetModState();
    if (down && !k->repeat) {   /* front-end hot keys (not passed to the game) */
        SDL_Scancode sc = k->keysym.scancode;
        if ((sc == SDL_SCANCODE_RETURN && (mod & KMOD_ALT)) || sc == SDL_SCANCODE_F11) { set_fullscreen(!g_fullscreen); return; }
        if (sc == SDL_SCANCODE_M && (mod & KMOD_CTRL) && (mod & KMOD_ALT)) { O.grab = !O.grab; set_grab(O.grab); return; }
    }
    map_key(k->keysym.scancode, &vk, &dik);
    if (!vk) return;
    /* characters come from SDL_TEXTINPUT (keyboard layout, IME); control characters are sent with the key */
    int ch = 0;
    if (down) switch (vk) { case 0x0D: ch = 13; break; case 0x08: ch = 8; break; case 0x09: ch = 9; break; case 0x1B: ch = 27; break; }
    if (vk >= 0xA0 && vk <= 0xA5) hle_ui_key(0x10 + (vk - 0xA0) / 2, 0, down, 0);   /* generic VK_SHIFT / CONTROL / MENU */
    hle_ui_key(vk, k->repeat ? 0 : dik, down, ch);   /* auto-repeat: WM_KEYDOWN again, DirectInput sees one press */
}
static void text_event(const char* s) {   /* UTF-8 -> UTF-16 code units (BMP) */
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        unsigned c = *p++, n = 0;
        if (c >= 0xF0) { c &= 7; n = 3; } else if (c >= 0xE0) { c &= 15; n = 2; } else if (c >= 0xC0) { c &= 31; n = 1; }
        while (n-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 63);
        if (c >= 32 && c < 0x10000 && c != 127) hle_ui_char((int)c);
    }
}
/* window point -> back-buffer pixel (the frame is letterboxed in the window) */
static int to_frame(float x, float y, int* fx, int* fy, int* fw, int* fh) {
    int vx, vy, vw, vh;
    if (!me1gl_get_view(&vx, &vy, &vw, &vh, fw, fh) || vw <= 0 || vh <= 0) return 0;
    x *= g_ptscale; y *= g_ptscale;
    *fx = (int)((x - vx) * *fw / vw); *fy = (int)((y - vy) * *fh / vh);
    if (*fx < 0) *fx = 0; if (*fy < 0) *fy = 0; if (*fx >= *fw) *fx = *fw - 1; if (*fy >= *fh) *fy = *fh - 1;
    return 1;
}
static float g_vcx = -1, g_vcy = -1;   /* virtual cursor in frame pixels while the mouse is captured */
static int g_moved;                     /* v1.1.5: mouse events since the last cursor_update */
static void mouse_motion(const SDL_MouseMotionEvent* m) {
    int vx, vy, vw, vh, fw, fh;
    g_moved = 1;
    if (g_grabbed && !g_menu) {
        if (!me1gl_get_view(&vx, &vy, &vw, &vh, &fw, &fh) || vw <= 0 || fw <= 0) { hle_ui_mouse_rel(m->xrel, m->yrel); return; }
        if (g_vcx < 0) { g_vcx = fw / 2.0f; g_vcy = fh / 2.0f; }
        float s = g_ptscale * (float)fw / (float)vw;
        g_vcx += m->xrel * s; g_vcy += m->yrel * s;
        if (g_vcx < 0) g_vcx = 0; if (g_vcy < 0) g_vcy = 0; if (g_vcx > fw - 1) g_vcx = fw - 1; if (g_vcy > fh - 1) g_vcy = fh - 1;
        hle_ui_pointer_rel((int)g_vcx, (int)g_vcy, fw, fh, m->xrel, m->yrel);
    } else {
        int fx, fy;
        if (to_frame((float)m->x, (float)m->y, &fx, &fy, &fw, &fh)) { hle_ui_pointer(fx, fy, fw, fh, 0); g_vcx = fx; g_vcy = fy; }
    }
}
static void mouse_button(const SDL_MouseButtonEvent* b) {
    int down = b->type == SDL_MOUSEBUTTONDOWN;
    g_moved = 1;
    if (down && O.grab && !g_grabbed) { set_grab(1); if (g_grabbed) return; }   /* the click that captures the mouse */
    if (b->button == SDL_BUTTON_MIDDLE) { hle_ui_mouse_button(down ? 5 : 6); return; }
    if (b->button != SDL_BUTTON_LEFT && b->button != SDL_BUTTON_RIGHT) return;
    int code = b->button == SDL_BUTTON_LEFT ? (down ? 1 : 2) : (down ? 3 : 4);
    int fx, fy, fw, fh;
    if ((!g_grabbed || g_menu) && to_frame((float)b->x, (float)b->y, &fx, &fy, &fw, &fh)) hle_ui_pointer(fx, fy, fw, fh, code);
    else hle_ui_mouse_button(code);
}
/* v1.1.5: the game's cursor. ME1 has none of its own: on Windows the menus show the system cursor and the game hides
 * it while it captures the mouse for the camera (ShowCursor / SetCursor(NULL) / exclusive DirectInput: hle_ui_cursor).
 * With the mouse captured (O.grab), hidden -> relative mode (mouse look), shown -> the system cursor at the game's
 * cursor, absolute motion as with --no-grab, confined to the window. Called every event-loop pass (<= 20 ms). */
static int g_mcx = -1, g_mcy = -1;      /* game cursor (client pixels) at the last check */
static void cursor_to_window(int cx, int cy, int cw, int ch) {   /* client pixel -> window point; also the virtual cursor */
    int vx, vy, vw, vh, fw, fh;
    if (!me1gl_get_view(&vx, &vy, &vw, &vh, &fw, &fh) || vw <= 0 || vh <= 0 || fw <= 0 || fh <= 0 || cw <= 0 || ch <= 0) return;
    g_vcx = ((float)cx + 0.5f) * fw / cw; g_vcy = ((float)cy + 0.5f) * fh / ch;
    if (g_menu) SDL_WarpMouseInWindow(g_win, (int)((vx + g_vcx * vw / fw) / g_ptscale), (int)((vy + g_vcy * vh / fh) / g_ptscale));
}
static void cursor_update(void) {
    int cx, cy, cw, ch, moved = g_moved;
    g_moved = 0;
    if (!g_grabbed) { g_mcx = -1; return; }
    int s = hle_ui_cursor(&cx, &cy, &cw, &ch);
    if (s < 0) return;
    if ((s == 1) != g_menu) {
        static unsigned n;
        if (s == 1) {   /* menus: the system cursor */
            if (SDL_SetRelativeMouseMode(SDL_FALSE) != 0) return;
            SDL_SetWindowGrab(g_win, SDL_TRUE); g_menu = 1;
        } else {        /* camera: relative mode */
            SDL_SetWindowGrab(g_win, SDL_FALSE);
            if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0) { g_menu = 0; g_grabbed = 0; return; }   /* not captured any more */
            g_menu = 0;
        }
        if (n++ < 100) fprintf(stderr, "[linux] game cursor %s: %s\n", s == 1 ? "shown" : "hidden", s == 1 ? "system cursor" : "relative mouse (camera)");
        cursor_to_window(cx, cy, cw, ch);
    } else if (!moved && g_mcx >= 0 && (cx != g_mcx || cy != g_mcy)) cursor_to_window(cx, cy, cw, ch);   /* SetCursorPos */
    g_mcx = cx; g_mcy = cy;
}

/* gamepad: SDL GameController (any pad with an SDL mapping) -> XInput pad 0 */
static SDL_GameController* g_pad;
static int g_rumble = -1;
static void pad_open(int index) {
    if (g_pad || !SDL_IsGameController(index)) return;
    g_pad = SDL_GameControllerOpen(index);
    if (g_pad) { fprintf(stderr, "[linux] gamepad: %s\n", SDL_GameControllerName(g_pad)); hle_xinput_connect(1); g_rumble = -1; }
}
static void pad_close(SDL_JoystickID id) {
    if (!g_pad || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad)) != id) return;
    SDL_GameControllerClose(g_pad); g_pad = NULL; hle_xinput_connect(0);
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad_open(i); break; }
}
static void pad_state(void) {
    if (!g_pad) return;
    static const struct { SDL_GameControllerButton b; unsigned m; } B[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP, 0x1 }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x2 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x4 },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x8 }, { SDL_CONTROLLER_BUTTON_START, 0x10 }, { SDL_CONTROLLER_BUTTON_BACK, 0x20 },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x40 }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x80 },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x100 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x200 },
        { SDL_CONTROLLER_BUTTON_A, 0x1000 }, { SDL_CONTROLLER_BUTTON_B, 0x2000 }, { SDL_CONTROLLER_BUTTON_X, 0x4000 }, { SDL_CONTROLLER_BUTTON_Y, 0x8000 } };
    unsigned m = 0;
    for (unsigned i = 0; i < sizeof B / sizeof *B; i++) if (SDL_GameControllerGetButton(g_pad, B[i].b)) m |= B[i].m;
    int lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) * 255 / 32767;
    int rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) * 255 / 32767;
#define INV(a) (-(int)(a) > 32767 ? 32767 : -(int)(a))   /* SDL: down positive, XInput: up positive */
    hle_xinput_set(m, lt < 0 ? 0 : lt, rt < 0 ? 0 : rt,
                   SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX), INV(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY)),
                   SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX), INV(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY)));
#undef INV
}
static void pad_rumble(void) {
    if (!g_pad) return;
    static uint32_t last;
    int v = hle_xinput_vibration(); uint32_t now = SDL_GetTicks();
    if (v != g_rumble || (v && now - last > 800)) {   /* XInputSetState holds until changed; SDL rumble needs a duration */
        SDL_GameControllerRumble(g_pad, (Uint16)(v >> 16), (Uint16)(v & 0xFFFF), v ? 1000 : 0);
        g_rumble = v; last = now;
    }
}

/* -------------------------------------------------------------------------------------------------- audio --- */
static SDL_AudioDeviceID g_adev;
static void SDLCALL audio_cb(void* u, Uint8* stream, int len) { (void)u; hle_al_render((int16_t*)stream, len / 4); }
static int audio_open(int rate, const char** name) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "[linux] SDL audio: %s\n", SDL_GetError()); return 0; }
    SDL_AudioSpec want, have; memset(&want, 0, sizeof want);
    want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = rate > 32000 ? 1024 : 512; want.callback = audio_cb;
    g_adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!g_adev) { fprintf(stderr, "[linux] SDL_OpenAudioDevice: %s\n", SDL_GetError()); return 0; }
    static char nm[96]; snprintf(nm, sizeof nm, "SDL2 %s", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "audio");
    *name = nm;
    fprintf(stderr, "[linux] audio: %s, %d Hz, %d frames per callback\n", nm, have.freq, have.samples);
    return have.freq;
}
static void audio_start(void) { if (g_adev) SDL_PauseAudioDevice(g_adev, 0); }

/* -------------------------------------------------------------------------------------------- game thread --- */
static Uint32 g_ev_exit;
static int g_argc; static char** g_argv;
static void* game_main(void* p) {
    (void)p;
    int r = me1_exe_main(g_argc, g_argv);
    fprintf(stderr, "[exe] game exited (%d)\n", r); fflush(stdout); fflush(stderr);
    SDL_Event e; memset(&e, 0, sizeof e); e.type = g_ev_exit; e.user.code = r; SDL_PushEvent(&e);
    return NULL;
}
static void toast(const char* msg) {   /* DEVICE_LOST & co.: a message box would block the game thread - log + title */
    fprintf(stderr, "[linux] %s\n", msg);
}

/* --------------------------------------------------------------------------------------- loading screen --- */
/* v1.1: the picture of the Android launcher (assets/loading.jpg) and the start-up progress (shader compilation) until
 * the first D3D9 Present. The window is handed to the renderer only then (me1gl_set_window: the GL presenter and the
 * Vulkan swap chain attach to a window at any time, as on Android). X11: SDL's software renderer (XShm, no GL context
 * and no window re-creation, SDL_HINT_FRAMEBUFFER_ACCELERATION=0), the same X window goes to EGL/Vulkan afterwards.
 * Wayland: SDL has no software framebuffer there (its GL renderer re-creates the window and keeps an EGL surface on
 * it), so the game gets a fresh window at the hand-over. */
extern const unsigned char me1a_loading[]; extern const unsigned int me1a_loading_size;
static struct { SDL_Renderer* r; SDL_Texture* img; int iw, ih, on, wl; uint32_t t0, last; } LS;
static void loading_start(void) {
    if (g_mw.kind == ME1_WS_X11) {
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
        LS.r = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    } else {
        LS.wl = 1;
        LS.r = SDL_CreateRenderer(g_win, -1, 0);
    }
    if (!LS.r || !ui_init(LS.r)) {
        fprintf(stderr, "[linux] loading screen unavailable (%s)\n", LS.r ? "fonts" : SDL_GetError());
        if (LS.r) SDL_DestroyRenderer(LS.r);
        LS.r = NULL;
        if (LS.wl) { LS.wl = 0; }
        return;
    }
    /* Wayland: SDL's GL renderer re-created the window with SDL_WINDOW_OPENGL; destroying that window at the hand-over
     * would unload SDL's EGL and eglTerminate() the EGLDisplay, which Mesa shares with our GL presenter (same
     * wl_display) - its eglCreateWindowSurface then fails with EGL_NOT_INITIALIZED. One more reference keeps it. */
    if (LS.wl && SDL_GL_LoadLibrary(NULL) != 0) fprintf(stderr, "[linux] SDL_GL_LoadLibrary: %s\n", SDL_GetError());
    LS.img = ui_image(me1a_loading, (int)me1a_loading_size, &LS.iw, &LS.ih);
    LS.on = 1; LS.t0 = SDL_GetTicks(); LS.last = 0;
    SDL_SetWindowTitle(g_win, "Mass Effect");   /* v1.1.3: window titles are ASCII only (some window managers show UTF-8 as Latin-1) */
}
static void loading_draw(void) {
    int64_t s[10] = { 0 }; me1_status(s);
    ui_begin(g_win);
    float sc = ui.h / 820.0f; ui.scale = sc < 1 ? 1 : sc > 3 ? 3 : sc;
    SDL_SetRenderDrawColor(LS.r, 0, 0, 0, 255); SDL_RenderClear(LS.r);
    SDL_Rect full = { 0, 0, ui.w, ui.h };
    if (LS.img) ui_image_cover(LS.img, LS.iw, LS.ih, full, 0.5f);
    SDL_Rect fade = { 0, ui.h * 55 / 100, ui.w, ui.h - ui.h * 55 / 100 }; ui_vgrad(fade, UICA(0x05080c, 0), UICA(0x05080c, 230));
    /* status text: the same lines as GameActivity.showStatus */
    char b[1024]; uint32_t el = (SDL_GetTicks() - LS.t0) / 1000; int n = 0;
    int compiling = s[3] != 0;
    n += snprintf(b + n, sizeof b - n, "%s%u:%02u\n\n", T("Mass Effect запускается… "), el / 60, el % 60);
    n += snprintf(b + n, sizeof b - n, "%s%lld\n", T("Шейдеры D3D создано: "), (long long)s[8]);
    n += snprintf(b + n, sizeof b - n, "%s%lld%s%lld", T("Компиляция HLSL: "), (long long)(s[1] - s[2]), T(", из кэша: "), (long long)s[2]);
    if (compiling) n += snprintf(b + n, sizeof b - n, T("\nсейчас компилируется шейдер (%.1f с)"), s[4] / 1000.0);
    n += snprintf(b + n, sizeof b - n, "%s%lld%s%lld)\n\n", T("\nПрограммы GPU: "), (long long)s[5], T(" (из кэша "), (long long)s[6]);
    snprintf(b + n, sizeof b - n, "%s", compiling || s[1] > s[2] ? T("Идёт компиляция шейдеров. Первый запуск может занять\nнесколько минут; результат сохраняется в кэш.")
                                                              : T("Загрузка данных игры…"));
    int px = D(15), pw = D(560) < ui.w - D(40) ? D(560) : ui.w - D(40);
    int th = ui_textbox_h(UF_REG, px, pw - D(48), b);
    SDL_Rect box = { (ui.w - pw) / 2, ui.h - th - D(48) - D(28), pw, th + D(48) };
    ui_rrect(box, D(10), UICA(0x101418, 0xC8)); ui_rframe(box, D(10), 1, UICA(C_ACC, 110));
    /* indeterminate progress line on the top edge of the box */
    int bw = box.w / 4, span = box.w - D(20) - bw, ph = (int)((SDL_GetTicks() / 4) % (uint32_t)(2 * (span > 1 ? span : 1)));
    if (ph > span) ph = 2 * span - ph;
    SDL_Rect bar = { box.x + D(10) + ph, box.y + D(6), bw, D(3) }; ui_hgrad(bar, UICA(C_ACC, 0), UICA(C_ACC2, 230));
    ui_textbox(UF_REG, px, box.x + D(24), box.y + D(24), pw - D(48), UIC(0xE0E6EE), b);
    SDL_RenderPresent(LS.r);
    ui_end();
}
static void loading_end(void) {
    if (!LS.on) return;
    LS.on = 0;
    if (LS.img) SDL_DestroyTexture(LS.img);
    LS.img = NULL;
    ui_shutdown(); SDL_DestroyRenderer(LS.r); LS.r = NULL;
    if (LS.wl) {   /* Wayland: a fresh window (SDL's GL renderer left its EGL surface on the old one) */
        int w, h; SDL_GetWindowSize(g_win, &w, &h);
        Uint32 fl = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        SDL_Window* nw = SDL_CreateWindow("Mass Effect", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, fl);
        if (nw) { SDL_SetWindowMinimumSize(nw, 320, 200); set_icon(nw); SDL_DestroyWindow(g_win); g_win = nw; }
        else fprintf(stderr, "[linux] new window: %s (keeping the old one)\n", SDL_GetError());
    } else {
#if SDL_VERSION_ATLEAST(2, 28, 0)
        SDL_DestroyWindowSurface(g_win);
#endif
    }
    if (!wm_fetch(NULL)) fprintf(stderr, "[linux] window handles unavailable\n");
    g_mw.w = 0; win_size_update();
    fprintf(stderr, "[linux] first frame after %.1f s: window -> renderer\n", (SDL_GetTicks() - LS.t0) / 1000.0);
    me1gl_set_window(&g_mw);
    SDL_SetWindowTitle(g_win, "Mass Effect");
}

/* --------------------------------------------------------------------------------------------- settings ----- */
static int port_explicit(const char* docs, const char* key) {   /* [Port] key present in me1port.ini (not the Android default) */
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/BioWare/Mass Effect/me1port.ini", docs);
    FILE* f = fopen(p, "r"); if (!f) return 0;
    char line[512]; int found = 0; size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) { char* s = line; while (*s == ' ' || *s == '\t') s++; if (!strncasecmp(s, key, kl) && (s[kl] == '=' || s[kl] == ' ')) { found = 1; break; } }
    fclose(f); return found;
}
static void port_env(const char* docs) {
    char v[64];
    if (me1cfg_port_get(docs, "port.simple_shaders", v, sizeof v) && !strcmp(v, "1")) setenv("ME1_SHADER_LITE", "1", 0);
    if (me1cfg_port_get(docs, "port.phone_mod", v, sizeof v) && !strcmp(v, "1")) setenv("ME1_PHONE_MOD", "1", 0);
    if ((me1cfg_port_get(docs, "port.show_fps", v, sizeof v) && !strcmp(v, "1")) || (getenv("ME1_SHOW_FPS") && !strcmp(getenv("ME1_SHOW_FPS"), "1"))) O.show_fps = 1;
    if (O.fps_limit >= 0) { snprintf(v, sizeof v, "%d", O.fps_limit); setenv("ME1_FPS_LIMIT", v, 1); }
    else if (port_explicit(docs, "FpsLimit") && me1cfg_port_get(docs, "port.fps_limit", v, sizeof v) && atoi(v) > 0) setenv("ME1_FPS_LIMIT", v, 0);
    /* v1.2.1 GEARMASS (launcher -> me1port.ini [Port] Gearmass*): the environment and --gearmass win */
    char gm[64] = "auto";
    if (getenv("ME1_GEARMASS") && *getenv("ME1_GEARMASS")) snprintf(gm, sizeof gm, "%s", getenv("ME1_GEARMASS"));
    else if (me1cfg_port_get(docs, "port.gearmass", v, sizeof v) && *v) { snprintf(gm, sizeof gm, "%s", v); setenv("ME1_GEARMASS", v, 0); }
    if ((me1cfg_port_get(docs, "port.gm_hud", v, sizeof v) && !strcmp(v, "1")) || (getenv("ME1_GM_HUD") && !strcmp(getenv("ME1_GM_HUD"), "1"))) {
        setenv("ME1_GM_HUD", "1", 0); O.show_fps = 1;   /* the indicator lives in the window title next to the FPS */
    }
    if (me1cfg_port_get(docs, "port.gm_thermal", v, sizeof v) && atoi(v) > 0 && atoi(v) != 90) { char t[16]; snprintf(t, sizeof t, "%.2f", atoi(v) / 100.0); setenv("ME1_GM_THERMAL", t, 0); }
    if (!strcasecmp(gm, "custom")) {
        static const char* const K[][2] = { { "port.gm_workers", "ME1_GM_WORKERS" }, { "port.gm_ahead", "ME1_GM_AHEAD" }, { "port.gm_spin", "ME1_GM_SPIN" },
                                            { "port.gm_prime", "ME1_GM_PRIME" }, { "port.gm_aniso", "ME1_GM_ANISO" } };
        for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++)
            if (me1cfg_port_get(docs, K[i][0], v, sizeof v) && *v && strcmp(v, "auto")) setenv(K[i][1], v, 0);
    }
}

/* v1.1 defaults of the desktop: Vulkan when a GPU has it (and the SPIR-V compiler is there), exact D3D9 float rules */
static void renderer_default(void) {
    if (getenv("ME1_RENDERER") || getenv("ME1_GPU_DRIVER")) return;
    const char* sp = getenv("ME1_SPV_LIB");
    void* h = sp && *sp ? dlopen(sp, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!h || !dlsym(h, "me1spv_compile")) {
        fprintf(stderr, "[linux] renderer: gl (no libme1spv.so%s%s: Vulkan needs it, see linux/README.md)\n", sp ? " " : "", sp ? sp : "");
        return;
    }
    GpuDev d[8]; int n = gpu_probe_vulkan(d, 8), b = n > 0 ? gpu_best(d, n) : -1;
    if (b < 0) { fprintf(stderr, "[linux] renderer: gl (%s)\n", n < 0 ? "no Vulkan loader/driver" : "no Vulkan device"); return; }
    if (d[b].type == GPU_CPU) { fprintf(stderr, "[linux] renderer: gl (Vulkan only on the CPU: %s)\n", d[b].name); return; }
    setenv("ME1_RENDERER", "vulkan", 1);
    fprintf(stderr, "[linux] renderer: vulkan (auto: %s, Vulkan %u.%u; --renderer gl for OpenGL ES)\n", d[b].name, d[b].api >> 22, (d[b].api >> 12) & 0x3FF);
}
/* the key of a "key=value" file (the launcher's ~/.config/me1port/launcher.ini) */
static int kv_get(const char* path, const char* key, char* out, size_t cap) {
    FILE* f = fopen(path, "r"); if (!f) return 0;
    char line[1024]; size_t kl = strlen(key); int ok = 0;
    while (!ok && fgets(line, sizeof line, f)) if (!strncmp(line, key, kl) && line[kl] == '=') { snprintf(out, cap, "%s", line + kl + 1); out[strcspn(out, "\r\n")] = 0; ok = 1; }
    fclose(f); return ok;
}
static void config_dir(char* out, size_t cap) {
    const char* xc = getenv("XDG_CONFIG_HOME"); const char* home = getenv("HOME");
    if (xc && *xc) snprintf(out, cap, "%s/me1port", xc); else snprintf(out, cap, "%s/.config/me1port", home ? home : ".");
}
/* first start without the launcher: the desktop resolution instead of the shipped 1024x768 (at most ~1080p) */
static void first_run_resolution(ME1Config* c, const char* docs) {
    char p[PATH_MAX]; struct stat st;
    snprintf(p, sizeof p, "%s/BioWare/Mass Effect/Config/BIOEngine.ini", docs); if (stat(p, &st) == 0) return;
    snprintf(p, sizeof p, "%s/BioWare/Mass Effect/Config/me1port_overrides.ini", docs); if (stat(p, &st) == 0) return;
    SDL_DisplayMode dm; if (SDL_GetDesktopDisplayMode(0, &dm) != 0 || dm.w < 640 || dm.h < 400) return;
    int w = dm.w, h = dm.h;
    if ((long long)w * h > 2400000) { h = (int)((long long)h * 1920 / w); w = 1920; if (h > 1200) { w = w * 1200 / h; h = 1200; } }
    char v[32], err[256], msg[2048]; snprintf(v, sizeof v, "%dx%d", w & ~1, h & ~1);
    if (me1cfg_set(c, "screen.resolution", v, err, sizeof err) == 0 && me1cfg_save(c, msg, sizeof msg) == 0)
        fprintf(stderr, "[linux] first start: game resolution %s (the desktop; change: me1-launcher or me1cfg)\n", v);
}

/* ---------------------------------------------------------------------------------------------------- main --- */
int main(int argc, char** argv) {
    parse_args(argc, argv);
    exe_dir();
    /* game folder */
    static char game[PATH_MAX], docs[PATH_MAX], images[PATH_MAX], tmp[PATH_MAX];
    const char* g = O.game ? O.game : getenv("ME1_GAME") ? getenv("ME1_GAME") : NULL;
    static char cfg_game[PATH_MAX];
    if (!g) {   /* linux/install.sh --game DIR remembers the folder in $XDG_CONFIG_HOME/me1port/game_dir */
        const char* xc = getenv("XDG_CONFIG_HOME"); const char* home = getenv("HOME");
        if (xc && *xc) snprintf(tmp, sizeof tmp, "%s/me1port/game_dir", xc); else snprintf(tmp, sizeof tmp, "%s/.config/me1port/game_dir", home ? home : ".");
        FILE* f = fopen(tmp, "r");
        if (f) { if (fgets(cfg_game, sizeof cfg_game, f)) { cfg_game[strcspn(cfg_game, "\r\n")] = 0; if (*cfg_game) g = cfg_game; } fclose(f); }
        if (!g) g = ".";
    }
    if (!realpath(g, game) || !is_dir(game)) { fprintf(stderr, "me1-linux: нет папки игры %s (укажите --game)\n", g); return 2; }
    char bin[PATH_MAX];
    if (!ci_find(game, "Binaries", 1, bin, sizeof bin)) { fprintf(stderr, "me1-linux: в %s нет Binaries/ — это не папка Mass Effect (укажите --game)\n", game); return 2; }
#ifndef ME1_DEMO_BUILD
    if (!O.images && !ci_find(bin, "MassEffect.exe", 0, NULL, 0))
        fprintf(stderr, "me1-linux: внимание: в %s нет MassEffect.exe — нужна установленная игра (Steam/Origin/диск: v1.02)\n", bin);
#endif
    /* documents (saves, BIO*.ini, logs, caches) */
    if (O.docs) snprintf(docs, sizeof docs, "%s", O.docs);
    else {
        const char* xd = getenv("XDG_DATA_HOME"); const char* home = getenv("HOME");
        if (xd && *xd) snprintf(docs, sizeof docs, "%s/me1port/Documents", xd);
        else snprintf(docs, sizeof docs, "%s/.local/share/me1port/Documents", home && *home ? home : ".");
    }
    mkdirs(docs);
    open_log(docs);
    setvbuf(stdout, NULL, _IOLBF, 0);
    { time_t t = time(NULL); fprintf(stderr, "[linux] me1-linux " ME1_LINUX_VERSION " (%s), pid %d, %s", 
#if defined(__aarch64__)
        "aarch64",
#else
        "x86-64",
#endif
        (int)getpid(), ctime(&t)); }
    fprintf(stderr, "[linux] game %s\n[linux] docs %s\n", game, docs);
    /* module images: --images, or <exe dir>/images when installed with them, else the original DLLs/EXE */
    if (O.images) snprintf(images, sizeof images, "%s", O.images);
    else if (!near_exe("images", images, sizeof images, 1)) strcpy(images, "-");
    /* runtime environment (only where not given by the user) */
    if (O.renderer) {
        const char* r = !strcmp(O.renderer, "gl") || !strcmp(O.renderer, "gles") || !strcmp(O.renderer, "opengl") ? "gles" : O.renderer;
        if (strcmp(r, "gles") && strcmp(r, "vulkan") && strcmp(r, "zink")) { fprintf(stderr, "me1-linux: --renderer vulkan|gl|zink\n"); return 2; }
        setenv("ME1_RENDERER", r, 1);
    }
    if (!getenv("ME1_SPV_LIB") && near_exe("libme1spv.so", tmp, sizeof tmp, 0)) setenv("ME1_SPV_LIB", tmp, 1);
    if (!getenv("ME1_GL_CACHE")) {
        const char* xc = getenv("XDG_CACHE_HOME"); const char* home = getenv("HOME");
        if (xc && *xc) snprintf(tmp, sizeof tmp, "%s/me1port/gl_cache", xc); else snprintf(tmp, sizeof tmp, "%s/.cache/me1port/gl_cache", home && *home ? home : docs);
        mkdirs(tmp); setenv("ME1_GL_CACHE", tmp, 1);
    }
    if (O.mods) setenv("ME1_MODS", O.mods, 1);
    port_env(docs);
    if (!getenv("ME1_FLOAT_EMU")) setenv("ME1_FLOAT_EMU", "2", 1);   /* v1.1: exact float rules (0*inf = 0) - no artifacts on desktop GPUs */
    renderer_default();
    hle_toast_hook = toast;
    {   /* language of the loading screen: --lang, the launcher's choice, the system */
        char cd[PATH_MAX], p[PATH_MAX + 32], lang[32] = ""; config_dir(cd, sizeof cd); snprintf(p, sizeof p, "%s/launcher.ini", cd);
        int li = O.lang ? i18n_find(O.lang) : -1;
        if (li < 0 && kv_get(p, "lang", lang, sizeof lang)) li = i18n_find(lang);
        i18n_set(li >= 0 ? li : i18n_system());
    }
    /* the game's own settings: resolution (window size), fullscreen */
    static ME1Config cfg; me1cfg_load(&cfg, game, docs);

    /* SDL: video + gamepads (audio is opened when the game's OpenAL device starts) */
    if (O.video) setenv("SDL_VIDEODRIVER", O.video, 1);
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_APP_NAME, "Mass Effect");
    /* WM_CLASS / xdg app_id = the .desktop file name: the dock shows one "Mass Effect" icon for the launcher and the game */
    setenv("SDL_VIDEO_X11_WMCLASS", "me1port", 0); setenv("SDL_VIDEO_WAYLAND_WMCLASS", "me1port", 0);
    x11_init_threads();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "[linux] SDL_Init: %s\n", SDL_GetError()); return 1; }
    if (!getenv("ME1_SCREEN")) {
        SDL_DisplayMode dm; int w = 1920, h = 1080;
        if (O.scr_w) { w = O.scr_w; h = O.scr_h; } else if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w >= 640) { w = dm.w; h = dm.h; }
        snprintf(tmp, sizeof tmp, "%dx%d", w, h); setenv("ME1_SCREEN", tmp, 1);
    }
    first_run_resolution(&cfg, docs);
    { int i; if (me1cfg_find("screen.resolution", &i)) sscanf(cfg.value[i], "%dx%d", &g_game_w, &g_game_h);
      if (O.fullscreen < 0 && me1cfg_find("screen.fullscreen", &i)) O.fullscreen = atoi(cfg.value[i]) ? 1 : 0; }
    if (!open_window()) return 1;
    if (!O.no_loading && !getenv("ME1_NO_LOADING")) loading_start();
    if (!LS.on) me1gl_set_window(&g_mw);   /* before the device exists: only recorded; the presenter attaches on CreateDevice */
    hle_al_out_open = audio_open; hle_al_out_start = audio_start;
#ifdef ME1_DEMO_BUILD
    int vpad = -1;   /* ME1_DEMO_VPAD=1: an SDL virtual gamepad that the event loop moves (tests the XInput path) */
    if (getenv("ME1_DEMO_VPAD") && (vpad = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, SDL_CONTROLLER_BUTTON_MAX, 0)) >= 0)
        fprintf(stderr, "[linux] demo: virtual gamepad attached (index %d)\n", vpad);
#endif
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad_open(i); break; }
    signal_handlers();

    /* game thread: argv = prog, game, docs, images, --, game args */
    g_argv = calloc((size_t)O.gargc + 6, sizeof(char*));
    g_argv[0] = "MassEffect"; g_argv[1] = game; g_argv[2] = docs; g_argv[3] = images; g_argv[4] = "--";
    for (int i = 0; i < O.gargc; i++) g_argv[5 + i] = O.gargv[i];
    g_argc = 5 + O.gargc;
    fprintf(stderr, "[linux] screen %s, renderer %s, images %s\n", getenv("ME1_SCREEN"), getenv("ME1_RENDERER") ? getenv("ME1_RENDERER") : "gles (default)", images);
    g_ev_exit = SDL_RegisterEvents(1);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 16u << 20);
    pthread_t th;
    if (pthread_create(&th, &a, game_main, NULL)) { fprintf(stderr, "[linux] cannot start the game thread\n"); return 1; }
    pthread_detach(th);
    if (O.grab && g_focus && !LS.on) set_grab(1);

    /* event loop */
    int code = 0, closing = 0; uint32_t close_at = 0, title_at = 0;
    for (;;) {
        SDL_Event e;
        int got = SDL_WaitEventTimeout(&e, LS.on ? 50 : 20);
        while (got) {
            if (e.type == g_ev_exit) { code = e.user.code; goto out; }
            switch (e.type) {
            case SDL_QUIT:
                if (closing || !hle_ui_close()) { fprintf(stderr, "[linux] window closed: stopping\n"); me1_exe_on_signal(SIGTERM); }
                closing = 1; close_at = SDL_GetTicks();
                fprintf(stderr, "[linux] window close -> WM_CLOSE to the game (close again to stop at once)\n");
                break;
            case SDL_WINDOWEVENT:
                switch (e.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED: case SDL_WINDOWEVENT_RESIZED: case SDL_WINDOWEVENT_SHOWN:
                case SDL_WINDOWEVENT_DISPLAY_CHANGED: win_size_update(); LS.last = 0; break;
                case SDL_WINDOWEVENT_EXPOSED: LS.last = 0; break;
                case SDL_WINDOWEVENT_FOCUS_GAINED: g_focus = 1; if (O.grab && !LS.on) set_grab(1); break;
                case SDL_WINDOWEVENT_FOCUS_LOST: g_focus = 0; set_grab(0); break;
                }
                break;
            case SDL_KEYDOWN: case SDL_KEYUP: key_event(&e.key); break;
            case SDL_TEXTINPUT: text_event(e.text.text); break;
            case SDL_MOUSEMOTION: mouse_motion(&e.motion); break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: if (!LS.on) mouse_button(&e.button); break;
            case SDL_MOUSEWHEEL: {
                int d = e.wheel.y * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
                if (d) hle_ui_wheel(d * 120);
                break;
            }
            case SDL_CONTROLLERDEVICEADDED: pad_open(e.cdevice.which); break;
            case SDL_CONTROLLERDEVICEREMOVED: pad_close(e.cdevice.which); break;
            case SDL_CONTROLLERAXISMOTION: case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: pad_state(); break;
            }
            got = SDL_PollEvent(&e);
        }
        uint32_t now = SDL_GetTicks();
        if (!LS.on) cursor_update();
        if (LS.on) {
            int64_t st[10] = { 0 }; me1_status(st);
            if (st[0] > 0) { loading_end(); if (O.grab && g_focus) set_grab(1); }
            else if (!LS.last || now - LS.last >= 250) { LS.last = now ? now : 1; loading_draw(); }
        }
#ifdef ME1_DEMO_BUILD
        if (vpad >= 0 && g_pad) {   /* virtual pad: left stick up-left, A held, right trigger pulled after 1 s */
            SDL_Joystick* j = SDL_GameControllerGetJoystick(g_pad); int on = now > 1500;
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTX, on ? -26000 : 0);
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTY, on ? -20000 : 0);
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, on ? 30000 : -32768);
            SDL_JoystickSetVirtualButton(j, SDL_CONTROLLER_BUTTON_A, on);
        }
#endif
        pad_rumble();
        if (closing && now - close_at > 15000) { fprintf(stderr, "[linux] the game did not exit after WM_CLOSE: stopping\n"); me1_exe_on_signal(SIGTERM); }
        if (!LS.on && now - title_at > 1000) {   /* title: shader compilation, mouse capture hint, FPS (--show-fps); rarely changed:
                                          every new title redraws the decorations (Wayland: libdecor buffers) */
            title_at = now;
            int64_t p[4] = { 0 }, s[10] = { 0 }; me1_perf(p); me1_status(s);
            char t[256]; int n = snprintf(t, sizeof t, "Mass Effect");
            if (O.show_fps && p[0] > 0) {
                n += snprintf(t + n, sizeof t - n, " - %lld FPS, %s", (long long)((p[0] + 5) / 10), RENDERER_NAMES[p[2] & 3]);
                char gm[160]; if (gm_hud(gm, sizeof gm) && n < (int)sizeof t - 8) n += snprintf(t + n, sizeof t - n, " - %s", gm);   /* v1.2.1 GEARMASS line */
                if (n >= (int)sizeof t) n = (int)sizeof t - 1;
            }
            if (s[3]) n += snprintf(t + n, sizeof t - n, " - compiling shaders...");
            if (!g_grabbed && O.grab && n < (int)sizeof t - 40) snprintf(t + n, sizeof t - n, " - click to capture the mouse");
            static char last[256];
            if (strcmp(t, last)) { SDL_SetWindowTitle(g_win, t); snprintf(last, sizeof last, "%s", t); }
        }
    }
out:
    set_grab(0);
    if (g_adev) SDL_PauseAudioDevice(g_adev, 1);
    fflush(stdout); fflush(stderr);
    _exit(code);   /* the runtime's threads (presenter, audio, heartbeat) are not joined */
}
