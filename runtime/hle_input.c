/* DirectInput 8 (dinput8.dll): IDirectInput8 + system mouse/keyboard devices. MassEffect.exe creates them right after
 * the window (FWindowsViewport input init) and does not check DirectInput8Create's result, so "no DirectInput" is not
 * an option. Devices are always attached and acquired; their state comes from hle_input_key/hle_input_mouse (the
 * Android front end will feed touch/keyboard events there): immediate state (GetDeviceState) plus a buffered event
 * ring (GetDeviceData, DIPROP_BUFFERSIZE). Game controllers: none enumerated (the engine uses XInput for pads).
 * ME1_TRACE_INPUT=1 logs the device calls.
 * XInput 1.3 (xinput1_3.dll, imported by ordinal: #2 XInputGetState, #3 XInputSetState): one virtual pad (user 0) fed
 * by hle_xinput_set (physical Android gamepad or the on-screen pad). The engine polls users 0..3 every frame
 * (UWindowsClient JoystickInfo) and reads XboxTypeS_* keys - vanilla DefaultInput.ini has no bindings for them, the
 * ME1Controller mod (BIOInput.ini) does. ME1_XINPUT: 0 = no pad, 1 = pad 0 always connected, unset/2 = connected
 * from the first hle_xinput_set on. Rumble (XInputSetState) is read back by hle_xinput_vibration. */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define DI_OK 0u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOINTERFACE 0x80004002u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_NOTINITIALIZED 0x80070015u

enum { DEV_MOUSE = 1, DEV_KEYBOARD = 2 };
#define RING 256
typedef struct { uint32_t ofs, data, time, seq; } Ev;
typedef struct {
    int type, refs, acquired; uint32_t bufsize, coop, hevent;
    uint32_t unacq_ms;                                         /* v1.1.5: time of the last Unacquire */
    uint8_t keys[256]; int32_t mx, my, mz; uint8_t mbtn[8];   /* immediate state; mouse axes are relative deltas */
    Ev ring[RING]; unsigned head, tail;                        /* buffered data */
} Dev;
static Dev g_dev[3];
static uint32_t g_vt_di, g_vt_dev, g_obj_di, g_obj_dev[3], g_seq;
static int g_trace = -1;
static pthread_mutex_t g_imx = PTHREAD_MUTEX_INITIALIZER;   /* the front end feeds events from its own thread */
#define TR(...) do { if (g_trace) fprintf(stderr, "[dinput] " __VA_ARGS__); } while (0)

static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static Dev* dev_of(uint32_t self) { for (int i = 1; i < 3; i++) if (g_obj_dev[i] == self) return &g_dev[i]; return NULL; }
static void push_ev(Dev* d, uint32_t ofs, uint32_t data) {
    if (!d->bufsize || !d->acquired) return;
    unsigned n = d->head - d->tail; if (n >= RING || n >= d->bufsize) d->tail++;   /* overflow: drop the oldest */
    Ev* e = &d->ring[d->head++ % RING]; e->ofs = ofs; e->data = data; e->time = now_ms(); e->seq = ++g_seq;
}
/* front end API: DIK_* scan code; mouse deltas in mickeys + button mask (bit i = button i) */
void hle_input_key(int dik, int down) {
    if (dik < 0 || dik > 255) return;
    pthread_mutex_lock(&g_imx); Dev* d = &g_dev[DEV_KEYBOARD];
    uint8_t v = down ? 0x80 : 0; if (d->keys[dik] != v) { d->keys[dik] = v; push_ev(d, (uint32_t)dik, v); }
    pthread_mutex_unlock(&g_imx);
}
void hle_input_mouse(int dx, int dy, int dz, unsigned buttons) {
    pthread_mutex_lock(&g_imx); Dev* d = &g_dev[DEV_MOUSE];
    d->mx += dx; d->my += dy; d->mz += dz;
    if (dx) push_ev(d, 0, (uint32_t)dx);
    if (dy) push_ev(d, 4, (uint32_t)dy);
    if (dz) push_ev(d, 8, (uint32_t)dz);
    for (int b = 0; b < 8; b++) { uint8_t v = (buttons >> b & 1) ? 0x80 : 0; if (d->mbtn[b] != v) { d->mbtn[b] = v; push_ev(d, 12 + b, v); } }
    pthread_mutex_unlock(&g_imx);
}

/* ---- XInput ---- */
static struct { int connected; uint32_t packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; uint16_t vl, vr; uint32_t polls, sets; } g_xi;
static int g_xi_mode = -1;
static int xi_mode(void) {
    if (g_xi_mode < 0) { const char* e = getenv("ME1_XINPUT"); g_xi_mode = e && *e ? atoi(e) : 2; if (g_xi_mode == 1) g_xi.connected = 1; }
    return g_xi_mode;
}
static int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
void hle_xinput_set(unsigned buttons, int lt, int rt, int lx, int ly, int rx, int ry) {
    pthread_mutex_lock(&g_imx);
    if (xi_mode() && !g_xi.connected) { g_xi.connected = 1; fprintf(stderr, "[xinput] gamepad 0 connected\n"); }
    uint8_t l = (uint8_t)(lt < 0 ? 0 : lt > 255 ? 255 : lt), r = (uint8_t)(rt < 0 ? 0 : rt > 255 ? 255 : rt);
    int16_t a = clamp16(lx), b = clamp16(ly), x = clamp16(rx), y = clamp16(ry);
    if (g_xi.buttons != (uint16_t)buttons || g_xi.lt != l || g_xi.rt != r || g_xi.lx != a || g_xi.ly != b || g_xi.rx != x || g_xi.ry != y) {
        g_xi.buttons = (uint16_t)buttons; g_xi.lt = l; g_xi.rt = r; g_xi.lx = a; g_xi.ly = b; g_xi.rx = x; g_xi.ry = y; g_xi.packet++;
    }
    pthread_mutex_unlock(&g_imx);
}
void hle_xinput_connect(int on) {
    pthread_mutex_lock(&g_imx);
    if (xi_mode()) { if (g_xi.connected != !!on) fprintf(stderr, "[xinput] gamepad 0 %s\n", on ? "connected" : "disconnected"); g_xi.connected = !!on; }
    if (!on) { g_xi.buttons = 0; g_xi.lt = g_xi.rt = 0; g_xi.lx = g_xi.ly = g_xi.rx = g_xi.ry = 0; g_xi.vl = g_xi.vr = 0; g_xi.packet++; }
    pthread_mutex_unlock(&g_imx);
}
int hle_xinput_vibration(void) {   /* (left motor << 16) | right motor, 0..65535 each */
    pthread_mutex_lock(&g_imx); int v = (int)((uint32_t)g_xi.vl << 16 | g_xi.vr); pthread_mutex_unlock(&g_imx); return v;
}
void hle_xinput_brief(char* out, size_t cap) {
    snprintf(out, cap, "pad %s polls %u rumble-sets %u", g_xi.connected ? "on" : "off", g_xi.polls, g_xi.sets);
}
static void xi_GetState(CPU* c) {   /* (dwUserIndex, XINPUT_STATE*) */
    uint32_t u = ARG(0), p = ARG(1);
    pthread_mutex_lock(&g_imx);
    int ok = xi_mode() && g_xi.connected && u == 0 && p;
    g_xi.polls++;
    if (ok) {
        wr32(p, g_xi.packet); wr16(p + 4, g_xi.buttons); M[p + 6] = g_xi.lt; M[p + 7] = g_xi.rt;
        wr16(p + 8, (uint16_t)g_xi.lx); wr16(p + 10, (uint16_t)g_xi.ly); wr16(p + 12, (uint16_t)g_xi.rx); wr16(p + 14, (uint16_t)g_xi.ry);
    }
    pthread_mutex_unlock(&g_imx);
    RET(ok ? 0 : 1167);   /* ERROR_DEVICE_NOT_CONNECTED */
}
static void xi_SetState(CPU* c) {   /* (dwUserIndex, XINPUT_VIBRATION*) */
    uint32_t u = ARG(0), p = ARG(1);
    pthread_mutex_lock(&g_imx);
    int ok = xi_mode() && g_xi.connected && u == 0;
    if (ok && p) { g_xi.vl = rd16(p); g_xi.vr = rd16(p + 2); g_xi.sets++; }
    pthread_mutex_unlock(&g_imx);
    RET(ok ? 0 : 1167);
}

static void build(void) {
    if (g_vt_di) return;
    static const char* const di[] = { "QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices", "GetDeviceStatus",
        "RunControlPanel", "Initialize", "FindDevice", "EnumDevicesBySemantics", "ConfigureDevices" };
    static const char* const dv[] = { "QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects", "GetProperty",
        "SetProperty", "Acquire", "Unacquire", "GetDeviceState", "GetDeviceData", "SetDataFormat", "SetEventNotification",
        "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize", "CreateEffect", "EnumEffects",
        "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand", "EnumCreatedEffectObjects", "Escape", "Poll",
        "SendDeviceData", "EnumEffectsInFile", "WriteEffectToFile", "BuildActionMap", "SetActionMap", "GetImageInfo" };
    char n[96];
    g_vt_di = ghalloc(4 * 11, 1);
    for (int i = 0; i < 11; i++) { snprintf(n, sizeof n, "IDirectInput8::%s", di[i]); wr32(g_vt_di + 4 * i, hle_resolve("dinput8.com", n)); }
    g_vt_dev = ghalloc(4 * 32, 1);
    for (int i = 0; i < 32; i++) { snprintf(n, sizeof n, "IDirectInputDevice8::%s", dv[i]); wr32(g_vt_dev + 4 * i, hle_resolve("dinput8.com", n)); }
    g_obj_di = ghalloc(16, 1); wr32(g_obj_di, g_vt_di);
}
static void di_Create(CPU* c) {   /* (hinst, version, riid, ppvOut, punkOuter) */
    if (g_trace < 0) { const char* e = getenv("ME1_TRACE_INPUT"); g_trace = e && atoi(e); }
    build();
    if (ARG(3)) wr32(ARG(3), g_obj_di);
    fprintf(stderr, "[dinput] DirectInput8Create(version %x) -> %08x (system mouse + keyboard, no game controllers)\n", ARG(1), g_obj_di);
    RET(DI_OK);
}
#define SELF ARG(0)
static void qi(CPU* c) { if (ARG(2)) wr32(ARG(2), SELF); RET(DI_OK); }   /* same object for IDirectInput8A/W etc. */
static void addref(CPU* c) { Dev* d = dev_of(SELF); RET(d ? ++d->refs : 2); }
static void release(CPU* c) { Dev* d = dev_of(SELF); RET(d ? (d->refs > 1 ? --d->refs : (d->refs = 0)) : 1); }
static void di_CreateDevice(CPU* c) {   /* (this, rguid, ppDevice, punkOuter) */
    uint32_t g = ARG(1) ? rd32(ARG(1)) : 0; int t = g == 0x6F1D2B60u ? DEV_MOUSE : g == 0x6F1D2B61u ? DEV_KEYBOARD : 0;
    if (!t) { TR("CreateDevice(guid %08x) -> DEVICENOTREG\n", g); if (ARG(2)) wr32(ARG(2), 0); RET(DIERR_DEVICENOTREG); return; }
    if (!g_obj_dev[t]) { g_obj_dev[t] = ghalloc(16, 1); wr32(g_obj_dev[t], g_vt_dev); }
    pthread_mutex_lock(&g_imx); Dev* d = &g_dev[t]; d->type = t; d->refs++; pthread_mutex_unlock(&g_imx);
    if (ARG(2)) wr32(ARG(2), g_obj_dev[t]);
    fprintf(stderr, "[dinput] CreateDevice(%s) -> %08x\n", t == DEV_MOUSE ? "GUID_SysMouse" : "GUID_SysKeyboard", g_obj_dev[t]);
    RET(DI_OK);
}
static void di_EnumDevices(CPU* c) { TR("EnumDevices(type %x, flags %x): none enumerated\n", ARG(1), ARG(4)); RET(DI_OK); }
static void di_GetDeviceStatus(CPU* c) { uint32_t g = ARG(1) ? rd32(ARG(1)) : 0; RET(g == 0x6F1D2B60u || g == 0x6F1D2B61u ? DI_OK : DIERR_DEVICENOTREG); }
static void r_ok(CPU* c) { RET(DI_OK); }
static void r_unsup(CPU* c) { RET(DIERR_UNSUPPORTED); }

static void dv_GetCapabilities(CPU* c) {
    Dev* d = dev_of(SELF); uint32_t p = ARG(1); if (!d || !p) { RET(DIERR_INVALIDPARAM); return; }
    uint32_t sz = rd32(p); if (sz < 24) { RET(DIERR_INVALIDPARAM); return; }
    memset(M + p + 4, 0, sz - 4);
    wr32(p + 4, 1 | 0);   /* DIDC_ATTACHED */
    if (d->type == DEV_MOUSE) { wr32(p + 8, 0x112); wr32(p + 12, 3); wr32(p + 16, 8); }   /* DI8DEVTYPE_MOUSE, 3 axes, 8 buttons */
    else { wr32(p + 8, 0x413); wr32(p + 12, 0); wr32(p + 16, 128); }                        /* DI8DEVTYPE_KEYBOARD PCENH */
    RET(DI_OK);
}
static void dv_GetProperty(CPU* c) {   /* (this, rguidProp, pdiph) */
    Dev* d = dev_of(SELF); uint32_t prop = ARG(1), p = ARG(2);
    if (d && p && prop == 1) { wr32(p + 16, d->bufsize); RET(DI_OK); return; }   /* DIPROP_BUFFERSIZE: DIPROPDWORD.dwData */
    RET(DIERR_UNSUPPORTED);
}
static void dv_SetProperty(CPU* c) {
    Dev* d = dev_of(SELF); uint32_t prop = ARG(1), p = ARG(2);
    TR("SetProperty(%s, prop %u, value %u)\n", d && d->type == DEV_MOUSE ? "mouse" : "keyboard", prop, p ? rd32(p + 16) : 0);
    if (d && p && prop == 1) { pthread_mutex_lock(&g_imx); d->bufsize = rd32(p + 16); if (d->bufsize > RING) d->bufsize = RING; pthread_mutex_unlock(&g_imx); }
    RET(DI_OK);   /* DIPROP_AXISMODE (2), granularity, ...: accepted */
}
static void dv_Acquire(CPU* c) { Dev* d = dev_of(SELF); if (d) { if (!d->acquired) TR("Acquire(%s)\n", d->type == DEV_MOUSE ? "mouse" : "keyboard"); d->acquired = 1; } RET(DI_OK); }
static void dv_Unacquire(CPU* c) { Dev* d = dev_of(SELF); if (d) { d->acquired = 0; d->unacq_ms = now_ms(); } RET(DI_OK); }
/* v1.1.5: Windows hides the cursor while a mouse is acquired in exclusive mode (DISCL_EXCLUSIVE). A short Unacquire
 * (re-acquire with another cooperative level, every frame in UE3) does not count. Read without g_imx: called by
 * hle_exe.c with its window lock held, only plain ints are read. */
int hle_input_mouse_exclusive(void) {
    for (int i = 1; i < 3; i++) {
        Dev* d = &g_dev[i];
        if (d->type == DEV_MOUSE && (d->coop & 1) && (d->acquired || now_ms() - d->unacq_ms < 100)) return 1;
    }
    return 0;
}
static void dv_GetDeviceState(CPU* c) {   /* (this, cbData, lpvData) */
    Dev* d = dev_of(SELF); uint32_t n = ARG(1), p = ARG(2);
    if (!d || !p) { RET(DIERR_INVALIDPARAM); return; }
    pthread_mutex_lock(&g_imx);
    memset(M + p, 0, n);
    if (d->type == DEV_KEYBOARD) memcpy(M + p, d->keys, n < 256 ? n : 256);
    else if (n >= 16) {   /* DIMOUSESTATE (16) / DIMOUSESTATE2 (20) */
        wr32(p, (uint32_t)d->mx); wr32(p + 4, (uint32_t)d->my); wr32(p + 8, (uint32_t)d->mz); d->mx = d->my = d->mz = 0;
        memcpy(M + p + 12, d->mbtn, n >= 20 ? 8 : 4);
    }
    pthread_mutex_unlock(&g_imx);
    RET(DI_OK);
}
static void dv_GetDeviceData(CPU* c) {   /* (this, cbObjectData, rgdod, pdwInOut, dwFlags) */
    Dev* d = dev_of(SELF); uint32_t cb = ARG(1), out = ARG(2), pn = ARG(3), fl = ARG(4);
    if (!d || !pn) { RET(DIERR_INVALIDPARAM); return; }
    pthread_mutex_lock(&g_imx);
    uint32_t want = rd32(pn), got = 0;
    while (d->tail != d->head && got < want) {
        Ev* e = &d->ring[d->tail % RING];
        if (out) { uint32_t q = out + got * cb; memset(M + q, 0, cb); wr32(q, e->ofs); wr32(q + 4, e->data); wr32(q + 8, e->time); wr32(q + 12, e->seq); }
        if (!(fl & 1)) d->tail++;   /* DIGDD_PEEK */
        got++;
        if (fl & 1) break;
    }
    if (!out && want == 0xFFFFFFFFu) d->tail = d->head;   /* flush */
    pthread_mutex_unlock(&g_imx);
    wr32(pn, got);
    RET(DI_OK);
}
static void dv_SetDataFormat(CPU* c) { Dev* d = dev_of(SELF); TR("SetDataFormat(%s, %u bytes)\n", d && d->type == DEV_MOUSE ? "mouse" : "keyboard", ARG(1) ? rd32(ARG(1) + 12) : 0); RET(DI_OK); }
static void dv_SetEventNotification(CPU* c) { Dev* d = dev_of(SELF); if (d) d->hevent = ARG(1); RET(DI_OK); }
static void dv_SetCooperativeLevel(CPU* c) {
    Dev* d = dev_of(SELF);
    if (d && d->type == DEV_MOUSE && d->coop != ARG(2)) {   /* v1.1.5: exclusive (gameplay) / non-exclusive (cursor) - changes are logged */
        static unsigned n; if (n++ < 200) fprintf(stderr, "[dinput] mouse cooperative level %x -> %x (%s)\n", d->coop, ARG(2), ARG(2) & 1 ? "exclusive" : "non-exclusive");
    }
    if (d) d->coop = ARG(2); TR("SetCooperativeLevel(%s, %x)\n", d && d->type == DEV_MOUSE ? "mouse" : "keyboard", ARG(2)); RET(DI_OK); }
static void dv_GetDeviceInfo(CPU* c) {   /* DIDEVICEINSTANCEW: size, guidInstance, guidProduct, dwDevType, tszInstanceName[260], tszProductName[260], ... */
    Dev* d = dev_of(SELF); uint32_t p = ARG(1); if (!d || !p) { RET(DIERR_INVALIDPARAM); return; }
    uint32_t sz = rd32(p); if (sz < 44) { RET(DIERR_INVALIDPARAM); return; }
    memset(M + p + 4, 0, sz - 4);
    uint32_t g = d->type == DEV_MOUSE ? 0x6F1D2B60u : 0x6F1D2B61u;
    wr32(p + 4, g); wr32(p + 20, g); wr32(p + 36, d->type == DEV_MOUSE ? 0x112 : 0x413);
    const char* nm = d->type == DEV_MOUSE ? "Mouse" : "Keyboard";
    if (sz >= 40 + 520) for (int i = 0; nm[i]; i++) { wr16(p + 40 + 2 * i, (uint8_t)nm[i]); wr16(p + 40 + 520 + 2 * i, (uint8_t)nm[i]); }
    RET(DI_OK);
}
static void dv_GetObjectInfo(CPU* c) { RET(DIERR_UNSUPPORTED); }

#define D(n, a, f) { "dinput8.com", n, a, f, 0 },
const HleDef hle_input_defs[] = {
    { "xinput1_3.dll", "#2", 2, xi_GetState, 0 }, { "xinput1_3.dll", "#3", 2, xi_SetState, 0 },
    { "xinput1_3.dll", "XInputGetState", 2, xi_GetState, 0 }, { "xinput1_3.dll", "XInputSetState", 2, xi_SetState, 0 },
    { "dinput8.dll", "DirectInput8Create", 5, di_Create, 0 },
    D("IDirectInput8::QueryInterface", 3, qi) D("IDirectInput8::AddRef", 1, addref) D("IDirectInput8::Release", 1, release)
    D("IDirectInput8::CreateDevice", 4, di_CreateDevice) D("IDirectInput8::EnumDevices", 5, di_EnumDevices)
    D("IDirectInput8::GetDeviceStatus", 2, di_GetDeviceStatus) D("IDirectInput8::RunControlPanel", 3, r_ok)
    D("IDirectInput8::Initialize", 3, r_ok) D("IDirectInput8::FindDevice", 4, r_unsup)
    D("IDirectInput8::EnumDevicesBySemantics", 6, r_ok) D("IDirectInput8::ConfigureDevices", 5, r_unsup)
    D("IDirectInputDevice8::QueryInterface", 3, qi) D("IDirectInputDevice8::AddRef", 1, addref) D("IDirectInputDevice8::Release", 1, release)
    D("IDirectInputDevice8::GetCapabilities", 2, dv_GetCapabilities) D("IDirectInputDevice8::EnumObjects", 4, r_ok)
    D("IDirectInputDevice8::GetProperty", 3, dv_GetProperty) D("IDirectInputDevice8::SetProperty", 3, dv_SetProperty)
    D("IDirectInputDevice8::Acquire", 1, dv_Acquire) D("IDirectInputDevice8::Unacquire", 1, dv_Unacquire)
    D("IDirectInputDevice8::GetDeviceState", 3, dv_GetDeviceState) D("IDirectInputDevice8::GetDeviceData", 5, dv_GetDeviceData)
    D("IDirectInputDevice8::SetDataFormat", 2, dv_SetDataFormat) D("IDirectInputDevice8::SetEventNotification", 2, dv_SetEventNotification)
    D("IDirectInputDevice8::SetCooperativeLevel", 3, dv_SetCooperativeLevel) D("IDirectInputDevice8::GetObjectInfo", 4, dv_GetObjectInfo)
    D("IDirectInputDevice8::GetDeviceInfo", 2, dv_GetDeviceInfo) D("IDirectInputDevice8::RunControlPanel", 3, r_ok)
    D("IDirectInputDevice8::Initialize", 4, r_ok) D("IDirectInputDevice8::CreateEffect", 5, r_unsup)
    D("IDirectInputDevice8::EnumEffects", 4, r_ok) D("IDirectInputDevice8::GetEffectInfo", 3, r_unsup)
    D("IDirectInputDevice8::GetForceFeedbackState", 2, r_unsup) D("IDirectInputDevice8::SendForceFeedbackCommand", 2, r_unsup)
    D("IDirectInputDevice8::EnumCreatedEffectObjects", 4, r_ok) D("IDirectInputDevice8::Escape", 2, r_unsup)
    D("IDirectInputDevice8::Poll", 1, r_ok) D("IDirectInputDevice8::SendDeviceData", 5, r_unsup)
    D("IDirectInputDevice8::EnumEffectsInFile", 5, r_unsup) D("IDirectInputDevice8::WriteEffectToFile", 5, r_unsup)
    D("IDirectInputDevice8::BuildActionMap", 4, r_unsup) D("IDirectInputDevice8::SetActionMap", 4, r_unsup)
    D("IDirectInputDevice8::GetImageInfo", 2, r_unsup)
};
const unsigned hle_input_ndefs = sizeof hle_input_defs / sizeof hle_input_defs[0];
