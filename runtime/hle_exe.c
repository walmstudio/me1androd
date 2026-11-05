/* HLE for the system DLLs MassEffect.exe imports beyond what the CRT/PhysX stage needed: kernel32 extras, user32
 * (headless window/message stubs), gdi32, advapi32 (W registry, user name), ole32/oleaut32, winmm (timers), dbghelp,
 * wintrust, dsound (reports "no device"; d3d9: hle_d3d9.c, dinput8 + xinput: hle_input.c), wsock32 (network unavailable).
 * Behaviour is chosen so the engine takes its normal "feature unavailable" paths; rendering/input/audio backends replace
 * the device stubs later. ME1_TRACE_UI=1 logs window/message-box calls. */
#define _GNU_SOURCE
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define SETERR(e) hle_set_last_error(c, (e))
static void w2u(uint32_t p, char* o, size_t cap) { if (p) hle_wstr_utf8(p, o, cap); else { o[0] = 0; } }
static void put_w(uint32_t dst, const char* s, uint32_t cap_chars) {   /* ASCII -> guest UTF-16, NUL-terminated, truncating */
    uint32_t i = 0; if (!cap_chars) return;
    for (; s[i] && i + 1 < cap_chars; i++) wr16(dst + 2 * i, (uint8_t)s[i]);
    wr16(dst + 2 * i, 0);
}
static void r0(CPU* c) { RET(0); }
static void r1(CPU* c) { RET(1); }
static void rneg1(CPU* c) { RET(0xFFFFFFFFu); }

/* ---------------- kernel32 ---------------- */
static void k_GetStartupInfoW(CPU* c) { memset(M + ARG(0), 0, 68); wr32(ARG(0), 68); }
static void k_GetVersionExW(CPU* c) {
    uint32_t p = ARG(0), sz = rd32(p);
    wr32(p + 4, 5); wr32(p + 8, 1); wr32(p + 12, 2600); wr32(p + 16, 2);
    memset(M + p + 20, 0, 256); put_w(p + 20, "Service Pack 3", 128);
    if (sz >= 284) { wr16(p + 276, 3); wr16(p + 278, 0); wr16(p + 280, 0x100); wr8(p + 282, 1); wr8(p + 283, 0); }
    RET(1);
}
static int ncpu(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n < 1 ? 1 : n > 32 ? 32 : (int)n; }
static void k_GetSystemInfo(CPU* c) {
    uint32_t p = ARG(0); memset(M + p, 0, 36);
    wr16(p, 0); wr32(p + 4, 4096); wr32(p + 8, 0x10000); wr32(p + 12, 0x7FFEFFFF);
    wr32(p + 16, ncpu() >= 32 ? 0xFFFFFFFFu : (1u << ncpu()) - 1); wr32(p + 20, ncpu()); wr32(p + 24, 586);
    wr32(p + 28, 0x10000); wr16(p + 32, 6); wr16(p + 34, 0x0F02);
}
static void k_GlobalMemoryStatus(CPU* c) {
    uint32_t p = ARG(0); memset(M + p, 0, 32);
    wr32(p, 32); wr32(p + 4, 30); wr32(p + 8, 0x7FFFFFFF); wr32(p + 12, 0x60000000); wr32(p + 16, 0x7FFFFFFF);
    wr32(p + 20, 0x60000000); wr32(p + 24, 0x7FFE0000); wr32(p + 28, 0x60000000);
}
static void k_GetTimeZoneInformation(CPU* c) { memset(M + ARG(0), 0, 172); RET(0); }
static void k_GetComputerNameA(CPU* c) {
    const char* n = "SHEPARD-PC"; uint32_t l = strlen(n), cap = rd32(ARG(1));
    if (cap <= l) { wr32(ARG(1), l + 1); SETERR(111); RET(0); return; }
    memcpy(M + ARG(0), n, l + 1); wr32(ARG(1), l); RET(1);
}
static void k_GetComputerNameW(CPU* c) {
    const char* n = "SHEPARD-PC"; uint32_t l = strlen(n), cap = rd32(ARG(1));
    if (cap <= l) { wr32(ARG(1), l + 1); SETERR(111); RET(0); return; }
    put_w(ARG(0), n, l + 1); wr32(ARG(1), l); RET(1);
}
static void k_lstrlenA(CPU* c) { RET(ARG(0) ? strlen(GSTR(ARG(0))) : 0); }
static void k_lstrlenW(CPU* c) { uint32_t p = ARG(0), n = 0; if (p) while (rd16(p + 2 * n)) n++; RET(n); }
static void k_OutputDebugStringA(CPU* c) { if (ARG(0)) fprintf(stderr, "[dbg] %s%s", GSTR(ARG(0)), strchr(GSTR(ARG(0)), '\n') ? "" : "\n"); }
static void k_OutputDebugStringW(CPU* c) {
    char b[2048]; w2u(ARG(0), b, sizeof b); fprintf(stderr, "[dbg] %s%s", b, strchr(b, '\n') ? "" : "\n");
}
static void k_LoadLibraryW(CPU* c) { char b[520]; w2u(ARG(0), b, sizeof b); RET(hle_load_library(b)); }
static void k_GlobalAlloc(CPU* c) { RET(ghalloc(ARG(1) ? ARG(1) : 1, ARG(0) & 0x40)); }
static void k_GlobalLock(CPU* c) { RET(ARG(0)); }
static void k_GlobalFree(CPU* c) { ghfree(ARG(0)); RET(0); }
static void k_LocalAlloc(CPU* c) { RET(ghalloc(ARG(1) ? ARG(1) : 1, ARG(0) & 0x40)); }
static void k_LocalFree(CPU* c) { ghfree(ARG(0)); RET(0); }
static void k_FormatMessage(CPU* c) { SETERR(317); RET(0); }   /* ERROR_MR_MID_NOT_FOUND */
static void k_SetErrorMode(CPU* c) { static uint32_t m; uint32_t o = m; m = ARG(0); RET(o); }
static void k_CreateProcess(CPU* c) { SETERR(2); RET(0); }
static void k_GetProcessId(CPU* c) { RET(rd32(c->fs_base + 0x20)); }
static void k_GetExitCodeProcess(CPU* c) { if (ARG(1)) wr32(ARG(1), 259); RET(1); }
static void k_CreateNamedPipeW(CPU* c) { SETERR(50); RET(0xFFFFFFFFu); }
static void k_GetConsoleScreenBufferInfo(CPU* c) { SETERR(6); RET(0); }
static void k_GetVersion(CPU* c) { RET(0x0A280105u); }

/* ---------------- advapi32 ---------------- */
static uint32_t g_next_key = 0x5B000001u;
static void a_RegOpenKeyExW(CPU* c) {
    char k[520]; w2u(ARG(1), k, sizeof k);
    if (getenv("ME1_TRACE_REG")) fprintf(stderr, "[hle] RegOpenKeyExW(%08x, \"%s\") -> not found\n", ARG(0), k);
    RET(2);
}
static void a_RegCreateKeyExW(CPU* c) {   /* (hKey, subKey, reserved, class, options, sam, sec, phkResult, disposition) */
    char k[520]; w2u(ARG(1), k, sizeof k);
    if (getenv("ME1_TRACE_REG")) fprintf(stderr, "[hle] RegCreateKeyExW(%08x, \"%s\") -> volatile key\n", ARG(0), k);
    wr32(ARG(7), g_next_key++); if (ARG(8)) wr32(ARG(8), 1 /* REG_CREATED_NEW_KEY */); RET(0);
}
static void a_RegSetValueExW(CPU* c) { RET(0); }
static void a_RegQueryValueExW(CPU* c) { RET(2); }
static void a_GetUserNameA(CPU* c) {
    const char* n = "Shepard"; uint32_t l = strlen(n) + 1;
    if (rd32(ARG(1)) < l) { wr32(ARG(1), l); SETERR(122); RET(0); return; }
    memcpy(M + ARG(0), n, l); wr32(ARG(1), l); RET(1);
}
static void a_GetUserNameW(CPU* c) {
    const char* n = "Shepard"; uint32_t l = strlen(n) + 1;
    if (rd32(ARG(1)) < l) { wr32(ARG(1), l); SETERR(122); RET(0); return; }
    put_w(ARG(0), n, l); wr32(ARG(1), l); RET(1);
}

/* ---------------- user32 / gdi32: headless ---------------- */
static int ui_trace(void) { static int t = -1; if (t < 0) { const char* e = getenv("ME1_TRACE_UI"); t = e ? atoi(e) : 0; } return t; }
static void u_MessageBoxA(CPU* c) {
    fprintf(stderr, "[hle] MessageBoxA(\"%s\": \"%s\", type %x) -> IDOK\n", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : "", ARG(3));
    uint32_t ty = ARG(3) & 15; RET(ty == 4 || ty == 3 ? 6 : 1);
}
static void u_MessageBoxW(CPU* c) {
    char t[4096], cap[512]; w2u(ARG(1), t, sizeof t); w2u(ARG(2), cap, sizeof cap);
    fprintf(stderr, "[hle] MessageBoxW(\"%s\": \"%s\", type %x) -> IDOK\n", cap, t, ARG(3));
    uint32_t ty = ARG(3) & 15; RET(ty == 4 || ty == 3 ? 6 /* IDYES */ : 1 /* IDOK */);
}
/* Windows: a headless window manager. Windows exist only as records (class, window procedure, style, rectangle,
 * user data); nothing is drawn. The top-level window shown last is the active / foreground / focus window, which is
 * what the engine expects of its game viewport (Bink and the audio focus check look it up). SendMessage calls the
 * window procedure directly; messages posted by the game are dropped; input from the front end (hle_ui_pointer /
 * hle_ui_key) is queued for PeekMessage/DispatchMessage and also feeds DirectInput. */
static pthread_mutex_t g_wmx = PTHREAD_MUTEX_INITIALIZER;
#define WCLS_MAX 64
#define WIN_MAX 128
#define HWND_DESKTOP_ 0x00010010u
typedef struct { uint32_t atom, proc, style, inst; char name[80]; } WClass;
typedef struct { int used, visible; uint32_t hwnd, proc, style, exstyle, parent, userdata, id, inst, tid; int32_t x, y, w, h; char cls[80], title[160]; } Win;
static WClass g_cls[WCLS_MAX]; static int g_ncls;
static Win g_win[WIN_MAX];
static uint32_t g_atom = 0xC100, g_hwnd_seq = 0x0003001Cu, g_active;
static unsigned g_scr_w, g_scr_h;
static void screen_size(unsigned* w, unsigned* h) {   /* the desktop mode hle_d3d9.c reports (ME1_SCREEN) */
    if (!g_scr_w) {
        const char* e = getenv("ME1_SCREEN"); unsigned a = 0, b = 0;
        if (e && sscanf(e, "%ux%u", &a, &b) == 2 && a >= 320 && b >= 200) { g_scr_w = a; g_scr_h = b; } else { g_scr_w = 1280; g_scr_h = 720; }
    }
    *w = g_scr_w; *h = g_scr_h;
}
static void lower_copy(char* d, size_t cap, const char* s) { size_t i = 0; for (; s[i] && i + 1 < cap; i++) d[i] = (char)tolower((unsigned char)s[i]); d[i] = 0; }
static void cls_name(uint32_t p, int wide, char* o, size_t cap) {   /* class name or "#atom" */
    char t[256];
    if (p < 0x10000u) { snprintf(o, cap, "#%u", p); return; }
    if (wide) hle_wstr_utf8(p, t, sizeof t); else snprintf(t, sizeof t, "%s", GSTR(p));
    lower_copy(o, cap, t);
}
static uint32_t reg_class(CPU* c, uint32_t proc, uint32_t style, uint32_t inst, uint32_t name, int wide) {
    char n[80]; cls_name(name, wide, n, sizeof n);
    pthread_mutex_lock(&g_wmx);
    for (int i = 0; i < g_ncls; i++) if (!strcmp(g_cls[i].name, n)) { pthread_mutex_unlock(&g_wmx); SETERR(1410 /* ERROR_CLASS_ALREADY_EXISTS */); return 0; }
    if (g_ncls == WCLS_MAX) { pthread_mutex_unlock(&g_wmx); SETERR(8); return 0; }
    WClass* k = &g_cls[g_ncls++]; k->atom = g_atom++; k->proc = proc; k->style = style; k->inst = inst; snprintf(k->name, sizeof k->name, "%s", n);
    pthread_mutex_unlock(&g_wmx);
    if (ui_trace()) fprintf(stderr, "[ui] RegisterClass(\"%s\", wndproc %08x) -> atom %04x\n", n, proc, k->atom);
    return k->atom;
}
/* WNDCLASSW: style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground, lpszMenuName, lpszClassName
 * WNDCLASSEX: cbSize first, then the same fields and hIconSm */
static void u_RegisterClassW(CPU* c) { uint32_t w = ARG(0); RET(reg_class(c, rd32(w + 4), rd32(w), rd32(w + 16), rd32(w + 36), 1)); }
static void u_RegisterClassExW(CPU* c) { uint32_t w = ARG(0); RET(reg_class(c, rd32(w + 8), rd32(w + 4), rd32(w + 20), rd32(w + 40), 1)); }
static void u_RegisterClassExA(CPU* c) { uint32_t w = ARG(0); RET(reg_class(c, rd32(w + 8), rd32(w + 4), rd32(w + 20), rd32(w + 40), 0)); }
static void u_RegisterClassA(CPU* c) { uint32_t w = ARG(0); RET(reg_class(c, rd32(w + 4), rd32(w), rd32(w + 16), rd32(w + 36), 0)); }
static void unreg_class(CPU* c, int wide);
static void u_UnregisterClassW(CPU* c) { unreg_class(c, 1); }
static void u_UnregisterClassA(CPU* c) { unreg_class(c, 0); }
static void unreg_class(CPU* c, int wide) {
    char n[80]; cls_name(ARG(0), wide, n, sizeof n); int ok = 0;
    pthread_mutex_lock(&g_wmx);
    for (int i = 0; i < g_ncls; i++)
        if (!strcmp(g_cls[i].name, n) || (ARG(0) < 0x10000u && g_cls[i].atom == ARG(0))) { g_cls[i] = g_cls[--g_ncls]; ok = 1; break; }
    pthread_mutex_unlock(&g_wmx);
    if (!ok) SETERR(1411 /* ERROR_CLASS_DOES_NOT_EXIST */);
    RET(ok);
}
static Win* win_get(uint32_t h) {   /* caller holds g_wmx */
    if (!h) return NULL;
    for (int i = 0; i < WIN_MAX; i++) if (g_win[i].used && g_win[i].hwnd == h) return &g_win[i];
    return NULL;
}
static void reactivate(Win* gone) {   /* the active window was hidden/destroyed: fall back to another visible top-level one */
    if (g_active != gone->hwnd) return;
    g_active = 0;
    for (int i = 0; i < WIN_MAX; i++) if (g_win[i].used && &g_win[i] != gone && g_win[i].visible && !g_win[i].parent) g_active = g_win[i].hwnd;
}
static void create_window(CPU* c, int wide) {
    /* (exStyle, className, windowName, style, x, y, w, h, parent, menu, hInstance, lpParam) */
    char n[80], t[160]; cls_name(ARG(1), wide, n, sizeof n);
    if (ARG(2)) { if (wide) hle_wstr_utf8(ARG(2), t, sizeof t); else snprintf(t, sizeof t, "%s", GSTR(ARG(2))); } else t[0] = 0;
    unsigned sw, sh; screen_size(&sw, &sh);
    pthread_mutex_lock(&g_wmx);
    uint32_t proc = 0; int found = 0;
    for (int i = 0; i < g_ncls; i++)
        if (!strcmp(g_cls[i].name, n) || (ARG(1) < 0x10000u && g_cls[i].atom == ARG(1))) { proc = g_cls[i].proc; found = 1; break; }
    Win* w = NULL;
    for (int i = 0; i < WIN_MAX; i++) if (!g_win[i].used) { w = &g_win[i]; break; }
    if (!w) { pthread_mutex_unlock(&g_wmx); SETERR(1400); RET(0); return; }
    memset(w, 0, sizeof *w);
    w->used = 1; w->hwnd = g_hwnd_seq; g_hwnd_seq += 0x00010002u;
    w->proc = proc; w->exstyle = ARG(0); w->style = ARG(3); w->parent = ARG(8) == HWND_DESKTOP_ ? 0 : ARG(8);
    w->id = (w->style & 0x40000000u) ? ARG(9) : 0; w->inst = ARG(10); w->tid = rd32(c->fs_base + 0x24);
    w->x = ARG(4) == 0x80000000u ? 0 : (int32_t)ARG(4); w->y = ARG(4) == 0x80000000u ? 0 : (int32_t)ARG(5);
    w->w = ARG(6) == 0x80000000u ? (int32_t)sw : (int32_t)ARG(6); w->h = ARG(6) == 0x80000000u ? (int32_t)sh : (int32_t)ARG(7);
    w->visible = (w->style & 0x10000000u) != 0;   /* WS_VISIBLE */
    snprintf(w->cls, sizeof w->cls, "%s", n); snprintf(w->title, sizeof w->title, "%s", t);
    if (w->visible && !w->parent) g_active = w->hwnd;
    uint32_t h = w->hwnd;
    pthread_mutex_unlock(&g_wmx);
    fprintf(stderr, "[ui] CreateWindowEx%s(class \"%s\"%s, \"%s\", style %08x/%08x, %d,%d %dx%d, parent %08x) -> %08x\n",
            wide ? "W" : "A", n, found ? "" : " (unregistered)", t, ARG(3), ARG(0), w->x, w->y, w->w, w->h, ARG(8), h);
    SETERR(0); RET(h);
}
static void u_CreateWindowExW(CPU* c) { create_window(c, 1); }
static void u_CreateWindowExA(CPU* c) { create_window(c, 0); }
static void u_DestroyWindow(CPU* c) {
    pthread_mutex_lock(&g_wmx);
    Win* w = win_get(ARG(0));
    if (w) {
        for (int i = 0; i < WIN_MAX; i++) if (g_win[i].used && g_win[i].parent == w->hwnd) g_win[i].used = 0;   /* children go too */
        reactivate(w);
        w->used = 0;
    }
    pthread_mutex_unlock(&g_wmx);
    if (ui_trace()) fprintf(stderr, "[ui] DestroyWindow(%08x) -> %d\n", ARG(0), w != NULL);
    if (!w) SETERR(1400);
    RET(w != NULL);
}
static void u_IsWindow(CPU* c) { pthread_mutex_lock(&g_wmx); int ok = win_get(ARG(0)) != NULL || ARG(0) == HWND_DESKTOP_; pthread_mutex_unlock(&g_wmx); RET(ok); }
static void u_IsWindowVisible(CPU* c) { pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0)); int v = w && w->visible; pthread_mutex_unlock(&g_wmx); RET(v); }
static void u_GetParent(CPU* c) { pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0)); uint32_t p = w ? w->parent : 0; pthread_mutex_unlock(&g_wmx); RET(p); }
static void u_ShowWindow(CPU* c) {   /* returns the previous visibility */
    uint32_t cmd = ARG(1); int was = 0;
    pthread_mutex_lock(&g_wmx);
    Win* w = win_get(ARG(0));
    if (w) {
        was = w->visible;
        if (cmd == 0 /* SW_HIDE */) { w->visible = 0; reactivate(w); }
        else { w->visible = 1; if (!w->parent && cmd != 4 && cmd != 7 && cmd != 8 && cmd != 6 && cmd != 2) g_active = w->hwnd; }   /* not SW_*NOACTIVATE / minimize */
    }
    pthread_mutex_unlock(&g_wmx);
    if (ui_trace()) fprintf(stderr, "[ui] ShowWindow(%08x, %u)\n", ARG(0), cmd);
    RET(was);
}
static uint32_t activate(uint32_t h) {   /* returns the previously active window */
    pthread_mutex_lock(&g_wmx);
    uint32_t o = g_active; Win* w = win_get(h);
    if (w) { while (w->parent && win_get(w->parent)) w = win_get(w->parent); g_active = w->hwnd; }
    pthread_mutex_unlock(&g_wmx);
    return o;
}
static void u_SetForegroundWindow(CPU* c) { uint32_t h = ARG(0); int ok; pthread_mutex_lock(&g_wmx); ok = win_get(h) != NULL; pthread_mutex_unlock(&g_wmx); if (ok) activate(h); RET(ok); }
static void u_SetActiveWindow(CPU* c) { RET(activate(ARG(0))); }
static void u_SetFocus(CPU* c) { RET(ARG(0) ? activate(ARG(0)) : g_active); }
static void u_GetActive(CPU* c) { RET(g_active); }   /* GetForegroundWindow / GetActiveWindow / GetFocus / GetTopWindow(NULL) */
static void u_GetTopWindow(CPU* c) {
    if (ARG(0) && ARG(0) != HWND_DESKTOP_) {
        uint32_t r = 0; pthread_mutex_lock(&g_wmx);
        for (int i = 0; i < WIN_MAX; i++) if (g_win[i].used && g_win[i].parent == ARG(0)) { r = g_win[i].hwnd; break; }
        pthread_mutex_unlock(&g_wmx); RET(r); return;
    }
    RET(g_active);
}
static void u_GetWindowThreadProcessId(CPU* c) {
    pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0)); uint32_t tid = w ? w->tid : 0; pthread_mutex_unlock(&g_wmx);
    if (!w) { SETERR(1400); RET(0); return; }
    if (ARG(1)) wr32(ARG(1), rd32(c->fs_base + 0x20));   /* our process */
    RET(tid);
}
static void u_GetRect(CPU* c) {   /* GetClientRect / GetWindowRect */
    unsigned sw, sh; screen_size(&sw, &sh); uint32_t r = ARG(1);
    pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0));
    int32_t x = 0, y = 0, cw = sw, ch = sh; if (w) { x = w->x; y = w->y; cw = w->w; ch = w->h; }
    pthread_mutex_unlock(&g_wmx);
    wr32(r, 0); wr32(r + 4, 0); wr32(r + 8, cw); wr32(r + 12, ch);
    (void)x; (void)y; RET(1);
}
static void u_GetWindowRect(CPU* c) {
    unsigned sw, sh; screen_size(&sw, &sh); uint32_t r = ARG(1);
    pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0));
    int32_t x = 0, y = 0, cw = sw, ch = sh; if (w) { x = w->x; y = w->y; cw = w->w; ch = w->h; }
    pthread_mutex_unlock(&g_wmx);
    wr32(r, x); wr32(r + 4, y); wr32(r + 8, x + cw); wr32(r + 12, y + ch); RET(1);
}
static void u_ClientToScreen(CPU* c) {   /* (hwnd, POINT*) */
    pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0)); int32_t x = w ? w->x : 0, y = w ? w->y : 0; pthread_mutex_unlock(&g_wmx);
    if (ARG(1)) { wr32(ARG(1), rd32(ARG(1)) + x); wr32(ARG(1) + 4, rd32(ARG(1) + 4) + y); }
    RET(1);
}
static void u_ScreenToClient(CPU* c) {
    pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0)); int32_t x = w ? w->x : 0, y = w ? w->y : 0; pthread_mutex_unlock(&g_wmx);
    if (ARG(1)) { wr32(ARG(1), rd32(ARG(1)) - x); wr32(ARG(1) + 4, rd32(ARG(1) + 4) - y); }
    RET(1);
}
static void u_SetWindowPos(CPU* c) {   /* (hwnd, after, x, y, cx, cy, flags) */
    uint32_t f = ARG(6);
    pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0));
    if (w) {
        if (!(f & 2)) { w->x = (int32_t)ARG(2); w->y = (int32_t)ARG(3); }     /* SWP_NOMOVE */
        if (!(f & 1)) { w->w = (int32_t)ARG(4); w->h = (int32_t)ARG(5); }     /* SWP_NOSIZE */
        if (f & 0x40) w->visible = 1;                                         /* SWP_SHOWWINDOW */
        if (f & 0x80) { w->visible = 0; reactivate(w); }                      /* SWP_HIDEWINDOW */
        if (!(f & 0x10) && w->visible && !w->parent) g_active = w->hwnd;      /* not SWP_NOACTIVATE */
    }
    pthread_mutex_unlock(&g_wmx);
    if (ui_trace()) fprintf(stderr, "[ui] SetWindowPos(%08x, %d,%d %dx%d, flags %x)\n", ARG(0), ARG(2), ARG(3), ARG(4), ARG(5), f);
    if (!w) SETERR(1400);
    RET(w != NULL);
}
static uint32_t* win_long(Win* w, int32_t i) {
    switch (i) {
    case -4: return &w->proc; case -6: return &w->inst; case -8: return &w->parent; case -12: return &w->id;
    case -16: return &w->style; case -20: return &w->exstyle; case -21: return &w->userdata; default: return NULL;
    }
}
static void u_SetWindowLong(CPU* c) {   /* SetWindowLongW/A(hwnd, index, value) -> previous value */
    pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0)); uint32_t* p = w ? win_long(w, (int32_t)ARG(1)) : NULL, o = 0;
    if (p) { o = *p; *p = ARG(2); if ((int32_t)ARG(1) == -16) w->visible = (ARG(2) & 0x10000000u) != 0 || w->visible; }
    pthread_mutex_unlock(&g_wmx);
    if (ui_trace()) fprintf(stderr, "[ui] SetWindowLong(%08x, %d, %08x) -> %08x\n", ARG(0), (int32_t)ARG(1), ARG(2), o);
    SETERR(p ? 0 : w ? 1413 /* ERROR_INVALID_INDEX */ : 1400); RET(o);
}
static void u_GetWindowLong(CPU* c) {
    pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0)); uint32_t* p = w ? win_long(w, (int32_t)ARG(1)) : NULL, v = p ? *p : 0;
    pthread_mutex_unlock(&g_wmx);
    SETERR(p ? 0 : w ? 1413 : 1400); RET(v);
}
static void u_SetWindowTextW(CPU* c) {
    pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0)); if (w && ARG(1)) hle_wstr_utf8(ARG(1), w->title, sizeof w->title);
    pthread_mutex_unlock(&g_wmx);
    RET(1);
}
/* SendMessage: the window procedure runs on the calling thread (Windows would switch to the owner thread for
 * cross-thread sends; the engine only sends to its own windows). Runs without the big HLE lock. */
static void u_SendMessage(CPU* c) {
    pthread_mutex_lock(&g_wmx); Win* w = win_get(ARG(0)); uint32_t proc = w ? w->proc : 0; pthread_mutex_unlock(&g_wmx);
    if (ui_trace()) fprintf(stderr, "[ui] SendMessage(%08x, %04x, %08x, %08x) -> wndproc %08x\n", ARG(0), ARG(1), ARG(2), ARG(3), proc);
    if (!proc) { RET(0); return; }
    uint32_t a[4] = { ARG(0), ARG(1), ARG(2), ARG(3) };
    CPU save = *c;
    hle_hb_leave();
    uint32_t r = rt_call(c, proc, 4, a);
    c->ebx = save.ebx; c->esi = save.esi; c->edi = save.edi; c->ebp = save.ebp; c->esp = save.esp;
    RET(r);
}
static void u_SendMessageTimeoutW(CPU* c) {   /* (hwnd, msg, wp, lp, flags, timeout, &result): only our windows answer */
    if (ARG(6)) wr32(ARG(6), 0);
    pthread_mutex_lock(&g_wmx); int ok = win_get(ARG(0)) != NULL; pthread_mutex_unlock(&g_wmx);
    if (!ok) { SETERR(1400); RET(0); return; }
    RET(0); SETERR(1460 /* ERROR_TIMEOUT */);   /* the single-instance probe must not find ourselves */
}
/* Input from the front end (Android touch / keys): a message queue of the active window read by PeekMessage and
 * delivered by DispatchMessage, plus the cursor position (GetCursorPos) and key state (GetKeyState). */
#define MQ 256
typedef struct { uint32_t hwnd, msg, wp, lp, time; int32_t x, y; uint32_t frame; } QMsg;   /* frame: Present count when queued */
static QMsg g_mq[MQ]; static unsigned g_mq_head, g_mq_tail;
/* Touch pacing: a tap is a cursor jump + button down + button up within a few milliseconds. The engine reads the
 * cursor position once per frame (UI scene client / Scaleform) and evaluates clicks against the position of the
 * previous frame, and a press and release inside one frame can be lost. So mouse button messages are held back:
 * a button-down until ME1_TOUCH_FRAMES (default 2) frames after it was queued (the jump is seen first), a button-up
 * until ME1_TOUCH_HOLD (default 2) frames after its button-down was delivered. Each waits at most 400 ms (loading
 * screens present no frames). */
static int g_tp_frames = -1, g_tp_hold = 2;
static uint32_t g_down_frame, g_down_ms;
static void touch_cfg(void) {
    if (g_tp_frames >= 0) return;
    const char* e = getenv("ME1_TOUCH_FRAMES"); g_tp_frames = e ? atoi(e) : 2; if (g_tp_frames < 0) g_tp_frames = 0;
    e = getenv("ME1_TOUCH_HOLD"); g_tp_hold = e ? atoi(e) : 2; if (g_tp_hold < 0) g_tp_hold = 0;
}
static int32_t g_curx = -1, g_cury = -1;                 /* cursor, screen coordinates (-1: screen center) */
/* v1.1.5: the Windows cursor as the OS would draw it. UE3 hides it with ShowCursor(FALSE) when it captures the mouse
 * and shows it again when it releases it; the front ends draw a cursor while it is visible (hle_ui_cursor).
 * Visible: ShowCursor display count >= 0 (0 at start, as with a mouse installed) and no SetCursor(NULL). */
static int g_showcount, g_cursor_null;
static uint32_t g_capture;                                /* SetCapture window */
static int cursor_visible(void) { return g_showcount >= 0 && !g_cursor_null && !hle_input_mouse_exclusive(); }   /* caller holds g_wmx;
                                                                   a DirectInput mouse acquired exclusively hides it too */
/* cursor state changes are rare (mouse capture on/off) and logged: they tell when the game shows the cursor */
static unsigned g_curlog;
#define CURLOG(...) do { if (g_curlog < 400 || ui_trace()) { g_curlog++; fprintf(stderr, __VA_ARGS__); } } while (0)
static int32_t g_lastx = -1, g_lasty = -1;                /* client position of the last pointer event (DirectInput deltas) */
/* Key state, Windows semantics: g_avk is the physical state (GetAsyncKeyState, set when the front end reports the
 * input), g_vk the message-synchronized one (GetKeyState): it changes when PeekMessage removes the key / button
 * message, so a held-back WM_LBUTTONDOWN still reads as "down" when the window procedure handles it even if the finger
 * was already lifted (UE3 sends IE_Pressed / IE_Released for a button message from GetKeyState). bit 7: down, bit 0: toggled */
static uint8_t g_vk[256], g_avk[256];
static uint8_t g_apress[256];   /* v1.1: GetAsyncKeyState bit 0 - pressed since the last call (short taps between polls) */
static void vk_set(uint8_t* t, unsigned vk, int down) {
    vk &= 255;
    if (down && t == g_avk) g_apress[vk] = 1;
    if (down) t[vk] = (uint8_t)(0x80 | ((t[vk] & 0x80) ? (t[vk] & 1) : ((t[vk] ^ 1) & 1))); else t[vk] &= 1;
}
static void vk_sync(uint32_t msg, uint32_t wp) {   /* a message was removed from the queue (caller holds g_wmx) */
    switch (msg) {
    case 0x0201: vk_set(g_vk, 1, 1); break;  case 0x0202: vk_set(g_vk, 1, 0); break;    /* WM_LBUTTONDOWN/UP */
    case 0x0204: vk_set(g_vk, 2, 1); break;  case 0x0205: vk_set(g_vk, 2, 0); break;    /* WM_RBUTTONDOWN/UP */
    case 0x0207: vk_set(g_vk, 4, 1); break;  case 0x0208: vk_set(g_vk, 4, 0); break;    /* WM_MBUTTONDOWN/UP */
    case 0x0100: case 0x0104: vk_set(g_vk, wp, 1); break;                                /* WM_(SYS)KEYDOWN */
    case 0x0101: case 0x0105: vk_set(g_vk, wp, 0); break;                                /* WM_(SYS)KEYUP */
    }
}
static uint32_t msg_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static void mq_push(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {   /* caller holds g_wmx */
    if (g_mq_head - g_mq_tail >= MQ) g_mq_tail++;
    QMsg* q = &g_mq[g_mq_head++ % MQ]; q->hwnd = hwnd; q->msg = msg; q->wp = wp; q->lp = lp; q->time = msg_time(); q->frame = hle_d3d9_frames();
    unsigned sw, sh; screen_size(&sw, &sh);
    q->x = g_curx < 0 ? (int32_t)sw / 2 : g_curx; q->y = g_cury < 0 ? (int32_t)sh / 2 : g_cury;
}
static uint32_t mk_buttons(void) {   /* MK_* of wParam */
    const uint8_t* v = g_avk;
    return (v[1] & 0x80 ? 1u : 0) | (v[2] & 0x80 ? 2u : 0) | ((v[0x10] | v[0xA0] | v[0xA1]) & 0x80 ? 4u : 0) |
           ((v[0x11] | v[0xA2] | v[0xA3]) & 0x80 ? 8u : 0) | (v[4] & 0x80 ? 0x10u : 0);
}
/* v1.1.5: Windows sends WM_SETCURSOR (wParam hwnd, lParam HTCLIENT | mouse message << 16) to the window under the
 * cursor before a mouse message unless the mouse is captured; UE3 answers with SetCursor (arrow / NULL). One pending
 * at a time (a fast mouse would fill the queue). Caller holds g_wmx. */
static void mq_setcursor(uint32_t hwnd, uint32_t mmsg) {
    if (g_capture) return;
    for (unsigned i = g_mq_tail; i != g_mq_head; i++) if (g_mq[i % MQ].msg == 0x0020) return;
    mq_push(hwnd, 0x0020, hwnd, (mmsg << 16) | 1u);
}
/* pointer at (fx, fy) of a frame of fw x fh pixels (the back buffer), button: 0 move, 1 left down, 2 left up,
 * 3 right down, 4 right up; also feeds the DirectInput mouse (relative motion) */
static unsigned g_dibtn;   /* DirectInput mouse buttons (bit 0 left, bit 1 right) */
static void ui_pointer(int fx, int fy, int fw, int fh, int button, int rel, int rdx, int rdy);
void hle_ui_pointer(int fx, int fy, int fw, int fh, int button) { ui_pointer(fx, fy, fw, fh, button, 0, 0, 0); }
/* v1.1 desktop (captured mouse): cursor at (fx, fy) for the window messages, DirectInput gets the real relative motion
 * (not the clamped cursor difference - mouse look keeps turning at the window edge) */
void hle_ui_pointer_rel(int fx, int fy, int fw, int fh, int dx, int dy) { ui_pointer(fx, fy, fw, fh, 0, 1, dx, dy); }
static void ui_pointer(int fx, int fy, int fw, int fh, int button, int rel, int rdx, int rdy) {
    unsigned dibtn;
    pthread_mutex_lock(&g_wmx);
    Win* w = win_get(g_active);
    int32_t cw = w ? w->w : 0, ch = w ? w->h : 0; unsigned sw, sh; screen_size(&sw, &sh);
    if (cw <= 0 || ch <= 0) { cw = (int32_t)sw; ch = (int32_t)sh; }
    int32_t cx = fw > 0 ? (int32_t)((int64_t)fx * cw / fw) : fx, cy = fh > 0 ? (int32_t)((int64_t)fy * ch / fh) : fy;
    if (cx < 0) cx = 0; if (cy < 0) cy = 0; if (cx >= cw) cx = cw - 1; if (cy >= ch) cy = ch - 1;
    g_curx = (w ? w->x : 0) + cx; g_cury = (w ? w->y : 0) + cy;
    if (button >= 1 && button <= 4) {
        vk_set(g_avk, button <= 2 ? 1 : 2, button == 1 || button == 3);
        if (!w) vk_set(g_vk, button <= 2 ? 1 : 2, button == 1 || button == 3);   /* no window, no message */
    }
    uint32_t lp = ((uint32_t)cy << 16) | ((uint32_t)cx & 0xFFFF);
    if (w) {
        mq_setcursor(w->hwnd, button >= 1 && button <= 4 ? (button == 1 ? 0x0201u : button == 3 ? 0x0204u : 0x0200u) : 0x0200u);
        mq_push(w->hwnd, 0x0200, mk_buttons(), lp);                                   /* WM_MOUSEMOVE */
        static const uint32_t bm[5] = { 0, 0x0201, 0x0202, 0x0204, 0x0205 };          /* WM_L/RBUTTONDOWN/UP */
        if (button >= 1 && button <= 4) mq_push(w->hwnd, bm[button], mk_buttons(), lp);
    }
    int dx = g_lastx < 0 || button == 1 || button == 3 ? 0 : cx - g_lastx, dy = g_lasty < 0 || button == 1 || button == 3 ? 0 : cy - g_lasty;
    g_lastx = cx; g_lasty = cy;
    if (rel) { dx = rdx; dy = rdy; }
    dibtn = g_dibtn;
    if (button == 1) dibtn |= 1; else if (button == 2) dibtn &= ~1u; else if (button == 3) dibtn |= 2; else if (button == 4) dibtn &= ~2u;
    unsigned b = g_dibtn = dibtn;
    pthread_mutex_unlock(&g_wmx);
    hle_input_mouse(dx, dy, 0, b);
    if (ui_trace() > 1) fprintf(stderr, "[ui] pointer %d,%d of %dx%d -> client %d,%d button %d\n", fx, fy, fw, fh, cx, cy, button);
}
/* on-screen controls (v0.9.5): relative mouse motion of the look area - DirectInput only, the cursor stays where it is.
 * v1.1.5: while the game shows the cursor (menus) the motion also moves it, like a real mouse (touchpad-like look area,
 * right stick of a pad in the keyboard/mouse profile) */
void hle_ui_mouse_rel(int dx, int dy) {
    if (!dx && !dy) return;
    pthread_mutex_lock(&g_wmx); unsigned b = g_dibtn;
    Win* w = win_get(g_active);
    if (w && w->w > 0 && w->h > 0 && cursor_visible()) {
        unsigned sw, sh; screen_size(&sw, &sh);
        int32_t cx = (g_curx < 0 ? (int32_t)sw / 2 : g_curx) - w->x + dx, cy = (g_cury < 0 ? (int32_t)sh / 2 : g_cury) - w->y + dy;
        if (cx < 0) cx = 0; if (cy < 0) cy = 0; if (cx >= w->w) cx = w->w - 1; if (cy >= w->h) cy = w->h - 1;
        g_curx = w->x + cx; g_cury = w->y + cy; g_lastx = cx; g_lasty = cy;
        mq_setcursor(w->hwnd, 0x0200);
        mq_push(w->hwnd, 0x0200, mk_buttons(), ((uint32_t)cy << 16) | ((uint32_t)cx & 0xFFFF));   /* WM_MOUSEMOVE */
    }
    pthread_mutex_unlock(&g_wmx);
    hle_input_mouse(dx, dy, 0, b);
}
/* v1.1.5: the cursor for the front ends - client position and size of the active window; returns 1 when it is shown
 * (Windows rules, see cursor_visible; ME1_CURSOR=0 never, 2 always), 0 hidden, -1 no window yet */
int hle_ui_cursor(int* x, int* y, int* w, int* h) {
    static int mode = -1;
    if (mode < 0) { const char* e = getenv("ME1_CURSOR"); mode = e && *e ? atoi(e) : 1; }
    pthread_mutex_lock(&g_wmx);
    Win* wn = win_get(g_active); int r = -1;
    if (wn && wn->w > 0 && wn->h > 0) {
        unsigned sw, sh; screen_size(&sw, &sh);
        *x = (g_curx < 0 ? (int32_t)sw / 2 : g_curx) - wn->x; *y = (g_cury < 0 ? (int32_t)sh / 2 : g_cury) - wn->y; *w = wn->w; *h = wn->h;
        r = mode == 0 ? 0 : mode == 2 ? 1 : cursor_visible();
        static int last = -1;
        int v = cursor_visible();
        if (v != last) {
            last = v;
            CURLOG("[ui] cursor %s (ShowCursor %d, SetCursor %s, DirectInput mouse %s, ME1_CURSOR %d, frame %u)\n", v ? "shown" : "hidden",
                   g_showcount, g_cursor_null ? "NULL" : "arrow", hle_input_mouse_exclusive() ? "exclusive" : "non-exclusive", mode, hle_d3d9_frames());
        }
    }
    pthread_mutex_unlock(&g_wmx);
    return r;
}
/* on-screen fire / aim buttons: button as in hle_ui_pointer (1 left down, 2 left up, 3 right down, 4 right up) at the
 * current cursor position */
void hle_ui_mouse_button(int button) {
    if (button == 5 || button == 6) {   /* v1.1: middle button down / up */
        pthread_mutex_lock(&g_wmx);
        Win* w = win_get(g_active);
        unsigned sw, sh; screen_size(&sw, &sh);
        int32_t sx = g_curx < 0 ? (int32_t)sw / 2 : g_curx, sy = g_cury < 0 ? (int32_t)sh / 2 : g_cury;
        int32_t cx = sx - (w ? w->x : 0), cy = sy - (w ? w->y : 0);
        vk_set(g_avk, 4, button == 5);
        if (w) mq_push(w->hwnd, button == 5 ? 0x0207u : 0x0208u, mk_buttons(), ((uint32_t)cy << 16) | ((uint32_t)cx & 0xFFFF));
        else vk_set(g_vk, 4, button == 5);
        if (button == 5) g_dibtn |= 4u; else g_dibtn &= ~4u;
        unsigned b = g_dibtn;
        pthread_mutex_unlock(&g_wmx);
        hle_input_mouse(0, 0, 0, b);
        return;
    }
    if (button < 1 || button > 4) return;
    pthread_mutex_lock(&g_wmx);
    Win* w = win_get(g_active);
    unsigned sw, sh; screen_size(&sw, &sh);
    int32_t sx = g_curx < 0 ? (int32_t)sw / 2 : g_curx, sy = g_cury < 0 ? (int32_t)sh / 2 : g_cury;
    int32_t cx = sx - (w ? w->x : 0), cy = sy - (w ? w->y : 0);
    vk_set(g_avk, button <= 2 ? 1 : 2, button == 1 || button == 3);
    if (w) {
        static const uint32_t bm[5] = { 0, 0x0201, 0x0202, 0x0204, 0x0205 };
        mq_push(w->hwnd, bm[button], mk_buttons(), ((uint32_t)cy << 16) | ((uint32_t)cx & 0xFFFF));
    } else vk_set(g_vk, button <= 2 ? 1 : 2, button == 1 || button == 3);
    unsigned m = button <= 2 ? 1u : 2u;
    if (button == 1 || button == 3) g_dibtn |= m; else g_dibtn &= ~m;
    unsigned b = g_dibtn;
    pthread_mutex_unlock(&g_wmx);
    hle_input_mouse(0, 0, 0, b);
}
/* v1.1 desktop: mouse wheel, delta in WHEEL_DELTA units (120 per notch, > 0 = away from the user) */
void hle_ui_wheel(int delta) {
    if (!delta) return;
    pthread_mutex_lock(&g_wmx);
    Win* w = win_get(g_active);
    unsigned sw, sh; screen_size(&sw, &sh);
    int32_t sx = g_curx < 0 ? (int32_t)sw / 2 : g_curx, sy = g_cury < 0 ? (int32_t)sh / 2 : g_cury;
    if (w) mq_push(w->hwnd, 0x020A /* WM_MOUSEWHEEL */, ((uint32_t)(delta & 0xFFFF) << 16) | mk_buttons(), ((uint32_t)sy << 16) | ((uint32_t)sx & 0xFFFF));
    unsigned b = g_dibtn;
    pthread_mutex_unlock(&g_wmx);
    hle_input_mouse(0, 0, delta, b);
}
/* v1.1 desktop: the user closed the window -> WM_CLOSE to the game window (UE3 then asks the engine to exit);
 * returns 0 when there is no window to tell (the front end then stops the process itself) */
int hle_ui_close(void) {
    pthread_mutex_lock(&g_wmx);
    Win* w = win_get(g_active);
    if (w) mq_push(w->hwnd, 0x0010 /* WM_CLOSE */, 0, 0);
    pthread_mutex_unlock(&g_wmx);
    return w != NULL;
}
/* v1.1 desktop: a typed character (text input, already composed by the window system) -> WM_CHAR */
void hle_ui_char(int ch) {
    if (ch <= 0 || ch >= 0x10000) return;
    pthread_mutex_lock(&g_wmx);
    Win* w = win_get(g_active);
    if (w) mq_push(w->hwnd, 0x0102, (uint32_t)ch, 1u);
    pthread_mutex_unlock(&g_wmx);
}
/* key: Windows virtual key + DirectInput scan code (0: none), ch: WM_CHAR character (0: none) */
void hle_ui_key(int vk, int dik, int down, int ch) {
    pthread_mutex_lock(&g_wmx);
    Win* w = win_get(g_active);
    int was = (g_avk[vk & 255] & 0x80) != 0;
    vk_set(g_avk, (unsigned)vk, down);
    if (!w) vk_set(g_vk, (unsigned)vk, down);   /* no window, no message: keep both in step */
    if (w && vk) {
        uint32_t lp = 1u | ((uint32_t)(dik & 255) << 16) | (was ? 1u << 30 : 0) | (down ? 0 : 3u << 30);
        mq_push(w->hwnd, down ? 0x0100u : 0x0101u, (uint32_t)vk, lp);                    /* WM_KEYDOWN / WM_KEYUP */
        if (down && ch) mq_push(w->hwnd, 0x0102, (uint32_t)ch, lp);                       /* WM_CHAR */
    }
    pthread_mutex_unlock(&g_wmx);
    if (dik) hle_input_key(dik, down);
    if (ui_trace() > 1) fprintf(stderr, "[ui] key vk %02x dik %02x %s\n", vk, dik, down ? "down" : "up");
}
static void u_PostMessageW(CPU* c) {
    if (ui_trace() > 1) fprintf(stderr, "[ui] PostMessage(%08x, %04x, %08x, %08x) dropped\n", ARG(0), ARG(1), ARG(2), ARG(3));
    RET(1);
}
static void u_IsIconic(CPU* c) { RET(0); }
static void u_RegisterWindowMessageW(CPU* c) { static uint32_t m = 0xC200; RET(m++); }
static void u_LoadRes(CPU* c) { static uint32_t h = 0x00C10000; RET(h += 0x10); }
static void u_GetSystemMetrics(CPU* c) {
    unsigned w, h, i = ARG(0), r; screen_size(&w, &h);
    switch (i) { case 0: case 16: case 78: r = w; break; case 1: case 17: case 79: r = h; break;
                 case 80: r = 1; break;                                      /* SM_CMONITORS */
                 case 19: case 75: r = 1; break;                             /* v1.1.5: SM_MOUSEPRESENT, SM_MOUSEWHEELPRESENT */
                 case 43: r = 3; break;                                      /* SM_CMOUSEBUTTONS */
                 case 13: case 14: r = 32; break;                            /* SM_CXCURSOR, SM_CYCURSOR */
                 default: r = 0; }
    static uint8_t seen[128];
    if (i < 128 && !seen[i]) { seen[i] = 1; fprintf(stderr, "[ui] GetSystemMetrics(%u) = %u\n", i, r); }
    RET(r);
}
static void u_GetDesktopWindow(CPU* c) { RET(HWND_DESKTOP_); }
static unsigned g_n_curpos, g_n_clicks, g_n_moves; static int32_t g_click_x = -1, g_click_y = -1; static uint32_t g_click_lag;
void hle_ui_brief(char* o, size_t n) {   /* [hb]: how the engine reads the pointer (diagnoses touch problems) */
    pthread_mutex_lock(&g_wmx);
    int k = snprintf(o, n, "ui: %u GetCursorPos, %u moves, %u clicks (last at %d,%d, +%u frames), cursor %s (ShowCursor %d)", g_n_curpos, g_n_moves,
                     g_n_clicks, g_click_x, g_click_y, g_click_lag, cursor_visible() ? "shown" : "hidden", g_showcount);
    pthread_mutex_unlock(&g_wmx);
    if (k > 0 && (size_t)k + 3 < n) { o[k] = ','; o[k + 1] = ' '; hle_xinput_brief(o + k + 2, n - (size_t)k - 2); }
}
static void u_GetCursorPos(CPU* c) {
    unsigned w, h; screen_size(&w, &h);
    __atomic_add_fetch(&g_n_curpos, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_wmx); int32_t x = g_curx < 0 ? (int32_t)w / 2 : g_curx, y = g_cury < 0 ? (int32_t)h / 2 : g_cury; pthread_mutex_unlock(&g_wmx);
    wr32(ARG(0), (uint32_t)x); wr32(ARG(0) + 4, (uint32_t)y); RET(1);
}
static void u_SetCursorPos(CPU* c) {
    pthread_mutex_lock(&g_wmx); g_curx = (int32_t)ARG(0); g_cury = (int32_t)ARG(1); pthread_mutex_unlock(&g_wmx);
    CURLOG("[ui] SetCursorPos(%d, %d) (frame %u)\n", (int)ARG(0), (int)ARG(1), hle_d3d9_frames());
    RET(1);
}
static void u_ShowCursor(CPU* c) {
    pthread_mutex_lock(&g_wmx); int n = g_showcount += ARG(0) ? 1 : -1; pthread_mutex_unlock(&g_wmx);
    CURLOG("[ui] ShowCursor(%s) -> %d (frame %u)\n", ARG(0) ? "TRUE" : "FALSE", n, hle_d3d9_frames());
    RET((uint32_t)n);
}
static void u_SetCursor(CPU* c) {
    pthread_mutex_lock(&g_wmx); int was = g_cursor_null; g_cursor_null = ARG(0) == 0; pthread_mutex_unlock(&g_wmx);
    if (was != (ARG(0) == 0)) CURLOG("[ui] SetCursor(%08x) (frame %u)\n", ARG(0), hle_d3d9_frames());
    RET(was ? 0 : 0x00C10010u);
}
static void u_SetCapture(CPU* c) {
    pthread_mutex_lock(&g_wmx); uint32_t o = g_capture; g_capture = ARG(0); pthread_mutex_unlock(&g_wmx);
    CURLOG("[ui] SetCapture(%08x) (frame %u)\n", ARG(0), hle_d3d9_frames());
    RET(o);
}
static void u_ReleaseCapture(CPU* c) {
    pthread_mutex_lock(&g_wmx); g_capture = 0; pthread_mutex_unlock(&g_wmx);
    CURLOG("[ui] ReleaseCapture (frame %u)\n", hle_d3d9_frames());
    RET(1);
}
static void u_GetCapture(CPU* c) { RET(g_capture); }
static void u_ClipCursor(CPU* c) {   /* called every frame: only changes are logged */
    static int last = -1; int on = ARG(0) != 0;
    if (on != last) { last = on; CURLOG("[ui] ClipCursor(%s) (frame %u)\n", on ? "rect" : "NULL", hle_d3d9_frames()); }
    RET(1);
}
static int msg_match(const QMsg* q, uint32_t hwnd, uint32_t lo, uint32_t hi) {
    if (hwnd && hwnd != 0xFFFFFFFFu && q->hwnd != hwnd) return 0;
    return (!lo && !hi) || (q->msg >= lo && q->msg <= hi);
}
static void u_PeekMessage(CPU* c) {   /* (MSG*, hwnd, min, max, flags): MSG = hwnd, message, wParam, lParam, time, pt */
    uint32_t m = ARG(0), r = 0;
    pthread_mutex_lock(&g_wmx);
    touch_cfg();
    uint32_t fr = hle_d3d9_frames(), now = msg_time();
    for (unsigned i = g_mq_tail; i != g_mq_head; i++) {
        QMsg* q = &g_mq[i % MQ];
        if (!msg_match(q, ARG(1), ARG(2), ARG(3))) continue;
        if (q->msg == 0x0201 || q->msg == 0x0204) {   /* WM_L/RBUTTONDOWN: after the cursor jump was seen */
            if ((fr - q->frame < (uint32_t)g_tp_frames || now - q->time < 25) && now - q->time < 400) break;
        } else if (q->msg == 0x0202 || q->msg == 0x0205) {   /* WM_L/RBUTTONUP: hold the button for a few frames */
            if ((fr - g_down_frame < (uint32_t)g_tp_hold || now - g_down_ms < 40) && now - g_down_ms < 400) break;
        }
        if (ARG(4) & 1) {
            if (q->msg == 0x0201 || q->msg == 0x0204) {
                g_down_frame = fr; g_down_ms = now;
                g_n_clicks++; g_click_x = (int16_t)(q->lp & 0xFFFF); g_click_y = (int16_t)(q->lp >> 16); g_click_lag = fr - q->frame;
            } else if (q->msg == 0x0200) g_n_moves++;
            if (q->msg >= 0x0201 && q->msg <= 0x0205 && ui_trace() > 1)
                fprintf(stderr, "[ui] deliver %04x at frame %u (+%u frames, +%u ms after queued)\n", q->msg, fr, fr - q->frame, now - q->time);
        }
        wr32(m, q->hwnd); wr32(m + 4, q->msg); wr32(m + 8, q->wp); wr32(m + 12, q->lp); wr32(m + 16, q->time);
        wr32(m + 20, (uint32_t)q->x); wr32(m + 24, (uint32_t)q->y); r = 1;
        if (ARG(4) & 1) {   /* PM_REMOVE */
            vk_sync(q->msg, q->wp);
            for (unsigned k = i; k + 1 != g_mq_head; k++) g_mq[k % MQ] = g_mq[(k + 1) % MQ];
            g_mq_head--;
        }
        break;
    }
    pthread_mutex_unlock(&g_wmx);
    RET(r);
}
/* DispatchMessage: the window procedure of msg->hwnd (without the big HLE lock, like SendMessage) */
static void u_DispatchMessage(CPU* c) {
    uint32_t m = ARG(0);
    if (!m) { RET(0); return; }
    uint32_t a[4] = { rd32(m), rd32(m + 4), rd32(m + 8), rd32(m + 12) };
    pthread_mutex_lock(&g_wmx); Win* w = win_get(a[0]); uint32_t proc = w ? w->proc : 0; pthread_mutex_unlock(&g_wmx);
    if (!proc) { RET(0); return; }
    if (ui_trace() > 1) fprintf(stderr, "[ui] DispatchMessage(%08x, %04x, %08x, %08x)\n", a[0], a[1], a[2], a[3]);
    CPU save = *c;
    hle_hb_leave();
    uint32_t r = rt_call(c, proc, 4, a);
    c->ebx = save.ebx; c->esi = save.esi; c->edi = save.edi; c->ebp = save.ebp; c->esp = save.esp;
    RET(r);
}
static void u_GetMessageW(CPU* c) { memset(M + ARG(0), 0, 28); wr32(ARG(0) + 4, 0x12 /* WM_QUIT */); RET(0); }
static void u_SystemParametersInfoW(CPU* c) { RET(1); }
static void u_GetKeyState(CPU* c) {   /* bit 15: down, bit 0: toggled */
    uint32_t vk = ARG(0) & 255; pthread_mutex_lock(&g_wmx);
    uint8_t v = g_vk[vk];
    if (vk == 0x10 || vk == 0x11 || vk == 0x12) v |= g_vk[0xA0 + 2 * (vk - 0x10)] | g_vk[0xA1 + 2 * (vk - 0x10)];   /* shift/ctrl/alt: either side */
    pthread_mutex_unlock(&g_wmx);
    RET((v & 0x80 ? 0xFF80u : 0) | (v & 1));
}
static void u_GetAsyncKeyState(CPU* c) {
    uint32_t vk = ARG(0) & 255; pthread_mutex_lock(&g_wmx);
    uint8_t v = g_avk[vk];
    if (vk == 0x10 || vk == 0x11 || vk == 0x12) v |= g_avk[0xA0 + 2 * (vk - 0x10)] | g_avk[0xA1 + 2 * (vk - 0x10)];
    unsigned tap = g_apress[vk]; g_apress[vk] = 0;
    pthread_mutex_unlock(&g_wmx);
    RET((v & 0x80 ? 0x8000u : 0) | tap);
}
static void u_GetClipCursor(CPU* c) {
    unsigned w, h; screen_size(&w, &h); uint32_t r = ARG(0);
    wr32(r, 0); wr32(r + 4, 0); wr32(r + 8, w); wr32(r + 12, h); RET(1);
}
static void u_wsprintfW(CPU* c) { wr16(ARG(0), 0); RET(0); }   /* cdecl varargs: unformatted (only used for window titles) */
static void u_DialogBoxParamW(CPU* c) { fprintf(stderr, "[hle] DialogBoxParamW(template %x) -> IDCANCEL\n", ARG(1)); RET(2); }
static void g_GetStockObject(CPU* c) { RET(0x00D10000u + ARG(0)); }
static void g_CreateCompatibleDC(CPU* c) { RET(0x00D20001u); }
static void u_GetDC(CPU* c) { RET(0x00D20002u); }   /* a screen DC: only queried (GetDeviceCaps), never drawn to */
static void g_GetDeviceCaps(CPU* c) {
    unsigned w, h; screen_size(&w, &h);
    switch (ARG(1)) {
    case 8: RET(w); break;            /* HORZRES */
    case 10: RET(h); break;           /* VERTRES */
    case 12: RET(32); break;          /* BITSPIXEL */
    case 14: RET(1); break;           /* PLANES */
    case 88: case 90: RET(96); break; /* LOGPIXELSX/Y */
    case 116: RET(60); break;         /* VREFRESH */
    default: RET(0);
    }
}
static void g_GetTextExtentPoint32W(CPU* c) { wr32(ARG(3), 8 * ARG(2)); wr32(ARG(3) + 4, 16); RET(1); }

/* ---------------- ole32 / oleaut32 ---------------- */
static void o_CoCreateInstance(CPU* c) { if (ARG(4)) wr32(ARG(4), 0); RET(0x80040154u); }   /* REGDB_E_CLASSNOTREG */
static void o_CoCreateGuid(CPU* c) {
    static uint64_t s = 0x9E3779B97F4A7C15ull; s ^= (uint64_t)time(NULL);
    for (int i = 0; i < 16; i++) { s = s * 6364136223846793005ull + 1442695040888963407ull; wr8(ARG(0) + i, s >> 56); }
    RET(0);
}
static void o_StringFromGUID2(CPU* c) {
    uint32_t g = ARG(0); char b[64];
    snprintf(b, sizeof b, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rd32(g), rd16(g + 4), rd16(g + 6),
             rd8(g + 8), rd8(g + 9), rd8(g + 10), rd8(g + 11), rd8(g + 12), rd8(g + 13), rd8(g + 14), rd8(g + 15));
    if (ARG(2) < 39) { RET(0); return; }
    put_w(ARG(1), b, 39); RET(39);
}
static void oa_SysAllocString(CPU* c) {
    uint32_t s = ARG(0), n = 0; if (!s) { RET(0); return; }
    while (rd16(s + 2 * n)) n++;
    uint32_t p = ghalloc(4 + 2 * n + 2, 0); wr32(p, 2 * n); memcpy(M + p + 4, M + s, 2 * n + 2); RET(p + 4);
}
static void oa_SysFreeString(CPU* c) { if (ARG(0)) ghfree(ARG(0) - 4); }

/* ---------------- winmm ---------------- */
static void mm_timeGetTime(CPU* c) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); RET(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u); }
static void mm_timeGetDevCaps(CPU* c) { int hle_mm_period(void); wr32(ARG(0), (uint32_t)hle_mm_period()); wr32(ARG(0) + 4, 1000000); RET(0); }
static void mm_nodriver(CPU* c) { RET(6); }   /* MMSYSERR_NODRIVER */

/* ---------------- devices: not present yet ---------------- */
/* Bink video (binkw32.dll, stdcall): movies are skipped until binkw32 is translated */
static void bk_BinkOpen(CPU* c) {
    static int n; char p[200] = "";
    if (ARG(0) && !(ARG(1) & 0x00800000u)) snprintf(p, sizeof p, "%s", GSTR(ARG(0)));   /* not BINKFILEHANDLE */
    if (n++ < 20) fprintf(stderr, "[bink] BinkOpen(\"%s\") -> NULL (video playback not implemented; movie skipped)\n", p);
    RET(0);
}
static void ws_WSAStartup(CPU* c) { RET(10091); }
static void ws_WSAGetLastError(CPU* c) { RET(10050); }
static void ws_inet_addr(CPU* c) { RET(0xFFFFFFFFu); }
static void ws_hton16(CPU* c) { uint32_t v = ARG(0) & 0xffff; RET(((v & 0xff) << 8) | (v >> 8)); }
static void ws_hton32(CPU* c) { RET(__builtin_bswap32(ARG(0))); }

/* ---------------- NxCooking.dll (PhysX 2.7 cooking, cdecl): no runtime cooking yet ---------------- */
/* PhysX 2.7: "const NxCookingParams& NxGetCookingParams()" and "bool NxSetCookingParams(const NxCookingParams&)", cdecl.
 * The params live in one guest block {NxPlatform targetPlatform, NxReal skinWidth, bool hintCollisionSpeed}. Up to v0.9
 * this stub treated ARG(0) as a hidden return pointer - but there is none, so [esp+4] was a stale argument of the
 * preceding NxInitCooking(allocator, ...) call and {0, 0.025f, 0} was written over the game's PhysX allocator object:
 * its vtable became 0 and the next PhysX allocation (createScene) jumped to 00000000. */
static uint32_t g_nx_cook;
static uint32_t nx_cook_params(void) {
    if (!g_nx_cook) {
        float sw = 0.025f; uint32_t u; memcpy(&u, &sw, 4);
        g_nx_cook = ghalloc(16, 1); wr32(g_nx_cook, 0 /* PLATFORM_PC */); wr32(g_nx_cook + 4, u); wr32(g_nx_cook + 8, 0);
    }
    return g_nx_cook;
}
static void nx_GetCookingParams(CPU* c) { RET(nx_cook_params()); }
static void nx_SetCookingParams(CPU* c) {
    uint32_t s = ARG(0), d = nx_cook_params();
    if (s) { wr32(d, rd32(s)); wr32(d + 4, rd32(s + 4)); wr32(d + 8, rd32(s + 8) & 0xff); }
    RET(1);
}

/* ---------------- extra imports of the recompiled d3dx9_31.dll (shader compiler; fonts/resources unused) ---------------- */
static void x_IsBadPtr(CPU* c) { uint32_t p = ARG(0), n = ARG(1); RET(n && (!p || (uint64_t)p + n > 0x100000000ull)); }
static void x_IsBadCodePtr(CPU* c) { RET(ARG(0) == 0); }
static void x_lstrcmpiA(CPU* c) { int r = strcasecmp(ARG(0) ? GSTR(ARG(0)) : "", ARG(1) ? GSTR(ARG(1)) : ""); RET(r < 0 ? (uint32_t)-1 : r > 0); }
static void x_CompareStringA(CPU* c) {   /* (lcid, flags, s1, n1, s2, n2) -> CSTR_LESS_THAN 1 / EQUAL 2 / GREATER 3 */
    uint32_t fl = ARG(1); const char *a = GSTR(ARG(2)), *b = GSTR(ARG(4));
    size_t na = (int32_t)ARG(3) < 0 ? strlen(a) : ARG(3), nb = (int32_t)ARG(5) < 0 ? strlen(b) : ARG(5);
    int r = 0;
    for (size_t i = 0; !r && i < na && i < nb; i++) {
        int x = (uint8_t)a[i], y = (uint8_t)b[i];
        if (fl & 1) { x = tolower(x); y = tolower(y); }   /* NORM_IGNORECASE */
        r = x - y;
    }
    if (!r) r = na < nb ? -1 : na > nb;
    RET(r < 0 ? 1 : r > 0 ? 3 : 2);
}
static void x_RegOpenKeyA(CPU* c) { if (ARG(2)) wr32(ARG(2), 0); RET(2); }   /* Software\Microsoft\Direct3D: not present -> defaults */
static void x_GetTempFileNameA(CPU* c) {   /* (path, prefix, unique, out): name only, the caller creates the file */
    static uint32_t n; uint32_t u = ARG(2) ? ARG(2) & 0xFFFF : (__atomic_add_fetch(&n, 1, __ATOMIC_RELAXED) & 0xFFFF);
    snprintf((char*)M + ARG(3), 260, "%s\\%.3s%X.tmp", GSTR(ARG(0)), GSTR(ARG(1)), u); RET(u);
}
static void x_GetTempFileNameW(CPU* c) {
    static uint32_t n; uint32_t u = ARG(2) ? ARG(2) & 0xFFFF : (__atomic_add_fetch(&n, 1, __ATOMIC_RELAXED) & 0xFFFF) | 0x8000;
    char t[300]; size_t k = 0; uint32_t p = ARG(0), q = ARG(1);
    for (; k < 250 && rd16(p + 2 * k); k++) t[k] = (char)rd16(p + 2 * k);
    t[k++] = '\\'; for (int i = 0; i < 3 && rd16(q + 2 * i); i++) t[k++] = (char)rd16(q + 2 * i);
    k += snprintf(t + k, sizeof t - k, "%X.tmp", u);
    for (size_t i = 0; i <= k; i++) wr16(ARG(3) + 2 * i, i < k ? (uint8_t)t[i] : 0);
    RET(u);
}

#define D(dll, n, a, f) { dll, n, a, f },
const HleDef hle_exe_defs[] = {
#define K(n, a, f) D("kernel32.dll", n, a, f)
    K("GetStartupInfoW", 1, k_GetStartupInfoW) K("GetVersionExW", 1, k_GetVersionExW) K("GetVersion", 0, k_GetVersion)
    K("GetSystemInfo", 1, k_GetSystemInfo) K("GlobalMemoryStatus", 1, k_GlobalMemoryStatus)
    K("GetTimeZoneInformation", 1, k_GetTimeZoneInformation) K("GetComputerNameA", 2, k_GetComputerNameA)
    K("GetComputerNameW", 2, k_GetComputerNameW) K("lstrlenA", 1, k_lstrlenA) K("lstrlenW", 1, k_lstrlenW)
    K("OutputDebugStringA", 1, k_OutputDebugStringA) K("OutputDebugStringW", 1, k_OutputDebugStringW)
    { "kernel32.dll", "LoadLibraryW", 1, k_LoadLibraryW, HLE_NOLOCK },
    K("GlobalAlloc", 2, k_GlobalAlloc) K("GlobalLock", 1, k_GlobalLock) K("GlobalUnlock", 1, r1) K("GlobalFree", 1, k_GlobalFree)
    K("LocalAlloc", 2, k_LocalAlloc) K("LocalFree", 1, k_LocalFree)
    K("FormatMessageA", 7, k_FormatMessage) K("FormatMessageW", 7, k_FormatMessage) K("SetErrorMode", 1, k_SetErrorMode)
    K("CreateProcessA", 10, k_CreateProcess) K("CreateProcessW", 10, k_CreateProcess) K("GetProcessId", 1, k_GetProcessId)
    K("GetExitCodeProcess", 2, k_GetExitCodeProcess) K("CreateNamedPipeW", 8, k_CreateNamedPipeW)
    K("ConnectNamedPipe", 2, r0) K("DisconnectNamedPipe", 1, r0)
    K("SetConsoleCtrlHandler", 2, r1) K("SetConsoleTitleW", 1, r1) K("SetConsoleTextAttribute", 2, r1) K("AllocConsole", 0, r0)
    K("FreeConsole", 0, r1) K("GetConsoleWindow", 0, r0) K("GetConsoleScreenBufferInfo", 2, k_GetConsoleScreenBufferInfo)
    K("SetConsoleWindowInfo", 3, r1) K("SetConsoleScreenBufferSize", 2, r1)
#undef K
#define A(n, a, f) D("advapi32.dll", n, a, f)
    A("RegOpenKeyExW", 5, a_RegOpenKeyExW) A("RegCreateKeyExW", 9, a_RegCreateKeyExW) A("RegSetValueExW", 6, a_RegSetValueExW)
    A("RegQueryValueExW", 6, a_RegQueryValueExW) A("GetUserNameA", 2, a_GetUserNameA) A("GetUserNameW", 2, a_GetUserNameW)
#undef A
#define U(n, a, f) D("user32.dll", n, a, f)
    U("MessageBoxA", 4, u_MessageBoxA) U("RegisterClassA", 1, u_RegisterClassA) U("UnregisterClassA", 2, u_UnregisterClassA)
    U("GetDC", 1, u_GetDC) U("ReleaseDC", 2, r1) U("GetWindow", 2, r0) U("GetClassLongA", 2, r0) U("ChangeDisplaySettingsA", 2, r0)
    D("gdi32.dll", "GetDeviceCaps", 2, g_GetDeviceCaps)
    D("gdi32.dll", "CreateCompatibleBitmap", 3, r0) D("gdi32.dll", "GetDIBits", 7, r0) D("gdi32.dll", "GetPixel", 3, r0)
    D("gdi32.dll", "SetPixel", 4, r0) D("gdi32.dll", "SetStretchBltMode", 2, r0) D("gdi32.dll", "StretchBlt", 11, r0)
    D("winmm.dll", "waveOutOpen", 6, mm_nodriver) D("winmm.dll", "waveOutClose", 1, mm_nodriver) D("winmm.dll", "waveOutPause", 1, mm_nodriver)
    D("winmm.dll", "waveOutRestart", 1, mm_nodriver) D("winmm.dll", "waveOutReset", 1, mm_nodriver) D("winmm.dll", "waveOutSetVolume", 2, mm_nodriver)
    D("winmm.dll", "waveOutPrepareHeader", 3, mm_nodriver) D("winmm.dll", "waveOutUnprepareHeader", 3, mm_nodriver)
    D("winmm.dll", "waveOutWrite", 3, mm_nodriver)
    U("MessageBoxW", 4, u_MessageBoxW) U("RegisterClassW", 1, u_RegisterClassW) U("RegisterClassExW", 1, u_RegisterClassExW)
    U("RegisterClassExA", 1, u_RegisterClassExA) U("UnregisterClassW", 2, u_UnregisterClassW) U("RegisterWindowMessageW", 1, u_RegisterWindowMessageW)
    U("LoadIconW", 2, u_LoadRes) U("LoadIconA", 2, u_LoadRes) U("LoadCursorW", 2, u_LoadRes) U("LoadImageW", 6, u_LoadRes)
    U("GetSystemMetrics", 1, u_GetSystemMetrics) U("GetDesktopWindow", 0, u_GetDesktopWindow) U("GetForegroundWindow", 0, u_GetActive)
    U("GetActiveWindow", 0, u_GetActive) U("GetFocus", 0, u_GetActive) U("GetTopWindow", 1, u_GetTopWindow) U("EnumWindows", 2, r1)
    U("GetCursorPos", 1, u_GetCursorPos) U("SetCursorPos", 2, u_SetCursorPos) U("ClipCursor", 1, u_ClipCursor) U("GetClipCursor", 1, u_GetClipCursor)
    U("ShowCursor", 1, u_ShowCursor) U("SetCursor", 1, u_SetCursor) U("ScreenToClient", 2, u_ScreenToClient) U("ClientToScreen", 2, u_ClientToScreen)
    U("MapWindowPoints", 4, r0) U("GetClientRect", 2, u_GetRect) U("GetWindowRect", 2, u_GetWindowRect) U("AdjustWindowRect", 3, r1)
    U("PeekMessageW", 5, u_PeekMessage) U("PeekMessageA", 5, u_PeekMessage) U("GetMessageW", 4, u_GetMessageW)
    U("TranslateMessage", 1, r0)
    { "user32.dll", "DispatchMessageW", 1, u_DispatchMessage, HLE_NOLOCK }, { "user32.dll", "DispatchMessageA", 1, u_DispatchMessage, HLE_NOLOCK }, U("PostQuitMessage", 1, r0)
    U("PostMessageW", 4, u_PostMessageW) { "user32.dll", "SendMessageW", 4, u_SendMessage, HLE_NOLOCK },
    U("SendMessageTimeoutW", 7, u_SendMessageTimeoutW)
    U("SystemParametersInfoW", 4, u_SystemParametersInfoW) U("GetKeyState", 1, u_GetKeyState) U("GetAsyncKeyState", 1, u_GetAsyncKeyState) U("MapVirtualKeyW", 2, r0)
    U("CreateWindowExW", 12, u_CreateWindowExW) U("CreateWindowExA", 12, u_CreateWindowExA) U("DestroyWindow", 1, u_DestroyWindow)
    U("ShowWindow", 2, u_ShowWindow) U("UpdateWindow", 1, r1) U("SetWindowPos", 7, u_SetWindowPos)
    U("SetWindowLongW", 3, u_SetWindowLong) U("SetWindowLongA", 3, u_SetWindowLong)
    U("GetWindowLongW", 2, u_GetWindowLong) U("GetWindowLongA", 2, u_GetWindowLong) U("IsWindow", 1, u_IsWindow)
    U("IsWindowVisible", 1, u_IsWindowVisible) U("GetParent", 1, u_GetParent)
    U("SetWindowTextW", 2, u_SetWindowTextW) U("SetForegroundWindow", 1, u_SetForegroundWindow) U("SetActiveWindow", 1, u_SetActiveWindow)
    U("SetFocus", 1, u_SetFocus) U("SetCapture", 1, u_SetCapture) U("ReleaseCapture", 0, u_ReleaseCapture) U("GetCapture", 0, u_GetCapture) U("IsIconic", 1, u_IsIconic) U("InvalidateRect", 3, r1) U("RedrawWindow", 4, r1)
    U("BeginPaint", 2, r0) U("EndPaint", 2, r1) U("DrawStateW", 10, r1) U("GetWindowThreadProcessId", 2, u_GetWindowThreadProcessId)
    U("DefWindowProcW", 4, r0) U("DefWindowProcA", 4, r0) U("DialogBoxParamW", 5, u_DialogBoxParamW) U("EndDialog", 2, r1)
    U("GetDlgItem", 2, r0) U("SetDlgItemTextW", 3, r1) U("RegisterHotKey", 4, r1) U("UnregisterHotKey", 2, r1)
    U("OpenClipboard", 1, r0) U("CloseClipboard", 0, r1) U("EmptyClipboard", 0, r1) U("GetClipboardData", 1, r0)
    U("SetClipboardData", 2, r0) U("wsprintfW", 0, u_wsprintfW)
#undef U
    D("gdi32.dll", "GetStockObject", 1, g_GetStockObject) D("gdi32.dll", "CreateCompatibleDC", 1, g_CreateCompatibleDC)
    D("gdi32.dll", "DeleteDC", 1, r1) D("gdi32.dll", "DeleteObject", 1, r1) D("gdi32.dll", "GetObjectW", 3, r0)
    D("gdi32.dll", "GetTextExtentPoint32W", 4, g_GetTextExtentPoint32W)
    D("ole32.dll", "CoInitialize", 1, r0) D("ole32.dll", "CoInitializeEx", 2, r0) D("ole32.dll", "CoUninitialize", 0, r0)
    D("ole32.dll", "CoCreateInstance", 5, o_CoCreateInstance) D("ole32.dll", "CoCreateGuid", 1, o_CoCreateGuid)
    D("ole32.dll", "StringFromGUID2", 3, o_StringFromGUID2) D("ole32.dll", "CoSetProxyBlanket", 8, r0)
    D("oleaut32.dll", "#2", 1, oa_SysAllocString) D("oleaut32.dll", "#6", 1, oa_SysFreeString)
    D("winmm.dll", "timeBeginPeriod", 1, r0) D("winmm.dll", "timeEndPeriod", 1, r0) { "winmm.dll", "timeGetTime", 0, mm_timeGetTime, HLE_NOLOCK },
    D("winmm.dll", "timeGetDevCaps", 2, mm_timeGetDevCaps)
    D("winmm.dll", "midiOutOpen", 5, mm_nodriver) D("winmm.dll", "midiOutClose", 1, mm_nodriver) D("winmm.dll", "midiOutReset", 1, mm_nodriver)
    D("winmm.dll", "midiOutShortMsg", 2, mm_nodriver) D("winmm.dll", "midiOutLongMsg", 3, mm_nodriver)
    D("dbghelp.dll", "SymInitialize", 3, r0) D("dbghelp.dll", "SymSetOptions", 1, r0) D("dbghelp.dll", "SymGetOptions", 0, r0)
    D("dbghelp.dll", "SymLoadModule64", 6, r0) D("dbghelp.dll", "SymGetModuleBase64", 3, r0) D("dbghelp.dll", "SymFunctionTableAccess64", 3, r0)
    D("dbghelp.dll", "SymGetModuleInfo64", 4, r0) D("dbghelp.dll", "SymGetSymFromAddr64", 5, r0) D("dbghelp.dll", "SymGetLineFromAddr64", 5, r0)
    D("dbghelp.dll", "StackWalk64", 9, r0) D("dbghelp.dll", "MiniDumpWriteDump", 7, r0)
    D("wintrust.dll", "WinVerifyTrust", 3, r0)
#define G(n, a) D("gdi32.dll", n, a, r0)
    G("SetMapMode", 2) G("GetOutlineTextMetricsA", 3) G("GetGlyphOutlineW", 7) G("GetGlyphOutlineA", 7) G("MoveToEx", 4)
    G("CreateFontIndirectW", 1) G("CreateFontIndirectA", 1) G("SetTextAlign", 2) G("GetFontLanguageInfo", 1)
    G("GetTextMetricsW", 2) G("GetTextMetricsA", 2) G("SetBkMode", 2) G("SetBkColor", 2) G("SetTextColor", 2)
    G("GetCharacterPlacementW", 6) G("GetCharacterPlacementA", 6) G("SelectObject", 2) G("CreateDIBSection", 6) G("GetObjectA", 3)
    G("TranslateCharsetInfo", 3) G("ExtTextOutW", 8) G("ExtTextOutA", 8) G("GetCurrentObject", 2)
#undef G
    D("kernel32.dll", "IsBadReadPtr", 2, x_IsBadPtr) D("kernel32.dll", "IsBadWritePtr", 2, x_IsBadPtr)
    D("kernel32.dll", "IsBadCodePtr", 1, x_IsBadCodePtr) D("kernel32.dll", "lstrcmpiA", 2, x_lstrcmpiA)
    D("kernel32.dll", "CompareStringA", 6, x_CompareStringA) D("kernel32.dll", "IsDBCSLeadByte", 1, r0)
    D("kernel32.dll", "FindResourceA", 3, r0) D("kernel32.dll", "FindResourceW", 3, r0) D("kernel32.dll", "LoadResource", 2, r0)
    D("kernel32.dll", "LockResource", 1, r0) D("kernel32.dll", "SizeofResource", 2, r0) D("kernel32.dll", "FreeResource", 1, r0)
    D("kernel32.dll", "CreateFileMappingA", 6, r0) D("kernel32.dll", "GetTempFileNameA", 4, x_GetTempFileNameA)
    D("kernel32.dll", "GetTempFileNameW", 4, x_GetTempFileNameW) D("advapi32.dll", "RegOpenKeyA", 3, x_RegOpenKeyA)
    D("NxCooking.dll", "NxInitCooking", 0, r1) D("NxCooking.dll", "NxCloseCooking", 0, r0)
    D("NxCooking.dll", "NxSetCookingParams", 0, nx_SetCookingParams) D("NxCooking.dll", "NxGetCookingParams", 0, nx_GetCookingParams)
    D("NxCooking.dll", "NxPlatformMismatch", 0, r0) D("NxCooking.dll", "NxCookTriangleMesh", 0, r0)
    D("NxCooking.dll", "NxCookConvexMesh", 0, r0) D("NxCooking.dll", "NxCookClothMesh", 0, r0)
    /* d3d9.dll / d3dx9_31.dll: hle_d3d9.c */
#define B(n, a, f) D("binkw32.dll", n, a, f)
    B("_BinkOpen@8", 2, bk_BinkOpen) B("_BinkSetSoundSystem@8", 2, r1) B("_BinkOpenDirectSound@4", 1, r0) B("_BinkGetRects@8", 2, r0)
    B("_BinkSetSoundTrack@8", 2, r0) B("_BinkGoto@12", 3, r0) B("_BinkGetSummary@8", 2, r0) B("_BinkGetKeyFrame@12", 3, r0)
    B("_BinkGetRealtime@12", 3, r0) B("_BinkGetTrackID@8", 2, r0) B("_BinkSetVolume@12", 3, r0) B("_BinkClose@4", 1, r0)
    B("_BinkWait@4", 1, r0) B("_BinkRegisterFrameBuffers@8", 2, r0) B("_BinkDoFrame@4", 1, r0) B("_BinkShouldSkip@4", 1, r0)
    B("_BinkNextFrame@4", 1, r0) B("_BinkGetFrameBuffersInfo@8", 2, r0) B("_BinkPause@8", 2, r0)
#undef B
    /* OpenAL32.dll: hle_al.c */
    /* dinput8.dll: hle_input.c */
    /* dsound.dll #1 DirectSoundCreate / #11 DirectSoundCreate8: hle_al.c */ D("dsound.dll", "#3", 2, r0)

    /* wsock32 ordinals (wsock32.def): 1 accept 2 bind 3 closesocket 4 connect 6 getsockname 7 getsockopt 8 htonl 9 htons
     * 10 inet_addr 12 ioctlsocket 13 listen 14 ntohl 15 ntohs 16 recv 17 recvfrom 18 select 19 send 20 sendto
     * 21 setsockopt 23 socket 52 gethostbyname 57 gethostname 111 WSAGetLastError 115 WSAStartup 116 WSACleanup */
    D("wsock32.dll", "#1", 3, rneg1) D("wsock32.dll", "#2", 3, rneg1) D("wsock32.dll", "#3", 1, r0) D("wsock32.dll", "#4", 3, rneg1)
    D("wsock32.dll", "#6", 3, rneg1) D("wsock32.dll", "#7", 5, rneg1) D("wsock32.dll", "#8", 1, ws_hton32) D("wsock32.dll", "#9", 1, ws_hton16)
    D("wsock32.dll", "#10", 1, ws_inet_addr) D("wsock32.dll", "#12", 3, rneg1) D("wsock32.dll", "#13", 2, rneg1)
    D("wsock32.dll", "#14", 1, ws_hton32) D("wsock32.dll", "#15", 1, ws_hton16) D("wsock32.dll", "#16", 4, rneg1)
    D("wsock32.dll", "#17", 6, rneg1) D("wsock32.dll", "#18", 5, rneg1) D("wsock32.dll", "#19", 4, rneg1) D("wsock32.dll", "#20", 6, rneg1)
    D("wsock32.dll", "#21", 5, rneg1) D("wsock32.dll", "#23", 3, rneg1) D("wsock32.dll", "#52", 1, r0) D("wsock32.dll", "#57", 2, rneg1)
    D("wsock32.dll", "#111", 0, ws_WSAGetLastError) D("wsock32.dll", "#115", 2, ws_WSAStartup) D("wsock32.dll", "#116", 0, r0)
};
const unsigned hle_exe_ndefs = sizeof hle_exe_defs / sizeof hle_exe_defs[0];
