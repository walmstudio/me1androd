#pragma once
#include "rt.h"
#define THUNK_BASE 0xF0000000u
#define THUNK_END  0xF0100000u
void hle_init(void);
uint32_t hle_resolve(const char* dll, const char* name);
static inline int hle_is_thunk(uint32_t t) { return t >= THUNK_BASE && t < THUNK_END; }
void hle_invoke(CPU* c, uint32_t t);
/* guest heap */
uint32_t ghalloc(uint32_t size, int zero);
void ghfree(uint32_t p);
uint32_t ghsize(uint32_t p);
uint32_t gstrdup(const char* s);
#define ARG(n) rd32(c->esp + 4 + 4 * (n))
#define GSTR(p) ((const char*)(M + (p)))
uint32_t hle_module_handle(const char* name);
const char* hle_module_path(uint32_t h);
uint32_t hle_load_library(const char* name);
uint32_t hle_dll_attach(Module* m);
uint32_t hle_get_proc(uint32_t h, const char* name, uint32_t ord);
/* HLE table entries (shared by hle_win32.c and hle_fs.c) */
typedef void (*HleFn)(CPU*);
typedef struct { const char* dll; const char* name; int nargs; HleFn fn; int flags; } HleDef;
#define HLE_NOLOCK 1   /* runs without the big HLE lock: blocks, calls guest code, or does its own locking */
#define RET(v) do { c->eax = (uint32_t)(v); } while (0)
static inline void hle_set_last_error(CPU* c, uint32_t e) { wr32(c->fs_base + 0x34, e); }
/* virtual file system (hle_fs.c): Windows paths -> host directories
 *   C:\MassEffect\...                 -> game_dir   (Binaries, BioGame, ...)
 *   C:\Users\Shepard\Documents\...   -> docs_dir   (My Documents: BioWare\Mass Effect\Config, Save)
 *   any other C:\...                   -> drive_c_dir
 * NULL arguments: $ME1_GAME (default "."), $ME1_DOCS (default <game>/Documents), $ME1_DRIVE_C (default <game>/drive_c) */
void hle_fs_init(const char* game_dir, const char* docs_dir, const char* drive_c_dir);
int  hle_fs_host_path(const char* win_path, char* host, size_t cap);   /* 1 if the host path exists */
extern const HleDef hle_fs_defs[]; extern const unsigned hle_fs_ndefs;
/* PhoneModify engine modifiers (hle_mod.c, v0.10.4): <game>/PhoneModify/*.txt applied when ME1_PHONE_MOD=1 */
void hle_mod_init(const char* game_dir);
void hle_mod_apply(void);            /* write queued guest-memory patches whose address is mapped by now */
int  hle_mod_on(void);
unsigned hle_mod_maxdraws(void);     /* draw-call cap per frame, 0 = off (modifier "maxdraws") */
unsigned hle_mod_maxprims(void);     /* primitive cap per frame, 0 = off (modifier "maxprims") */
const char* hle_mod_dir(void);
void hle_fs_mod_key(const char* sec, const char* key, const char* val, char how);   /* ini directive -> BIOEngine.ini in memory */
void hle_fs_mod_keys_clear(void);
unsigned hle_fs_mod_key_count(void);
void hle_count_call(const char* name);
void hle_report(void);
void hle_register_module(Module* m, const char* path);
uint32_t hle_resolve_impl(const char* dll, const char* name);
/* call an implemented HLE API through its thunk exactly like guest code does (tests, launcher self-check) */
uint32_t hle_call(const char* dll, const char* name, int nargs, const uint32_t* args);
/* threads + sync objects (hle_thread.c) */
extern const HleDef hle_thread_defs[]; extern const unsigned hle_thread_ndefs;
void hle_thread_init(void);
void hle_cpu_place(int heavy);   /* v0.10.13: 2 = guest main thread, 1 = render thread (big cores), 0 = other game thread (light cores); v1.2.1: 3 = D3D9 replay thread (like 1) */
int  hle_heavy_tid(int which);   /* v0.10.13: 0 = main, 1 = render, 2 = D3D9 replay thread (v1.2.1); 0 if not registered yet */
void hle_adpf_frame(int fps_limit, double slept_s); /* v0.10.13: Android ADPF performance hint, once per frame */
int  hle_cpu_tidy(void);         /* v0.10.2: unpin threads we did not pin (Mali driver, GL presenter, AL/AAudio) that inherited a mask */
void hle_thread_check(void);                    /* TerminateThread takes effect at the next HLE call */
int  hle_kobj_close(uint32_t h);                /* CloseHandle for events/mutexes/semaphores/threads; 0 if not one */
uint32_t hle_wait(CPU* c, uint32_t n, const uint32_t* handles, int all, uint32_t ms);
unsigned hle_threads_started(void);
/* SEH / exceptions (hle_seh.c) */
extern const HleDef hle_seh_defs[]; extern const unsigned hle_seh_ndefs;
/* system DLLs of MassEffect.exe: user32/gdi32/ole32/winmm/devices (hle_exe.c) */
extern const HleDef hle_exe_defs[]; extern const unsigned hle_exe_ndefs;
void hle_set_cmdline(const char* s);
void hle_unwind_locks(int depth);
void hle_dll_notify(CPU* c, uint32_t reason);   /* DllMain(DLL_THREAD_ATTACH/DETACH) for loaded DLLs */
void hle_dll_disable_thread_calls(uint32_t base);
uint32_t hle_wstr(const char* utf8);                         /* guest UTF-16 copy (BMP) */
void     hle_wstr_utf8(uint32_t p, char* out, size_t cap);   /* guest UTF-16 -> UTF-8 */

/* module loader extras (rt.c): rt_load_module also accepts the original PE file; the next load is checked against crc */
void rt_expect_image_crc(uint32_t crc);
uint32_t rt_crc32(const uint8_t* p, size_t n);
/* guest virtual memory (hle_vmem.c): VirtualAlloc/VirtualFree with real release (madvise) */
uint32_t hle_vmem_alloc(uint32_t addr, uint32_t size, uint32_t type);   /* type: MEM_COMMIT/RESERVE/TOP_DOWN bits */
int hle_vmem_free(uint32_t addr, uint32_t size, uint32_t type);         /* MEM_RELEASE / MEM_DECOMMIT */
int hle_vmem_query(uint32_t a, uint32_t* base, uint32_t* size);
void hle_vmem_stats(uint32_t* nres, uint64_t* reserved, uint64_t* peak);
/* Direct3D 9 / D3DX (hle_d3d9.c) */
extern const HleDef hle_d3d9_defs[]; extern const unsigned hle_d3d9_ndefs;
/* optional one-line UI notification (Toast), set by android/jni/game_jni.c; NULL elsewhere (v0.10.2: VK_ERROR_DEVICE_LOST) */
extern void (*hle_toast_hook)(const char* msg);
extern void (*hle_restart_hook)(void);   /* v0.10.10: serial DEVICE_LOST - restart the game process (set by game_jni.c) */
extern void (*hle_exit_hook)(int code);   /* v1.2.2: ExitProcess - the platform says goodbye before _exit (set by game_jni.c) */
void hle_d3d9_report(void);
unsigned hle_d3d9_frames(void);                 /* Present count (input pacing) */
void hle_d3dx_attach(uint32_t compile, uint32_t assemble, uint32_t preprocess, uint32_t disassemble);   /* recompiled d3dx9_31.dll */
void hle_d3dx_attach_module(Module* dx);   /* + D3DXCreateBuffer/D3DXGetShaderConstantTable for the HLSL compile cache */
void hle_d3d9_heartbeat(char* out, size_t cap);
void hle_d3dx_cache_stats(uint64_t* hits, uint64_t* stored);   /* one-line D3D/D3DX status (and dumps a long-running compile's source) */
void hle_fs_brief(char* out, size_t cap);         /* one-line file I/O status */
int  hle_count_snapshot(const char** names, unsigned* counts, int max);

/* heartbeat + sampling profiler (hle_hb.c). hle_invoke marks the API each guest thread is in; a host thread samples
 * all guest threads (ME1_PROF_HZ, default 100/s) and prints "[hb]" status lines every ME1_HB seconds (default 10, 0 = off). */
typedef struct HbSlot { const char* volatile api; volatile uint32_t seq; } HbSlot;
extern _Thread_local HbSlot* hle_hb_cur;
HbSlot* hle_hb_new(CPU* c);
static inline void hle_hb_enter(CPU* c, const char* api) { HbSlot* s = hle_hb_cur; if (!s) s = hle_hb_new(c); s->api = api; s->seq++; }
static inline void hle_hb_leave(void) { HbSlot* s = hle_hb_cur; if (s) s->api = NULL; }
void hle_hb_thread_exit(void);
void hle_hb_start(void);
void hle_hb_now(const char* why);   /* print a beat immediately (signal/fault reports) */
void hle_hb_stacks(const char* why, CPU* me);     /* every guest thread's API + stack now ("*" marks me) */
void hle_watch_start(void);                       /* ME1_WATCH memory watch (hle_hb.c) */
void hle_watch_release(uint32_t a, uint32_t n, const char* what);   /* hle_vmem: range released/decommitted */
/* DirectInput 8 (hle_input.c): system mouse + keyboard; the front end feeds them */
extern const HleDef hle_input_defs[]; extern const unsigned hle_input_ndefs;
void hle_input_key(int dik, int down);                          /* DIK_* scan code */
void hle_input_mouse(int dx, int dy, int dz, unsigned buttons); /* relative motion + button mask */
int hle_input_mouse_exclusive(void);   /* v1.1.5: a DirectInput mouse is acquired exclusively (Windows hides the cursor) */
/* XInput pad 0: XINPUT_GAMEPAD buttons mask, triggers 0..255, sticks -32768..32767 (Y up = positive) */
void hle_xinput_set(unsigned buttons, int lt, int rt, int lx, int ly, int rx, int ry);
void hle_xinput_connect(int on);
int  hle_xinput_vibration(void);                                /* (left << 16) | right motor speed */
void hle_xinput_brief(char* out, size_t cap);
/* window messages + cursor (hle_exe.c): pointer in back buffer pixels (button 0 move, 1/2 left down/up, 3/4 right), keys */
void hle_ui_pointer(int fx, int fy, int fw, int fh, int button);
void hle_ui_key(int vk, int dik, int down, int ch);
void hle_ui_mouse_rel(int dx, int dy);       /* on-screen look area: DirectInput relative motion only */
void hle_ui_mouse_button(int button);        /* on-screen fire/aim: 1/2 left down/up, 3/4 right down/up at the cursor; 5/6 middle (v1.1) */
void hle_ui_pointer_rel(int fx, int fy, int fw, int fh, int dx, int dy);   /* v1.1 desktop: cursor + real DirectInput motion */
void hle_ui_wheel(int delta);                /* v1.1 desktop: WM_MOUSEWHEEL + DirectInput Z (120 per notch) */
void hle_ui_char(int ch);                    /* v1.1 desktop: WM_CHAR of text input */
int  hle_ui_close(void);                     /* v1.1 desktop: WM_CLOSE to the game window (0: no window) */
void hle_ui_brief(char* out, size_t cap);   /* pointer use counters for [hb] */
int  hle_ui_cursor(int* x, int* y, int* w, int* h);   /* v1.1.5: cursor in client pixels + client size; 1 shown, 0 hidden, -1 no window */
/* OpenAL 1.1 = OpenAL32.dll (hle_al.c): software mixer, AAudio output on Android, else a real-time null sink */
extern const HleDef hle_al_defs[]; extern const unsigned hle_al_ndefs;
int hle_al_enabled(void);
uint32_t hle_al_module(void);                      /* pseudo module handle of OpenAL32.dll (0 when ME1_AL=0) */
int  hle_al_tick_due(uint64_t now_ns);             /* T-5: throttle guest audio timer ticks (0: skip this tick) */
extern int (*hle_al_out_open)(int rate, const char** name);   /* v1.1: desktop front end audio device (SDL2), see hle_al.c */
extern void (*hle_al_out_start)(void);
void hle_al_render(int16_t* out, int frames);      /* mix stereo s16 frames (ME1_AL_SINK=manual: tests drive the mixer) */
int  hle_al_rate(void);
void hle_al_brief(char* out, size_t cap);
void hle_al_report(void);
void hle_mm_brief(char* out, size_t cap);           /* winmm multimedia timers (hle_thread.c) */
int  hle_mm_period(void);
