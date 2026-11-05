/* Direct3D 9 + D3DX for recompiled guest code - hle_d3d9.c
 *
 * The game talks to D3D9 through COM objects: a guest pointer whose first dword points to a vtable. Every object here
 * is a 16-byte guest stub {vtable, magic, host index}; the vtables hold HLE thunks ("d3d9.com" entries, in exact
 * d3d9.h order), so a guest `call [eax+0xA8]` lands in a host function. Memory the game locks (texture levels,
 * vertex/index data) is guest memory.
 *
 * Rendering: hle_d3d9_gl.h (OpenGL ES 3 through EGL, loaded at run time; included at the end of this file). Without
 * EGL/GLES (or ME1_GL=0) this is a "null" backend: full object model, states, locks, queries and statistics, nothing
 * drawn - the engine still runs through content loading and the frame loop.
 * Env: ME1_D3D_LOG=1|2   log resource creation (2: also every draw)
 *      ME1_D3D_DUMP=dir  append every distinct shader to dir/shaders.pak for offline translation tests (build/sm3_check)
 *      ME1_MAX_FRAMES=N  exit(0) after N presented frames, with the report
 *      ME1_MAX_SECONDS=N exit(0) N seconds after the first presented frame, with the report
 *      ME1_SCREEN=WxH    desktop mode reported to the game (default 1280x720)
 *      ME1_VRAM_MB=n     GetAvailableTextureMem (default 512)
 *      ME1_OCCLUSION=0|1|2  v1.1.3 occlusion queries: 1 (default) real GPU results with D3D9 semantics (S_FALSE until the
 *                        GPU is done), 2 never wait (the query's last finished result), 0 always "visible" (before v1.1.3) */
#define _GNU_SOURCE
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define OK_ 0u
#define E_NOINTERFACE_ 0x80004002u
#define E_POINTER_ 0x80004003u
#define E_FAIL_ 0x80004005u
#define ERR_NOTFOUND 0x88760866u
#define ERR_NOTAVAILABLE 0x8876086Au
#define ERR_INVALIDCALL 0x8876086Cu
#define OK_NOAUTOGEN 0x0876086Fu
#define MAGIC 0x39443344u
#define FCC(a, b, c_, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c_) << 16 | (uint32_t)(d) << 24)

enum { T_D3D = 1, T_DEV, T_SWAP, T_TEX, T_CUBE, T_VOLTEX, T_SURF, T_VOL, T_VB, T_IB, T_DECL, T_VS, T_PS, T_QUERY, T_SB, T_BUF, T_N };
static const char* tname[T_N] = { "?", "IDirect3D9", "IDirect3DDevice9", "IDirect3DSwapChain9", "IDirect3DTexture9",
    "IDirect3DCubeTexture9", "IDirect3DVolumeTexture9", "IDirect3DSurface9", "IDirect3DVolume9", "IDirect3DVertexBuffer9",
    "IDirect3DIndexBuffer9", "IDirect3DVertexDeclaration9", "IDirect3DVertexShader9", "IDirect3DPixelShader9",
    "IDirect3DQuery9", "IDirect3DStateBlock9", "ID3DXBuffer" };

typedef struct Obj Obj;
struct Obj {
    uint32_t g; int type, refs, irefs, idx, owned, locks;
    Obj* parent;                  /* container texture of a surface/volume: refcounts are forwarded to it */
    uint32_t w, h, d, levels, usage, fmt, pool, size, fvf, ms, msq, face, level, qtype, lod, prio;
    uint32_t* lockv;              /* guest lock buffer per subresource (textures: face * levels + level) */
    Obj** sub;                    /* textures: child surfaces/volumes, created on demand */
    uint32_t shadow;              /* dynamic VB/IB: persistent guest copy; ID3DXBuffer: its data */
    uint8_t* blob; uint32_t bloblen, crc;
    uint32_t pp[14];              /* swap chain: D3DPRESENT_PARAMETERS */
    Obj* bb;                      /* swap chain: back buffer */
    void* ext;                    /* device: Dev */
    void* gx;                     /* GL backend object (GlRes / GlSh) */
    uint32_t* lockr;              /* GL backend: locked rect + flags per subresource */
    uint32_t boff, bend, bflags;  /* buffers: union of the ranges locked since the last upload, OR of the lock flags */
};
typedef struct {
    Obj* self; Obj* d3d; Obj* swap; Obj* autods;
    Obj* rt[4]; Obj* ds; Obj* tex[21]; Obj* vb[16]; uint32_t vboff[16], vbstride[16], vbfreq[16];
    Obj* ib; Obj* decl; uint32_t fvf; Obj* vs; Obj* ps;
    uint32_t rs[256], ss[21][14], tss[8][33];
    float vsf[256][4], psf[224][4]; int32_t vsi[16][4], psi[16][4]; uint32_t vsb[16], psb[16];
    uint32_t vp[6], scissor[4], clip[8][4]; float xf[512][16];
    uint32_t hwnd, flags, devtype, cursor, recording, mat[17]; uint16_t gamma[768];
    uint32_t cver[2];             /* shader constant versions (vs, ps): bumped by every Set*ShaderConstant* */
} Dev;
static int g_gl = -1;             /* -1: not initialized, 0: null backend, 1: GPU renderer */
static int g_vk;                  /* the GPU renderer is Vulkan (hle_d3d9_vk.h), else GLES */
static int g_zink;                /* GLES through Mesa zink (hle_d3d9_gl.h) */
void (*hle_toast_hook)(const char* msg);   /* one-line UI notification (set by android/jni/game_jni.c, see hle.h) */
void (*hle_restart_hook)(void);   /* serial DEVICE_LOST -> game process restart (set by android/jni/game_jni.c) */
static Obj* g_devobj;
static void gl_free(Obj* o); static void gl_note_lock(Obj* r, uint32_t s, uint32_t lvl, uint32_t area, int vol, uint32_t flags);
static void gl_tex_unlock(Obj* r, uint32_t s, uint32_t lvl); static void gl_buf_unlock(Obj* b, uint32_t off, uint32_t end, uint32_t flags);
static void gl_shader_new(Obj* o); static int gl_init(void); static void gl_release(void); static void gl_present(Obj* bb); static void gl_frame_times(char* o, size_t cap, uint64_t frames);
static void gl_clear(Dev* d, uint32_t n, uint32_t rects, uint32_t flags, uint32_t color, uint32_t zbits, uint32_t stencil);
static void gl_draw_x(Dev* d, uint32_t pt, uint32_t prims, int indexed, int32_t base, uint32_t minv, uint32_t nv, uint32_t start,
                      int up, uint32_t upv, uint32_t upstride, uint32_t upi, uint32_t upifmt);
static void gl_stretch(Obj* src, uint32_t sr, Obj* dst, uint32_t dr, uint32_t filter); static void gl_colorfill(Obj* surf, uint32_t rect, uint32_t color);
static void gl_update_surface(Obj* src, uint32_t sr, Obj* dst, uint32_t dpt); static void gl_update_texture(Obj* src, Obj* dst);
static void gl_get_rt_data(Obj* rt, Obj* dst); static void gl_rt_lock(Obj* r, uint32_t s, uint32_t lvl, uint32_t mem); static void gl_report(void);
/* v1.1.3: occlusion queries (D3DQUERYTYPE_OCCLUSION). Until v1.1.2 every query reported "100000 pixels visible": the
 * engine never culled anything hidden (all draws behind walls went to the GPU) and lens flares / coronas, whose
 * brightness and size follow the visible pixel count, shone at full strength through walls and far too big.
 * Vulkan: precise sample counts (VK_QUERY_TYPE_OCCLUSION); GLES: GL_ANY_SAMPLES_PASSED_CONSERVATIVE (visible or not). */
typedef struct {
    int st;                 /* 0 never issued, 1 between BEGIN and END, 2 issued (result pending), 3 result known */
    int over;               /* the result cannot be measured (pool full, frame split, draw skipped): report "visible" */
    int n, att, rec, polls; /* n: backend sub-queries; att/rec: draws inside the query attempted / recorded by the backend */
    uint32_t slot, sub[8], res, last; int has_last; uint64_t ser; double t0;
    uint32_t glq[8]; void* ctx; uint32_t rtw, rth;   /* GLES: query names, their context; render target size at the first draw */
} QRes;
static Obj* g_qopen;        /* the occlusion query between Issue(BEGIN) and Issue(END) */
static int g_occl = 1;      /* ME1_OCCLUSION */
static struct { uint64_t issued, ready, zero, fallback, notready, flushes, waits; double wait_s; } g_qs, g_qs0;
static void gl_query_begin(Obj* q); static void gl_query_end(Obj* q); static int gl_query_data(Obj* q, uint32_t flags, uint32_t* out);

static pthread_mutex_t g_mx;
static __thread const char* g_cur;          /* method being executed (for logs) */
static Obj** g_obj; static int g_nobj, g_capobj; static int* g_freeidx; static int g_nfreeidx, g_capfree;
static uint32_t g_vt[T_N];
static int g_log = -1; static unsigned g_scrw = 1280, g_scrh = 720, g_vram = 512; static uint64_t g_maxframes; static double g_maxsecs, g_tfirst;
static const char* g_dump;

/* statistics */
static struct {
    uint64_t frames, draws, prims, updraws, clears, locks, lockbytes, fdraws, fprims, dxloads;
    unsigned created[T_N], alive[T_N];
    double t0, tlast; uint64_t flast;
} g_st;
typedef struct { uint32_t key, n; uint64_t bytes; } Hist;
static Hist g_fmt[96]; static int g_nfmt;          /* texture formats */
static Hist g_shv[16]; static int g_nshv;          /* shader version tokens */
static uint32_t* g_shcrc; static int g_nshcrc, g_capshcrc;
static void hist_add(Hist* h, int* n, int cap, uint32_t key, uint64_t bytes) {
    for (int i = 0; i < *n; i++) if (h[i].key == key) { h[i].n++; h[i].bytes += bytes; return; }
    if (*n < cap) { h[*n].key = key; h[*n].n = 1; h[*n].bytes = bytes; (*n)++; }
}
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
/* v0.10.7: Vulkan DEVICE_LOST - report it to the game through the standard D3D9 flow (TestCooperativeLevel/Present
 * return D3DERR_DEVICELOST, the game calls Reset, which re-creates the device and renderer resources).
 * v0.10.11 ROLLBACK: the flow is OFF by default - Mali drivers die serially and recovery made sessions worse
 * (log 0.10.9: 4 deaths in 9 frames). ME1_VK_LOSTRESET=1 opts back in; without it: old behaviour
 * (one DEVICE_LOST -> black screen + Toast). GLES is unaffected. */
#define D3DERR_DEVICELOST_ 0x88760868u
static int g_d3dlost; static double g_d3dlostt; static int g_lostreset_ok;
static int g_vklost;   /* defined in hle_d3d9_vk.h */
static void d3d_device_lost(void) {
    const char* e = getenv("ME1_VK_LOSTRESET"); if (!(e && *e == '1')) return;
    g_lostreset_ok = 1;
    if (!g_d3dlost) { g_d3dlost = 1; g_d3dlostt = mono(); fprintf(stderr, "[d3d] Vulkan device lost - recovery is off (ME1_VK_LOSTRESET=1 to enable)\n"); }
}
static int vk_device_reset(void);   /* hle_d3d9_vk.h */

/* guest memory for objects and locks: small blocks from size-class free lists (kept), big ones from VirtualAlloc
 * (released on free, so streaming textures do not pin memory) */
static uint32_t g_pool[17];
static int cls(uint32_t n) { int k = 6; while ((1u << k) < n) k++; return k; }
static uint32_t galloc(uint32_t n) {
    if (n >= 0x10000u) return hle_vmem_alloc(0, n, 0x3000u);
    int k = cls(n); uint32_t p = g_pool[k];
    if (p) { g_pool[k] = rd32(p); memset(M + p, 0, 1u << k); return p; }
    return ghalloc(1u << k, 1);
}
static void gfree(uint32_t p, uint32_t n) {
    if (!p) return;
    if (n >= 0x10000u) { hle_vmem_free(p, 0, 0x8000u); return; }
    int k = cls(n); wr32(p, g_pool[k]); g_pool[k] = p;
}

/* ---- formats ---- */
enum { F_TEX = 1, F_RT = 2, F_DS = 4, F_FILT = 8, F_BLEND = 16, F_SRGB = 32, F_VTX = 64, F_DISP = 128, F_GEN = 256, F_DTEX = 512 };
typedef struct { uint32_t fmt; uint8_t bytes, block; uint16_t fl; const char* name; } Fmt;
#define C8 (F_TEX | F_RT | F_FILT | F_BLEND)
static const Fmt g_fmts[] = {
    { 21, 4, 0, C8 | F_SRGB | F_DISP | F_GEN, "A8R8G8B8" }, { 22, 4, 0, C8 | F_SRGB | F_DISP | F_GEN, "X8R8G8B8" },
    { 23, 2, 0, C8 | F_DISP | F_GEN, "R5G6B5" }, { 24, 2, 0, C8, "X1R5G5B5" }, { 25, 2, 0, C8, "A1R5G5B5" },
    { 26, 2, 0, F_TEX | F_FILT, "A4R4G4B4" }, { 28, 1, 0, F_TEX | F_FILT, "A8" }, { 31, 4, 0, C8, "A2B10G10R10" },
    { 32, 4, 0, C8 | F_SRGB, "A8B8G8R8" }, { 33, 4, 0, C8, "X8B8G8R8" }, { 34, 4, 0, F_TEX | F_FILT, "G16R16" },
    { 35, 4, 0, C8, "A2R10G10B10" }, { 36, 8, 0, F_TEX | F_FILT, "A16B16G16R16" }, { 50, 1, 0, F_TEX | F_FILT, "L8" },
    { 51, 2, 0, F_TEX | F_FILT, "A8L8" }, { 60, 2, 0, F_TEX | F_FILT, "V8U8" }, { 63, 4, 0, F_TEX | F_FILT, "Q8W8V8U8" },
    { 64, 4, 0, F_TEX | F_FILT, "V16U16" }, { 81, 2, 0, F_TEX | F_FILT, "L16" },
    { 70, 2, 0, F_DS, "D16_LOCKABLE" }, { 71, 4, 0, F_DS, "D32" }, { 75, 4, 0, F_DS, "D24S8" },
    { 77, 4, 0, F_DS, "D24X8" }, { 80, 2, 0, F_DS, "D16" }, { 82, 4, 0, F_DS, "D32F_LOCKABLE" },
    { 83, 4, 0, F_DS, "D24FS8" },
    { 111, 2, 0, C8, "R16F" }, { 112, 4, 0, C8, "G16R16F" }, { 113, 8, 0, C8 | F_VTX, "A16B16G16R16F" },
    { 114, 4, 0, F_TEX | F_RT | F_VTX, "R32F" }, { 115, 8, 0, F_TEX | F_RT, "G32R32F" }, { 116, 16, 0, F_TEX | F_RT | F_VTX, "A32B32G32R32F" },
    { FCC('D','X','T','1'), 8, 1, F_TEX | F_FILT | F_SRGB, "DXT1" }, { FCC('D','X','T','2'), 16, 1, F_TEX | F_FILT | F_SRGB, "DXT2" },
    { FCC('D','X','T','3'), 16, 1, F_TEX | F_FILT | F_SRGB, "DXT3" }, { FCC('D','X','T','4'), 16, 1, F_TEX | F_FILT | F_SRGB, "DXT4" },
    { FCC('D','X','T','5'), 16, 1, F_TEX | F_FILT | F_SRGB, "DXT5" },
    { FCC('A','T','I','1'), 8, 1, F_TEX | F_FILT, "ATI1" }, { FCC('A','T','I','2'), 16, 1, F_TEX | F_FILT, "ATI2" },
};
static const Fmt* fmt_info(uint32_t f) {
    for (unsigned i = 0; i < sizeof g_fmts / sizeof g_fmts[0]; i++) if (g_fmts[i].fmt == f) return &g_fmts[i];
    return NULL;
}
static const char* fmt_name(uint32_t f) {
    static __thread char b[16]; const Fmt* i = fmt_info(f); if (i) return i->name;
    if (f > 0xFFFF) { snprintf(b, sizeof b, "'%c%c%c%c'", f & 255, f >> 8 & 255, f >> 16 & 255, f >> 24); return b; }
    snprintf(b, sizeof b, "fmt%u", f); return b;
}
static uint32_t dimv(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1; }
/* bytes per row (of blocks) and number of rows of one mip level */
static void layout(uint32_t fmt, uint32_t w, uint32_t h, uint32_t* pitch, uint32_t* rows, uint32_t* unit, int* blk) {
    const Fmt* f = fmt_info(fmt);
    uint32_t b = f ? f->bytes : 4; int k = f ? f->block : 0;
    if (k) { *pitch = ((w + 3) / 4) * b; *rows = (h + 3) / 4; } else { *pitch = w * b; *rows = h; }
    *unit = b; *blk = k;
}
static uint32_t full_levels(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t n = 1; while (w > 1 || h > 1 || d > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; d = d > 1 ? d / 2 : 1; n++; }
    return n;
}
/* CheckDeviceFormat */
static uint32_t check_format(uint32_t usage, uint32_t rtype, uint32_t fmt) {
    const Fmt* f = fmt_info(fmt);
    if (!f) return ERR_NOTAVAILABLE;
    int tex = rtype == 3 || rtype == 4 || rtype == 5;
    if (usage & 2) {   /* DEPTHSTENCIL */
        if (!(f->fl & F_DS)) return ERR_NOTAVAILABLE;
        if (tex && (rtype == 4 || !(f->fl & F_DTEX))) return ERR_NOTAVAILABLE;
    } else if (f->fl & F_DS) return ERR_NOTAVAILABLE;
    if ((usage & 1) && !(f->fl & F_RT)) return ERR_NOTAVAILABLE;
    if (tex && !(usage & 2) && !(f->fl & F_TEX)) return ERR_NOTAVAILABLE;
    if (rtype == 4 && f->block) return ERR_NOTAVAILABLE;
    if ((usage & 0x20000u) && !(f->fl & F_FILT)) return ERR_NOTAVAILABLE;            /* QUERY_FILTER */
    if ((usage & 0x80000u) && !(f->fl & F_BLEND)) return ERR_NOTAVAILABLE;           /* QUERY_POSTPIXELSHADER_BLENDING */
    if ((usage & 0x50000u) && !(f->fl & F_SRGB)) return ERR_NOTAVAILABLE;            /* QUERY_SRGBREAD/WRITE */
    if ((usage & 0x100000u) && !(f->fl & F_VTX)) return ERR_NOTAVAILABLE;            /* QUERY_VERTEXTEXTURE */
    if (usage & 0x8000u) return ERR_NOTAVAILABLE;                                     /* QUERY_LEGACYBUMPMAP */
    if (usage & 0x400u) return (f->fl & F_GEN) ? OK_ : OK_NOAUTOGEN;                 /* AUTOGENMIPMAP */
    return OK_;
}

/* ---- caps / adapter: a Shader Model 3 GPU with what GLES 3.2 on Mali/Adreno can actually do ---- */
static void wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(a, u); }
static void fill_caps(uint32_t p) {
    memset(M + p, 0, 304);
    wr32(p + 0, 1); wr32(p + 4, 0);                          /* HAL, adapter 0 */
    wr32(p + 8, 0x00020000u);                                 /* Caps: READ_SCANLINE */
    wr32(p + 12, 0x60020000u);                                /* Caps2: FULLSCREENGAMMA | DYNAMICTEXTURES | CANAUTOGENMIPMAP */
    wr32(p + 16, 0x000003A0u);                                /* Caps3 */
    wr32(p + 20, 0x8000000Fu);                                /* PresentationIntervals: IMMEDIATE | ONE..FOUR */
    wr32(p + 24, 1);                                          /* CursorCaps */
    wr32(p + 28, 0x001BBEF0u);                                /* DevCaps (HW T&L, pure device, ...) */
    wr32(p + 32, 0x000EF2F2u);                                /* PrimitiveMiscCaps: separate alpha blend, MRT, blendop, ... */
    wr32(p + 36, 0x07732191u);                                /* RasterCaps: scissor, depth bias, anisotropy, ... */
    wr32(p + 40, 0xFF); wr32(p + 44, 0x3FFF); wr32(p + 48, 0x3FFF); wr32(p + 52, 0xFF); wr32(p + 56, 0x84208);
    wr32(p + 60, 0x0001EC45u);                                /* TextureCaps: no POW2/NONPOW2CONDITIONAL restrictions */
    wr32(p + 64, 0x07030700u); wr32(p + 68, 0x07030700u); wr32(p + 72, 0x03030300u);
    wr32(p + 76, 0x3F); wr32(p + 80, 0x3F); wr32(p + 84, 0x1F);
    wr32(p + 88, 4096); wr32(p + 92, 4096); wr32(p + 96, 2048); wr32(p + 100, 8192); wr32(p + 104, 8192); wr32(p + 108, 16);
    wrf(p + 112, 1e10f); wrf(p + 116, -1e8f); wrf(p + 120, -1e8f); wrf(p + 124, 1e8f); wrf(p + 128, 1e8f);
    wr32(p + 136, 0x1FF); wr32(p + 140, 0x00180008u); wr32(p + 144, 0x03FEFFFFu); wr32(p + 148, 8); wr32(p + 152, 8);
    wr32(p + 156, 0x17B); wr32(p + 160, 8); wr32(p + 164, 8); wr32(p + 168, 4); wr32(p + 172, 0);
    wrf(p + 176, 8192.0f); wr32(p + 180, 0xFFFFF); wr32(p + 184, 0xFFFFF); wr32(p + 188, 16); wr32(p + 192, 255);
    wr32(p + 196, 0xFFFE0300u); wr32(p + 200, 256);           /* vs_3_0, 256 float constants */
    wr32(p + 204, 0xFFFF0300u); wrf(p + 208, 3.4e38f);        /* ps_3_0 */
    wr32(p + 212, 0x51);                                      /* DevCaps2: STREAMOFFSET | CAN_STRETCHRECT_FROM_TEXTURES | ... */
    wr32(p + 232, 1); wr32(p + 236, 0x3FF);                   /* adapters in group, DeclTypes (all incl. FLOAT16) */
    wr32(p + 240, 4); wr32(p + 244, 0x03000300u);             /* 4 MRTs, StretchRect filters */
    wr32(p + 248, 1); wr32(p + 252, 24); wr32(p + 256, 32); wr32(p + 260, 4);                     /* VS20Caps */
    wr32(p + 264, 0x1F); wr32(p + 268, 24); wr32(p + 272, 32); wr32(p + 276, 4); wr32(p + 280, 512); /* PS20Caps */
    wr32(p + 284, 0x03000300u); wr32(p + 288, 0xFFFF); wr32(p + 292, 0xFFFF); wr32(p + 296, 4096); wr32(p + 300, 4096);
}
static void fill_identifier(uint32_t p) {
    memset(M + p, 0, 1100);
    strcpy((char*)M + p, "nv4_disp.dll"); strcpy((char*)M + p + 512, "NVIDIA GeForce 8800 GT");
    strcpy((char*)M + p + 1024, "\\\\.\\DISPLAY1");
    wr32(p + 1056, 0x000B1B09u); wr32(p + 1060, 0x0006000Eu);   /* driver 6.14.11.6921 */
    wr32(p + 1064, 0x10DE); wr32(p + 1068, 0x0611); wr32(p + 1072, 0); wr32(p + 1076, 0xA2);
    static const uint8_t guid[16] = { 0xD7, 0xB7, 0x1E, 0x3E, 0x47, 0x51, 0x11, 0xCF, 0x6F, 0x4D, 0x69, 0x5E, 0x00, 0x00, 0x00, 0x01 };
    memcpy(M + p + 1080, guid, 16); wr32(p + 1096, 1);
}
static unsigned g_modes[16][2]; static int g_nmodes;
static void init_modes(void) {
    static const unsigned std[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1366, 768 },
                                       { 1600, 900 }, { 1920, 1080 } };
    unsigned cand[16][2]; int n = 0;
    for (unsigned i = 0; i < sizeof std / sizeof std[0]; i++) if (std[i][0] <= g_scrw && std[i][1] <= g_scrh) { cand[n][0] = std[i][0]; cand[n][1] = std[i][1]; n++; }
    cand[n][0] = g_scrw; cand[n][1] = g_scrh; n++;
    for (int i = 0; i < n; i++) {
        int dup = 0; for (int j = 0; j < g_nmodes; j++) if (g_modes[j][0] == cand[i][0] && g_modes[j][1] == cand[i][1]) dup = 1;
        if (dup) continue;
        int k = g_nmodes++;
        while (k > 0 && (g_modes[k - 1][0] > cand[i][0] || (g_modes[k - 1][0] == cand[i][0] && g_modes[k - 1][1] > cand[i][1]))) {
            g_modes[k][0] = g_modes[k - 1][0]; g_modes[k][1] = g_modes[k - 1][1]; k--;
        }
        g_modes[k][0] = cand[i][0]; g_modes[k][1] = cand[i][1];
    }
}

/* ---- objects ---- */
static Obj* newobj(int type) {
    Obj* o = calloc(1, sizeof *o);
    int i;
    if (g_nfreeidx) i = g_freeidx[--g_nfreeidx];
    else {
        if (g_nobj == g_capobj) { g_capobj = g_capobj ? 2 * g_capobj : 4096; g_obj = realloc(g_obj, g_capobj * sizeof *g_obj); }
        i = g_nobj++;
    }
    g_obj[i] = o; o->idx = i; o->type = type; o->refs = 1;
    o->g = galloc(16); wr32(o->g, g_vt[type]); wr32(o->g + 4, MAGIC); wr32(o->g + 8, (uint32_t)i);
    g_st.created[type]++; g_st.alive[type]++;
    return o;
}
static Obj* O(uint32_t g) {
    if (!g || g >= 0xFFFFFFF0u) return NULL;
    if (rd32(g + 4) != MAGIC) {
        static int n; if (n++ < 10) fprintf(stderr, "[d3d] %s: %08x is not a live D3D object\n", g_cur, g);
        return NULL;
    }
    uint32_t i = rd32(g + 8);
    return i < (uint32_t)g_nobj ? g_obj[i] : NULL;
}
static Obj* root(Obj* o) { return o->parent ? o->parent : o; }
static uint32_t addref(Obj* o) { o = root(o); return (uint32_t)++o->refs; }
static void destroy(Obj* o);
static uint32_t release(Obj* o) {
    o = root(o);
    if (o->refs <= 0) return 0;
    uint32_t r = (uint32_t)--o->refs;
    if (!r && !o->irefs && !o->owned) destroy(o);
    return r;
}
static void iref(Obj* o) { root(o)->irefs++; }
static void iunref(Obj* o) { o = root(o); if (--o->irefs == 0 && o->refs <= 0 && !o->owned) destroy(o); }
static void bind(Obj** slot, Obj* n) { if (*slot == n) return; if (n) iref(n); Obj* old = *slot; *slot = n; if (old) iunref(old); }
static uint32_t sub_count(Obj* o) { return o->type == T_CUBE ? 6 * o->levels : o->type >= T_TEX && o->type <= T_VOLTEX ? o->levels : 1; }
static uint32_t level_bytes(Obj* o, uint32_t l) {
    uint32_t pitch, rows, unit; int blk;
    layout(o->fmt, dimv(o->w, l), dimv(o->h, l), &pitch, &rows, &unit, &blk);
    return pitch * rows * (o->type == T_VOLTEX || o->type == T_VOL ? dimv(o->d, l) : 1);
}
static void free_obj_mem(Obj* o) {
    uint32_t n = sub_count(o);
    if (o->lockv) for (uint32_t i = 0; i < n; i++) if (o->lockv[i]) {
        uint32_t l = o->type == T_CUBE ? i % o->levels : o->type >= T_TEX && o->type <= T_VOLTEX ? i : 0;
        gfree(o->lockv[i], o->type == T_VB || o->type == T_IB ? o->size : level_bytes(o, l));
    }
    free(o->lockv); o->lockv = NULL; free(o->lockr); o->lockr = NULL;
    if (o->shadow) { gfree(o->shadow, o->size); o->shadow = 0; }
    free(o->blob); o->blob = NULL;
}
/* v0.10.13 (R1): the last device seen by Set{Render,Sampler}State, for the lock-free redundant-value fast path */
static Dev* g_fdev; static uint32_t g_fdev_g;
static void fdev_set(uint32_t g, Dev* d) {
    if (__atomic_load_n(&g_fdev, __ATOMIC_RELAXED) == d && __atomic_load_n(&g_fdev_g, __ATOMIC_RELAXED) == g) return;
    __atomic_store_n(&g_fdev, (Dev*)NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fdev_g, g, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fdev, d, __ATOMIC_RELEASE);
}
static void destroy(Obj* o) {
    if (o->type == T_DEV && o->ext == (void*)__atomic_load_n(&g_fdev, __ATOMIC_ACQUIRE)) __atomic_store_n(&g_fdev, (Dev*)NULL, __ATOMIC_RELEASE);
    if (g_log > 1) fprintf(stderr, "[d3d] destroy %s %08x\n", tname[o->type], o->g);
    if (o->gx) gl_free(o);
    free_obj_mem(o);
    if (o->sub) { for (uint32_t i = 0; i < sub_count(o); i++) if (o->sub[i]) { o->sub[i]->parent = NULL; o->sub[i]->owned = 0; o->sub[i]->refs = 0; destroy(o->sub[i]); } free(o->sub); }
    if (o->bb) { o->bb->owned = 0; destroy(o->bb); }
    wr32(o->g + 4, 0xDEADD3D9u); gfree(o->g, 16);
    g_obj[o->idx] = NULL;
    if (g_nfreeidx == g_capfree) { g_capfree = g_capfree ? 2 * g_capfree : 4096; g_freeidx = realloc(g_freeidx, g_capfree * sizeof *g_freeidx); }
    g_freeidx[g_nfreeidx++] = o->idx;
    g_st.alive[o->type]--;
    free(o->ext); free(o);
}
static uint32_t out_obj(uint32_t pp, Obj* o) { if (pp) wr32(pp, o ? o->g : 0); return o ? OK_ : ERR_NOTFOUND; }
#define A(n) ARG(n)
#define SELF O(ARG(0))
static Dev* DEVOF(Obj* o) { return o && o->type == T_DEV ? (Dev*)o->ext : NULL; }
static void disown(Obj* o) { if (!o) return; o->owned = 0; if (o->refs <= 0 && !o->irefs) destroy(o); }

/* ---- init / locking ---- */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
/* v0.10: frame limiter (ME1_FPS_LIMIT / [Port] FpsLimit) and the numbers behind the on-screen FPS counter (me1_perf) */
static int g_fpslimit; static double g_fpsnext, g_pft0; static uint64_t g_pff0; static volatile int64_t g_perf[4];
static void d3d_init(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_mx, &a); pthread_mutexattr_destroy(&a);
    const char* e;
    g_log = (e = getenv("ME1_D3D_LOG")) ? atoi(e) : 0;
    if ((e = getenv("ME1_SCREEN")) && (sscanf(e, "%ux%u", &g_scrw, &g_scrh) != 2 || g_scrw < 320 || g_scrh < 200)) { g_scrw = 1280; g_scrh = 720; }
    if ((e = getenv("ME1_VRAM_MB")) && atoi(e) > 0) g_vram = (unsigned)atoi(e);
    if ((e = getenv("ME1_MAX_FRAMES"))) g_maxframes = strtoull(e, NULL, 10);
    if ((e = getenv("ME1_MAX_SECONDS"))) g_maxsecs = atof(e);
    if ((e = getenv("ME1_FPS_LIMIT")) && atoi(e) > 0) { int l = atoi(e); g_fpslimit = l < 10 ? 10 : l > 240 ? 240 : l; }
    if ((e = getenv("ME1_D3D_DUMP")) && *e) { g_dump = e; mkdir(e, 0755); }
    if ((e = getenv("ME1_OCCLUSION")) && *e >= '0' && *e <= '2') g_occl = *e - '0';
    if (g_fpslimit) fprintf(stderr, "[d3d] fps limit %d (ME1_FPS_LIMIT)\n", g_fpslimit); else fprintf(stderr, "[d3d] fps limit off\n");
    init_modes();
    g_st.t0 = g_st.tlast = mono();
}
static void build_vtables(void);
static __thread int t_depth;
static void lock_(void) { pthread_once(&g_once, d3d_init); pthread_mutex_lock(&g_mx); t_depth++; if (!g_vt[1]) build_vtables(); }
static void unlock_(void) { if (--t_depth == 0 && g_gl == 1) gl_release(); pthread_mutex_unlock(&g_mx); }

/* ---- IUnknown / IDirect3DResource9 / IDirect3DBaseTexture9 ---- */
static const uint32_t g_iid[T_N] = { 0, 0x81BDCBCAu, 0xD0223B96u, 0x794950F2u, 0x85C31227u, 0xFFF32F81u, 0x2518526Cu,
    0x0CFBAF3Au, 0x24F416E6u, 0xB64BB1B5u, 0x7C9DD65Eu, 0xDD13C59Cu, 0xEFC5557Eu, 0x6D3BDBDCu, 0xD9771460u, 0xB07C4FE5u, 0x8BA5FB08u };
static uint32_t m_ok(CPU* c) { (void)c; return OK_; }
static uint32_t m_inv(CPU* c) { (void)c; return ERR_INVALIDCALL; }
static uint32_t m_zero(CPU* c) { (void)c; return 0; }
static uint32_t m_QueryInterface(CPU* c) {
    Obj* o = SELF; uint32_t riid = A(1), pp = A(2);
    if (!pp) return E_POINTER_;
    wr32(pp, 0);
    if (!o || !riid) return E_NOINTERFACE_;
    uint32_t d1 = rd32(riid);
    int ok = d1 == g_iid[o->type] || (d1 == 0 && rd32(riid + 4) == 0 && rd32(riid + 8) == 0xC0 && rd32(riid + 12) == 0x46000000u);
    if (!ok && o->type >= T_TEX && o->type <= T_IB && o->type != T_VOL) ok = d1 == 0x05EEC05Du;     /* IDirect3DResource9 */
    if (!ok && o->type >= T_TEX && o->type <= T_VOLTEX) ok = d1 == 0x580CA87Eu;                       /* IDirect3DBaseTexture9 */
    if (!ok) { static int n; if (n++ < 20) fprintf(stderr, "[d3d] %s::QueryInterface({%08x-...}) -> E_NOINTERFACE\n", tname[o->type], d1); return E_NOINTERFACE_; }
    addref(o); wr32(pp, o->g); return OK_;
}
static uint32_t m_AddRef(CPU* c) { Obj* o = SELF; return o ? addref(o) : 0; }
static uint32_t m_Release(CPU* c) {
    Obj* o = SELF; if (!o) return 0;
    uint32_t r = release(o);
    if (!r && o->type == T_DEV) {   /* the device's reference on its IDirect3D9 goes with it */
        Dev* d = (Dev*)o->ext; Obj* d3d = d->d3d; d->d3d = NULL;
        if (g_log) fprintf(stderr, "[d3d] device released\n");
        if (d3d) release(d3d);
    }
    return r;
}
static uint32_t m_GetDevice(CPU* c) { if (!A(1)) return ERR_INVALIDCALL; if (g_devobj) addref(g_devobj); return out_obj(A(1), g_devobj); }
static uint32_t m_GetPrivateData(CPU* c) { (void)c; return ERR_NOTFOUND; }
static uint32_t m_SetPriority(CPU* c) { Obj* o = SELF; if (!o) return 0; uint32_t old = o->prio; o->prio = A(1); return old; }
static uint32_t m_GetPriority(CPU* c) { Obj* o = SELF; return o ? o->prio : 0; }
static uint32_t m_GetType(CPU* c) {
    Obj* o = SELF; if (!o) return 0;
    switch (o->type) { case T_SURF: return 1; case T_VOL: return 2; case T_TEX: return 3; case T_VOLTEX: return 4; case T_CUBE: return 5;
                       case T_VB: return 6; case T_IB: return 7; case T_QUERY: return o->qtype; default: return 0; }
}
static uint32_t m_SetLOD(CPU* c) { Obj* o = SELF; if (!o) return 0; uint32_t old = o->lod; if (o->pool == 1) o->lod = A(1) < o->levels ? A(1) : o->levels - 1; return old; }
static uint32_t m_GetLOD(CPU* c) { Obj* o = SELF; return o ? o->lod : 0; }
static uint32_t m_GetLevelCount(CPU* c) { Obj* o = SELF; return o ? o->levels : 0; }
static uint32_t m_GetAutoGenFilterType(CPU* c) { (void)c; return 2; }

/* ---- resources ---- */
static Obj* mkres(int type, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
    Obj* o = newobj(type);
    o->w = w; o->h = h; o->d = d; o->levels = levels; o->usage = usage; o->fmt = fmt; o->pool = pool;
    return o;
}
static uint64_t res_bytes(Obj* o) { uint64_t s = 0; for (uint32_t l = 0; l < o->levels; l++) s += level_bytes(o, l); return o->type == T_CUBE ? 6 * s : s; }
static Hist g_rtfmt[48]; static int g_nrtfmt;   /* render target / depth formats (textures and surfaces) */
static void note_fmt(Obj* o) {
    uint64_t b = res_bytes(o);
    if (o->usage & 3) hist_add(g_rtfmt, &g_nrtfmt, 48, o->fmt, b); else hist_add(g_fmt, &g_nfmt, 96, o->fmt, b);
}
static uint32_t create_tex(int type, uint32_t w, uint32_t h, uint32_t d, uint32_t lv, uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t pp) {
    if (!pp) return ERR_INVALIDCALL;
    wr32(pp, 0);
    if (!w || !h || !d || w > 8192 || h > 8192 || d > 2048) return ERR_INVALIDCALL;
    if (!fmt_info(fmt)) { static int n; if (n++ < 20) fprintf(stderr, "[d3d] %s: unsupported format %s\n", g_cur, fmt_name(fmt)); return ERR_INVALIDCALL; }
    uint32_t full = full_levels(w, h, type == T_VOLTEX ? d : 1);
    if (usage & 0x400) lv = 1; else if (!lv || lv > full) lv = full;
    Obj* o = mkres(type, w, h, d, lv, usage, fmt, pool);
    note_fmt(o);
    if (g_log) fprintf(stderr, "[d3d] %s %ux%u%s%.0u x%u %s usage %x pool %u -> %08x\n", g_cur, w, h, type == T_VOLTEX ? "x" : "",
                       type == T_VOLTEX ? d : 0, lv, fmt_name(fmt), usage, pool, o->g);
    wr32(pp, o->g); return OK_;
}
static Obj* mksurf(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, uint32_t ms, uint32_t msq) {
    Obj* o = mkres(T_SURF, w, h, 1, 1, usage, fmt, pool); o->ms = ms; o->msq = msq;
    note_fmt(o);
    return o;
}
static uint32_t create_surf(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, uint32_t ms, uint32_t msq, uint32_t pp) {
    if (!pp) return ERR_INVALIDCALL;
    wr32(pp, 0);
    if (!w || !h || w > 8192 || h > 8192) return ERR_INVALIDCALL;
    if (!fmt_info(fmt)) { static int n; if (n++ < 20) fprintf(stderr, "[d3d] %s: unsupported format %s\n", g_cur, fmt_name(fmt)); return ERR_INVALIDCALL; }
    Obj* o = mksurf(w, h, fmt, usage, pool, ms, msq);
    if (g_log) fprintf(stderr, "[d3d] %s %ux%u %s ms %u -> %08x\n", g_cur, w, h, fmt_name(fmt), ms, o->g);
    wr32(pp, o->g); return OK_;
}
/* surface / volume of a texture level (created on first request; its refcount is the texture's) */
static Obj* child(Obj* t, uint32_t face, uint32_t lvl) {
    uint32_t i = face * t->levels + lvl;
    if (!t->sub) t->sub = calloc(sub_count(t), sizeof(Obj*));
    if (!t->sub[i]) {
        Obj* s = newobj(t->type == T_VOLTEX ? T_VOL : T_SURF);
        s->parent = t; s->owned = 1; s->refs = 0;
        s->w = dimv(t->w, lvl); s->h = dimv(t->h, lvl); s->d = dimv(t->d, lvl); s->levels = 1;
        s->usage = t->usage; s->fmt = t->fmt; s->pool = t->pool; s->face = face; s->level = lvl;
        t->sub[i] = s;
    }
    return t->sub[i];
}
static void surf_desc(uint32_t p, Obj* o, uint32_t w, uint32_t h) {
    wr32(p, o->fmt); wr32(p + 4, 1); wr32(p + 8, o->usage); wr32(p + 12, o->pool); wr32(p + 16, o->ms); wr32(p + 20, o->msq);
    wr32(p + 24, w); wr32(p + 28, h);
}
static void vol_desc(uint32_t p, Obj* o, uint32_t w, uint32_t h, uint32_t d) {
    wr32(p, o->fmt); wr32(p + 4, 2); wr32(p + 8, o->usage); wr32(p + 12, o->pool); wr32(p + 16, w); wr32(p + 20, h); wr32(p + 24, d);
}
/* locks: the bits live in guest memory; kept for SYSTEMMEM/SCRATCH/DYNAMIC resources, otherwise freed on unlock
 * (a real backend uploads them there) */
static int persistent(Obj* r) {
    /* v0.10.8: keep the guest copy of every locked texture/buffer, so a Vulkan device reset can re-upload
     * everything: D3D9 MANAGED would, and DEFAULT-pool textures filled once by the game (world textures, the
     * Bink staging) lose their only copy otherwise - after a recovery the world stays black. Render targets and
     * depth/stencil are excluded: they are cleared and redrawn by the game every frame. */
    if (r->type >= T_TEX && r->type <= T_VOLTEX) return !(r->usage & 0x101);
    if (r->type == T_VB || r->type == T_IB) return 1;   /* static buffers are filled once - keep them too */
    return r->pool >= 1 || (r->usage & 0x200);
}
static uint32_t lock_sub(Obj* r, uint32_t s, uint32_t lvl, uint32_t area, int vol, uint32_t out, uint32_t flags) {
    if (!out) return ERR_INVALIDCALL;
    uint32_t pitch, rows, unit; int blk;
    layout(r->fmt, dimv(r->w, lvl), dimv(r->h, lvl), &pitch, &rows, &unit, &blk);
    uint32_t bytes = level_bytes(r, lvl);
    if (!r->lockv) r->lockv = calloc(sub_count(r), 4);
    int fresh = !r->lockv[s];
    if (!r->lockv[s] && !(r->lockv[s] = galloc(bytes))) {
        fprintf(stderr, "[d3d] %s: out of guest memory (%u bytes)\n", g_cur, bytes); return 0x8876017Cu;   /* OUTOFVIDEOMEMORY */
    }
    if (g_gl == 1) {
        gl_note_lock(r, s, lvl, area, vol, flags);
        if (fresh && (r->usage & 1) && r->pool == 0) gl_rt_lock(r, s, lvl, r->lockv[s]);   /* render target: its pixels */
    }
    uint32_t p = r->lockv[s], k = blk ? 4 : 1;
    if (area) p += rd32(area + 4) / k * pitch + rd32(area) / k * unit + (vol ? rd32(area + 16) * pitch * rows : 0);
    g_st.locks++; g_st.lockbytes += bytes;
    if (vol) { wr32(out, pitch); wr32(out + 4, pitch * rows); wr32(out + 8, p); } else { wr32(out, pitch); wr32(out + 4, p); }
    return OK_;
}
static uint32_t unlock_sub(Obj* r, uint32_t s, uint32_t lvl) {
    if (!r->lockv || !r->lockv[s]) return OK_;
    if (g_gl == 1) gl_tex_unlock(r, s, lvl);
    if (!persistent(r)) { gfree(r->lockv[s], level_bytes(r, lvl)); r->lockv[s] = 0; }
    return OK_;
}
/* IDirect3DTexture9 */
static uint32_t m_Tex_GetLevelDesc(CPU* c) {
    Obj* o = SELF; uint32_t l = A(1);
    if (!o || l >= o->levels || !A(2)) return ERR_INVALIDCALL;
    surf_desc(A(2), o, dimv(o->w, l), dimv(o->h, l)); return OK_;
}
static uint32_t m_Tex_GetSurfaceLevel(CPU* c) {
    Obj* o = SELF; uint32_t l = A(1);
    if (!o || l >= o->levels || !A(2)) return ERR_INVALIDCALL;
    Obj* s = child(o, 0, l); addref(s); return out_obj(A(2), s);
}
static uint32_t m_Tex_LockRect(CPU* c) { Obj* o = SELF; uint32_t l = A(1); if (!o || l >= o->levels) return ERR_INVALIDCALL; return lock_sub(o, l, l, A(3), 0, A(2), A(4)); }
static uint32_t m_Tex_UnlockRect(CPU* c) { Obj* o = SELF; uint32_t l = A(1); if (!o || l >= o->levels) return ERR_INVALIDCALL; return unlock_sub(o, l, l); }
/* IDirect3DCubeTexture9 */
static uint32_t m_Cube_GetCubeMapSurface(CPU* c) {
    Obj* o = SELF; uint32_t f = A(1), l = A(2);
    if (!o || f >= 6 || l >= o->levels || !A(3)) return ERR_INVALIDCALL;
    Obj* s = child(o, f, l); addref(s); return out_obj(A(3), s);
}
static uint32_t m_Cube_LockRect(CPU* c) {
    Obj* o = SELF; uint32_t f = A(1), l = A(2);
    if (!o || f >= 6 || l >= o->levels) return ERR_INVALIDCALL;
    return lock_sub(o, f * o->levels + l, l, A(4), 0, A(3), A(5));
}
static uint32_t m_Cube_UnlockRect(CPU* c) {
    Obj* o = SELF; uint32_t f = A(1), l = A(2);
    if (!o || f >= 6 || l >= o->levels) return ERR_INVALIDCALL;
    return unlock_sub(o, f * o->levels + l, l);
}
/* IDirect3DVolumeTexture9 */
static uint32_t m_VolTex_GetLevelDesc(CPU* c) {
    Obj* o = SELF; uint32_t l = A(1);
    if (!o || l >= o->levels || !A(2)) return ERR_INVALIDCALL;
    vol_desc(A(2), o, dimv(o->w, l), dimv(o->h, l), dimv(o->d, l)); return OK_;
}
static uint32_t m_VolTex_GetVolumeLevel(CPU* c) {
    Obj* o = SELF; uint32_t l = A(1);
    if (!o || l >= o->levels || !A(2)) return ERR_INVALIDCALL;
    Obj* s = child(o, 0, l); addref(s); return out_obj(A(2), s);
}
static uint32_t m_VolTex_LockBox(CPU* c) { Obj* o = SELF; uint32_t l = A(1); if (!o || l >= o->levels) return ERR_INVALIDCALL; return lock_sub(o, l, l, A(3), 1, A(2), A(4)); }
static uint32_t m_VolTex_UnlockBox(CPU* c) { Obj* o = SELF; uint32_t l = A(1); if (!o || l >= o->levels) return ERR_INVALIDCALL; return unlock_sub(o, l, l); }
/* IDirect3DSurface9 / IDirect3DVolume9 */
static uint32_t m_Surf_GetContainer(CPU* c) {
    Obj* o = SELF; if (!o || !A(2)) return ERR_INVALIDCALL;
    Obj* p = o->parent ? o->parent : g_devobj;
    if (p) addref(p);
    return out_obj(A(2), p);
}
static uint32_t m_Surf_GetDesc(CPU* c) { Obj* o = SELF; if (!o || !A(1)) return ERR_INVALIDCALL; surf_desc(A(1), o, o->w, o->h); return OK_; }
static uint32_t m_Surf_LockRect(CPU* c) {
    Obj* o = SELF; if (!o) return ERR_INVALIDCALL;
    if (o->parent) return lock_sub(o->parent, o->face * o->parent->levels + o->level, o->level, A(2), 0, A(1), A(3));
    return lock_sub(o, 0, 0, A(2), 0, A(1), A(3));
}
static uint32_t m_Surf_UnlockRect(CPU* c) {
    Obj* o = SELF; if (!o) return ERR_INVALIDCALL;
    if (o->parent) return unlock_sub(o->parent, o->face * o->parent->levels + o->level, o->level);
    return unlock_sub(o, 0, 0);
}
static uint32_t m_Vol_GetDesc(CPU* c) { Obj* o = SELF; if (!o || !A(1)) return ERR_INVALIDCALL; vol_desc(A(1), o, o->w, o->h, o->d); return OK_; }
static uint32_t m_Vol_LockBox(CPU* c) { Obj* o = SELF; if (!o || !o->parent) return ERR_INVALIDCALL; return lock_sub(o->parent, o->level, o->level, A(2), 1, A(1), A(3)); }
static uint32_t m_Vol_UnlockBox(CPU* c) { Obj* o = SELF; if (!o || !o->parent) return ERR_INVALIDCALL; return unlock_sub(o->parent, o->level, o->level); }
/* IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9 */
static uint32_t create_buf(int type, uint32_t len, uint32_t usage, uint32_t fmt_fvf, uint32_t pool, uint32_t pp) {
    if (!pp) return ERR_INVALIDCALL;
    wr32(pp, 0);
    if (!len || len > 0x10000000u) return ERR_INVALIDCALL;
    Obj* o = newobj(type); o->size = len; o->usage = usage; o->pool = pool;
    if (type == T_VB) o->fvf = fmt_fvf; else o->fmt = fmt_fvf;
    if (persistent(o) && !(o->shadow = galloc(len))) { destroy(o); return 0x8876017Cu; }
    if (g_log) fprintf(stderr, "[d3d] %s %u bytes usage %x pool %u -> %08x\n", g_cur, len, usage, pool, o->g);
    wr32(pp, o->g); return OK_;
}
static uint32_t m_Buf_Lock(CPU* c) {
    Obj* o = SELF; uint32_t off = A(1), sz = A(2), pp = A(3);
    if (!o || !pp) return ERR_INVALIDCALL;
    if (off >= o->size) off = 0;
    uint32_t base;
    if (o->shadow) base = o->shadow;
    else {
        if (!o->lockv) o->lockv = calloc(1, 4);
        if (!o->lockv[0] && !(o->lockv[0] = galloc(o->size))) { wr32(pp, 0); return 0x8876017Cu; }
        base = o->lockv[0]; o->locks++;
    }
    {   /* range for the GL upload at the last Unlock */
        uint32_t end = sz && sz <= o->size - off ? off + sz : o->size;
        if (o->bend <= o->boff) { o->boff = off; o->bend = end; o->bflags = A(4); }
        else { if (off < o->boff) o->boff = off; if (end > o->bend) o->bend = end; o->bflags &= A(4); }
    }
    g_st.locks++; g_st.lockbytes += sz && sz <= o->size - off ? sz : o->size - off;
    wr32(pp, base + off); return OK_;
}
static uint32_t m_Buf_Unlock(CPU* c) {
    Obj* o = SELF;
    if (!o) return OK_;
    if (o->shadow) { o->boff = o->bend = 0; return OK_; }   /* dynamic: streamed from guest memory at draw time */
    if (o->locks > 0 && --o->locks == 0) {
        if (g_gl == 1) gl_buf_unlock(o, o->boff, o->bend, o->bflags);
        o->boff = o->bend = 0;
        gfree(o->lockv[0], o->size); o->lockv[0] = 0;
    }
    return OK_;
}
static uint32_t m_VB_GetDesc(CPU* c) {
    Obj* o = SELF; uint32_t p = A(1); if (!o || !p) return ERR_INVALIDCALL;
    wr32(p, 100); wr32(p + 4, 6); wr32(p + 8, o->usage); wr32(p + 12, o->pool); wr32(p + 16, o->size); wr32(p + 20, o->fvf); return OK_;
}
static uint32_t m_IB_GetDesc(CPU* c) {
    Obj* o = SELF; uint32_t p = A(1); if (!o || !p) return ERR_INVALIDCALL;
    wr32(p, o->fmt); wr32(p + 4, 7); wr32(p + 8, o->usage); wr32(p + 12, o->pool); wr32(p + 16, o->size); return OK_;
}
/* vertex declarations */
static uint32_t m_GetDeclaration(CPU* c) {
    Obj* o = SELF; uint32_t p = A(1), pn = A(2); if (!o || !pn) return ERR_INVALIDCALL;
    if (p) memcpy(M + p, o->blob, o->bloblen);
    wr32(pn, o->bloblen / 8); return OK_;
}
/* shaders: token stream length; SM2+ instructions carry their length, SM1 parameters have bit 31 set */
static uint32_t shader_len(uint32_t p) {
    uint32_t major = rd32(p) >> 8 & 0xFF, i = 1;
    while (i < 0x40000) {
        uint32_t t = rd32(p + 4 * i);
        if (t == 0x0000FFFFu) return i + 1;
        uint32_t op = t & 0xFFFF;
        if (op == 0xFFFE) { i += 1 + (t >> 16 & 0x7FFF); continue; }
        if (major >= 2) { i += 1 + (t >> 24 & 0xF); continue; }
        if (op == 0x51) { i += 6; continue; }
        i++; while (i < 0x40000 && (rd32(p + 4 * i) & 0x80000000u)) i++;
    }
    return 0;
}
static int shcrc_insert(uint32_t crc) {   /* 1 if new */
    int lo = 0, hi = g_nshcrc;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_shcrc[m] < crc) lo = m + 1; else hi = m; }
    if (lo < g_nshcrc && g_shcrc[lo] == crc) return 0;
    if (g_nshcrc == g_capshcrc) { g_capshcrc = g_capshcrc ? 2 * g_capshcrc : 1024; g_shcrc = realloc(g_shcrc, g_capshcrc * sizeof *g_shcrc); }
    memmove(g_shcrc + lo + 1, g_shcrc + lo, (g_nshcrc - lo) * sizeof *g_shcrc); g_shcrc[lo] = crc; g_nshcrc++;
    return 1;
}
/* ME1_D3D_DUMP: every distinct shader goes to <dir>/shaders.pak = "ME1SHPK1" + records { u32 crc, u32 bytes, bytecode }
 * (the version token is the bytecode's first dword). One buffered file: the game creates thousands of shaders at startup,
 * one file each cost seconds on the phone's storage. build/sm3_check translates a pack offline. */
static FILE* g_dumpf; static unsigned g_ndumped;
static void dump_shader(const Obj* o) {
    if (!g_dumpf) {
        static int failed; if (failed) return;
        char path[600]; snprintf(path, sizeof path, "%s/shaders.pak", g_dump);
        if (!(g_dumpf = fopen(path, "wb"))) { failed = 1; fprintf(stderr, "[d3d] cannot write %s\n", path); return; }
        setvbuf(g_dumpf, NULL, _IOFBF, 1 << 20);
        fwrite("ME1SHPK1", 1, 8, g_dumpf);
    }
    uint32_t h[2] = { o->crc, o->bloblen };
    fwrite(h, 4, 2, g_dumpf); fwrite(o->blob, 1, o->bloblen, g_dumpf);
    g_ndumped++;
}
static uint32_t create_shader(CPU* c, int type) {
    uint32_t f = A(1), pp = A(2);
    if (!f || !pp) return ERR_INVALIDCALL;
    wr32(pp, 0);
    uint32_t ver = rd32(f), n = shader_len(f);
    if ((ver >> 16) != (type == T_VS ? 0xFFFEu : 0xFFFFu) || !n) {
        fprintf(stderr, "[d3d] %s: bad shader bytecode (version %08x)\n", g_cur, ver); return ERR_INVALIDCALL;
    }
    Obj* o = newobj(type);
    o->bloblen = 4 * n; o->blob = malloc(o->bloblen); memcpy(o->blob, M + f, o->bloblen);
    o->crc = rt_crc32(o->blob, o->bloblen) ^ ver;
    hist_add(g_shv, &g_nshv, 16, ver, 1);
    if (shcrc_insert(o->crc) && g_dump) dump_shader(o);
    if (g_log > 1) fprintf(stderr, "[d3d] %s %s_%u_%u %u tokens crc %08x -> %08x\n", g_cur, type == T_VS ? "vs" : "ps", ver >> 8 & 0xFF, ver & 0xFF, n, o->crc, o->g);
    if (g_gl == 1) gl_shader_new(o);
    wr32(pp, o->g); return OK_;
}
static uint32_t m_GetFunction(CPU* c) {
    Obj* o = SELF; uint32_t p = A(1), pn = A(2); if (!o || !pn) return ERR_INVALIDCALL;
    if (p) memcpy(M + p, o->blob, o->bloblen);
    wr32(pn, o->bloblen); return OK_;
}
/* queries: events are always signalled (the D3D calls are executed in order anyway); occlusion queries (v1.1.3): the
 * backend measures the draws between BEGIN and END, GetData returns S_FALSE until the GPU has finished them */
static uint32_t m_Query_GetDataSize(CPU* c) { (void)c; return 4; }
static uint32_t q_area(const QRes* q) {   /* a "visible" pixel count without a measurement: 0.5% of the render target */
    uint32_t a = q && q->rtw ? q->rtw * q->rth : g_scrw * g_scrh;
    return a / 200 ? a / 200 : 1;
}
static uint32_t q_visible(const QRes* q) {   /* nothing could be measured: the query's last result (at least 1), else q_area */
    if (q && q->has_last) return q->last ? q->last : 1;
    return q_area(q);
}
static void q_done(QRes* q, uint32_t v, int measured) {
    q->st = 3; q->res = v; q->last = v; q->has_last = 1;
    if (measured) { g_qs.ready++; if (!v) g_qs.zero++; } else g_qs.fallback++;
}
static void q_close(Obj* o) {   /* Issue(END) of the open query (or BEGIN of another one) */
    QRes* q = o->gx; if (!q) return;
    if (g_gl == 1) gl_query_end(o);
    if (g_qopen == o) g_qopen = NULL;
    q->st = 2; q->polls = 0; q->t0 = 0;
    if (!q->att) q_done(q, 0, 1);                                    /* nothing drawn: 0 samples, as in D3D9 */
    else if (q->over || q->att != q->rec || !q->n) q_done(q, q_visible(q), 0);   /* a draw was skipped / not measurable */
}
static uint32_t m_Query_Issue(CPU* c) {
    Obj* o = SELF; uint32_t f = A(1); if (!o || o->type != T_QUERY) return ERR_INVALIDCALL;
    QRes* q = o->gx;
    if (o->qtype != 9 || !q) return OK_;
    if (f & 2) {   /* D3DISSUE_BEGIN */
        if (g_qopen) q_close(g_qopen);   /* one occlusion query at a time: BEGIN of another one ends it */
        q->st = 1; q->over = 0; q->n = q->att = q->rec = 0; q->polls = 0; q->t0 = 0; q->ctx = NULL; q->rtw = q->rth = 0;
        g_qopen = o; g_qs.issued++;
        if (g_gl == 1) gl_query_begin(o);
    } else if (f & 1) {   /* D3DISSUE_END */
        if (g_qopen == o) q_close(o);
        else if (q->st != 2) q_done(q, 0, 1);   /* END without BEGIN: an empty query */
    }
    return OK_;
}
static uint32_t m_Query_GetData(CPU* c) {
    Obj* o = SELF; uint32_t p = A(1), n = A(2); if (!o) return ERR_INVALIDCALL;
    uint32_t v = 1;
    if (o->qtype == 9) {
        QRes* q = o->gx;
        if (!q || !g_occl || g_gl != 1 || q->st == 0 || q->st == 1) v = q_visible(q);   /* never issued / still open */
        else if (q->st == 3) v = q->res;
        else if (!gl_query_data(o, A(3), &v)) {   /* not finished on the GPU */
            if (g_occl == 2) v = q_visible(q);
            else { g_qs.notready++; return 1; }   /* S_FALSE */
        }
    }
    if (p && n >= 4) wr32(p, v);
    return OK_;
}
/* ID3DXBuffer */
static uint32_t m_Buf_GetBufferPointer(CPU* c) { Obj* o = SELF; return o ? o->shadow : 0; }
static uint32_t m_Buf_GetBufferSize(CPU* c) { Obj* o = SELF; return o ? o->size : 0; }
static uint32_t mk_xbuf(const char* text) {
    Obj* o = newobj(T_BUF); o->size = (uint32_t)strlen(text) + 1; o->shadow = galloc(o->size);
    memcpy(M + o->shadow, text, o->size); return o->g;
}

/* ---- IDirect3D9 ---- */
static int is_display_fmt(uint32_t f) { return f == 22 || f == 23 || f == 24 || f == 35; }
static uint32_t m_GetAdapterCount(CPU* c) { (void)c; return 1; }
static uint32_t m_GetAdapterIdentifier(CPU* c) { if (A(1) || !A(3)) return ERR_INVALIDCALL; fill_identifier(A(3)); return OK_; }
static uint32_t m_GetAdapterModeCount(CPU* c) { return !A(1) && (A(2) == 22 || A(2) == 23) ? (uint32_t)g_nmodes : 0; }
static void mode_out(uint32_t p, unsigned w, unsigned h, uint32_t fmt) { wr32(p, w); wr32(p + 4, h); wr32(p + 8, 60); wr32(p + 12, fmt); }
static uint32_t m_EnumAdapterModes(CPU* c) {
    uint32_t f = A(2), i = A(3), p = A(4);
    if (A(1) || !p || (f != 22 && f != 23)) return ERR_INVALIDCALL;
    if (i >= (uint32_t)g_nmodes) return ERR_INVALIDCALL;
    mode_out(p, g_modes[i][0], g_modes[i][1], f); return OK_;
}
static uint32_t m_GetAdapterDisplayMode(CPU* c) { if (A(1) || !A(2)) return ERR_INVALIDCALL; mode_out(A(2), g_scrw, g_scrh, 22); return OK_; }
static uint32_t m_CheckDeviceType(CPU* c) {
    uint32_t af = A(3), bf = A(4), win = A(5);
    if (A(1)) return ERR_INVALIDCALL;
    if (!is_display_fmt(af) && !win) return ERR_NOTAVAILABLE;
    if (bf && bf != 21 && !is_display_fmt(bf)) return ERR_NOTAVAILABLE;
    return OK_;
}
static uint32_t m_CheckDeviceFormat(CPU* c) {
    if (A(1)) return ERR_INVALIDCALL;
    uint32_t r = check_format(A(4), A(5), A(6));
    if (g_log > 1 || (g_log && r)) fprintf(stderr, "[d3d] CheckDeviceFormat(usage %x, rtype %u, %s) -> %08x\n", A(4), A(5), fmt_name(A(6)), r);
    return r;
}
static uint32_t m_CheckDeviceMultiSampleType(CPU* c) {
    uint32_t f = A(3), ms = A(5), pq = A(6);
    const Fmt* fi = fmt_info(f);
    int ok = ms == 0 || ((ms == 2 || ms == 4) && fi && (fi->fl & (F_RT | F_DS)) && !fi->block && fi->bytes <= 8) || ms == 1;
    if (pq) wr32(pq, ok ? (ms == 1 ? 4 : 1) : 0);
    return ok ? OK_ : ERR_NOTAVAILABLE;
}
static uint32_t m_CheckDepthStencilMatch(CPU* c) { const Fmt* f = fmt_info(A(5)); return f && (f->fl & F_DS) ? OK_ : ERR_NOTAVAILABLE; }
static uint32_t m_CheckDeviceFormatConversion(CPU* c) {
    const Fmt* s = fmt_info(A(3)); const Fmt* t = fmt_info(A(4));
    return s && t && (t->fl & F_RT) && !(s->fl & F_DS) && !(t->fl & F_DS) ? OK_ : ERR_NOTAVAILABLE;
}
static uint32_t m_D3D_GetDeviceCaps(CPU* c) { if (A(1) || !A(3)) return ERR_INVALIDCALL; fill_caps(A(3)); return OK_; }
static uint32_t m_GetAdapterMonitor(CPU* c) { (void)c; return 0x00010001u; }

/* ---- device ---- */
static void wrf(uint32_t a, float f);
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void set_vp(Dev* d, uint32_t w, uint32_t h) {
    d->vp[0] = 0; d->vp[1] = 0; d->vp[2] = w; d->vp[3] = h; d->vp[4] = fbits(0.f); d->vp[5] = fbits(1.f);
    d->scissor[0] = 0; d->scissor[1] = 0; d->scissor[2] = w; d->scissor[3] = h;
}
static void reset_state(Dev* d) {
    for (int i = 0; i < 4; i++) bind(&d->rt[i], NULL);
    bind(&d->ds, NULL);
    for (int i = 0; i < 21; i++) bind(&d->tex[i], NULL);
    for (int i = 0; i < 16; i++) { bind(&d->vb[i], NULL); d->vboff[i] = d->vbstride[i] = 0; d->vbfreq[i] = 1; }
    bind(&d->ib, NULL); bind(&d->decl, NULL); bind(&d->vs, NULL); bind(&d->ps, NULL); d->fvf = 0;
    memset(d->rs, 0, sizeof d->rs);
    static const uint16_t def[][2] = { { 7, 1 }, { 8, 3 }, { 9, 2 }, { 14, 1 }, { 16, 1 }, { 19, 2 }, { 20, 1 }, { 22, 3 }, { 23, 4 },
        { 25, 8 }, { 53, 1 }, { 54, 1 }, { 55, 1 }, { 56, 8 }, { 136, 1 }, { 137, 1 }, { 139, 1 }, { 145, 1 }, { 146, 2 },
        { 161, 1 }, { 168, 15 }, { 171, 1 }, { 186, 1 }, { 187, 1 }, { 188, 1 }, { 189, 8 }, { 190, 15 }, { 191, 15 }, { 192, 15 },
        { 207, 2 }, { 208, 1 }, { 209, 1 } };
    for (unsigned i = 0; i < sizeof def / sizeof def[0]; i++) d->rs[def[i][0]] = def[i][1];
    d->rs[58] = d->rs[59] = d->rs[60] = d->rs[162] = d->rs[193] = 0xFFFFFFFFu;   /* stencil masks, texture factor, ms mask, blend factor */
    d->rs[154] = d->rs[155] = fbits(1.f); d->rs[166] = fbits(64.f); d->rs[147] = d->rs[148] = fbits(1.f);  /* point sizes, tween */
    d->rs[36] = fbits(1.f); d->rs[37] = fbits(1.f);  /* fog end/density */
    for (int s = 0; s < 21; s++) {
        memset(d->ss[s], 0, sizeof d->ss[s]);
        d->ss[s][1] = d->ss[s][2] = d->ss[s][3] = 1; d->ss[s][5] = d->ss[s][6] = 1; d->ss[s][10] = 1;
    }
    for (int t = 0; t < 8; t++) {
        memset(d->tss[t], 0, sizeof d->tss[t]);
        d->tss[t][1] = t ? 1 : 4; d->tss[t][2] = 2; d->tss[t][3] = 1; d->tss[t][4] = t ? 1 : 2; d->tss[t][5] = 2; d->tss[t][6] = 1;
        d->tss[t][11] = t; d->tss[t][26] = d->tss[t][27] = d->tss[t][28] = 1;
    }
    memset(d->xf, 0, sizeof d->xf);
    for (int m = 0; m < 512; m++) for (int k = 0; k < 4; k++) d->xf[m][k * 5] = 1.f;
    memset(d->vsf, 0, sizeof d->vsf); memset(d->psf, 0, sizeof d->psf); memset(d->vsi, 0, sizeof d->vsi); memset(d->psi, 0, sizeof d->psi);
    memset(d->vsb, 0, sizeof d->vsb); memset(d->psb, 0, sizeof d->psb); memset(d->clip, 0, sizeof d->clip); memset(d->mat, 0, sizeof d->mat);
    Obj* s = d->swap;
    bind(&d->rt[0], s->bb); bind(&d->ds, d->autods);
    set_vp(d, s->pp[0], s->pp[1]);
    d->recording = 0;
}
static void read_pp(Obj* s, uint32_t ppp) {
    for (int i = 0; i < 14; i++) s->pp[i] = rd32(ppp + 4 * i);
    if (!s->pp[0]) s->pp[0] = g_scrw;
    if (!s->pp[1]) s->pp[1] = g_scrh;
    if (!s->pp[2]) s->pp[2] = 22;
    if (!s->pp[3]) s->pp[3] = 1;
    for (int i = 0; i < 4; i++) wr32(ppp + 4 * i, s->pp[i]);
    s->bb = mksurf(s->pp[0], s->pp[1], s->pp[2], 1, 0, s->pp[4], s->pp[5]); s->bb->owned = 1; s->bb->refs = 0;
}
static Obj* mk_swap(uint32_t ppp) { Obj* s = newobj(T_SWAP); read_pp(s, ppp); return s; }
static void mk_autods(Dev* d) {
    Obj* s = d->swap; d->autods = NULL;
    if (s->pp[9]) { d->autods = mksurf(s->pp[0], s->pp[1], s->pp[10], 2, 0, s->pp[4], s->pp[5]); d->autods->owned = 1; d->autods->refs = 0; }
}
static void log_pp(const char* what, Obj* s) {
    fprintf(stderr, "[d3d] %s: %ux%u %s x%u, ms %u/%u, swap %u, %s, depth %s, flags %x, %u Hz, interval %x\n", what,
            s->pp[0], s->pp[1], fmt_name(s->pp[2]), s->pp[3], s->pp[4], s->pp[5], s->pp[6], s->pp[8] ? "windowed" : "fullscreen",
            s->pp[9] ? fmt_name(s->pp[10]) : "none", s->pp[11], s->pp[12], s->pp[13]);
}
static uint32_t m_CreateDevice(CPU* c) {
    Obj* d3d = SELF; uint32_t ppp = A(5), out = A(6);
    if (!ppp || !out) return ERR_INVALIDCALL;
    wr32(out, 0);
    if (A(1)) return ERR_INVALIDCALL;
    Obj* o = newobj(T_DEV); Dev* d = calloc(1, sizeof *d);
    o->ext = d; o->owned = 1; d->self = o; d->d3d = d3d; if (d3d) addref(d3d);
    d->devtype = A(2); d->hwnd = A(3); d->flags = A(4); d->cursor = 1;
    d->swap = mk_swap(ppp); d->swap->owned = 1;
    mk_autods(d);
    reset_state(d);
    for (int i = 0; i < 256; i++) d->gamma[i] = d->gamma[256 + i] = d->gamma[512 + i] = (uint16_t)(i * 257);
    g_devobj = o;
    log_pp("CreateDevice", d->swap);
    if (g_gl < 0) g_gl = gl_init();
    fprintf(stderr, "[d3d] device %08x (behavior %x); %s\n", o->g, d->flags, g_gl == 1 ? (g_vk ? "Vulkan renderer" : "GLES backend") : "null backend: frames are not drawn");
    wr32(out, o->g); return OK_;
}
static uint32_t m_Reset(CPU* c) {
    Dev* d = DEVOF(SELF); uint32_t ppp = A(1);
    if (!d || !ppp) return ERR_INVALIDCALL;
    if (g_d3dlost && g_vk) {   /* v0.10.7: the game reacts to D3DERR_DEVICELOST - re-create the Vulkan device */
        if (g_vklost && !vk_device_reset()) return D3DERR_DEVICELOST_;
        g_d3dlost = 0;
        fprintf(stderr, "[d3d] Reset: device re-created, rendering continues\n");
    }
    Obj* s = d->swap;
    for (int i = 0; i < 4; i++) bind(&d->rt[i], NULL);
    bind(&d->ds, NULL);
    Obj* bb = s->bb; s->bb = NULL; disown(bb);
    Obj* ds = d->autods; d->autods = NULL; disown(ds);
    read_pp(s, ppp); mk_autods(d); reset_state(d);
    log_pp("Reset", s);
    return OK_;
}
static double fps_limit(void) {   /* v0.10.13: returns the seconds slept (for the ADPF work-duration report) */
    double t = mono(), per = 1.0 / g_fpslimit, slept = 0;
    if (g_fpsnext <= 0 || t > g_fpsnext + 0.25) g_fpsnext = t;   /* first frame or a hitch: do not try to catch up */
    else if (t < g_fpsnext) {
        double w = g_fpsnext - t;
        int rel = t_depth == 1;   /* outermost lock: let other threads (streaming, audio) use the device while this one sleeps */
        if (rel) pthread_mutex_unlock(&g_mx);
        struct timespec ts = { (time_t)w, (long)((w - (time_t)w) * 1e9) }; nanosleep(&ts, NULL);
        if (rel) pthread_mutex_lock(&g_mx);
        slept = w;
    }
    g_fpsnext += per;
    return slept;
}
void me1_perf(int64_t o[4]);
void me1_perf(int64_t o[4]) { for (int i = 0; i < 4; i++) o[i] = g_perf[i]; }
/* v0.10.4 PhoneModify caps: draws/prims beyond the modifier's per-frame limits are skipped */
static unsigned g_cap_draws, g_cap_prims, g_cap_skipped, g_cap_last_skipped;
static uint32_t present(Obj* bb) {
    if (g_d3dlost) {   /* v0.10.7: the game has 5 s to call Reset; otherwise we re-create the device ourselves */
        if (!g_vk || mono() - g_d3dlostt < 5.0 || !vk_device_reset()) return D3DERR_DEVICELOST_;
        g_d3dlost = 0;
    }
    hle_cpu_place(1);   /* the render thread (no-op after the first frame of a thread) */
    if (g_gl == 1) gl_present(bb);
    double slept = g_fpslimit ? fps_limit() : 0;
    hle_adpf_frame(g_fpslimit, slept);   /* v0.10.13: ADPF hint (Android 12+; ME1_ADPF=0 disables) */
    g_st.frames++;
    double t = mono();
    if (t - g_pft0 >= 0.5) {
        if (g_pft0 > 0) {
            uint64_t nf = g_st.frames - g_pff0; double dt = t - g_pft0;
            g_perf[0] = (int64_t)(nf / dt * 10 + 0.5); g_perf[1] = nf ? (int64_t)(dt * 1000 / nf * 10 + 0.5) : 0;
        }
        g_perf[2] = g_gl != 1 ? 0 : g_vk ? 3 : g_zink ? 2 : 1; g_perf[3] = g_fpslimit;
        g_pft0 = t; g_pff0 = g_st.frames;
    }
    if (g_st.frames == 1) { g_tfirst = t; fprintf(stderr, "[d3d] first Present, %.1f s after Direct3DCreate9\n", t - g_st.t0); }
    if (t - g_st.tlast >= 2.0) {
        hle_cpu_tidy();   /* v0.10.2: unpin driver/presenter/audio threads that inherited a pinned mask */
        hle_mod_apply();  /* v0.10.4: PhoneModify patches whose guest address is mapped by now */
        uint64_t nf = g_st.frames - g_st.flast; double dt = t - g_st.tlast;
        uint32_t nres; uint64_t resv, peak; hle_vmem_stats(&nres, &resv, &peak);
        unsigned alive = 0; for (int i = 1; i < T_N; i++) alive += g_st.alive[i];
        char gt[320] = ""; if (g_gl == 1) gl_frame_times(gt, sizeof gt, nf);
        char cap[200] = ""; size_t ck = 0; if (g_cap_last_skipped) ck = (size_t)snprintf(cap, sizeof cap, ", capped %u draws", g_cap_last_skipped);
        if (g_qs.issued != g_qs0.issued && nf) {   /* v1.1.3: occlusion queries per frame, share hidden, time spent waiting for results */
            double qi = (double)(g_qs.issued - g_qs0.issued), qr = (double)(g_qs.ready - g_qs0.ready);
            snprintf(cap + ck, sizeof cap - ck, ", occlusion %.0f/frame (%.0f%% hidden, %.0f%% unmeasured, %.1f ms/frame waits, %.1f flushes/frame)",
                     qi / nf, qr > 0 ? 100.0 * (double)(g_qs.zero - g_qs0.zero) / qr : 0.0, 100.0 * (double)(g_qs.fallback - g_qs0.fallback) / qi,
                     (g_qs.wait_s - g_qs0.wait_s) * 1000.0 / nf, (double)(g_qs.flushes - g_qs0.flushes) / nf);
            g_qs0 = g_qs;
        }
        long rss = 0; { FILE* f = fopen("/proc/self/status", "r"); char l[128];
                         while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) { rss = atol(l + 6); break; }
                         if (f) fclose(f); }   /* v0.10.12 (P2-7): process memory in the frame log */
        fprintf(stderr, "[d3d] frame %llu: %.1f fps, %.0f draws/frame, %.0f prims/frame, %u objects, vmem %llu MB (peak %llu), RSS %ld MB, limit %d%s%s\n",
                (unsigned long long)g_st.frames, nf / dt, (double)(g_st.draws - g_st.fdraws) / nf, (double)(g_st.prims - g_st.fprims) / nf,
                alive, (unsigned long long)(resv >> 20), (unsigned long long)(peak >> 20), rss / 1024, g_fpslimit, gt, cap);
        g_st.tlast = t; g_st.flast = g_st.frames; g_st.fdraws = g_st.draws; g_st.fprims = g_st.prims;
    }
    g_cap_last_skipped = g_cap_skipped; g_cap_skipped = 0; g_cap_draws = g_cap_prims = 0;   /* v0.10.4: next frame's caps */
    if ((g_maxframes && g_st.frames >= g_maxframes) || (g_maxsecs > 0 && t - g_tfirst >= g_maxsecs)) {
        fprintf(stderr, "[d3d] frame limit reached (%llu frames, %.1f s after the first), exiting\n", (unsigned long long)g_st.frames, t - g_tfirst);
        hle_report(); fflush(stdout); fflush(stderr); _exit(0);
    }
    return OK_;
}
static uint32_t m_TestCooperativeLevel(CPU* c) { (void)c; return g_d3dlost ? D3DERR_DEVICELOST_ : OK_; }   /* v0.10.7 */
static uint32_t m_GetAvailableTextureMem(CPU* c) { (void)c; return g_vram >= 4095 ? 0xFFF00000u : g_vram << 20; }
static uint32_t m_GetDirect3D(CPU* c) { Dev* d = DEVOF(SELF); if (!d || !A(1)) return ERR_INVALIDCALL; if (d->d3d) addref(d->d3d); return out_obj(A(1), d->d3d); }
static uint32_t m_Dev_GetDeviceCaps(CPU* c) { if (!A(1)) return ERR_INVALIDCALL; fill_caps(A(1)); return OK_; }
static uint32_t m_Dev_GetDisplayMode(CPU* c) {
    Dev* d = DEVOF(SELF); if (!d || !A(2)) return ERR_INVALIDCALL;
    if (d->swap->pp[8]) mode_out(A(2), g_scrw, g_scrh, 22); else mode_out(A(2), d->swap->pp[0], d->swap->pp[1], 22);
    return OK_;
}
static uint32_t m_GetCreationParameters(CPU* c) {
    Dev* d = DEVOF(SELF); uint32_t p = A(1); if (!d || !p) return ERR_INVALIDCALL;
    wr32(p, 0); wr32(p + 4, d->devtype); wr32(p + 8, d->hwnd); wr32(p + 12, d->flags); return OK_;
}
static uint32_t m_Dev_ShowCursor(CPU* c) { Dev* d = DEVOF(SELF); if (!d) return 0; uint32_t o = d->cursor; d->cursor = A(1) != 0; return o; }
static uint32_t m_CreateAdditionalSwapChain(CPU* c) {
    uint32_t ppp = A(1), pp = A(2); if (!ppp || !pp) return ERR_INVALIDCALL;
    Obj* s = mk_swap(ppp); log_pp("CreateAdditionalSwapChain", s); wr32(pp, s->g); return OK_;
}
static uint32_t m_GetSwapChain(CPU* c) {
    Dev* d = DEVOF(SELF); if (!d || !A(2)) return ERR_INVALIDCALL;
    if (A(1)) { wr32(A(2), 0); return ERR_INVALIDCALL; }
    addref(d->swap); return out_obj(A(2), d->swap);
}
static uint32_t m_one(CPU* c) { (void)c; return 1; }
static uint32_t m_Dev_Present(CPU* c) { if (g_d3dlost) return D3DERR_DEVICELOST_; Dev* d = DEVOF(SELF); return present(d && d->swap ? d->swap->bb : NULL); }
static uint32_t m_Dev_GetBackBuffer(CPU* c) {
    Dev* d = DEVOF(SELF); uint32_t pp = A(4); if (!d || !pp) return ERR_INVALIDCALL;
    if (A(1) || A(2) >= d->swap->pp[3]) { wr32(pp, 0); return ERR_INVALIDCALL; }
    addref(d->swap->bb); return out_obj(pp, d->swap->bb);
}
static uint32_t m_Dev_GetRasterStatus(CPU* c) { if (!A(2)) return ERR_INVALIDCALL; wr32(A(2), 0); wr32(A(2) + 4, 0); return OK_; }
static uint32_t m_SetGammaRamp(CPU* c) { Dev* d = DEVOF(SELF); if (d && A(3)) memcpy(d->gamma, M + A(3), sizeof d->gamma); return 0; }
static uint32_t m_GetGammaRamp(CPU* c) { Dev* d = DEVOF(SELF); if (d && A(2)) memcpy(M + A(2), d->gamma, sizeof d->gamma); return 0; }
static uint32_t m_CreateTexture(CPU* c) { return create_tex(T_TEX, A(1), A(2), 1, A(3), A(4), A(5), A(6), A(7)); }
static uint32_t m_CreateVolumeTexture(CPU* c) { return create_tex(T_VOLTEX, A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8)); }
static uint32_t m_CreateCubeTexture(CPU* c) { return create_tex(T_CUBE, A(1), A(1), 1, A(2), A(3), A(4), A(5), A(6)); }
static uint32_t m_CreateVertexBuffer(CPU* c) { return create_buf(T_VB, A(1), A(2), A(3), A(4), A(5)); }
static uint32_t m_CreateIndexBuffer(CPU* c) { return create_buf(T_IB, A(1), A(2), A(3), A(4), A(5)); }
static uint32_t m_CreateRenderTarget(CPU* c) { return create_surf(A(1), A(2), A(3), 1, 0, A(4), A(5), A(7)); }
static uint32_t m_CreateDepthStencilSurface(CPU* c) { return create_surf(A(1), A(2), A(3), 2, 0, A(4), A(5), A(7)); }
static uint32_t m_CreateOffscreenPlainSurface(CPU* c) { return create_surf(A(1), A(2), A(3), 0, A(4), 0, 0, A(5)); }
static uint32_t m_SetRenderTarget(CPU* c) {
    Dev* d = DEVOF(SELF); uint32_t i = A(1); Obj* s = O(A(2));
    if (!d || i >= 4 || (i == 0 && !s)) return ERR_INVALIDCALL;
    bind(&d->rt[i], s);
    if (i == 0) set_vp(d, s->w, s->h);
    return OK_;
}
static uint32_t m_GetRenderTarget(CPU* c) {
    Dev* d = DEVOF(SELF); uint32_t i = A(1), pp = A(2);
    if (!d || i >= 4 || !pp) return ERR_INVALIDCALL;
    if (!d->rt[i]) { wr32(pp, 0); return ERR_NOTFOUND; }
    addref(d->rt[i]); return out_obj(pp, d->rt[i]);
}
static uint32_t m_SetDepthStencilSurface(CPU* c) { Dev* d = DEVOF(SELF); if (!d) return ERR_INVALIDCALL; bind(&d->ds, O(A(1))); return OK_; }
static uint32_t m_GetDepthStencilSurface(CPU* c) {
    Dev* d = DEVOF(SELF); uint32_t pp = A(1); if (!d || !pp) return ERR_INVALIDCALL;
    if (!d->ds) { wr32(pp, 0); return ERR_NOTFOUND; }
    addref(d->ds); return out_obj(pp, d->ds);
}
static uint32_t m_Clear(CPU* c) {
    Dev* d = DEVOF(SELF); g_st.clears++;
    if (d && g_gl == 1) gl_clear(d, A(1), A(2), A(3), A(4), A(5), A(6));
    return OK_;
}
static Obj* surf_arg(uint32_t g) { Obj* o = O(g); return o && o->type == T_SURF ? o : NULL; }
static uint32_t m_StretchRect(CPU* c) {
    Obj* s = surf_arg(A(1)); Obj* t = surf_arg(A(3)); if (!s || !t) return ERR_INVALIDCALL;
    if (g_gl == 1) gl_stretch(s, A(2), t, A(4), A(5));
    return OK_;
}
static uint32_t m_ColorFill(CPU* c) { Obj* s = surf_arg(A(1)); if (!s) return ERR_INVALIDCALL; if (g_gl == 1) gl_colorfill(s, A(2), A(3)); return OK_; }
static uint32_t m_UpdateSurface(CPU* c) {
    Obj* s = surf_arg(A(1)); Obj* t = surf_arg(A(3)); if (!s || !t) return ERR_INVALIDCALL;
    if (g_gl == 1) gl_update_surface(s, A(2), t, A(4));
    return OK_;
}
static uint32_t m_UpdateTexture(CPU* c) {
    Obj* s = O(A(1)); Obj* t = O(A(2)); if (!s || !t || s->type != t->type || s->type < T_TEX || s->type > T_VOLTEX) return ERR_INVALIDCALL;
    if (g_gl == 1) gl_update_texture(s, t);
    return OK_;
}
static uint32_t m_GetRenderTargetData(CPU* c) {
    Obj* s = surf_arg(A(1)); Obj* t = surf_arg(A(2)); if (!s || !t) return ERR_INVALIDCALL;
    if (g_gl == 1) gl_get_rt_data(s, t);
    return OK_;
}
static uint32_t m_SetTransform(CPU* c) { Dev* d = DEVOF(SELF); uint32_t s = A(1); if (!d || !A(2)) return ERR_INVALIDCALL; if (s < 512) memcpy(d->xf[s], M + A(2), 64); return OK_; }
static uint32_t m_GetTransform(CPU* c) { Dev* d = DEVOF(SELF); uint32_t s = A(1); if (!d || !A(2) || s >= 512) return ERR_INVALIDCALL; memcpy(M + A(2), d->xf[s], 64); return OK_; }
static uint32_t m_MultiplyTransform(CPU* c) {
    Dev* d = DEVOF(SELF); uint32_t s = A(1); if (!d || !A(2) || s >= 512) return ERR_INVALIDCALL;
    float m[16], r[16]; memcpy(m, M + A(2), 64);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float v = 0; for (int k = 0; k < 4; k++) v += m[i * 4 + k] * d->xf[s][k * 4 + j]; r[i * 4 + j] = v; }
    memcpy(d->xf[s], r, 64); return OK_;
}
static uint32_t m_SetViewport(CPU* c) { Dev* d = DEVOF(SELF); if (!d || !A(1)) return ERR_INVALIDCALL; memcpy(d->vp, M + A(1), 24); return OK_; }
static uint32_t m_GetViewport(CPU* c) { Dev* d = DEVOF(SELF); if (!d || !A(1)) return ERR_INVALIDCALL; memcpy(M + A(1), d->vp, 24); return OK_; }
static uint32_t m_SetMaterial(CPU* c) { Dev* d = DEVOF(SELF); if (!d || !A(1)) return ERR_INVALIDCALL; memcpy(d->mat, M + A(1), 68); return OK_; }
static uint32_t m_GetMaterial(CPU* c) { Dev* d = DEVOF(SELF); if (!d || !A(1)) return ERR_INVALIDCALL; memcpy(M + A(1), d->mat, 68); return OK_; }
static uint32_t m_GetLightEnable(CPU* c) { if (!A(2)) return ERR_INVALIDCALL; wr32(A(2), 0); return OK_; }
static uint32_t m_SetClipPlane(CPU* c) { Dev* d = DEVOF(SELF); if (!d || A(1) >= 8 || !A(2)) return ERR_INVALIDCALL; memcpy(d->clip[A(1)], M + A(2), 16); return OK_; }
static uint32_t m_GetClipPlane(CPU* c) { Dev* d = DEVOF(SELF); if (!d || A(1) >= 8 || !A(2)) return ERR_INVALIDCALL; memcpy(M + A(2), d->clip[A(1)], 16); return OK_; }
static uint32_t m_SetRenderState(CPU* c) { Dev* d = DEVOF(SELF); if (d) fdev_set(A(0), d); if (d && A(1) < 256) { if (d->rs[A(1)] == A(2)) return OK_; d->rs[A(1)] = A(2); } return OK_; }   /* v0.10.6: redundant values skipped */
static uint32_t m_GetRenderState(CPU* c) { Dev* d = DEVOF(SELF); if (!d || !A(2)) return ERR_INVALIDCALL; wr32(A(2), A(1) < 256 ? d->rs[A(1)] : 0); return OK_; }
static uint32_t m_CreateStateBlock(CPU* c) { uint32_t pp = A(2); if (!pp) return ERR_INVALIDCALL; Obj* o = newobj(T_SB); wr32(pp, o->g); return OK_; }
static uint32_t m_BeginStateBlock(CPU* c) { Dev* d = DEVOF(SELF); if (d) d->recording = 1; return OK_; }
static uint32_t m_EndStateBlock(CPU* c) {
    Dev* d = DEVOF(SELF); uint32_t pp = A(1); if (!d || !pp) return ERR_INVALIDCALL;
    d->recording = 0; Obj* o = newobj(T_SB); wr32(pp, o->g); return OK_;
}
static uint32_t m_GetClipStatus(CPU* c) { if (!A(1)) return ERR_INVALIDCALL; wr32(A(1), 0); wr32(A(1) + 4, 0); return OK_; }
static int smap(uint32_t s) { return s < 16 ? (int)s : s >= 256 && s <= 260 ? (int)(s - 240) : -1; }
static uint32_t m_GetTexture(CPU* c) {
    Dev* d = DEVOF(SELF); int s = smap(A(1)); uint32_t pp = A(2);
    if (!d || s < 0 || !pp) return ERR_INVALIDCALL;
    if (d->tex[s]) addref(d->tex[s]);
    wr32(pp, d->tex[s] ? d->tex[s]->g : 0); return OK_;
}
static uint32_t m_SetTexture(CPU* c) {
    Dev* d = DEVOF(SELF); int s = smap(A(1)); if (!d || s < 0) return ERR_INVALIDCALL;
    Obj* t = O(A(2)); if (t && (t->type < T_TEX || t->type > T_VOLTEX)) return ERR_INVALIDCALL;
    bind(&d->tex[s], t); return OK_;
}
static uint32_t m_GetTextureStageState(CPU* c) {
    Dev* d = DEVOF(SELF); if (!d || !A(3) || A(1) >= 8 || A(2) >= 33) return ERR_INVALIDCALL;
    wr32(A(3), d->tss[A(1)][A(2)]); return OK_;
}
static uint32_t m_SetTextureStageState(CPU* c) { Dev* d = DEVOF(SELF); if (d && A(1) < 8 && A(2) < 33) { if (d->tss[A(1)][A(2)] == A(3)) return OK_; d->tss[A(1)][A(2)] = A(3); } return OK_; }
static uint32_t m_GetSamplerState(CPU* c) {
    Dev* d = DEVOF(SELF); int s = smap(A(1)); if (!d || s < 0 || !A(3) || A(2) >= 14) return ERR_INVALIDCALL;
    wr32(A(3), d->ss[s][A(2)]); return OK_;
}
static uint32_t m_SetSamplerState(CPU* c) { Dev* d = DEVOF(SELF); int s = smap(A(1)); if (d) fdev_set(A(0), d); if (d && s >= 0 && A(2) < 14) { if (d->ss[s][A(2)] == A(3)) return OK_; d->ss[s][A(2)] = A(3); } return OK_; }   /* v0.10.6: redundant values skipped */
/* v0.10.13 (R1): redundant Set{Render,Sampler}State without taking the device mutex (most of these calls are redundant).
   Only the device the locked path has seen, only when the cached value already equals the new one; ME1_D3D_FASTSTATE=0 disables. */
static int g_faststate = -1;
static Dev* fdev_get(CPU* c) {
    if (g_faststate < 0) { const char* e = getenv("ME1_D3D_FASTSTATE"); g_faststate = !(e && *e == '0'); }
    if (!g_faststate || g_log) return NULL;
    Dev* d = __atomic_load_n(&g_fdev, __ATOMIC_ACQUIRE);
    if (!d || __atomic_load_n(&g_fdev_g, __ATOMIC_ACQUIRE) != A(0) || __atomic_load_n(&g_fdev, __ATOMIC_ACQUIRE) != d) return NULL;
    return d;
}
static int rs_fast(CPU* c) { Dev* d = fdev_get(c); return d && A(1) < 256 && __atomic_load_n(&d->rs[A(1)], __ATOMIC_RELAXED) == A(2); }
static int ss_fast(CPU* c) { Dev* d = fdev_get(c); int s = smap(A(1)); return d && s >= 0 && A(2) < 14 && __atomic_load_n(&d->ss[s][A(2)], __ATOMIC_RELAXED) == A(3); }
static uint32_t m_ValidateDevice(CPU* c) { if (A(1)) wr32(A(1), 1); return OK_; }
static uint32_t m_GetCurrentTexturePalette(CPU* c) { if (!A(1)) return ERR_INVALIDCALL; wr32(A(1), 0xFFFF); return OK_; }
static uint32_t m_SetScissorRect(CPU* c) { Dev* d = DEVOF(SELF); if (!d || !A(1)) return ERR_INVALIDCALL; memcpy(d->scissor, M + A(1), 16); return OK_; }
static uint32_t m_GetScissorRect(CPU* c) { Dev* d = DEVOF(SELF); if (!d || !A(1)) return ERR_INVALIDCALL; memcpy(M + A(1), d->scissor, 16); return OK_; }
static uint32_t m_GetNPatchMode(CPU* c) { fpush(c, 0.0); return 0; }
static const char* const ptname[7] = { "?", "points", "lines", "linestrip", "tris", "tristrip", "trifan" };
static int draw(const char* what, uint32_t pt, uint32_t n) {   /* v0.10.12 (P0-5): 0 = cut by the PhoneModify cap */
    unsigned md = hle_mod_maxdraws(), mp = hle_mod_maxprims();
    if (g_qopen) { QRes* q = g_qopen->gx; q->att++; }   /* v1.1.3: a draw inside an occlusion query */
    if ((md && g_cap_draws >= md) || (mp && g_cap_prims + n > mp)) { g_cap_skipped++; return 0; }
    g_cap_draws++; g_cap_prims += n;
    g_st.draws++; g_st.prims += n;
    if (g_log > 1) fprintf(stderr, "[d3d] %s %s x%u\n", what, pt < 7 ? ptname[pt] : "?", n);
    return 1;
}
static uint32_t m_DrawPrimitive(CPU* c) {
    Dev* d = DEVOF(SELF); int go = draw("DrawPrimitive", A(1), A(3));
    if (go && d && g_gl == 1) gl_draw_x(d, A(1), A(3), 0, 0, 0, 0, A(2), 0, 0, 0, 0, 0);
    return OK_;
}
static uint32_t m_DrawIndexedPrimitive(CPU* c) {
    Dev* d = DEVOF(SELF); int go = draw("DrawIndexedPrimitive", A(1), A(6));
    if (go && d && g_gl == 1) gl_draw_x(d, A(1), A(6), 1, (int32_t)A(2), A(3), A(4), A(5), 0, 0, 0, 0, 0);
    return OK_;
}
static uint32_t m_DrawPrimitiveUP(CPU* c) {
    Dev* d = DEVOF(SELF); g_st.updraws++; int go = draw("DrawPrimitiveUP", A(1), A(2));
    if (go && d && g_gl == 1 && A(3)) gl_draw_x(d, A(1), A(2), 0, 0, 0, 0, 0, 1, A(3), A(4), 0, 0);
    if (d) { bind(&d->vb[0], NULL); d->vboff[0] = d->vbstride[0] = 0; }   /* D3D9 semantics after *UP: always unbind */
    return OK_;
}
static uint32_t m_DrawIndexedPrimitiveUP(CPU* c) {
    Dev* d = DEVOF(SELF); g_st.updraws++; int go = draw("DrawIndexedPrimitiveUP", A(1), A(4));
    if (go && d && g_gl == 1 && A(5) && A(7)) gl_draw_x(d, A(1), A(4), 1, 0, A(2), A(3), 0, 1, A(7), A(8), A(5), A(6));
    if (d) { bind(&d->vb[0], NULL); d->vboff[0] = d->vbstride[0] = 0; bind(&d->ib, NULL); }
    return OK_;
}
static uint32_t m_CreateVertexDeclaration(CPU* c) {
    uint32_t e = A(1), pp = A(2); if (!e || !pp) return ERR_INVALIDCALL;
    uint32_t n = 0; while (n < 64 && rd16(e + 8 * n) != 0xFF) n++;
    if (n == 64) { wr32(pp, 0); return ERR_INVALIDCALL; }
    Obj* o = newobj(T_DECL); o->bloblen = 8 * (n + 1); o->blob = malloc(o->bloblen); memcpy(o->blob, M + e, o->bloblen);
    wr32(pp, o->g); return OK_;
}
#define SETOBJ(fn, field, ty) static uint32_t fn(CPU* c) { \
    Dev* d = DEVOF(SELF); if (!d) return ERR_INVALIDCALL; Obj* o = O(A(1)); if (o && o->type != ty) return ERR_INVALIDCALL; bind(&d->field, o); return OK_; }
#define GETOBJ(fn, field) static uint32_t fn(CPU* c) { \
    Dev* d = DEVOF(SELF); if (!d || !A(1)) return ERR_INVALIDCALL; if (d->field) addref(d->field); wr32(A(1), d->field ? d->field->g : 0); return OK_; }
/* SetFVF and SetVertexDeclaration replace each other (D3D9 turns an FVF into an internal declaration) */
static uint32_t m_SetVertexDeclaration(CPU* c) {
    Dev* d = DEVOF(SELF); if (!d) return ERR_INVALIDCALL; Obj* o = O(A(1)); if (o && o->type != T_DECL) return ERR_INVALIDCALL;
    bind(&d->decl, o); if (o) d->fvf = 0; return OK_;
}
GETOBJ(m_GetVertexDeclaration, decl)
SETOBJ(m_SetVertexShader, vs, T_VS) GETOBJ(m_GetVertexShader, vs)
SETOBJ(m_SetPixelShader, ps, T_PS) GETOBJ(m_GetPixelShader, ps)
SETOBJ(m_SetIndices, ib, T_IB) GETOBJ(m_GetIndices, ib)
static uint32_t m_SetFVF(CPU* c) { Dev* d = DEVOF(SELF); if (d) { d->fvf = A(1); if (d->fvf) bind(&d->decl, NULL); } return OK_; }
static uint32_t m_GetFVF(CPU* c) { Dev* d = DEVOF(SELF); if (!d || !A(1)) return ERR_INVALIDCALL; wr32(A(1), d->fvf); return OK_; }
static uint32_t m_CreateVertexShader(CPU* c) { return create_shader(c, T_VS); }
static uint32_t m_CreatePixelShader(CPU* c) { return create_shader(c, T_PS); }
static uint32_t cset(void* dst, uint32_t cap, uint32_t esz, uint32_t start, uint32_t p, uint32_t n) {
    if (!p || start > cap || n > cap - start) return ERR_INVALIDCALL;
    memcpy((uint8_t*)dst + (size_t)start * esz, M + p, (size_t)n * esz); return OK_;
}
static uint32_t cget(const void* src, uint32_t cap, uint32_t esz, uint32_t start, uint32_t p, uint32_t n) {
    if (!p || start > cap || n > cap - start) return ERR_INVALIDCALL;
    memcpy(M + p, (const uint8_t*)src + (size_t)start * esz, (size_t)n * esz); return OK_;
}
#define CONSTS(sfx, arr, cap, esz, v) \
    static uint32_t m_Set##sfx(CPU* c) { Dev* d = DEVOF(SELF); if (!d) return ERR_INVALIDCALL; d->cver[v]++; return cset(d->arr, cap, esz, A(1), A(2), A(3)); } \
    static uint32_t m_Get##sfx(CPU* c) { Dev* d = DEVOF(SELF); return d ? cget(d->arr, cap, esz, A(1), A(2), A(3)) : ERR_INVALIDCALL; }
CONSTS(VertexShaderConstantF, vsf, 256, 16, 0) CONSTS(VertexShaderConstantI, vsi, 16, 16, 0) CONSTS(VertexShaderConstantB, vsb, 16, 4, 0)
CONSTS(PixelShaderConstantF, psf, 224, 16, 1) CONSTS(PixelShaderConstantI, psi, 16, 16, 1) CONSTS(PixelShaderConstantB, psb, 16, 4, 1)
static uint32_t m_SetStreamSource(CPU* c) {
    Dev* d = DEVOF(SELF); uint32_t n = A(1); if (!d || n >= 16) return ERR_INVALIDCALL;
    Obj* o = O(A(2)); if (o && o->type != T_VB) return ERR_INVALIDCALL;
    bind(&d->vb[n], o); d->vboff[n] = A(3); d->vbstride[n] = A(4); return OK_;
}
static uint32_t m_GetStreamSource(CPU* c) {
    Dev* d = DEVOF(SELF); uint32_t n = A(1); if (!d || n >= 16 || !A(2)) return ERR_INVALIDCALL;
    if (d->vb[n]) addref(d->vb[n]);
    wr32(A(2), d->vb[n] ? d->vb[n]->g : 0);
    if (A(3)) wr32(A(3), d->vboff[n]);
    if (A(4)) wr32(A(4), d->vbstride[n]);
    return OK_;
}
static uint32_t m_SetStreamSourceFreq(CPU* c) { Dev* d = DEVOF(SELF); if (!d || A(1) >= 16) return ERR_INVALIDCALL; d->vbfreq[A(1)] = A(2); return OK_; }
static uint32_t m_GetStreamSourceFreq(CPU* c) { Dev* d = DEVOF(SELF); if (!d || A(1) >= 16 || !A(2)) return ERR_INVALIDCALL; wr32(A(2), d->vbfreq[A(1)]); return OK_; }
static uint32_t m_CreateQuery(CPU* c) {
    uint32_t t = A(1), pp = A(2);
    if (t != 8 && t != 9) {
        static uint32_t seen; if (t < 32 && !(seen >> t & 1)) { seen |= 1u << t; fprintf(stderr, "[d3d] CreateQuery(type %u) -> NOTAVAILABLE\n", t); }
        if (pp) wr32(pp, 0);
        return ERR_NOTAVAILABLE;
    }
    if (!pp) return OK_;
    Obj* o = newobj(T_QUERY); o->qtype = t;
    if (t == 9) o->gx = calloc(1, sizeof(QRes));   /* freed by gl_free (hle_d3d9_vk.h) */
    wr32(pp, o->g); return OK_;
}
/* IDirect3DSwapChain9 */
static uint32_t m_Swap_Present(CPU* c) { if (g_d3dlost) return D3DERR_DEVICELOST_; Obj* s = SELF; return present(s && s->type == T_SWAP ? s->bb : NULL); }
static uint32_t m_Swap_GetBackBuffer(CPU* c) {
    Obj* s = SELF; uint32_t pp = A(3); if (!s || !pp) return ERR_INVALIDCALL;
    if (A(1) >= s->pp[3]) { wr32(pp, 0); return ERR_INVALIDCALL; }
    addref(s->bb); return out_obj(pp, s->bb);
}
static uint32_t m_Swap_GetRasterStatus(CPU* c) { if (!A(1)) return ERR_INVALIDCALL; wr32(A(1), 0); wr32(A(1) + 4, 0); return OK_; }
static uint32_t m_Swap_GetDisplayMode(CPU* c) {
    Obj* s = SELF; if (!s || !A(1)) return ERR_INVALIDCALL;
    if (s->pp[8]) mode_out(A(1), g_scrw, g_scrh, 22); else mode_out(A(1), s->pp[0], s->pp[1], 22);
    return OK_;
}
static uint32_t m_Swap_GetPresentParameters(CPU* c) { Obj* s = SELF; if (!s || !A(1)) return ERR_INVALIDCALL; memcpy(M + A(1), s->pp, 56); return OK_; }

/* D3DXLoadSurfaceFromMemory(dst, dstPal, dstRect, src, srcFmt, srcPitch, srcPal, srcRect, filter, colorKey).
 * The Scaleform (GFx 2) renderer of the game creates its textures (glyph cache, bitmaps such as the menu logo) in
 * D3DPOOL_DEFAULT and fills every one of them through this call (source A8 or A8B8G8R8), so it must really copy:
 * up to v0.9.2 it was a no-op and all UI text and images were invisible. Native implementation: per-pixel conversion
 * between the common uncompressed formats (raw row copy for equal formats, also block-aligned DXT/ATI copies),
 * point sampling when the rectangles differ in size, colour key; written through the normal lock/unlock path (so the
 * GL backend uploads only the destination rectangle). */
static int px_fmt_ok(uint32_t f) {
    switch (f) { case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 28: case 32: case 33: case 50: case 51: return 1; }
    return 0;
}
static uint32_t px_bytes(uint32_t f) { return f == 20 ? 3u : fmt_info(f) ? fmt_info(f)->bytes : 4u; }
static uint32_t px_get(uint32_t f, const uint8_t* p) {   /* -> 0xAARRGGBB */
    uint32_t v, r, g, b, a;
    switch (f) {
    case 20: return 0xFF000000u | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
    case 21: memcpy(&v, p, 4); return v;
    case 22: memcpy(&v, p, 4); return v | 0xFF000000u;
    case 32: case 33: memcpy(&v, p, 4); v = (v & 0xFF00FF00u) | (v >> 16 & 0xFFu) | (v & 0xFFu) << 16; return f == 33 ? v | 0xFF000000u : v;
    case 23: v = (uint32_t)p[0] | (uint32_t)p[1] << 8; r = v >> 11 & 31; g = v >> 5 & 63; b = v & 31;
        return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    case 24: case 25: v = (uint32_t)p[0] | (uint32_t)p[1] << 8; r = v >> 10 & 31; g = v >> 5 & 31; b = v & 31;
        a = f == 24 || (v & 0x8000u) ? 0xFFu : 0u;
        return a << 24 | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    case 26: v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        return (v >> 12 & 15) * 0x11u << 24 | (v >> 8 & 15) * 0x11u << 16 | (v >> 4 & 15) * 0x11u << 8 | (v & 15) * 0x11u;
    case 28: return (uint32_t)p[0] << 24;
    case 50: return 0xFF000000u | p[0] * 0x010101u;
    case 51: return (uint32_t)p[1] << 24 | p[0] * 0x010101u;
    }
    return 0;
}
static void px_put(uint32_t f, uint8_t* p, uint32_t v) {
    uint32_t a = v >> 24, r = v >> 16 & 255, g = v >> 8 & 255, b = v & 255, u;
    switch (f) {
    case 20: p[0] = (uint8_t)b; p[1] = (uint8_t)g; p[2] = (uint8_t)r; return;
    case 21: case 22: memcpy(p, &v, 4); return;
    case 32: case 33: u = (v & 0xFF00FF00u) | r | b << 16; memcpy(p, &u, 4); return;
    case 23: u = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8); return;
    case 24: case 25: u = (a >= 128 || f == 24 ? 0x8000u : 0u) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8); return;
    case 26: u = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8); return;
    case 28: p[0] = (uint8_t)a; return;
    case 50: case 51: p[0] = (uint8_t)((r * 54u + g * 183u + b * 19u + 128u) >> 8); if (f == 51) p[1] = (uint8_t)a; return;
    }
}
static uint32_t x_D3DXLoadSurfaceFromMemory(CPU* c) {
    Obj* d = O(A(0)); uint32_t drp = A(2), src = A(3), sf = A(4), spitch = A(5), srp = A(7), key = A(9);
    if (!d || d->type != T_SURF || !src || !srp) return ERR_INVALIDCALL;
    uint32_t df = d->fmt, dw = d->w, dh = d->h;
    uint32_t sx0 = rd32(srp), sy0 = rd32(srp + 4), sx1 = rd32(srp + 8), sy1 = rd32(srp + 12);
    uint32_t dx0 = 0, dy0 = 0, dx1 = dw, dy1 = dh;
    if (drp) { dx0 = rd32(drp); dy0 = rd32(drp + 4); dx1 = rd32(drp + 8); dy1 = rd32(drp + 12); }
    if (sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0 || dx1 > dw || dy1 > dh) return ERR_INVALIDCALL;
    const Fmt* sfi = fmt_info(sf); const Fmt* dfi = fmt_info(df);
    int blk = (sfi && sfi->block) || (dfi && dfi->block);
    uint32_t sw = sx1 - sx0, sh = sy1 - sy0, ow = dx1 - dx0, oh = dy1 - dy0;
    if (blk && (sf != df || sw != ow || sh != oh || (sx0 | sy0 | dx0 | dy0) & 3)) {
        static int n; if (n++ < 10) fprintf(stderr, "[d3dx] D3DXLoadSurfaceFromMemory: %s -> %s %ux%u -> %ux%u not supported\n", fmt_name(sf), fmt_name(df), sw, sh, ow, oh);
        return ERR_INVALIDCALL;
    }
    if (!blk && (!px_fmt_ok(sf) || !px_fmt_ok(df))) {
        static int n; if (n++ < 10) fprintf(stderr, "[d3dx] D3DXLoadSurfaceFromMemory: format %s -> %s not supported\n", fmt_name(sf), fmt_name(df));
        return ERR_INVALIDCALL;
    }
    static uint32_t scratch; if (!scratch) scratch = ghalloc(32, 1);
    uint32_t rect = scratch, lr = scratch + 16;
    wr32(rect, dx0); wr32(rect + 4, dy0); wr32(rect + 8, dx1); wr32(rect + 12, dy1);
    Obj* root = d->parent ? d->parent : d; uint32_t sub = d->parent ? d->face * d->parent->levels + d->level : 0, lvl = d->parent ? d->level : 0;
    uint32_t hr = lock_sub(root, sub, lvl, rect, 0, lr, 0);
    if (hr) return hr;
    uint32_t dpitch = rd32(lr); uint8_t* dp = M + rd32(lr + 4);
    if (blk) {   /* same compressed format and size, block aligned: copy rows of blocks */
        uint32_t bb = sfi->bytes, rowb = (sw + 3) / 4 * bb, rows = (sh + 3) / 4;
        const uint8_t* sp = M + src + (size_t)(sy0 / 4) * spitch + (size_t)(sx0 / 4) * bb;
        for (uint32_t y = 0; y < rows; y++) memcpy(dp + (size_t)y * dpitch, sp + (size_t)y * spitch, rowb);
    } else {
        uint32_t sb = px_bytes(sf), db = px_bytes(df);
        for (uint32_t y = 0; y < oh; y++) {
            uint32_t yy = sy0 + (sh == oh ? y : (uint32_t)(((uint64_t)y * sh + sh / 2) / oh)); if (yy >= sy1) yy = sy1 - 1;
            const uint8_t* srow = M + src + (size_t)yy * spitch; uint8_t* drow = dp + (size_t)y * dpitch;
            if (sf == df && sw == ow && !key) { memcpy(drow, srow + (size_t)sx0 * sb, (size_t)ow * db); continue; }
            for (uint32_t x = 0; x < ow; x++) {
                uint32_t xx = sx0 + (sw == ow ? x : (uint32_t)(((uint64_t)x * sw + sw / 2) / ow)); if (xx >= sx1) xx = sx1 - 1;
                uint32_t v = px_get(sf, srow + (size_t)xx * sb);
                if (key && v == key) v = 0;
                px_put(df, drow + (size_t)x * db, v);
            }
        }
    }
    g_st.dxloads++;
    return unlock_sub(root, sub, lvl);
}
/* ---- d3d9.dll / d3dx9_31.dll exports ---- */
static uint32_t f_Direct3DCreate9(CPU* c) {
    Obj* o = newobj(T_D3D);
    fprintf(stderr, "[d3d] Direct3DCreate9(%u) -> %08x (null backend; desktop %ux%u)\n", A(0), o->g, g_scrw, g_scrh);
    return o->g;
}
static void wrf_(uint32_t a, double v) { wrf(a, (float)v); }
static uint32_t x_D3DXMatrixInverse(CPU* c) {
    uint32_t out = A(0), pdet = A(1), pm = A(2);
    if (!out || !pm) return 0;
    float f[16]; memcpy(f, M + pm, 64);
    double m[16], v[16]; for (int i = 0; i < 16; i++) m[i] = f[i];
    v[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    v[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    v[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    v[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    v[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    v[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    v[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    v[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    v[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    v[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    v[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    v[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    v[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    v[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    v[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    v[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    double det = m[0] * v[0] + m[1] * v[4] + m[2] * v[8] + m[3] * v[12];
    if (pdet) wrf_(pdet, det);
    if (det == 0.0 || det != det) return 0;
    for (int i = 0; i < 16; i++) wrf_(out + 4 * i, v[i] / det);
    return out;
}
static void zero_out(uint32_t p) { if (p) wr32(p, 0); }
static uint32_t x_nocompiler(CPU* c, const char* what, uint32_t ppcode, uint32_t pperr) {
    static int n;
    if (n++ < 40) {
        char fn[64] = "", pr[16] = "";
        if (!strcmp(what, "D3DXCompileShader")) { if (A(4)) snprintf(fn, sizeof fn, "%s", GSTR(A(4))); if (A(5)) snprintf(pr, sizeof pr, "%s", GSTR(A(5))); }
        fprintf(stderr, "[d3dx] %s(%s%s%s, %u bytes) -> E_FAIL (no HLSL compiler; shaders must come from the shader cache)\n",
                what, fn, *pr ? ", " : "", pr, A(1));
    }
    zero_out(ppcode);
    if (pperr) wr32(pperr, mk_xbuf("error X0000: the HLSL compiler is not available in this port (D3DX HLE)\n"));
    return E_FAIL_;
}
static uint32_t x_D3DXCompileShader(CPU* c) { zero_out(A(9)); return x_nocompiler(c, "D3DXCompileShader", A(7), A(8)); }
static uint32_t x_D3DXAssembleShader(CPU* c) { return x_nocompiler(c, "D3DXAssembleShader", A(5), A(6)); }
static uint32_t x_D3DXPreprocessShader(CPU* c) { return x_nocompiler(c, "D3DXPreprocessShader", A(4), A(5)); }
static uint32_t x_D3DXDisassembleShader(CPU* c) { zero_out(A(3)); return E_FAIL_; }
static uint32_t x_mesh_fail(CPU* c, const char* what, int a1, int a2, int a3) {
    static int n; if (n++ < 10) fprintf(stderr, "[d3dx] %s -> E_FAIL (not implemented)\n", what);
    if (a1 >= 0) zero_out(A(a1));
    if (a2 >= 0) zero_out(A(a2));
    if (a3 >= 0) zero_out(A(a3));
    return E_FAIL_;
}
static uint32_t x_D3DXCreateMesh(CPU* c) { return x_mesh_fail(c, "D3DXCreateMesh", 5, -1, -1); }
static uint32_t x_D3DXCleanMesh(CPU* c) { return x_mesh_fail(c, "D3DXCleanMesh", 3, 5, -1); }
static uint32_t x_D3DXWeldVertices(CPU* c) { return x_mesh_fail(c, "D3DXWeldVertices", 6, -1, -1); }
static uint32_t x_D3DXSimplifyMesh(CPU* c) { return x_mesh_fail(c, "D3DXSimplifyMesh", 6, -1, -1); }
static uint32_t x_D3DXUVAtlasCreate(CPU* c) { return x_mesh_fail(c, "D3DXUVAtlasCreate", 14, 15, 16); }

/* ---- interface tables (exact d3d9.h vtable order) ---- */
#define D9 IDirect3DDevice9
#define UNK(I, X) X(I, QueryInterface, 3, m_QueryInterface) X(I, AddRef, 1, m_AddRef) X(I, Release, 1, m_Release)
#define RES(I, X) UNK(I, X) X(I, GetDevice, 2, m_GetDevice) X(I, SetPrivateData, 5, m_ok) X(I, GetPrivateData, 4, m_GetPrivateData) \
    X(I, FreePrivateData, 2, m_ok) X(I, SetPriority, 2, m_SetPriority) X(I, GetPriority, 1, m_GetPriority) X(I, PreLoad, 1, m_zero) \
    X(I, GetType, 1, m_GetType)
#define BTEX(I, X) RES(I, X) X(I, SetLOD, 2, m_SetLOD) X(I, GetLOD, 1, m_GetLOD) X(I, GetLevelCount, 1, m_GetLevelCount) \
    X(I, SetAutoGenFilterType, 2, m_ok) X(I, GetAutoGenFilterType, 1, m_GetAutoGenFilterType) X(I, GenerateMipSubLevels, 1, m_zero)
#define L_D3D(X) UNK(IDirect3D9, X) X(IDirect3D9, RegisterSoftwareDevice, 2, m_inv) X(IDirect3D9, GetAdapterCount, 1, m_GetAdapterCount) \
    X(IDirect3D9, GetAdapterIdentifier, 4, m_GetAdapterIdentifier) X(IDirect3D9, GetAdapterModeCount, 3, m_GetAdapterModeCount) \
    X(IDirect3D9, EnumAdapterModes, 5, m_EnumAdapterModes) X(IDirect3D9, GetAdapterDisplayMode, 3, m_GetAdapterDisplayMode) \
    X(IDirect3D9, CheckDeviceType, 6, m_CheckDeviceType) X(IDirect3D9, CheckDeviceFormat, 7, m_CheckDeviceFormat) \
    X(IDirect3D9, CheckDeviceMultiSampleType, 7, m_CheckDeviceMultiSampleType) X(IDirect3D9, CheckDepthStencilMatch, 6, m_CheckDepthStencilMatch) \
    X(IDirect3D9, CheckDeviceFormatConversion, 5, m_CheckDeviceFormatConversion) X(IDirect3D9, GetDeviceCaps, 4, m_D3D_GetDeviceCaps) \
    X(IDirect3D9, GetAdapterMonitor, 2, m_GetAdapterMonitor) X(IDirect3D9, CreateDevice, 7, m_CreateDevice)
#define L_DEV(X) UNK(D9, X) X(D9, TestCooperativeLevel, 1, m_TestCooperativeLevel) X(D9, GetAvailableTextureMem, 1, m_GetAvailableTextureMem) \
    X(D9, EvictManagedResources, 1, m_ok) X(D9, GetDirect3D, 2, m_GetDirect3D) X(D9, GetDeviceCaps, 2, m_Dev_GetDeviceCaps) \
    X(D9, GetDisplayMode, 3, m_Dev_GetDisplayMode) X(D9, GetCreationParameters, 2, m_GetCreationParameters) \
    X(D9, SetCursorProperties, 4, m_ok) X(D9, SetCursorPosition, 4, m_zero) X(D9, ShowCursor, 2, m_Dev_ShowCursor) \
    X(D9, CreateAdditionalSwapChain, 3, m_CreateAdditionalSwapChain) X(D9, GetSwapChain, 3, m_GetSwapChain) \
    X(D9, GetNumberOfSwapChains, 1, m_one) X(D9, Reset, 2, m_Reset) X(D9, Present, 5, m_Dev_Present) \
    X(D9, GetBackBuffer, 5, m_Dev_GetBackBuffer) X(D9, GetRasterStatus, 3, m_Dev_GetRasterStatus) X(D9, SetDialogBoxMode, 2, m_ok) \
    X(D9, SetGammaRamp, 4, m_SetGammaRamp) X(D9, GetGammaRamp, 3, m_GetGammaRamp) X(D9, CreateTexture, 9, m_CreateTexture) \
    X(D9, CreateVolumeTexture, 10, m_CreateVolumeTexture) X(D9, CreateCubeTexture, 8, m_CreateCubeTexture) \
    X(D9, CreateVertexBuffer, 7, m_CreateVertexBuffer) X(D9, CreateIndexBuffer, 7, m_CreateIndexBuffer) \
    X(D9, CreateRenderTarget, 9, m_CreateRenderTarget) X(D9, CreateDepthStencilSurface, 9, m_CreateDepthStencilSurface) \
    X(D9, UpdateSurface, 5, m_UpdateSurface) X(D9, UpdateTexture, 3, m_UpdateTexture) X(D9, GetRenderTargetData, 3, m_GetRenderTargetData) X(D9, GetFrontBufferData, 3, m_ok) \
    X(D9, StretchRect, 6, m_StretchRect) X(D9, ColorFill, 4, m_ColorFill) X(D9, CreateOffscreenPlainSurface, 7, m_CreateOffscreenPlainSurface) \
    X(D9, SetRenderTarget, 3, m_SetRenderTarget) X(D9, GetRenderTarget, 3, m_GetRenderTarget) \
    X(D9, SetDepthStencilSurface, 2, m_SetDepthStencilSurface) X(D9, GetDepthStencilSurface, 2, m_GetDepthStencilSurface) \
    X(D9, BeginScene, 1, m_ok) X(D9, EndScene, 1, m_ok) X(D9, Clear, 7, m_Clear) X(D9, SetTransform, 3, m_SetTransform) \
    X(D9, GetTransform, 3, m_GetTransform) X(D9, MultiplyTransform, 3, m_MultiplyTransform) X(D9, SetViewport, 2, m_SetViewport) \
    X(D9, GetViewport, 2, m_GetViewport) X(D9, SetMaterial, 2, m_SetMaterial) X(D9, GetMaterial, 2, m_GetMaterial) \
    X(D9, SetLight, 3, m_ok) X(D9, GetLight, 3, m_inv) X(D9, LightEnable, 3, m_ok) X(D9, GetLightEnable, 3, m_GetLightEnable) \
    X(D9, SetClipPlane, 3, m_SetClipPlane) X(D9, GetClipPlane, 3, m_GetClipPlane) X(D9, SetRenderState, 3, m_SetRenderState) \
    X(D9, GetRenderState, 3, m_GetRenderState) X(D9, CreateStateBlock, 3, m_CreateStateBlock) X(D9, BeginStateBlock, 1, m_BeginStateBlock) \
    X(D9, EndStateBlock, 2, m_EndStateBlock) X(D9, SetClipStatus, 2, m_ok) X(D9, GetClipStatus, 2, m_GetClipStatus) \
    X(D9, GetTexture, 3, m_GetTexture) X(D9, SetTexture, 3, m_SetTexture) X(D9, GetTextureStageState, 4, m_GetTextureStageState) \
    X(D9, SetTextureStageState, 4, m_SetTextureStageState) X(D9, GetSamplerState, 4, m_GetSamplerState) \
    X(D9, SetSamplerState, 4, m_SetSamplerState) X(D9, ValidateDevice, 2, m_ValidateDevice) X(D9, SetPaletteEntries, 3, m_ok) \
    X(D9, GetPaletteEntries, 3, m_ok) X(D9, SetCurrentTexturePalette, 2, m_ok) X(D9, GetCurrentTexturePalette, 2, m_GetCurrentTexturePalette) \
    X(D9, SetScissorRect, 2, m_SetScissorRect) X(D9, GetScissorRect, 2, m_GetScissorRect) X(D9, SetSoftwareVertexProcessing, 2, m_ok) \
    X(D9, GetSoftwareVertexProcessing, 1, m_zero) X(D9, SetNPatchMode, 2, m_ok) X(D9, GetNPatchMode, 1, m_GetNPatchMode) \
    X(D9, DrawPrimitive, 4, m_DrawPrimitive) X(D9, DrawIndexedPrimitive, 7, m_DrawIndexedPrimitive) \
    X(D9, DrawPrimitiveUP, 5, m_DrawPrimitiveUP) X(D9, DrawIndexedPrimitiveUP, 9, m_DrawIndexedPrimitiveUP) \
    X(D9, ProcessVertices, 7, m_ok) X(D9, CreateVertexDeclaration, 3, m_CreateVertexDeclaration) \
    X(D9, SetVertexDeclaration, 2, m_SetVertexDeclaration) X(D9, GetVertexDeclaration, 2, m_GetVertexDeclaration) \
    X(D9, SetFVF, 2, m_SetFVF) X(D9, GetFVF, 2, m_GetFVF) X(D9, CreateVertexShader, 3, m_CreateVertexShader) \
    X(D9, SetVertexShader, 2, m_SetVertexShader) X(D9, GetVertexShader, 2, m_GetVertexShader) \
    X(D9, SetVertexShaderConstantF, 4, m_SetVertexShaderConstantF) X(D9, GetVertexShaderConstantF, 4, m_GetVertexShaderConstantF) \
    X(D9, SetVertexShaderConstantI, 4, m_SetVertexShaderConstantI) X(D9, GetVertexShaderConstantI, 4, m_GetVertexShaderConstantI) \
    X(D9, SetVertexShaderConstantB, 4, m_SetVertexShaderConstantB) X(D9, GetVertexShaderConstantB, 4, m_GetVertexShaderConstantB) \
    X(D9, SetStreamSource, 5, m_SetStreamSource) X(D9, GetStreamSource, 5, m_GetStreamSource) \
    X(D9, SetStreamSourceFreq, 3, m_SetStreamSourceFreq) X(D9, GetStreamSourceFreq, 3, m_GetStreamSourceFreq) \
    X(D9, SetIndices, 2, m_SetIndices) X(D9, GetIndices, 2, m_GetIndices) X(D9, CreatePixelShader, 3, m_CreatePixelShader) \
    X(D9, SetPixelShader, 2, m_SetPixelShader) X(D9, GetPixelShader, 2, m_GetPixelShader) \
    X(D9, SetPixelShaderConstantF, 4, m_SetPixelShaderConstantF) X(D9, GetPixelShaderConstantF, 4, m_GetPixelShaderConstantF) \
    X(D9, SetPixelShaderConstantI, 4, m_SetPixelShaderConstantI) X(D9, GetPixelShaderConstantI, 4, m_GetPixelShaderConstantI) \
    X(D9, SetPixelShaderConstantB, 4, m_SetPixelShaderConstantB) X(D9, GetPixelShaderConstantB, 4, m_GetPixelShaderConstantB) \
    X(D9, DrawRectPatch, 4, m_ok) X(D9, DrawTriPatch, 4, m_ok) X(D9, DeletePatch, 2, m_ok) X(D9, CreateQuery, 3, m_CreateQuery)
#define L_SWAP(X) UNK(IDirect3DSwapChain9, X) X(IDirect3DSwapChain9, Present, 6, m_Swap_Present) \
    X(IDirect3DSwapChain9, GetFrontBufferData, 2, m_ok) X(IDirect3DSwapChain9, GetBackBuffer, 4, m_Swap_GetBackBuffer) \
    X(IDirect3DSwapChain9, GetRasterStatus, 2, m_Swap_GetRasterStatus) X(IDirect3DSwapChain9, GetDisplayMode, 2, m_Swap_GetDisplayMode) \
    X(IDirect3DSwapChain9, GetDevice, 2, m_GetDevice) X(IDirect3DSwapChain9, GetPresentParameters, 2, m_Swap_GetPresentParameters)
#define L_TEX(X) BTEX(IDirect3DTexture9, X) X(IDirect3DTexture9, GetLevelDesc, 3, m_Tex_GetLevelDesc) \
    X(IDirect3DTexture9, GetSurfaceLevel, 3, m_Tex_GetSurfaceLevel) X(IDirect3DTexture9, LockRect, 5, m_Tex_LockRect) \
    X(IDirect3DTexture9, UnlockRect, 2, m_Tex_UnlockRect) X(IDirect3DTexture9, AddDirtyRect, 2, m_ok)
#define L_CUBE(X) BTEX(IDirect3DCubeTexture9, X) X(IDirect3DCubeTexture9, GetLevelDesc, 3, m_Tex_GetLevelDesc) \
    X(IDirect3DCubeTexture9, GetCubeMapSurface, 4, m_Cube_GetCubeMapSurface) X(IDirect3DCubeTexture9, LockRect, 6, m_Cube_LockRect) \
    X(IDirect3DCubeTexture9, UnlockRect, 3, m_Cube_UnlockRect) X(IDirect3DCubeTexture9, AddDirtyRect, 3, m_ok)
#define L_VOLTEX(X) BTEX(IDirect3DVolumeTexture9, X) X(IDirect3DVolumeTexture9, GetLevelDesc, 3, m_VolTex_GetLevelDesc) \
    X(IDirect3DVolumeTexture9, GetVolumeLevel, 3, m_VolTex_GetVolumeLevel) X(IDirect3DVolumeTexture9, LockBox, 5, m_VolTex_LockBox) \
    X(IDirect3DVolumeTexture9, UnlockBox, 2, m_VolTex_UnlockBox) X(IDirect3DVolumeTexture9, AddDirtyBox, 2, m_ok)
#define L_SURF(X) RES(IDirect3DSurface9, X) X(IDirect3DSurface9, GetContainer, 3, m_Surf_GetContainer) \
    X(IDirect3DSurface9, GetDesc, 2, m_Surf_GetDesc) X(IDirect3DSurface9, LockRect, 4, m_Surf_LockRect) \
    X(IDirect3DSurface9, UnlockRect, 1, m_Surf_UnlockRect) X(IDirect3DSurface9, GetDC, 2, m_inv) X(IDirect3DSurface9, ReleaseDC, 2, m_inv)
#define L_VOL(X) UNK(IDirect3DVolume9, X) X(IDirect3DVolume9, GetDevice, 2, m_GetDevice) X(IDirect3DVolume9, SetPrivateData, 5, m_ok) \
    X(IDirect3DVolume9, GetPrivateData, 4, m_GetPrivateData) X(IDirect3DVolume9, FreePrivateData, 2, m_ok) \
    X(IDirect3DVolume9, GetContainer, 3, m_Surf_GetContainer) X(IDirect3DVolume9, GetDesc, 2, m_Vol_GetDesc) \
    X(IDirect3DVolume9, LockBox, 4, m_Vol_LockBox) X(IDirect3DVolume9, UnlockBox, 1, m_Vol_UnlockBox)
#define L_VB(X) RES(IDirect3DVertexBuffer9, X) X(IDirect3DVertexBuffer9, Lock, 5, m_Buf_Lock) \
    X(IDirect3DVertexBuffer9, Unlock, 1, m_Buf_Unlock) X(IDirect3DVertexBuffer9, GetDesc, 2, m_VB_GetDesc)
#define L_IB(X) RES(IDirect3DIndexBuffer9, X) X(IDirect3DIndexBuffer9, Lock, 5, m_Buf_Lock) \
    X(IDirect3DIndexBuffer9, Unlock, 1, m_Buf_Unlock) X(IDirect3DIndexBuffer9, GetDesc, 2, m_IB_GetDesc)
#define L_DECL(X) UNK(IDirect3DVertexDeclaration9, X) X(IDirect3DVertexDeclaration9, GetDevice, 2, m_GetDevice) \
    X(IDirect3DVertexDeclaration9, GetDeclaration, 3, m_GetDeclaration)
#define L_VS(X) UNK(IDirect3DVertexShader9, X) X(IDirect3DVertexShader9, GetDevice, 2, m_GetDevice) \
    X(IDirect3DVertexShader9, GetFunction, 3, m_GetFunction)
#define L_PS(X) UNK(IDirect3DPixelShader9, X) X(IDirect3DPixelShader9, GetDevice, 2, m_GetDevice) \
    X(IDirect3DPixelShader9, GetFunction, 3, m_GetFunction)
#define L_QUERY(X) UNK(IDirect3DQuery9, X) X(IDirect3DQuery9, GetDevice, 2, m_GetDevice) X(IDirect3DQuery9, GetType, 1, m_GetType) \
    X(IDirect3DQuery9, GetDataSize, 1, m_Query_GetDataSize) X(IDirect3DQuery9, Issue, 2, m_Query_Issue) X(IDirect3DQuery9, GetData, 4, m_Query_GetData)
#define L_SB(X) UNK(IDirect3DStateBlock9, X) X(IDirect3DStateBlock9, GetDevice, 2, m_GetDevice) \
    X(IDirect3DStateBlock9, Capture, 1, m_ok) X(IDirect3DStateBlock9, Apply, 1, m_ok)
#define L_XBUF(X) UNK(ID3DXBuffer, X) X(ID3DXBuffer, GetBufferPointer, 1, m_Buf_GetBufferPointer) X(ID3DXBuffer, GetBufferSize, 1, m_Buf_GetBufferSize)
#define ALL(X) L_D3D(X) L_DEV(X) L_SWAP(X) L_TEX(X) L_CUBE(X) L_VOLTEX(X) L_SURF(X) L_VOL(X) L_VB(X) L_IB(X) L_DECL(X) L_VS(X) \
    L_PS(X) L_QUERY(X) L_SB(X) L_XBUF(X)
#define L_FN(X) X("d3d9.dll", Direct3DCreate9, 1, f_Direct3DCreate9) X("d3d9.dll", D3DPERF_BeginEvent, 2, m_zero) \
    X("d3d9.dll", D3DPERF_EndEvent, 0, m_zero) X("d3d9.dll", D3DPERF_SetMarker, 2, m_zero) X("d3d9.dll", D3DPERF_GetStatus, 0, m_zero) \
    X("d3dx9_31.dll", D3DXDebugMute, 1, m_zero) X("d3dx9_31.dll", D3DXMatrixInverse, 3, x_D3DXMatrixInverse) \
    X("d3dx9_31.dll", D3DXLoadSurfaceFromMemory, 10, x_D3DXLoadSurfaceFromMemory) X("d3dx9_31.dll", D3DXCreateMesh, 6, x_D3DXCreateMesh) \
    X("d3dx9_31.dll", D3DXCleanMesh, 6, x_D3DXCleanMesh) X("d3dx9_31.dll", D3DXWeldVertices, 7, x_D3DXWeldVertices) \
    X("d3dx9_31.dll", D3DXSimplifyMesh, 7, x_D3DXSimplifyMesh) X("d3dx9_31.dll", D3DXUVAtlasCreate, 19, x_D3DXUVAtlasCreate)

/* wrappers: own recursive lock (no guest code runs inside), current method name for logs */
#define WRAP_(I, N, n, f) static void w_##I##_##N(CPU* c) { \
    if ((f == m_SetRenderState && rs_fast(c)) || (f == m_SetSamplerState && ss_fast(c))) { c->eax = OK_; return; }   /* v0.10.13 (R1) */ \
    lock_(); const char* sv = g_cur; g_cur = #I "::" #N; c->eax = f(c); g_cur = sv; unlock_(); }
#define WRAP(I, N, n, f) WRAP_(I, N, n, f)
#define DEF_(I, N, n, f) { "d3d9.com", #I "::" #N, n, w_##I##_##N, HLE_NOLOCK },
#define DEF(I, N, n, f) DEF_(I, N, n, f)
#define FWRAP(dll, N, n, f) static void w_##N(CPU* c) { if (f == x_D3DXMatrixInverse) { c->eax = f(c); return; } /* pure math: no lock */ lock_(); const char* sv = g_cur; g_cur = #N; c->eax = f(c); g_cur = sv; unlock_(); }
#define FDEF(dll, N, n, f) { dll, #N, n, w_##N, HLE_NOLOCK },
ALL(WRAP)
L_FN(FWRAP)

/* D3DX shader functions: forwarded to the recompiled d3dx9_31.dll when main_exe.c loaded it (hle_d3dx_attach), else the
 * HLE fallback (E_FAIL). They run without the D3D lock: the HLSL compiler calls back into the game (ID3DXInclude) and
 * may take long; the game's other threads keep using D3D meanwhile. */
enum { DX_COMPILE, DX_ASSEMBLE, DX_PREPROCESS, DX_DISASM, DX_N };
static const char* const dx_name[DX_N] = { "D3DXCompileShader", "D3DXAssembleShader", "D3DXPreprocessShader", "D3DXDisassembleShader" };
static const int dx_nargs[DX_N] = { 10, 7, 6, 4 }, dx_ppcode[DX_N] = { 7, 5, 4, 3 }, dx_pperr[DX_N] = { 8, 6, 5, -1 };
static uint32_t g_dxr[DX_N], g_dx_mkbuf, g_dx_ctab;
static struct { uint64_t n, fail, hits, stored; double secs; } g_dxs[DX_N];
static pthread_mutex_t g_dxmx = PTHREAD_MUTEX_INITIALIZER;
/* compile in progress, for the heartbeat; src = its preprocessed HLSL (written to <cache>/src when it runs long) */
static struct { int active, dumped; uint64_t n; char entry[48], prof[16]; uint32_t flags; double t0; char* src; size_t srcn; } g_dxc;
static double g_dx_busy;   /* total compile time */
void hle_d3dx_attach(uint32_t compile, uint32_t assemble, uint32_t preprocess, uint32_t disassemble) {
    g_dxr[DX_COMPILE] = compile; g_dxr[DX_ASSEMBLE] = assemble; g_dxr[DX_PREPROCESS] = preprocess; g_dxr[DX_DISASM] = disassemble;
}
void hle_d3dx_attach_module(Module* m) {
    hle_d3dx_attach(rt_find_export(m, "D3DXCompileShader"), rt_find_export(m, "D3DXAssembleShader"),
                    rt_find_export(m, "D3DXPreprocessShader"), rt_find_export(m, "D3DXDisassembleShader"));
    g_dx_mkbuf = rt_find_export(m, "D3DXCreateBuffer"); g_dx_ctab = rt_find_export(m, "D3DXGetShaderConstantTable");
}
/* HLSL compile cache (ME1_HLSL_CACHE=<dir>, "off" disables): the first run of the game compiles ~130 global shaders
 * (~3 min on a phone) and the engine saves them only at a clean exit. Key = FNV-1a 64 of the preprocessed source
 * (D3DXPreprocessShader with the game's defines and include handler) + entry + profile + flags; value = bytecode. A hit
 * returns a D3DXCreateBuffer copy and D3DXGetShaderConstantTable of it - the same objects a compile returns.
 * <dir>/src/ receives the preprocessed source of slow (> 2 s), failed and still-running (> 15 s) compiles. */
static char g_hc_dir[900]; static int g_hc = -1;
static int hc_on(void) {
    if (g_hc >= 0) return g_hc;
    const char* e = getenv("ME1_HLSL_CACHE");
    g_hc = e && *e && strcmp(e, "off") && strcmp(e, "0") && g_dxr[DX_PREPROCESS] && g_dx_mkbuf && g_dx_ctab;
    if (g_hc) {
        char s[1000]; snprintf(g_hc_dir, sizeof g_hc_dir, "%s", e); mkdir(g_hc_dir, 0755);
        snprintf(s, sizeof s, "%s/src", g_hc_dir); mkdir(s, 0755);
        fprintf(stderr, "[d3dx] HLSL compile cache: %s\n", g_hc_dir);
    }
    return g_hc;
}
static uint64_t fnv(uint64_t h, const void* p, size_t n) { const uint8_t* b = p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static uint8_t* hc_load(uint64_t key, uint32_t* n) {
    char path[1000]; snprintf(path, sizeof path, "%s/%016llx.bin", g_hc_dir, (unsigned long long)key);
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    uint32_t h[4]; uint8_t* b = NULL;
    if (fread(h, 4, 4, f) == 4 && h[0] == 0x4831454Du && h[1] == 1 && h[2] && h[2] < (16u << 20) && (b = malloc(h[2]))
        && fread(b, 1, h[2], f) == h[2] && rt_crc32(b, h[2]) == h[3]) *n = h[2];
    else { free(b); b = NULL; fprintf(stderr, "[d3dx] cache: %s is damaged, recompiling\n", path); }
    fclose(f); return b;
}
static void hc_store(uint64_t key, const uint8_t* b, uint32_t n) {
    char path[1000], tmp[1010]; snprintf(path, sizeof path, "%s/%016llx.bin", g_hc_dir, (unsigned long long)key);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "wb"); if (!f) return;
    uint32_t h[4] = { 0x4831454Du, 1, n, rt_crc32(b, n) };   /* "ME1H" */
    int ok = fwrite(h, 4, 4, f) == 4 && fwrite(b, 1, n, f) == n; ok &= fclose(f) == 0;
    if (ok) rename(tmp, path); else unlink(tmp);
}
static void dump_src(const char* tag, uint64_t n, const char* entry, const char* prof, uint32_t flags, double secs, uint32_t hr, const char* src, size_t len) {
    char path[1100]; snprintf(path, sizeof path, "%s/src/%s%04llu_%s_%s.hlsl", g_hc_dir, tag, (unsigned long long)n, entry, prof);
    FILE* f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "// D3DXCompileShader entry=%s profile=%s flags=0x%x hr=0x%08x time=%.1f s (#%llu, preprocessed source)\n",
            entry, prof, flags, hr, secs, (unsigned long long)n);
    fwrite(src, 1, len && !src[len - 1] ? len - 1 : len, f); fclose(f);
    fprintf(stderr, "[d3dx]   source saved: %s\n", path);
}
static uint32_t buf_call(CPU* c, uint32_t b, int slot) { uint32_t a1[1] = { b }; return rt_call(c, rd32(rd32(b) + 4 * slot), 1, a1); }
#define BUF_RELEASE 2
#define BUF_PTR 3
#define BUF_SIZE 4
static char* dx_preprocess(CPU* c, const uint32_t* a, size_t* n) {
    uint32_t sp = c->esp; c->esp -= 16; uint32_t o = c->esp; wr32(o, 0); wr32(o + 4, 0);
    uint32_t pa[6] = { a[0], a[1], a[2], a[3], o, o + 4 };
    uint32_t hr = rt_call(c, g_dxr[DX_PREPROCESS], 6, pa);
    uint32_t b = rd32(o), e = rd32(o + 4); char* r = NULL;
    if (!hr && b) { uint32_t p = buf_call(c, b, BUF_PTR), sz = buf_call(c, b, BUF_SIZE); if ((r = malloc(sz + 1))) { memcpy(r, M + p, sz); r[sz] = 0; *n = sz; } }
    if (b) buf_call(c, b, BUF_RELEASE);
    if (e) buf_call(c, e, BUF_RELEASE);
    c->esp = sp; return r;
}
static int dx_cached(CPU* c, const uint32_t* a, const uint8_t* code, uint32_t n) {   /* 1: the outputs are set */
    uint32_t sp = c->esp; c->esp -= 16; uint32_t o = c->esp; wr32(o, 0);
    uint32_t ca[2] = { n, o }; int ok = 0;
    if (!rt_call(c, g_dx_mkbuf, 2, ca) && rd32(o)) {
        uint32_t b = rd32(o), p = buf_call(c, b, BUF_PTR);
        memcpy(M + p, code, n); ok = 1;
        if (a[9]) { uint32_t ta[2] = { p, a[9] }; wr32(a[9], 0); if (rt_call(c, g_dx_ctab, 2, ta) || !rd32(a[9])) ok = 0; }
        if (ok && a[7]) wr32(a[7], b); else buf_call(c, b, BUF_RELEASE);
        if (ok && a[8]) wr32(a[8], 0);
    }
    c->esp = sp; return ok;
}
static uint32_t dx_forward(CPU* c, int k) {
    static int log = -1; if (log < 0) { const char* e = getenv("ME1_D3D_LOG"); log = e ? atoi(e) : 0; }
    uint32_t a[10]; for (int i = 0; i < dx_nargs[k]; i++) a[i] = ARG(i);
    CPU save = *c;
    hle_hb_leave();   /* running guest code from here on: let the profiler sample it */
    double t0 = mono();
    uint32_t hr = 0; int hit = 0; char* src = NULL; size_t srcn = 0; uint64_t key = 0, n;
    char entry[48] = "", prof[16] = "";
    pthread_mutex_lock(&g_dxmx); n = g_dxs[k].n + 1; pthread_mutex_unlock(&g_dxmx);
    if (k == DX_COMPILE) {
        snprintf(entry, sizeof entry, "%s", a[4] ? GSTR(a[4]) : ""); snprintf(prof, sizeof prof, "%s", a[5] ? GSTR(a[5]) : "");
        for (char* q = entry; *q; q++) if (*q == '/' || *q == '\\' || *q == ' ') *q = '_';
        for (char* q = prof; *q; q++) if (*q == '/' || *q == '\\' || *q == ' ') *q = '_';
        if (hc_on() && (src = dx_preprocess(c, a, &srcn))) {
            key = fnv(fnv(fnv(fnv(0xcbf29ce484222325ull, src, srcn), entry, strlen(entry) + 1), prof, strlen(prof) + 1), &a[6], 4);
            uint32_t cn; uint8_t* code = hc_load(key, &cn);
            if (code) { hit = dx_cached(c, a, code, cn); free(code); }
        }
        pthread_mutex_lock(&g_dxmx);
        if (!hit && !g_dxc.active) {
            g_dxc.active = 1; g_dxc.dumped = 0; g_dxc.n = n; g_dxc.flags = a[6]; g_dxc.t0 = t0; g_dxc.src = src; g_dxc.srcn = srcn;
            snprintf(g_dxc.entry, sizeof g_dxc.entry, "%s", entry); snprintf(g_dxc.prof, sizeof g_dxc.prof, "%s", prof);
        }
        pthread_mutex_unlock(&g_dxmx);
    }
    if (!hit) hr = rt_call(c, g_dxr[k], dx_nargs[k], a);
    double dt = mono() - t0;
    uint32_t code = !hr && dx_ppcode[k] >= 0 && a[dx_ppcode[k]] ? rd32(a[dx_ppcode[k]]) : 0;
    uint32_t err = dx_pperr[k] >= 0 && a[dx_pperr[k]] ? rd32(a[dx_pperr[k]]) : 0;
    uint32_t csz = code ? buf_call(c, code, BUF_SIZE) : 0;
    int stored = 0;
    if (k == DX_COMPILE && !hit && !hr && key && code && csz) { hc_store(key, M + buf_call(c, code, BUF_PTR), csz); stored = 1; }
    pthread_mutex_lock(&g_dxmx);
    g_dxs[k].n++; if (hr) g_dxs[k].fail++; g_dxs[k].secs += dt; g_dxs[k].hits += hit; g_dxs[k].stored += stored;
    if (k == DX_COMPILE && !hit) g_dx_busy += dt;
    if (g_dxc.active && g_dxc.n == n && k == DX_COMPILE) { g_dxc.active = 0; g_dxc.src = NULL; }
    pthread_mutex_unlock(&g_dxmx);
    if (hr || log || n <= 8 || n % 25 == 0 || dt > 1.0) {   /* every 25th call: progress during the first-run shader build */
        char what[96];
        if (k == DX_COMPILE) snprintf(what, sizeof what, "%.40s, %.12s, flags %x, %u bytes", entry, prof, a[6], a[1]);
        else if (k == DX_DISASM) snprintf(what, sizeof what, "shader %08x", a[0]);
        else snprintf(what, sizeof what, "%u bytes", a[1]);
        fprintf(stderr, "[d3dx] %s(%s) -> %08x, %u bytes out, %.1f ms (#%llu)%s%s\n", dx_name[k], what, hr, csz, dt * 1e3,
                (unsigned long long)n, hit ? " [cache]" : "", dt > 1.0 ? " [slow]" : "");
        if (hr && err) { uint32_t p = buf_call(c, err, BUF_PTR); if (p) fprintf(stderr, "[d3dx]   %.600s\n", GSTR(p)); }
    }
    if (src && (hr || dt > 2.0) && !hit) dump_src(hr ? "fail_" : "slow_", n, entry, prof, a[6], dt, hr, src, srcn);
    free(src);
    c->ebx = save.ebx; c->esi = save.esi; c->edi = save.edi; c->ebp = save.ebp; c->esp = save.esp;
    return hr;
}
static void dx_status(char* o, size_t cap, int final) {   /* final: fault/signal report - no blocking, save a running compile */
    double t = mono(); size_t L = 0; o[0] = 0;
    if (final) { if (pthread_mutex_trylock(&g_dxmx)) return; } else pthread_mutex_lock(&g_dxmx);
    const uint64_t done = g_dxs[DX_COMPILE].n;
    if (g_dxc.active) {
        L += (size_t)snprintf(o + L, cap - L, "d3dx: compiling #%llu %s %s for %.1f s; ", (unsigned long long)g_dxc.n, g_dxc.entry, g_dxc.prof, t - g_dxc.t0);
        if ((final || t - g_dxc.t0 > 15) && !g_dxc.dumped && g_dxc.src) { g_dxc.dumped = 1; dump_src("running_", g_dxc.n, g_dxc.entry, g_dxc.prof, g_dxc.flags, t - g_dxc.t0, 0, g_dxc.src, g_dxc.srcn); }
    } else L += (size_t)snprintf(o + L, cap - L, "d3dx: ");
    L += (size_t)snprintf(o + L, cap - L, "%llu compiled in %.1f s, %llu from cache", (unsigned long long)(done - g_dxs[DX_COMPILE].hits), g_dx_busy,
                          (unsigned long long)g_dxs[DX_COMPILE].hits);
    pthread_mutex_unlock(&g_dxmx);
    if (final) return;
    unsigned alive = 0; for (int i = 1; i < T_N; i++) alive += g_st.alive[i];
    if (L < cap) snprintf(o + L, cap - L, " | d3d: %llu frames, %llu draws, %u objects, %d shaders", (unsigned long long)g_st.frames,
                          (unsigned long long)(g_st.draws + g_st.updraws), alive, g_nshcrc);
}
void hle_d3d9_heartbeat(char* o, size_t cap) { dx_status(o, cap, 0); if (g_dumpf) fflush(g_dumpf); }   /* stdio locks the FILE itself */
void hle_d3dx_cache_stats(uint64_t* hits, uint64_t* stored) { *hits = g_dxs[DX_COMPILE].hits; *stored = g_dxs[DX_COMPILE].stored; }
#define DXWRAP(K, N, fb) static void w_##N(CPU* c) { \
    if (g_dxr[K]) { c->eax = dx_forward(c, K); return; } \
    lock_(); const char* sv = g_cur; g_cur = #N; c->eax = fb(c); g_cur = sv; unlock_(); }
DXWRAP(DX_COMPILE, D3DXCompileShader, x_D3DXCompileShader)
DXWRAP(DX_ASSEMBLE, D3DXAssembleShader, x_D3DXAssembleShader)
DXWRAP(DX_PREPROCESS, D3DXPreprocessShader, x_D3DXPreprocessShader)
DXWRAP(DX_DISASM, D3DXDisassembleShader, x_D3DXDisassembleShader)
#define L_DXF(X) X("d3dx9_31.dll", D3DXCompileShader, 10, 0) X("d3dx9_31.dll", D3DXAssembleShader, 7, 0) \
    X("d3dx9_31.dll", D3DXPreprocessShader, 6, 0) X("d3dx9_31.dll", D3DXDisassembleShader, 4, 0)
const HleDef hle_d3d9_defs[] = { ALL(DEF) L_FN(FDEF) L_DXF(FDEF) };
const unsigned hle_d3d9_ndefs = sizeof hle_d3d9_defs / sizeof hle_d3d9_defs[0];

static void build_vtables(void) {
    static const unsigned want[T_N] = { 0, 17, 119, 10, 22, 22, 22, 17, 11, 14, 14, 5, 5, 5, 8, 6, 5 };
    static uint32_t tmp[T_N][128]; unsigned n[T_N] = { 0 };
    for (unsigned i = 0; i < hle_d3d9_ndefs; i++) {
        const HleDef* e = &hle_d3d9_defs[i];
        if (strcmp(e->dll, "d3d9.com")) continue;
        size_t L = (size_t)(strstr(e->name, "::") - e->name);
        int t; for (t = 1; t < T_N; t++) if (strlen(tname[t]) == L && !strncmp(tname[t], e->name, L)) break;
        if (t == T_N || n[t] >= 128) { fprintf(stderr, "[d3d] bad interface entry %s\n", e->name); continue; }
        tmp[t][n[t]++] = hle_resolve_impl("d3d9.com", e->name);
    }
    for (int t = 1; t < T_N; t++) {
        if (n[t] != want[t]) fprintf(stderr, "[d3d] BUG: %s has %u methods, expected %u\n", tname[t], n[t], want[t]);
        g_vt[t] = ghalloc(4 * n[t] + 4, 1);
        for (unsigned k = 0; k < n[t]; k++) wr32(g_vt[t] + 4 * k, tmp[t][k]);
    }
}

unsigned hle_d3d9_frames(void) { return (unsigned)__atomic_load_n(&g_st.frames, __ATOMIC_RELAXED); }
void hle_d3d9_report(void) {
    for (int k = 0; k < DX_N; k++) if (g_dxs[k].n)
        fprintf(stderr, "[d3dx] %s: %llu calls, %llu failed, %.2f s total, %llu from cache, %llu stored (recompiled d3dx9_31.dll)\n", dx_name[k],
                (unsigned long long)g_dxs[k].n, (unsigned long long)g_dxs[k].fail, g_dxs[k].secs, (unsigned long long)g_dxs[k].hits,
                (unsigned long long)g_dxs[k].stored);
    if (g_dxc.active) { char b[512]; dx_status(b, sizeof b, 1); if (b[0]) fprintf(stderr, "[d3dx] in progress: %s\n", b); }
    if (!g_st.created[T_D3D]) return;
    fprintf(stderr, "[d3d] report: %llu frames, %llu draws (%llu UP), %llu primitives, %llu clears, %llu locks (%llu MB), %llu D3DXLoadSurfaceFromMemory\n",
            (unsigned long long)g_st.frames, (unsigned long long)g_st.draws, (unsigned long long)g_st.updraws,
            (unsigned long long)g_st.prims, (unsigned long long)g_st.clears, (unsigned long long)g_st.locks,
            (unsigned long long)(g_st.lockbytes >> 20), (unsigned long long)g_st.dxloads);
    fprintf(stderr, "[d3d] objects created/alive:");
    for (int t = 1; t < T_N; t++) if (g_st.created[t]) fprintf(stderr, " %s %u/%u", tname[t], g_st.created[t], g_st.alive[t]);
    fprintf(stderr, "\n[d3d] texture formats (count, MB):");
    for (int i = 0; i < g_nfmt; i++) fprintf(stderr, " %s %u %.1f", fmt_name(g_fmt[i].key), g_fmt[i].n, g_fmt[i].bytes / 1048576.0);
    fprintf(stderr, "\n[d3d] render target/depth formats (count, MB):");
    for (int i = 0; i < g_nrtfmt; i++) fprintf(stderr, " %s %u %.1f", fmt_name(g_rtfmt[i].key), g_rtfmt[i].n, g_rtfmt[i].bytes / 1048576.0);
    fprintf(stderr, "\n[d3d] shaders: %d unique;", g_nshcrc);
    for (int i = 0; i < g_nshv; i++)
        fprintf(stderr, " %s_%u_%u x%u", (g_shv[i].key >> 16) == 0xFFFEu ? "vs" : "ps", g_shv[i].key >> 8 & 0xFF, g_shv[i].key & 0xFF, g_shv[i].n);
    if (g_dumpf) { fflush(g_dumpf); fprintf(stderr, "; %u dumped to %s/shaders.pak\n", g_ndumped, g_dump); } else fprintf(stderr, "\n");
    gl_report();
}

#include "hle_d3d9_gl.h"
#include "hle_d3d9_vk.h"
