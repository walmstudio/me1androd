/* OpenGL ES 3 backend of hle_d3d9.c - included once at its end (shares its statics: Obj, Dev, formats, locks).
 *
 * Model: every D3D9 resource that the GPU can touch gets a GL object lazily (GlRes in Obj.gx): textures and render
 * targets -> immutable GL textures (glTexStorage), depth/stencil surfaces -> D24S8 renderbuffers, static vertex/index
 * buffers -> GL buffers (the locked range is uploaded at Unlock). Dynamic buffers and DrawPrimitiveUP data stay in guest
 * memory and the range a draw needs is streamed into a ring buffer. Shaders: sm3_glsl.c (D3D9 bytecode -> GLSL ES 3.00),
 * one GL program per (vertex shader, pixel shader) pair.
 * Images keep D3D's row order (row 0 = top = GL row 0): the vertex shader flips y (dx_fix), so render targets and
 * uploaded textures share one orientation and only the final blit to the window flips.
 * Threads: every thread that enters the D3D lock gets its own EGL context (shared objects); a non-presenting thread
 * flushes and leaves a fence at unlock, which the next context waits for (glWaitSync). The frame is copied to one of
 * three present slots; a presenter thread (own context) blits it letterboxed to the window (Android: me1gl_set_window).
 * Env: ME1_GL=0 null backend | 1 (default when EGL/GLES load), ME1_GL_DEBUG=1 (glGetError checks, shader logs),
 *      ME1_FRAME_DUMP=dir (PNG of presented frames), ME1_FRAME_DUMP_EVERY=n (default 60), ME1_GLSL_DUMP=dir (programs),
 *      ME1_TEX_MAXSIZE=n (skip top mip levels above n), ME1_TEX_16=0|1|2 (CPU-decoded DXT1 -> RGB5A1 (default 1), 2: DXT3/5 -> RGBA4 too),
 *      ME1_GL_NOS3TC=1 / ME1_GL_NORGTC=1 (force CPU decoding), ME1_GL_CACHE=dir (program binaries),
 *      ME1_GL_FAKEWIN=WxH (presenter draws into a pbuffer "window": tests without a display). */
#define GL_GLES_PROTOTYPES 0
#if !defined(__ANDROID__) && !defined(__GBM__)
#define __GBM__ 1
#endif
/* v0.10: the renderer hooks of the D3D9 layer are dispatched in hle_d3d9_vk.h (Vulkan renderer or this GLES backend) */
#define gl_free gles_free
#define gl_tex_unlock gles_tex_unlock
#define gl_buf_unlock gles_buf_unlock
#define gl_init gles_init
#define gl_release gles_release
#define gl_present gles_present
#define gl_frame_times gles_frame_times
#define gl_clear gles_clear
#define gl_draw_x gles_draw_x
#define gl_stretch gles_stretch
#define gl_colorfill gles_colorfill
#define gl_update_surface gles_update_surface
#define gl_update_texture gles_update_texture
#define gl_get_rt_data gles_get_rt_data
#define gl_rt_lock gles_rt_lock
#define gl_report gles_report
#define me1gl_set_window gles_set_window
#define me1gl_frame_times gles_frame_times_api
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>
#include "sm3_glsl.h"
#include "me1_platform.h"
/* v1.1 (Linux port): window system of the desktop front end (me1_platform.h); 0 = none (Android, tests) */
static int g_ws_kind; static void* g_ws_dpy; static unsigned long g_ws_visual;
void me1_platform_set(int kind, void* display, unsigned long x11_visual) { g_ws_kind = display ? kind : 0; g_ws_dpy = display; g_ws_visual = x11_visual; }
int me1_platform_kind(void) { return g_ws_kind; }

#define XS3TC_DXT1 0x83F1
#define XS3TC_DXT3 0x83F2
#define XS3TC_DXT5 0x83F3
#define XRGTC1 0x8DBB
#define XRGTC2 0x8DBD
#define XANISO 0x84FE
#define XMAXANISO 0x84FF
#define XMIRRORCLAMP 0x8743
#define XEGL_SURFACELESS 0x31DD
#define XEGL_ES3_BIT 0x40

/* ---- loader ---- */
#define GLF(X) X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLGETSTRINGIPROC, glGetStringi) X(PFNGLGETINTEGERVPROC, glGetIntegerv) \
    X(PFNGLGETFLOATVPROC, glGetFloatv) X(PFNGLGETERRORPROC, glGetError) X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate) \
    X(PFNGLBLENDCOLORPROC, glBlendColor) X(PFNGLCOLORMASKPROC, glColorMask) X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLDEPTHMASKPROC, glDepthMask) X(PFNGLDEPTHRANGEFPROC, glDepthRangef) X(PFNGLCULLFACEPROC, glCullFace) \
    X(PFNGLFRONTFACEPROC, glFrontFace) X(PFNGLSTENCILFUNCSEPARATEPROC, glStencilFuncSeparate) \
    X(PFNGLSTENCILOPSEPARATEPROC, glStencilOpSeparate) X(PFNGLSTENCILMASKSEPARATEPROC, glStencilMaskSeparate) \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset) X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLSCISSORPROC, glScissor) \
    X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLCLEARDEPTHFPROC, glClearDepthf) X(PFNGLCLEARSTENCILPROC, glClearStencil) \
    X(PFNGLCLEARPROC, glClear) X(PFNGLCLEARBUFFERFVPROC, glClearBufferfv) X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLDELETETEXTURESPROC, glDeleteTextures) \
    X(PFNGLBINDTEXTUREPROC, glBindTexture) X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D) \
    X(PFNGLTEXSTORAGE3DPROC, glTexStorage3D) X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) X(PFNGLTEXSUBIMAGE3DPROC, glTexSubImage3D) \
    X(PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC, glCompressedTexSubImage2D) X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) X(PFNGLGENSAMPLERSPROC, glGenSamplers) X(PFNGLBINDSAMPLERPROC, glBindSampler) \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri) X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf) \
    X(PFNGLSAMPLERPARAMETERFVPROC, glSamplerParameterfv) X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLFRAMEBUFFERTEXTURELAYERPROC, glFramebufferTextureLayer) X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) X(PFNGLDRAWBUFFERSPROC, glDrawBuffers) \
    X(PFNGLREADBUFFERPROC, glReadBuffer) X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM4FVPROC, glUniform4fv) \
    X(PFNGLUNIFORM4IVPROC, glUniform4iv) X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLVERTEXATTRIB4FPROC, glVertexAttrib4f) X(PFNGLVERTEXATTRIBDIVISORPROC, glVertexAttribDivisor) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLDRAWELEMENTSPROC, glDrawElements) \
    X(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced) X(PFNGLFENCESYNCPROC, glFenceSync) \
    X(PFNGLWAITSYNCPROC, glWaitSync) X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) X(PFNGLDELETESYNCPROC, glDeleteSync) \
    X(PFNGLINVALIDATEFRAMEBUFFERPROC, glInvalidateFramebuffer) X(PFNGLFLUSHPROC, glFlush) X(PFNGLFINISHPROC, glFinish)
#define GLF_OPT(X) X(PFNGLCOLORMASKIPROC, glColorMaski) X(PFNGLGETPROGRAMBINARYPROC, glGetProgramBinary) \
    X(PFNGLPROGRAMBINARYPROC, glProgramBinary) X(PFNGLPROGRAMPARAMETERIPROC, glProgramParameteri) \
    X(PFNGLGENQUERIESPROC, glGenQueries) X(PFNGLDELETEQUERIESPROC, glDeleteQueries) X(PFNGLBEGINQUERYPROC, glBeginQuery) \
    X(PFNGLENDQUERYPROC, glEndQuery) X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv)
#define GL_DECL(T, n) static T n;
GLF(GL_DECL) GLF_OPT(GL_DECL)
#define EGLF(X) X(eglGetProcAddress) X(eglGetDisplay) X(eglInitialize) X(eglBindAPI) X(eglChooseConfig) X(eglGetConfigAttrib) \
    X(eglCreateContext) X(eglDestroyContext) X(eglCreatePbufferSurface) X(eglCreateWindowSurface) X(eglDestroySurface) \
    X(eglMakeCurrent) X(eglSwapBuffers) X(eglSwapInterval) X(eglGetError) X(eglQueryString) X(eglQuerySurface)
#define EGL_DECL(n) static __typeof__(n)* p_##n;
EGLF(EGL_DECL)

/* ---- backend state ---- */
static EGLDisplay g_dpy; static EGLConfig g_cfg; static EGLContext g_ctx0; static int g_surfless, g_winbit;
static struct { int s3tc, rgtc, aniso, border, flin, maski, nbin, es32, mirror; float maxaniso; int maxtex; } GX;
static struct { uint64_t draws, skipped, texup, texbytes, bufup, bufbytes, ring, progs, progfail, progbin, fbos, clears, blits, shfail, shtrans, decoded, rhw; int64_t texmem; double tbuilt; } GS;   /* v0.10.12 (P0-8): texmem signed - the log showed a wrap to 2^64 */
static void texmem_add(int64_t v) {
    GS.texmem += v;
    if (GS.texmem < 0) { static int k; if (k++ < 3) fprintf(stderr, "[gl] texmem accounting went negative (%lld) - a double free or a changed size\n", (long long)GS.texmem); GS.texmem = 0; }
}
static int g_gldbg, g_texmax, g_tex16, g_dumpevery = 60; static const char* g_fdump; static const char* g_glsldump; static char g_bincache[900];
static uint32_t g_serial;

typedef struct { uint64_t k[5]; GLuint fbo; uint32_t tick; } FboEnt;
#define NFBO 24
enum { S_BLEND, S_DEPTH, S_CULL, S_STENCIL, S_SCISSOR, S_POFS, S_BFUNC, S_BEQ, S_BCOL, S_CMASK, S_CMASK1, S_CMASK2, S_CMASK3,
       S_DMASK, S_DFUNC, S_CULLF, S_SFRONT, S_SBACK, S_SOPF, S_SOPB, S_SWMASK, S_PO1, S_PO2, S_VP0, S_VP1, S_VP2, S_VP3,
       S_DR0, S_DR1, S_SC0, S_SC1, S_SC2, S_SC3, S_PROG, S_FBO, S_ABUF, S_EBUF, S_N };
typedef struct GlCtx {
    EGLContext ctx; EGLSurface pb; int id, used;
    FboEnt fb[NFBO]; uint32_t tick; GLuint scr[2]; int fblast;   /* fblast: v0.10.2 last FBO returned by fbo_get */
    GLuint ring[2]; uint32_t roff[2], rsize[2];
    GLsync fence; uint32_t fseq, seen[16];
    uint32_t* stale; int nstale, capstale;
    uint32_t st[S_N]; GLuint tex[24]; GLenum ttg[24]; GLuint smp[24]; uint32_t attr_on, div[16];
} GlCtx;
static GlCtx* g_ctxs[16]; static int g_nctx;
static __thread GlCtx* t_ctx;
static GlCtx* g_lastctx; static GlCtx* g_primary;

typedef struct { uint32_t d3d; GLenum ifmt, fmt, type; uint8_t conv, nch; const char* sw; const char* swrt; } GFmt;
enum { CV_RAW, CV_DXT1, CV_DXT3, CV_DXT5, CV_BC4, CV_BC5, CV_1555, CV_U16, CV_S16 };
typedef struct GlRes {
    uint32_t serial; int kind;           /* 1 texture, 2 renderbuffer, 3 buffer */
    GLuint name; GLenum target; const GFmt* f;
    int dec;                             /* compressed format decoded on the CPU into ifmt/fmt/type */
    GLenum ifmt, fmt, type; int obpp;    /* storage and upload format (after conversion) */
    int drop, levels, w, h, d, rt, base;
    uint64_t bytes;
} GlRes;
typedef struct { Sm3 s; int ok, done; uint32_t serial; } GlSh;   /* done: translated (lazily, at the first program link) */
typedef struct GlProg {
    struct GlProg* next; uint32_t vs, ps; GLuint p; int ok;
    GLint l_vc, l_vi, l_vb, l_fix, l_bgra, l_pc, l_pi, l_pb, l_at, l_srgb, l_srgbw, l_vp;
    uint32_t vver, pver; Dev* dev; float fix[4], at[2], vpu[4]; int bgra, srgb, srgbw, init;
} GlProg;
#define PHASH 1024
static GlProg* g_prog[PHASH];
typedef struct { uint64_t k; uint32_t border; GLuint s; } SmpEnt;
#define NSMP 1024
static SmpEnt g_smp[NSMP]; static int g_nsmp;
static GLuint g_dummy[3];   /* 1x1 (0,0,0,1): 2D, cube, 3D */

static void glog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void presenter_start(void); static void gt_init(const char* ex);
static void glog(const char* fmt, ...) { va_list ap; va_start(ap, fmt); fputs("[gl] ", stderr); vfprintf(stderr, fmt, ap); va_end(ap); }
static void glcheck(const char* where) {
    if (!g_gldbg) return;
    GLenum e; int n = 0;
    while ((e = glGetError()) != GL_NO_ERROR && n++ < 4) { static int k; if (k++ < 200) glog("GL error 0x%04x in %s (%s)\n", e, where, g_cur ? g_cur : "-"); }
}

static void* dlopen_any(const char* const* names) {
    for (int i = 0; names[i]; i++) { void* h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL); if (h) return h; }
    return NULL;
}
/* ---- GPU driver (v0.9.8): ME1_GPU_DRIVER = "" / "system" (the device's GLES), "zink" (Mesa zink on the system Vulkan) or the
 * path of an AdrenoTools-style custom Vulkan driver (Turnip, Qualcomm/Mali builds: <dir>/<libraryName>.so), opened with libadrenotools.
 * The game keeps talking GLES: Mesa's libEGL_mesa/libGLESv2_mesa (zink) translate it to Vulkan. Surfaceless Mesa has no Android
 * window system, so frames reach the window through the CPU (ANativeWindow_lock, see the presenter). Any failure -> system GLES. */
static int g_zink, g_cpupresent, g_cpuforce;
static char g_drvinfo[300];
static int vk_has_ext(const char* ex, uint32_t n, const char* e) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(ex + i * 260, e)) return 1;
    return 0;
}
/* v0.9.9: what Mesa zink 26 needs from the device - logged so a failing driver says why (zink itself is terse).
 * v0.10.12 (P1-1): also returns whether zink may run here; VK_KHR_maintenance5 is now REQUIRED (upstream Mesa 26
 * declares it required; zink on Adreno 750 without it died with VK_ERROR_UNKNOWN on the first pipelines). */
static int g_zink_ok = 1;
static int vk_dump_dev(void* (*gipa)(void*, const char*), void* inst, void* pd, uint32_t api, const char* what) {
    typedef int32_t (*EDE)(void*, const char*, uint32_t*, void*); typedef void (*GF2)(void*, void*); typedef void (*GFP)(void*, int32_t, void*);
    EDE ede = (EDE)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    GF2 gf2 = (GF2)gipa(inst, "vkGetPhysicalDeviceFeatures2"); if (!gf2) gf2 = (GF2)gipa(inst, "vkGetPhysicalDeviceFeatures2KHR");
    GFP gfp = (GFP)gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    uint32_t n = 0; char* ex = NULL;   /* VkExtensionProperties: name[256] + specVersion */
    if (ede && ede(pd, NULL, &n, NULL) >= 0 && n && (ex = calloc(n, 260))) { if (ede(pd, NULL, &n, ex) < 0) n = 0; } else n = 0;
#define HAS(e) vk_has_ext(ex, n, e)
    char line[1200]; int k = 0;
    for (uint32_t i = 0; i < n; i++) {   /* the whole list, ~1 KB per line */
        k += snprintf(line + k, sizeof line - k, "%s%s", k ? " " : "", ex + i * 260 + 0);
        if (k > 1000 || i + 1 == n) { glog("%s: device extensions: %s\n", what, line); k = 0; }
    }
    unsigned v = (api >> 22) * 100 + ((api >> 12) & 0x3FF);
    struct { const char* e; unsigned core; int need; } req[] = {
        { "VK_KHR_maintenance1", 101, 1 }, { "VK_KHR_descriptor_update_template", 101, 1 }, { "VK_KHR_create_renderpass2", 102, 1 },
        { "VK_KHR_timeline_semaphore", 102, 1 }, { "VK_KHR_dynamic_rendering", 103, 1 }, { "VK_KHR_maintenance5", 104, 1 },
        { "VK_KHR_push_descriptor", 104, 0 }, { "VK_EXT_scalar_block_layout", 102, 0 }, { "VK_EXT_custom_border_color", 999, 0 },
        { "VK_EXT_line_rasterization", 104, 0 }, { "VK_KHR_synchronization2", 103, 0 }, { "VK_EXT_extended_dynamic_state", 103, 0 },
        { "VK_EXT_extended_dynamic_state2", 103, 0 }, { "VK_EXT_calibrated_timestamps", 999, 0 }, { "VK_KHR_imageless_framebuffer", 102, 0 } };
    char miss[600] = "", opt[600] = ""; int nm = 0;
    int rb2 = HAS("VK_EXT_robustness2") || HAS("VK_KHR_robustness2");
    for (unsigned i = 0; i < sizeof req / sizeof *req; i++) {
        int ok = v >= req[i].core || HAS(req[i].e);
        if (!strcmp(req[i].e, "VK_EXT_line_rasterization")) ok = ok || HAS("VK_KHR_line_rasterization");
        if (!ok) { char* d = req[i].need ? miss : opt; size_t l = strlen(d); snprintf(d + l, 600 - l, "%s%s", l ? " " : "", req[i].e); nm += req[i].need; }
    }
    /* features: VkPhysicalDeviceFeatures2 -> robustness2 (nullDescriptor), timeline semaphore, dynamic rendering */
    uint32_t f2[2 + 2 + 55] = { 0 }, frb[4 + 3] = { 0 }, ftl[4 + 1] = { 0 }, fdr[4 + 1] = { 0 };
    void* chain = NULL;
#define LINK(arr, st) do { arr[0] = st; memcpy(&arr[2], &chain, sizeof chain); chain = arr; } while (0)
    if (rb2) LINK(frb, 1000286000u);
    if (v < 102 && HAS("VK_KHR_timeline_semaphore")) LINK(ftl, 1000207000u);
    if (v < 103 && HAS("VK_KHR_dynamic_rendering")) LINK(fdr, 1000044003u);
    f2[0] = 1000059000u; memcpy(&f2[2], &chain, sizeof chain);
    int nulld = -1, tl = -1, dr = -1, lop = -1;
    if (gf2) { gf2(pd, f2); lop = (int)f2[4 + 8]; if (rb2) nulld = (int)frb[6]; if (ftl[0] && v < 102) tl = (int)ftl[4]; if (fdr[0]) dr = (int)fdr[4]; }
#undef LINK
    if (nulld == 0 || !rb2) { size_t l = strlen(miss); snprintf(miss + l, 600 - l, "%s%s", l ? " " : "", rb2 ? "robustness2.nullDescriptor" : "VK_EXT_robustness2"); nm++; }
    if (tl == 0) { size_t l = strlen(miss); snprintf(miss + l, 600 - l, "%stimelineSemaphore", l ? " " : ""); nm++; }
    if (dr == 0) { size_t l = strlen(miss); snprintf(miss + l, 600 - l, "%sdynamicRendering", l ? " " : ""); nm++; }
    uint32_t fp[3] = { 0 }, fpb[3] = { 0 }, fpd[3] = { 0 };
    if (gfp) { gfp(pd, 37, fp); gfp(pd, 44, fpb); gfp(pd, 129, fpd); }   /* R8G8B8A8_UNORM, B8G8R8A8_UNORM, D24_UNORM_S8_UINT */
    glog("%s: zink check: %u extensions; nullDescriptor %d timeline %d dynamicRendering %d logicOp %d; RGBA8 rt %d BGRA8 rt %d D24S8 %d\n",
         what, n, nulld, tl, dr, lop, !!(fp[1] & 0x80), !!(fpb[1] & 0x80), !!(fpd[1] & 0x200));
    if (nm) glog("%s: Mesa zink 26 cannot run here - the driver lacks: %s\n", what, miss);
    else glog("%s: Mesa zink requirements met\n", what);
    if (*opt) glog("%s: optional for zink, missing: %s\n", what, opt);
#undef HAS
    free(ex);
    const char* ov = getenv("ME1_ZINK_NO_MAINT5");   /* P1-1: force zink on anyway (untested upstream paths) */
    g_zink_ok = nm == 0 || (ov && *ov != '0');
    return g_zink_ok;
}
static int vk_probe(void* lib, const char* what) {
    typedef void* (*GIPA)(void*, const char*);
    GIPA gipa = (GIPA)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) { glog("%s: no vkGetInstanceProcAddr\n", what); return 0; }
    typedef int32_t (*CI)(const void*, const void*, void**); typedef int32_t (*EPD)(void*, uint32_t*, void**);
    typedef void (*GPP)(void*, void*); typedef void (*DI)(void*, const void*);
    CI ci = (CI)gipa(NULL, "vkCreateInstance");
    struct { int32_t sType; const void* next; const char* app; uint32_t appv; const char* eng; uint32_t engv; uint32_t api; }
        ai = { 0, NULL, "MassEffect", 1, "me1port", 1, (1u << 22) | (1u << 12) };                        /* VkApplicationInfo, Vulkan 1.1 */
    struct { int32_t sType; const void* next; uint32_t flags; const void* app; uint32_t nl; const char* const* l; uint32_t ne; const char* const* e; }
        ici = { 1, NULL, 0, &ai, 0, NULL, 0, NULL };                                                     /* VkInstanceCreateInfo */
    void* inst = NULL; int32_t r = ci ? ci(&ici, NULL, &inst) : -1;
    if (r || !inst) { glog("%s: vkCreateInstance failed (%d)\n", what, r); return 0; }
    EPD epd = (EPD)gipa(inst, "vkEnumeratePhysicalDevices"); GPP gpp = (GPP)gipa(inst, "vkGetPhysicalDeviceProperties");
    DI di = (DI)gipa(inst, "vkDestroyInstance");
    uint32_t n = 4; void* pd[4] = { 0 };
    if (!epd || epd(inst, &n, pd) < 0) n = 0;
    if (!n) glog("%s: no Vulkan devices\n", what);
    for (uint32_t i = 0; i < n && gpp; i++) {
        static uint8_t p[4096]; memset(p, 0, sizeof p); gpp(pd[i], p);   /* VkPhysicalDeviceProperties: api, driver, vendor, device, type, name[256] */
        uint32_t api, drv, ven; memcpy(&api, p, 4); memcpy(&drv, p + 4, 4); memcpy(&ven, p + 8, 4); p[20 + 255] = 0;
        glog("%s: Vulkan device %u: %s, Vulkan %u.%u.%u, driver version 0x%08x, vendor 0x%04x\n", what, i, (char*)p + 20,
             api >> 22, (api >> 12) & 0x3FF, api & 0xFFF, drv, ven);
        if (!i) snprintf(g_drvinfo, sizeof g_drvinfo, "%s: %s, Vulkan %u.%u", what, (char*)p + 20, api >> 22, (api >> 12) & 0x3FF);
        if (!i) vk_dump_dev((void* (*)(void*, const char*))gipa, inst, pd[i], api, what);
    }
    if (di) di(inst, NULL);
    return (int)n;
}
/* a custom Vulkan driver package (AdrenoTools layout: <dir>/<library>.so): a handle exporting vkGetInstanceProcAddr
 * (libadrenotools' private loader, or the plain ICD behind libme1vkshim.so); NULL if it does not load or finds no GPU */
static void* vk_open_custom(const char* e) {
    void* vk = NULL;
    char dir[1024], hook[1024] = ""; snprintf(dir, sizeof dir, "%s", e);
    char* sl = strrchr(dir, '/');
    if (!sl) { glog("GPU driver %s: need an absolute path\n", e); return NULL; }
    const char* name = e + (sl - dir) + 1; sl[1] = 0;
    Dl_info di;   /* libadrenotools wants the app's nativeLibraryDir (its hook libraries live there, next to us) */
    if (dladdr((void*)vk_open_custom, &di) && di.dli_fname) { snprintf(hook, sizeof hook, "%s", di.dli_fname); char* h = strrchr(hook, '/'); if (h) h[1] = 0; }
    if (getenv("ME1_GPU_HOOKDIR")) snprintf(hook, sizeof hook, "%s", getenv("ME1_GPU_HOOKDIR"));
    const char* label = getenv("ME1_GPU_DRIVER_NAME"); if (!label || !*label) label = name;
    /* 1) libadrenotools: the system Vulkan loader in its own namespace, its HAL driver load redirected to the package */
    void* at = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
    typedef void* (*OPEN)(int, int, const char*, const char*, const char*, const char*, const char*, void**);
    OPEN op = at ? (OPEN)dlsym(at, "adrenotools_open_libvulkan") : NULL;
    if (!op) glog("GPU driver %s: libadrenotools not available (%s)\n", label, dlerror());
    else {
        vk = op(RTLD_NOW, 1 /* ADRENOTOOLS_DRIVER_CUSTOM */, NULL, hook, dir, name, NULL, NULL);
        if (!vk) glog("GPU driver %s: adrenotools_open_libvulkan failed (dir %s, hooks %s)\n", label, dir, hook);
        else if (!vk_probe(vk, label)) { glog("GPU driver %s: no GPU through libadrenotools\n", label); vk = NULL; }
    }
    /* 2) the driver as a plain Vulkan ICD (vk_icdGetInstanceProcAddr) behind libme1vkshim.so - drivers without vendor deps */
    if (!vk) {
        void* icd = dlopen(e, RTLD_NOW | RTLD_LOCAL);
        void* gipa = icd ? dlsym(icd, "vk_icdGetInstanceProcAddr") : NULL;
        if (icd && !gipa) gipa = dlsym(icd, "vkGetInstanceProcAddr");
        void* shim = gipa ? dlopen("libme1vkshim.so", RTLD_NOW | RTLD_LOCAL) : NULL;
        void (*set)(void*) = shim ? (void (*)(void*))dlsym(shim, "me1vk_set_icd") : NULL;
        if (!icd) glog("GPU driver %s: direct load failed (%s)\n", label, dlerror());
        else if (!gipa) glog("GPU driver %s: not a Vulkan ICD (no vk_icdGetInstanceProcAddr)\n", label);
        else if (!set) glog("GPU driver %s: libme1vkshim.so missing (%s)\n", label, dlerror());
        else {
            typedef int32_t (*NEG)(uint32_t*);
            NEG neg = (NEG)dlsym(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
            uint32_t iv = 5; if (neg) neg(&iv);
            set(gipa);
            char lb[300]; snprintf(lb, sizeof lb, "%s (direct ICD, interface %u)", label, neg ? iv : 0);
            if (vk_probe(shim, lb)) vk = shim; else glog("GPU driver %s: no GPU as a direct ICD\n", label);
        }
    }
    return vk;
}
static void gpu_driver_setup(void) {
    const char* e = getenv("ME1_GL_PRESENT");
    g_cpuforce = e && !strcmp(e, "cpu"); g_cpupresent = g_cpuforce; g_zink = 0;
    e = getenv("ME1_GPU_DRIVER");
    if (!e || !*e || !strcmp(e, "system") || !strcmp(e, "gles")) return;
    void* vk = NULL; int custom = strcmp(e, "zink") != 0;
    if (!custom) {
        vk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!vk) vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!vk) { glog("GPU driver zink: no libvulkan (%s) -> system GLES\n", dlerror()); return; }
        if (!vk_probe(vk, "system Vulkan")) { glog("GPU driver zink: no usable Vulkan device -> system GLES\n"); return; }
        if (!g_zink_ok) { glog("GPU driver zink: system Vulkan lacks Mesa-26 requirements (VK_KHR_maintenance5 et al) -> system GLES (ME1_ZINK_NO_MAINT5=1 to force)\n"); return; }   /* v0.10.12 (P1-1) */
    } else {
        vk = vk_open_custom(e);
        if (!vk) { glog("GPU driver %s: the driver did not load or found no GPU -> system GLES\n", e); return; }
        char h[40]; snprintf(h, sizeof h, "0x%llx", (unsigned long long)(uintptr_t)vk);
        setenv("ZINK_VK_LOADER_HANDLE", h, 1);   /* patched Mesa zink takes vkGetInstanceProcAddr from this handle */
    }
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1); setenv("GALLIUM_DRIVER", "zink", 1);
    if (!getenv("MESA_LOG")) setenv("MESA_LOG", "file,android", 1);   /* "file" without MESA_LOG_FILE = stderr -> me1exe_run.log */
    const char* gc = getenv("ME1_GL_CACHE");
    if (gc && *gc && strcmp(gc, "off") && !getenv("MESA_SHADER_CACHE_DIR")) {
        char d[1024]; snprintf(d, sizeof d, "%s/mesa", gc); mkdir(gc, 0755); mkdir(d, 0755); setenv("MESA_SHADER_CACHE_DIR", d, 1);
    }
    g_zink = 1; g_cpupresent = 1;
}
static void gpu_driver_fallback(const char* why) {
    glog("GPU driver: Mesa zink failed (%s) -> system GLES\n", why);
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE"); unsetenv("GALLIUM_DRIVER");   /* a Mesa-based system EGL must not try zink as well */
    g_zink = 0; g_cpupresent = g_cpuforce; snprintf(g_drvinfo, sizeof g_drvinfo, "system GLES (zink failed: %s)", why);
}
static int gl_load(void) {
    static const char* const egln[] = { "libEGL.so", "libEGL.so.1", NULL };
    static const char* const glesn[] = { "libGLESv3.so", "libGLESv2.so", "libGLESv2.so.2", NULL };
    static const char* const megln[] = { "libEGL_mesa.so", NULL }, * const mglesn[] = { "libGLESv2_mesa.so", NULL };
    void* le = dlopen_any(g_zink ? megln : egln); if (!le) { glog("no libEGL%s (%s): null backend\n", g_zink ? "_mesa" : "", dlerror()); return 0; }
    void* lg = dlopen_any(g_zink ? mglesn : glesn); if (!lg) { glog("no libGLESv2/v3%s (%s): null backend\n", g_zink ? "_mesa" : "", dlerror()); return 0; }
#define EGL_LOAD(n) p_##n = (__typeof__(p_##n))dlsym(le, #n); if (!p_##n) { glog("EGL: %s missing\n", #n); return 0; }
    EGLF(EGL_LOAD)
#define GL_LOAD(T, n) n = (T)dlsym(lg, #n); if (!n) n = (T)p_eglGetProcAddress(#n); if (!n) { glog("GLES: %s missing\n", #n); return 0; }
    GLF(GL_LOAD)
#define GL_LOADO(T, n) n = (T)dlsym(lg, #n); if (!n) n = (T)p_eglGetProcAddress(#n);
    GLF_OPT(GL_LOADO)
    return 1;
}
static int has_ext(const char* list, const char* e) {
    size_t n = strlen(e); const char* p = list;
    while (p && (p = strstr(p, e))) { if ((p == list || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return 1; p += n; }
    return 0;
}
static EGLContext mk_ctx(EGLContext share) {
    static const EGLint a32[] = { 0x3098, 3, 0x30FB, 2, EGL_NONE }, a31[] = { 0x3098, 3, 0x30FB, 1, EGL_NONE }, a30[] = { 0x3098, 3, EGL_NONE };
    const EGLint* at[3] = { a32, a31, a30 };
    for (int i = 0; i < 3; i++) { EGLContext c = p_eglCreateContext(g_dpy, g_cfg, share, at[i]); if (c != EGL_NO_CONTEXT) return c; }
    return EGL_NO_CONTEXT;
}
static EGLSurface mk_pb(int w, int h) { EGLint a[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE }; return p_eglCreatePbufferSurface(g_dpy, g_cfg, a); }
static void ctx_defaults(GlCtx* C) {
    memset(C->st, 0xEE, sizeof C->st); memset(C->tex, 0xFF, sizeof C->tex); memset(C->smp, 0xFF, sizeof C->smp);
    C->attr_on = 0; memset(C->div, 0, sizeof C->div);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glFrontFace(GL_CCW);
    glGenFramebuffers(2, C->scr);
}
static GlCtx* ctx_new(EGLContext share) {
    if (g_nctx >= 16) { static int n; if (!n++) glog("too many GL contexts (16)\n"); return NULL; }
    GlCtx* C = calloc(1, sizeof *C);
    C->ctx = mk_ctx(share);
    if (C->ctx == EGL_NO_CONTEXT) { glog("eglCreateContext failed: 0x%x\n", p_eglGetError()); free(C); return NULL; }
    if (!g_surfless) C->pb = mk_pb(16, 16);
    if (!p_eglMakeCurrent(g_dpy, C->pb ? C->pb : EGL_NO_SURFACE, C->pb ? C->pb : EGL_NO_SURFACE, C->ctx)) {
        glog("eglMakeCurrent failed: 0x%x\n", p_eglGetError()); p_eglDestroyContext(g_dpy, C->ctx); free(C); return NULL;
    }
    C->id = g_nctx; g_ctxs[g_nctx++] = C;
    ctx_defaults(C);
    return C;
}
static int g_eglmaj, g_eglmin;
/* EGL display, config and the first context (NULL: failed) */
static GlCtx* egl_open(void) {
    const char* cext = p_eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    g_dpy = EGL_NO_DISPLAY;
#ifndef __ANDROID__
    if (g_ws_kind && cext && has_ext(cext, "EGL_EXT_platform_base")) {   /* v1.1: the desktop window's display (X11 / Wayland) */
        typedef EGLDisplay (*GPD)(EGLenum, void*, const EGLint*);
        GPD gpd = (GPD)p_eglGetProcAddress("eglGetPlatformDisplayEXT");
        const char* need = g_ws_kind == ME1_WS_X11 ? "EGL_EXT_platform_x11" : "EGL_EXT_platform_wayland";
        if (gpd && (has_ext(cext, need) || has_ext(cext, g_ws_kind == ME1_WS_X11 ? "EGL_KHR_platform_x11" : "EGL_KHR_platform_wayland")))
            g_dpy = gpd(g_ws_kind == ME1_WS_X11 ? 0x31D5 /* EGL_PLATFORM_X11_KHR */ : 0x31D8 /* EGL_PLATFORM_WAYLAND_KHR */, g_ws_dpy, NULL);
        if (g_dpy == EGL_NO_DISPLAY) { glog("no EGL display for the %s window (%s missing?): no window output\n", g_ws_kind == ME1_WS_X11 ? "X11" : "Wayland", need); g_ws_kind = 0; }
        else glog("EGL on the %s display of the window\n", g_ws_kind == ME1_WS_X11 ? "X11" : "Wayland");
    }
    if (g_dpy == EGL_NO_DISPLAY && cext && has_ext(cext, "EGL_MESA_platform_surfaceless")) {
#else
    if (g_zink && cext && has_ext(cext, "EGL_MESA_platform_surfaceless")) {
#endif
        typedef EGLDisplay (*GPD)(EGLenum, void*, const EGLint*);
        GPD gpd = (GPD)p_eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (gpd) g_dpy = gpd(XEGL_SURFACELESS, EGL_DEFAULT_DISPLAY, NULL);
    }
    if (g_dpy == EGL_NO_DISPLAY) g_dpy = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    g_eglmaj = g_eglmin = 0;
    if (g_dpy == EGL_NO_DISPLAY || !p_eglInitialize(g_dpy, &g_eglmaj, &g_eglmin)) { glog("eglInitialize failed (0x%x): null backend\n", p_eglGetError()); return NULL; }
    p_eglBindAPI(EGL_OPENGL_ES_API);
    const char* dext = p_eglQueryString(g_dpy, EGL_EXTENSIONS);
    g_surfless = dext && has_ext(dext, "EGL_KHR_surfaceless_context");
    EGLint n = 0, want[] = { EGL_RENDERABLE_TYPE, XEGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLConfig cf[64];
    g_winbit = 1;
    if ((!p_eglChooseConfig(g_dpy, want, cf, 64, &n) || n < 1) && g_ws_kind && g_surfless) {   /* v1.1: Wayland has no pbuffers */
        want[3] = EGL_WINDOW_BIT; if (!p_eglChooseConfig(g_dpy, want, cf, 64, &n)) n = 0;
        want[3] = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
    }
    if (n < 1) {
        g_winbit = 0; want[3] = EGL_PBUFFER_BIT;
        if (!p_eglChooseConfig(g_dpy, want, cf, 64, &n) || n < 1) {   /* surfaceless-only displays */
            want[3] = 0;
            if (!g_surfless || !p_eglChooseConfig(g_dpy, want, cf, 64, &n) || n < 1) { glog("no GLES3 EGL config: null backend\n"); return NULL; }
        }
    }
    g_cfg = cf[0];
    int vmatch = 0;
#ifndef __ANDROID__
    if (g_ws_kind == ME1_WS_X11 && g_ws_visual && g_winbit)   /* v1.1: a config of the window's X visual (else BAD_MATCH) */
        for (int i = 0; i < n && !vmatch; i++) {
            EGLint vid = 0, dz = 0; p_eglGetConfigAttrib(g_dpy, cf[i], EGL_NATIVE_VISUAL_ID, &vid); p_eglGetConfigAttrib(g_dpy, cf[i], EGL_DEPTH_SIZE, &dz);
            if ((unsigned long)vid == g_ws_visual && dz == 0) { g_cfg = cf[i]; vmatch = 1; }
        }
#endif
    for (int i = 0; i < n && !vmatch; i++) {   /* prefer exactly RGB(A)888(8) without depth: we render into FBOs */
        EGLint r, a, dz; p_eglGetConfigAttrib(g_dpy, cf[i], EGL_RED_SIZE, &r); p_eglGetConfigAttrib(g_dpy, cf[i], EGL_ALPHA_SIZE, &a);
        p_eglGetConfigAttrib(g_dpy, cf[i], EGL_DEPTH_SIZE, &dz);
        if (r == 8 && dz == 0) { g_cfg = cf[i]; if (a == 8 || a == 0) break; }
    }
    GlCtx* C = ctx_new(EGL_NO_CONTEXT);
    if (!C) return NULL;
    return C;
}
static int gl_init(void) {
    const char* e = getenv("ME1_GL");
    if (e && !strcmp(e, "0")) { glog("ME1_GL=0: null backend\n"); return 0; }
    gpu_driver_setup();
    if (!gl_load()) { if (!g_zink) return 0; gpu_driver_fallback("libEGL_mesa / libGLESv2_mesa not loaded"); if (!gl_load()) return 0; }
    g_gldbg = (e = getenv("ME1_GL_DEBUG")) ? atoi(e) : 0;
    if ((e = getenv("ME1_TEX_MAXSIZE")) && atoi(e) >= 16) g_texmax = atoi(e);
    g_tex16 = (e = getenv("ME1_TEX_16")) ? atoi(e) : 1;   /* CPU-decoded DXT: 1 = DXT1 -> RGB5A1, 2 = also DXT3/5 -> RGBA4 */
    if ((e = getenv("ME1_FRAME_DUMP")) && *e) { g_fdump = e; mkdir(e, 0755); }
    if ((e = getenv("ME1_FRAME_DUMP_EVERY")) && atoi(e) > 0) g_dumpevery = atoi(e);
    if ((e = getenv("ME1_GLSL_DUMP")) && *e) { g_glsldump = e; mkdir(e, 0755); }
    if ((e = getenv("ME1_GL_CACHE")) && *e && strcmp(e, "off")) { snprintf(g_bincache, sizeof g_bincache, "%s", e); mkdir(g_bincache, 0755); }
    GlCtx* C = egl_open();
    if (!C && g_zink) { gpu_driver_fallback("no EGL display/context"); if (gl_load()) C = egl_open(); }
    if (!C) return 0;
    g_ctx0 = C->ctx; t_ctx = C; g_lastctx = C;
    const char* ver = (const char*)glGetString(GL_VERSION);
    GLint ne = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &ne);
    size_t cap = 1 << 16, L = 0; char* ex = malloc(cap); ex[0] = 0;
    for (GLint i = 0; i < ne; i++) {
        const char* s = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i); if (!s) continue;
        size_t k = strlen(s); if (L + k + 2 >= cap) { cap *= 2; ex = realloc(ex, cap); }
        memcpy(ex + L, s, k); L += k; ex[L++] = ' '; ex[L] = 0;
    }
    GX.es32 = ver && strstr(ver, "OpenGL ES 3.2") != NULL;
    GX.s3tc = has_ext(ex, "GL_EXT_texture_compression_s3tc") && !getenv("ME1_GL_NOS3TC");
    GX.rgtc = has_ext(ex, "GL_EXT_texture_compression_rgtc") && !getenv("ME1_GL_NORGTC");
    GX.aniso = has_ext(ex, "GL_EXT_texture_filter_anisotropic");
    GX.border = GX.es32 || has_ext(ex, "GL_EXT_texture_border_clamp") || has_ext(ex, "GL_OES_texture_border_clamp");
    GX.flin = has_ext(ex, "GL_OES_texture_float_linear");
    GX.maski = glColorMaski != NULL && (GX.es32 || has_ext(ex, "GL_EXT_draw_buffers_indexed") || has_ext(ex, "GL_OES_draw_buffers_indexed"));
    GX.mirror = has_ext(ex, "GL_EXT_texture_mirror_clamp_to_edge");
    if (GX.aniso) glGetFloatv(XMAXANISO, &GX.maxaniso);
    gt_init(ex);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &GX.maxtex);
    GLint vv = 256, pv = 224; glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &vv); glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &pv);
    sm3_vs_uniform_vectors = vv; sm3_ps_uniform_vectors = pv;
    { const char* fe = getenv("ME1_FLOAT_EMU"); if (fe && *fe >= '0' && *fe <= '2') sm3_float_emu = *fe - '0'; }
    { const char* iv = getenv("ME1_INVARIANT"); if (iv && *iv >= '0' && *iv <= '1') sm3_invariant = *iv - '0'; }
    { const char* sl = getenv("ME1_SHADER_LITE"); sm3_lite = sl && *sl == '1'; }
    glog("D3D9 float emulation %d (ME1_FLOAT_EMU: 0 IEEE, 1 clamp rcp/rsq/log/pow, 2 + 0*x=0 in mul/mad/dp), invariant position %d (ME1_INVARIANT), "
         "simplified calculation %d (ME1_SHADER_LITE: engine lighting/effects switches, FP16 for _pp values, cheap sRGB, anisotropy <= 2x, mm timer 5 ms, audio 32 kHz)\n", sm3_float_emu, sm3_invariant, sm3_lite);
    if (g_bincache[0] && glGetProgramBinary && glProgramBinary) glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &GX.nbin);
    glog("EGL %d.%d%s; %s | %s | %s\n", g_eglmaj, g_eglmin, g_surfless ? " surfaceless" : "", glGetString(GL_VENDOR), glGetString(GL_RENDERER), ver);
    glog("s3tc %d rgtc %d aniso %.0f border %d float_linear %d mirror_clamp %d; max texture %d, uniform vectors vs %d ps %d; "
         "program binaries %s; %d extensions\n", GX.s3tc, GX.rgtc, GX.maxaniso, GX.border, GX.flin, GX.mirror, GX.maxtex, vv, pv,
         GX.nbin > 0 ? g_bincache : "off", ne);
    if (g_zink || g_drvinfo[0]) glog("GPU driver: %s%s\n", g_zink ? "Mesa zink on " : "", g_drvinfo[0] ? g_drvinfo : "?");
    if (g_cpupresent) glog("frames reach the window by CPU copy (ANativeWindow_lock)\n");
    if (g_gldbg > 1) glog("extensions: %s\n", ex);
    free(ex);
    /* dummy textures for unbound samplers */
    glGenTextures(3, g_dummy);
    static const uint8_t blk[4 * 6] = { 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 };
    glActiveTexture(GL_TEXTURE0 + 20);
    glBindTexture(GL_TEXTURE_2D, g_dummy[0]); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blk);
    glBindTexture(GL_TEXTURE_CUBE_MAP, g_dummy[1]); glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGBA8, 1, 1);
    for (int f = 0; f < 6; f++) glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blk);
    glBindTexture(GL_TEXTURE_3D, g_dummy[2]); glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, 1, 1, 1);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blk);
    glcheck("init");
    presenter_start();
    return 1;
}

/* ---- per-thread contexts ---- */
static void fbo_purge(GlCtx* C, uint32_t serial) {
    for (int i = 0; i < NFBO; i++) {
        FboEnt* f = &C->fb[i]; if (!f->fbo) continue;
        int hit = 0; for (int k = 0; k < 5; k++) if (f->k[k] && (uint32_t)(f->k[k] >> 16) == serial) hit = 1;
        if (hit) { if (C->st[S_FBO] == f->fbo) { glBindFramebuffer(GL_FRAMEBUFFER, 0); C->st[S_FBO] = 0; } glDeleteFramebuffers(1, &f->fbo); memset(f, 0, sizeof *f); }
    }
}
static GlCtx* gl_cur(void) {
    if (g_gl != 1 || g_vk) return NULL;
    GlCtx* C = t_ctx;
    if (!C) {
        if (!(C = ctx_new(g_ctx0))) return NULL;
        t_ctx = C;
        if (g_log) glog("GL context %d for thread %lx\n", C->id, (unsigned long)pthread_self());
    }
    if (C != g_lastctx) {   /* another context worked since: wait for its fence (GPU side) */
        for (int i = 0; i < g_nctx; i++) {
            GlCtx* X = g_ctxs[i];
            if (X != C && X->fence && X->fseq != C->seen[i]) { glWaitSync(X->fence, 0, GL_TIMEOUT_IGNORED); C->seen[i] = X->fseq; }
        }
        g_lastctx = C;
    }
    if (C->nstale) { for (int i = 0; i < C->nstale; i++) fbo_purge(C, C->stale[i]); C->nstale = 0; }
    C->used = 1;
    return C;
}
/* end of a D3D call: a context other than the presenting one publishes its work */
static void gl_release(void) {
    GlCtx* C = t_ctx;
    if (!C || !C->used) return;
    C->used = 0;
    if (C == g_primary) return;
    if (C->fence) glDeleteSync(C->fence);
    C->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); C->fseq++;
    glFlush();
}
static void stale_all(uint32_t serial) {   /* an attached image died: other contexts drop their FBOs at next use */
    for (int i = 0; i < g_nctx; i++) {
        GlCtx* X = g_ctxs[i]; if (X == t_ctx) continue;
        if (X->nstale == X->capstale) { X->capstale = X->capstale ? 2 * X->capstale : 64; X->stale = realloc(X->stale, X->capstale * sizeof *X->stale); }
        X->stale[X->nstale++] = serial;
    }
}

/* ---- cached GL state ---- */
static int chg(GlCtx* C, int i, uint32_t v) { if (C->st[i] == v) return 0; C->st[i] = v; return 1; }
static void s_enable(GlCtx* C, int slot, GLenum cap, int on) { if (chg(C, slot, on != 0)) { if (on) glEnable(cap); else glDisable(cap); } }
static void s_prog(GlCtx* C, GLuint p) { if (chg(C, S_PROG, p)) glUseProgram(p); }
static void s_fbo(GlCtx* C, GLuint f) { if (chg(C, S_FBO, f)) glBindFramebuffer(GL_FRAMEBUFFER, f); }
static void s_abuf(GlCtx* C, GLuint b) { if (chg(C, S_ABUF, b)) glBindBuffer(GL_ARRAY_BUFFER, b); }
static void s_ebuf(GlCtx* C, GLuint b) { if (chg(C, S_EBUF, b)) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b); }
static void s_cmask(GlCtx* C, uint32_t m) {   /* all draw buffers */
    if (C->st[S_CMASK] == m && C->st[S_CMASK1] == m && C->st[S_CMASK2] == m && C->st[S_CMASK3] == m) return;
    glColorMask(m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1);
    C->st[S_CMASK] = C->st[S_CMASK1] = C->st[S_CMASK2] = C->st[S_CMASK3] = m;
}
static void s_cmaski(GlCtx* C, int i, uint32_t m) { if (chg(C, S_CMASK + i, m)) glColorMaski((GLuint)i, m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1); }
static void s_dmask(GlCtx* C, int on) { if (chg(C, S_DMASK, on)) glDepthMask(on ? GL_TRUE : GL_FALSE); }
static void s_swmask(GlCtx* C, uint32_t m) { if (chg(C, S_SWMASK, m)) glStencilMaskSeparate(GL_FRONT_AND_BACK, m); }
static void s_scissor(GlCtx* C, int x, int y, int w, int h) {
    int a = chg(C, S_SC0, (uint32_t)x), b = chg(C, S_SC1, (uint32_t)y), c = chg(C, S_SC2, (uint32_t)w), d = chg(C, S_SC3, (uint32_t)h);
    if (a | b | c | d) glScissor(x, y, w, h);
}
static void s_viewport(GlCtx* C, int x, int y, int w, int h) {
    int a = chg(C, S_VP0, (uint32_t)x), b = chg(C, S_VP1, (uint32_t)y), c = chg(C, S_VP2, (uint32_t)w), d = chg(C, S_VP3, (uint32_t)h);
    if (a | b | c | d) glViewport(x, y, w, h);
}
static void s_tex(GlCtx* C, int unit, GLenum tg, GLuint t) {
    if (C->tex[unit] == t && C->ttg[unit] == tg) return;
    glActiveTexture(GL_TEXTURE0 + unit);
    if (C->tex[unit] != 0xFFFFFFFFu && C->ttg[unit] != tg && C->tex[unit]) glBindTexture(C->ttg[unit], 0);
    glBindTexture(tg, t); C->tex[unit] = t; C->ttg[unit] = tg;
}
/* unit 20: uploads and creation (never sampled) */
static void s_scratch_tex(GlCtx* C, GLenum tg, GLuint t) { s_tex(C, 20, tg, t); }
static void s_attr(GlCtx* C, int loc, int on) {
    uint32_t m = 1u << loc;
    if (!!(C->attr_on & m) == !!on) return;
    if (on) { glEnableVertexAttribArray((GLuint)loc); C->attr_on |= m; } else { glDisableVertexAttribArray((GLuint)loc); C->attr_on &= ~m; }
}
static void s_div(GlCtx* C, int loc, uint32_t d) { if (C->div[loc] != d) { glVertexAttribDivisor((GLuint)loc, d); C->div[loc] = d; } }

/* ---- formats ---- */
static const GFmt g_gfmt[] = {
    { 21, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, CV_RAW, 4, "bgra", "rgba" }, { 22, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, CV_RAW, 4, "bgr1", "rgb1" },
    { 23, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, CV_RAW, 3, "rgba", "rgba" },
    { 24, GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, CV_1555, 4, "rgb1", "rgb1" },
    { 25, GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, CV_1555, 4, "rgba", "rgba" },
    { 26, GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, CV_RAW, 4, "gbar", "rgba" },
    { 28, GL_R8, GL_RED, GL_UNSIGNED_BYTE, CV_RAW, 1, "000r", "000r" },
    { 31, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, CV_RAW, 4, "rgba", "rgba" },
    { 32, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, CV_RAW, 4, "rgba", "rgba" }, { 33, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, CV_RAW, 4, "rgb1", "rgb1" },
    { 34, GL_RG16F, GL_RG, GL_HALF_FLOAT, CV_U16, 2, "rg11", "rg11" },
    { 35, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, CV_RAW, 4, "bgra", "rgba" },
    { 36, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, CV_U16, 4, "rgba", "rgba" },
    { 50, GL_R8, GL_RED, GL_UNSIGNED_BYTE, CV_RAW, 1, "rrr1", "rrr1" }, { 51, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, CV_RAW, 2, "rrrg", "rrrg" },
    { 60, GL_RG8_SNORM, GL_RG, GL_BYTE, CV_RAW, 2, "rg11", "rg11" }, { 63, GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, CV_RAW, 4, "rgba", "rgba" },
    { 64, GL_RG16F, GL_RG, GL_HALF_FLOAT, CV_S16, 2, "rg11", "rg11" }, { 81, GL_R16F, GL_RED, GL_HALF_FLOAT, CV_U16, 1, "rrr1", "rrr1" },
    { 111, GL_R16F, GL_RED, GL_HALF_FLOAT, CV_RAW, 1, "r111", "r111" }, { 112, GL_RG16F, GL_RG, GL_HALF_FLOAT, CV_RAW, 2, "rg11", "rg11" },
    { 113, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, CV_RAW, 4, "rgba", "rgba" },
    { 114, GL_R32F, GL_RED, GL_FLOAT, CV_RAW, 1, "r111", "r111" }, { 115, GL_RG32F, GL_RG, GL_FLOAT, CV_RAW, 2, "rg11", "rg11" },
    { 116, GL_RGBA32F, GL_RGBA, GL_FLOAT, CV_RAW, 4, "rgba", "rgba" },
    { FCC('D','X','T','1'), XS3TC_DXT1, 0, 0, CV_DXT1, 4, "rgba", "rgba" }, { FCC('D','X','T','2'), XS3TC_DXT3, 0, 0, CV_DXT3, 4, "rgba", "rgba" },
    { FCC('D','X','T','3'), XS3TC_DXT3, 0, 0, CV_DXT3, 4, "rgba", "rgba" }, { FCC('D','X','T','4'), XS3TC_DXT5, 0, 0, CV_DXT5, 4, "rgba", "rgba" },
    { FCC('D','X','T','5'), XS3TC_DXT5, 0, 0, CV_DXT5, 4, "rgba", "rgba" },
    { FCC('A','T','I','1'), XRGTC1, 0, 0, CV_BC4, 1, "rrrr", "rrrr" }, { FCC('A','T','I','2'), XRGTC2, 0, 0, CV_BC5, 2, "gr11", "gr11" },
};
static const GFmt* gfmt(uint32_t f) {
    for (unsigned i = 0; i < sizeof g_gfmt / sizeof g_gfmt[0]; i++) if (g_gfmt[i].d3d == f) return &g_gfmt[i];
    return NULL;
}
static int is_float_fmt(GLenum ifmt) { return ifmt == GL_R32F || ifmt == GL_RG32F || ifmt == GL_RGBA32F; }
static int is_half_fmt(GLenum ifmt) { return ifmt == GL_R16F || ifmt == GL_RG16F || ifmt == GL_RGBA16F; }
static GLint swz(char c) {
    switch (c) { case 'r': return GL_RED; case 'g': return GL_GREEN; case 'b': return GL_BLUE; case 'a': return GL_ALPHA; case '0': return GL_ZERO; default: return GL_ONE; }
}
static uint16_t f2h(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000u, e = (x >> 23) & 0xFF, m = x & 0x7FFFFFu;
    if (e == 0xFF) return (uint16_t)(s | 0x7C00u | (m ? 0x200u : 0));
    int ee = (int)e - 127 + 15;
    if (ee >= 31) return (uint16_t)(s | 0x7C00u);
    if (ee <= 0) { if (ee < -10) return (uint16_t)s; m |= 0x800000u; uint32_t t = 14 - ee; uint32_t hm = m >> t; if ((m >> (t - 1)) & 1) hm++; return (uint16_t)(s | hm); }
    uint32_t hm = m >> 13; if (m & 0x1000u) { hm++; if (hm == 0x400u) { hm = 0; ee++; if (ee >= 31) return (uint16_t)(s | 0x7C00u); } }
    return (uint16_t)(s | (uint32_t)ee << 10 | hm);
}
static float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 0x3FFu, x;
    if (!e) { if (!m) x = s; else { e = 127 - 15 + 1; while (!(m & 0x400u)) { m <<= 1; e--; } m &= 0x3FFu; x = s | e << 23 | m << 13; } }
    else if (e == 31) x = s | 0x7F800000u | m << 13;
    else x = s | (e + 127 - 15) << 23 | m << 13;
    float f; memcpy(&f, &x, 4); return f;
}
/* S3TC / RGTC block decoders (one 4x4 block -> RGBA8 / channel bytes) */
static void c565(uint32_t v, uint8_t* o) { uint32_t r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31; o[0] = (uint8_t)(r << 3 | r >> 2); o[1] = (uint8_t)(g << 2 | g >> 4); o[2] = (uint8_t)(b << 3 | b >> 2); o[3] = 255; }
static void dxt_color(const uint8_t* b, uint8_t o[16][4], int dxt1) {
    uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8; uint8_t p[4][4];
    c565(c0, p[0]); c565(c1, p[1]);
    if (c0 > c1 || !dxt1) for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((2 * p[0][k] + p[1][k]) / 3); p[3][k] = (uint8_t)((p[0][k] + 2 * p[1][k]) / 3); }
    else for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((p[0][k] + p[1][k]) / 2); p[3][k] = 0; }
    p[2][3] = 255; p[3][3] = (c0 > c1 || !dxt1) ? 255 : 0;
    uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(o[i], p[bits >> (2 * i) & 3], 4);
}
static void bc4_block(const uint8_t* b, uint8_t* o /* 16 */) {
    uint32_t a0 = b[0], a1 = b[1]; uint8_t p[8]; p[0] = (uint8_t)a0; p[1] = (uint8_t)a1;
    if (a0 > a1) for (int i = 2; i < 8; i++) p[i] = (uint8_t)(((8 - i) * a0 + (i - 1) * a1) / 7);
    else { for (int i = 2; i < 6; i++) p[i] = (uint8_t)(((6 - i) * a0 + (i - 1) * a1) / 5); p[6] = 0; p[7] = 255; }
    uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) o[i] = p[bits >> (3 * i) & 7];
}
static void decode_block(int cv, const uint8_t* b, uint8_t o[16][4]) {
    uint8_t a[16];
    switch (cv) {
    case CV_DXT1: dxt_color(b, o, 1); break;
    case CV_DXT3: dxt_color(b + 8, o, 0); for (int i = 0; i < 16; i++) { int v = b[i / 2] >> (4 * (i & 1)) & 15; o[i][3] = (uint8_t)(v * 17); } break;
    case CV_DXT5: dxt_color(b + 8, o, 0); bc4_block(b, a); for (int i = 0; i < 16; i++) o[i][3] = a[i]; break;
    case CV_BC4: bc4_block(b, a); for (int i = 0; i < 16; i++) o[i][0] = a[i]; break;
    case CV_BC5: bc4_block(b, a); for (int i = 0; i < 16; i++) o[i][0] = a[i]; bc4_block(b + 8, a); for (int i = 0; i < 16; i++) o[i][1] = a[i]; break;
    }
}

/* ---- resources ---- */
static __thread uint8_t* t_tmp; static __thread size_t t_tmpn;
static uint8_t* tmpbuf(size_t n) { if (n > t_tmpn) { free(t_tmp); t_tmpn = n + (n >> 2) + 4096; t_tmp = malloc(t_tmpn); } return t_tmp; }
static GLenum tex_target(const Obj* t) { return t->type == T_CUBE ? GL_TEXTURE_CUBE_MAP : t->type == T_VOLTEX ? GL_TEXTURE_3D : GL_TEXTURE_2D; }
static int is_block(int cv) { return cv >= CV_DXT1 && cv <= CV_BC5; }
static uint64_t gl_level_bytes(const GlRes* g, uint32_t w, uint32_t h, uint32_t d) {
    if (is_block(g->f->conv) && !g->dec) return (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * (g->f->conv == CV_DXT1 || g->f->conv == CV_BC4 ? 8 : 16) * d;
    return (uint64_t)w * h * d * (uint32_t)g->obpp;
}
/* GL object of a root resource (texture, cube, volume texture or stand-alone surface); NULL: none (system memory, unsupported) */
static GlRes* tex_res(GlCtx* C, Obj* t) {
    if (t->gx) { GlRes* g = t->gx; return g->kind ? g : NULL; }
    if ((t->type < T_TEX || t->type > T_SURF) || t->parent) return NULL;
    if (t->pool >= 2) return NULL;   /* SYSTEMMEM / SCRATCH: CPU only (UpdateSurface/UpdateTexture/GetRenderTargetData) */
    const Fmt* f = fmt_info(t->fmt); const GFmt* gf = gfmt(t->fmt);
    GlRes* g = calloc(1, sizeof *g); t->gx = g;
    g->serial = ++g_serial; g->w = (int)t->w; g->h = (int)t->h; g->d = t->d ? (int)t->d : 1; g->levels = (int)t->levels;
    if (f && (f->fl & F_DS)) {
        g->kind = 2; glGenRenderbuffers(1, &g->name); glBindRenderbuffer(GL_RENDERBUFFER, g->name);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)t->w, (GLsizei)t->h);
        g->bytes = (uint64_t)t->w * t->h * 4; texmem_add((int64_t)g->bytes);
        glcheck("renderbuffer");
        return g;
    }
    if (!f || !gf) { static int n; if (n++ < 20) glog("%s: format %s has no GL mapping\n", g_cur, fmt_name(t->fmt)); return NULL; }
    g->kind = 1; g->f = gf; g->target = tex_target(t); g->rt = (t->usage & 1) != 0;
    g->ifmt = gf->ifmt; g->fmt = gf->fmt; g->type = gf->type; g->obpp = f->bytes;
    if (is_block(gf->conv)) {
        int ext = (gf->conv <= CV_DXT5 ? GX.s3tc : GX.rgtc) && g->target != GL_TEXTURE_3D;
        if (!ext) {
            g->dec = 1;
            if (gf->conv == CV_BC4) { g->ifmt = GL_R8; g->fmt = GL_RED; g->type = GL_UNSIGNED_BYTE; g->obpp = 1; }
            else if (gf->conv == CV_BC5) { g->ifmt = GL_RG8; g->fmt = GL_RG; g->type = GL_UNSIGNED_BYTE; g->obpp = 2; }
            else if (g_tex16 && gf->conv == CV_DXT1) { g->ifmt = GL_RGB5_A1; g->fmt = GL_RGBA; g->type = GL_UNSIGNED_SHORT_5_5_5_1; g->obpp = 2; }
            else if (g_tex16 >= 2) { g->ifmt = GL_RGBA4; g->fmt = GL_RGBA; g->type = GL_UNSIGNED_SHORT_4_4_4_4; g->obpp = 2; }
            else { g->ifmt = GL_RGBA8; g->fmt = GL_RGBA; g->type = GL_UNSIGNED_BYTE; g->obpp = 4; }
        }
    } else if (gf->conv == CV_U16 || gf->conv == CV_S16) g->obpp = 2 * gf->nch;
    else if (gf->conv == CV_1555) g->obpp = 2;
    if (!g->rt && g->target != GL_TEXTURE_3D) {
        int lim = g_texmax && g_texmax < GX.maxtex ? g_texmax : GX.maxtex;
        while (g->drop < g->levels - 1 && ((int)dimv(t->w, (uint32_t)g->drop) > lim || (int)dimv(t->h, (uint32_t)g->drop) > lim)) g->drop++;
    }
    int lv = g->levels - g->drop; uint32_t w = dimv(t->w, (uint32_t)g->drop), h = dimv(t->h, (uint32_t)g->drop), dd = dimv((uint32_t)g->d, (uint32_t)g->drop);
    glGenTextures(1, &g->name);
    s_scratch_tex(C, g->target, g->name);
    if (g->target == GL_TEXTURE_3D) glTexStorage3D(GL_TEXTURE_3D, lv, g->ifmt, (GLsizei)w, (GLsizei)h, (GLsizei)dd);
    else glTexStorage2D(g->target, lv, g->ifmt, (GLsizei)w, (GLsizei)h);
    const char* sw = g->rt ? gf->swrt : gf->sw;
    if (strcmp(sw, "rgba")) for (int i = 0; i < 4; i++) glTexParameteri(g->target, GL_TEXTURE_SWIZZLE_R + i, swz(sw[i]));
    glTexParameteri(g->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(g->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    for (int l = 0; l < lv; l++) g->bytes += gl_level_bytes(g, dimv(w, (uint32_t)l), dimv(h, (uint32_t)l), dimv(dd, (uint32_t)l));
    if (g->target == GL_TEXTURE_CUBE_MAP) g->bytes *= 6;
    texmem_add((int64_t)g->bytes);
    if (g_gldbg) { GLenum e = glGetError(); if (e) glog("texture %ux%u x%d %s (GL 0x%04x, %s): error 0x%04x\n", t->w, t->h, lv, fmt_name(t->fmt), g->ifmt, g->dec ? "decoded" : "native", e); }
    return g;
}
/* upload the rect (x, y, z, w, h, dz) of level lvl / face from guest data src (first row of the rect; row/slice pitch in
 * the D3D layout - rows of 4x4 blocks for compressed formats) */
static void tex_upload(GlCtx* C, Obj* t, GlRes* g, uint32_t face, uint32_t lvl, uint32_t x, uint32_t y, uint32_t z,
                       uint32_t w, uint32_t h, uint32_t dz, const uint8_t* src, uint32_t pitch, uint32_t slice) {
    int gl = (int)lvl - g->drop;
    if (gl < 0 || g->kind != 1 || !w || !h || !dz) return;
    uint32_t lw = dimv(t->w, lvl), lh = dimv(t->h, lvl);
    if (x >= lw || y >= lh) return;
    if (x + w > lw) w = lw - x;
    if (y + h > lh) h = lh - y;
    GLenum tg = g->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : g->target;
    int is3d = g->target == GL_TEXTURE_3D;
    s_scratch_tex(C, g->target, g->name);
    const GFmt* f = g->f;
    GS.texup++; GS.texbytes += (uint64_t)pitch * ((h + 3) / 4) * dz;
    if (is_block(f->conv)) {
        uint32_t bsz = f->conv == CV_DXT1 || f->conv == CV_BC4 ? 8 : 16, bx = (w + 3) / 4, by = (h + 3) / 4;
        if (!g->dec) {
            const uint8_t* p = src;
            if (pitch != bx * bsz) { uint8_t* q = tmpbuf((size_t)bx * by * bsz); for (uint32_t r = 0; r < by; r++) memcpy(q + (size_t)r * bx * bsz, src + (size_t)r * pitch, (size_t)bx * bsz); p = q; }
            glCompressedTexSubImage2D(tg, gl, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, g->ifmt, (GLsizei)(bx * by * bsz), p);
        } else {
            uint32_t ow = bx * 4, oh = by * 4; uint8_t* q = tmpbuf((size_t)ow * oh * (uint32_t)g->obpp);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)ow);
            for (uint32_t k = 0; k < dz; k++) {
                const uint8_t* s0 = src + (size_t)k * slice;
                for (uint32_t r = 0; r < by; r++) for (uint32_t c = 0; c < bx; c++) {
                    uint8_t px[16][4]; memset(px, 0, sizeof px); decode_block(f->conv, s0 + (size_t)r * pitch + (size_t)c * bsz, px);
                    for (int i = 0; i < 16; i++) {
                        size_t o = ((size_t)(r * 4 + (uint32_t)i / 4) * ow + c * 4 + (uint32_t)i % 4) * (uint32_t)g->obpp; const uint8_t* v = px[i];
                        if (g->obpp == 4) memcpy(q + o, v, 4);
                        else if (g->obpp == 1) q[o] = v[0];
                        else if (g->ifmt == GL_RG8) { q[o] = v[0]; q[o + 1] = v[1]; }
                        else { uint16_t u = g->ifmt == GL_RGB5_A1 ? (uint16_t)((v[0] >> 3) << 11 | (v[1] >> 3) << 6 | (v[2] >> 3) << 1 | (v[3] >> 7))
                                                                   : (uint16_t)((v[0] >> 4) << 12 | (v[1] >> 4) << 8 | (v[2] >> 4) << 4 | (v[3] >> 4));
                               memcpy(q + o, &u, 2); }
                    }
                }
                if (is3d) glTexSubImage3D(GL_TEXTURE_3D, gl, (GLint)x, (GLint)y, (GLint)(z + k), (GLsizei)w, (GLsizei)h, 1, g->fmt, g->type, q);
                else glTexSubImage2D(tg, gl, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, g->fmt, g->type, q);
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            GS.decoded++;
        }
    } else if (f->conv == CV_RAW) {
        uint32_t bpp = fmt_info(t->fmt)->bytes;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / bpp));
        if (is3d) {
            glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, (GLint)(slice / pitch));
            glTexSubImage3D(GL_TEXTURE_3D, gl, (GLint)x, (GLint)y, (GLint)z, (GLsizei)w, (GLsizei)h, (GLsizei)dz, g->fmt, g->type, src);
            glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
        } else glTexSubImage2D(tg, gl, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, g->fmt, g->type, src);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else {   /* per-pixel conversion into a tight buffer */
        uint32_t ob = (uint32_t)g->obpp; uint8_t* q = tmpbuf((size_t)w * h * dz * ob); int n = f->nch;
        for (uint32_t k = 0; k < dz; k++) for (uint32_t r = 0; r < h; r++) {
            const uint8_t* s = src + (size_t)k * slice + (size_t)r * pitch; uint8_t* o = q + ((size_t)k * h + r) * w * ob;
            for (uint32_t c = 0; c < w; c++) {
                if (f->conv == CV_1555) { uint16_t v; memcpy(&v, s + 2 * c, 2); v = (uint16_t)((v & 0x7FFFu) << 1 | v >> 15); memcpy(o + 2 * c, &v, 2); }
                else for (int i = 0; i < n; i++) {
                    uint16_t v; memcpy(&v, s + (size_t)(c * n + i) * 2, 2);
                    float fv = f->conv == CV_S16 ? fmaxf((float)(int16_t)v / 32767.f, -1.f) : (float)v / 65535.f;
                    uint16_t hv = f2h(fv); memcpy(o + (size_t)(c * n + i) * 2, &hv, 2);
                }
            }
        }
        if (is3d) glTexSubImage3D(GL_TEXTURE_3D, gl, (GLint)x, (GLint)y, (GLint)z, (GLsizei)w, (GLsizei)h, (GLsizei)dz, g->fmt, g->type, q);
        else glTexSubImage2D(tg, gl, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, g->fmt, g->type, q);
    }
    glcheck("texture upload");
}
/* LockRect/LockBox bookkeeping: lockr[8 * sub] = x0, y0, x1, y1, z0, z1, flags */
static void gl_note_lock(Obj* r, uint32_t s, uint32_t lvl, uint32_t area, int vol, uint32_t flags) {
    if (!r->lockr) r->lockr = calloc(sub_count(r) * 8, 4);
    uint32_t* L = r->lockr + 8 * s, w = dimv(r->w, lvl), h = dimv(r->h, lvl), d = vol ? dimv(r->d, lvl) : 1;
    L[0] = 0; L[1] = 0; L[2] = w; L[3] = h; L[4] = 0; L[5] = d; L[6] = flags;
    if (area) {
        uint32_t l = rd32(area), t = rd32(area + 4), rr = rd32(area + 8), b = rd32(area + 12);
        if (l < rr && t < b) { L[0] = l < w ? l : w; L[1] = t < h ? t : h; L[2] = rr < w ? rr : w; L[3] = b < h ? b : h; }
        if (vol) { uint32_t f0 = rd32(area + 16), f1 = rd32(area + 20); if (f0 < f1) { L[4] = f0 < d ? f0 : d; L[5] = f1 < d ? f1 : d; } }
    }
}
static void gl_tex_unlock(Obj* r, uint32_t s, uint32_t lvl) {
    GlCtx* C = gl_cur(); if (!C || !r->lockr || !r->lockv || !r->lockv[s]) return;
    uint32_t* L = r->lockr + 8 * s;
    if (L[6] & 0x10u) return;   /* READONLY */
    GlRes* g = tex_res(C, r); if (!g || g->kind != 1) return;
    uint32_t pitch, rows, unit; int blk; layout(r->fmt, dimv(r->w, lvl), dimv(r->h, lvl), &pitch, &rows, &unit, &blk);
    uint32_t k = blk ? 4 : 1, face = r->type == T_CUBE ? s / r->levels : 0;
    const uint8_t* p = M + r->lockv[s] + (size_t)(L[1] / k) * pitch + (size_t)(L[0] / k) * unit + (size_t)L[4] * pitch * rows;
    tex_upload(C, r, g, face, lvl, L[0], L[1], L[4], L[2] - L[0], L[3] - L[1], L[5] - L[4], p, pitch, pitch * rows);
}
/* static vertex/index buffers */
static GlRes* buf_res(GlCtx* C, Obj* b) {
    (void)C;
    if (b->gx) return b->gx;
    if (b->shadow) return NULL;
    GlRes* g = calloc(1, sizeof *g); b->gx = g; g->serial = ++g_serial; g->kind = 3;
    glGenBuffers(1, &g->name); glBindBuffer(GL_COPY_WRITE_BUFFER, g->name);
    glBufferData(GL_COPY_WRITE_BUFFER, b->size, NULL, GL_STATIC_DRAW);
    g->bytes = b->size; GS.texmem += g->bytes;
    return g;
}
static void gl_buf_unlock(Obj* b, uint32_t off, uint32_t end, uint32_t flags) {
    GlCtx* C = gl_cur(); if (!C || (flags & 0x10u) || !b->lockv || !b->lockv[0] || end <= off) return;
    GlRes* g = buf_res(C, b); if (!g) return;
    if (end > b->size) end = b->size;
    glBindBuffer(GL_COPY_WRITE_BUFFER, g->name);
    glBufferSubData(GL_COPY_WRITE_BUFFER, off, end - off, M + b->lockv[0] + off);
    GS.bufup++; GS.bufbytes += end - off;
    glcheck("buffer upload");
}

/* ---- render targets ---- */
typedef struct { GlRes* g; Obj* root; uint32_t face, level; } Img;   /* one level/face of a GL texture, or a renderbuffer */
static int img_of(GlCtx* C, Obj* s, Img* im) {
    memset(im, 0, sizeof *im);
    if (!s) return 0;
    Obj* r = s->parent ? s->parent : s;
    GlRes* g = tex_res(C, r); if (!g) return 0;
    im->g = g; im->root = r; im->face = s->parent ? s->face : 0; im->level = s->parent ? s->level : 0;
    if (g->kind == 1 && (int)im->level < g->drop) { im->g = NULL; return 0; }
    return 1;
}
static uint64_t img_key(const Img* im) { return im->g ? ((uint64_t)im->g->serial << 16 | im->face << 8 | im->level) : 0; }
static void img_size(const Img* im, uint32_t* w, uint32_t* h) { *w = dimv(im->root->w, im->level); *h = dimv(im->root->h, im->level); }
static void attach(GLenum fbt, GLenum ap, const Img* im) {
    if (!im || !im->g) { glFramebufferTexture2D(fbt, ap, GL_TEXTURE_2D, 0, 0); return; }
    GlRes* g = im->g;
    if (g->kind == 2) glFramebufferRenderbuffer(fbt, ap, GL_RENDERBUFFER, g->name);
    else if (g->target == GL_TEXTURE_CUBE_MAP) glFramebufferTexture2D(fbt, ap, GL_TEXTURE_CUBE_MAP_POSITIVE_X + im->face, g->name, (GLint)im->level - g->drop);
    else if (g->target == GL_TEXTURE_3D) glFramebufferTextureLayer(fbt, ap, g->name, (GLint)im->level - g->drop, 0);
    else glFramebufferTexture2D(fbt, ap, GL_TEXTURE_2D, g->name, (GLint)im->level - g->drop);
}
static void detach(GLenum fbt, GLenum ap) { glFramebufferRenderbuffer(fbt, ap, GL_RENDERBUFFER, 0); }
/* cached FBO for (color 0-3, depth) */
static GLuint fbo_get(GlCtx* C, const Img* col, const Img* ds) {
    uint64_t k[5]; for (int i = 0; i < 4; i++) k[i] = img_key(&col[i]); k[4] = img_key(ds);
    C->tick++;
    /* v0.10.2: consecutive draws usually share the render targets - check the last FBO returned first */
    FboEnt* lb = &C->fb[C->fblast];
    if (lb->fbo && !memcmp(lb->k, k, sizeof k)) { lb->tick = C->tick; return lb->fbo; }
    int v = 0;
    for (int i = 0; i < NFBO; i++) {
        FboEnt* f = &C->fb[i];
        if (f->fbo && !memcmp(f->k, k, sizeof k)) { f->tick = C->tick; C->fblast = i; return f->fbo; }
        if (!f->fbo) { if (C->fb[v].fbo) v = i; } else if (C->fb[v].fbo && f->tick < C->fb[v].tick) v = i;
    }
    FboEnt* f = &C->fb[v];
    if (f->fbo) { if (C->st[S_FBO] == f->fbo) C->st[S_FBO] = 0; glDeleteFramebuffers(1, &f->fbo); }
    memcpy(f->k, k, sizeof k); f->tick = C->tick; C->fblast = v;
    glGenFramebuffers(1, &f->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo); C->st[S_FBO] = f->fbo;
    GLenum db[4]; int n = 0;
    for (int i = 0; i < 4; i++) if (col[i].g) { attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, &col[i]); db[i] = GL_COLOR_ATTACHMENT0 + i; n = i + 1; } else db[i] = GL_NONE;
    if (ds->g) attach(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, ds);
    glDrawBuffers(n ? n : 1, db);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    GS.fbos++;
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        static int nl; if (nl++ < 20) {
            uint32_t w = 0, h = 0, dw = 0, dh = 0; if (col[0].g) img_size(&col[0], &w, &h); if (ds->g) img_size(ds, &dw, &dh);
            glog("%s: framebuffer incomplete 0x%04x (color %ux%u %s GL 0x%04x, depth %ux%u)\n", g_cur, st, w, h,
                 col[0].g ? fmt_name(col[0].root->fmt) : "-", col[0].g ? col[0].g->ifmt : 0, dw, dh);
        }
    }
    glcheck("framebuffer");
    return f->fbo;
}
/* bind the device's render targets; returns 0 (nothing to draw into) or 1 | 2 (depth/stencil attached) | 4 (stencil bits) */
static int bind_targets(GlCtx* C, Dev* d, uint32_t* W, uint32_t* H) {
    Img col[4], ds;
    if (!img_of(C, d->rt[0], &col[0]) || col[0].g->kind != 1) return 0;
    for (int i = 1; i < 4; i++) if (!img_of(C, d->rt[i], &col[i]) || col[i].g->kind != 1) memset(&col[i], 0, sizeof col[i]);
    img_size(&col[0], W, H);
    int r = 1;
    if (img_of(C, d->ds, &ds) && ds.g->kind == 2) {
        uint32_t dw, dh; img_size(&ds, &dw, &dh);
        if (dw >= *W && dh >= *H) { r |= 2; uint32_t f = ds.root->fmt; if (f == 75 || f == 83 || f == 73) r |= 4; }
        else memset(&ds, 0, sizeof ds);
    } else memset(&ds, 0, sizeof ds);
    s_fbo(C, fbo_get(C, col, &ds));
    return r;
}

/* ---- shaders and programs ---- */
/* CreateVertex/PixelShader only registers the shader: UE3 creates thousands at startup (9392 pixel shaders in the first
 * two minutes on the phone) and draws with few of them, so translation waits for the first program that uses it */
static void gl_shader_new(Obj* o) {
    if (g_gl != 1 || o->gx) return;
    GlSh* s = calloc(1, sizeof *s); o->gx = s; s->serial = ++g_serial;
}
static GlSh* gl_shader_ready(Obj* o) {
    GlSh* s = o->gx; if (!s || s->done) return s;
    s->done = 1; GS.shtrans++;
    if (sm3_translate((const uint32_t*)o->blob, o->bloblen / 4, &s->s) == 0) {
        s->ok = 1;
        if (sm3_lite && s->s.ps) {   /* how much the "simplified shaders" switch actually gets to use FP16 */
            static uint64_t nps, npp, ntmp, nfp16; nps++; npp += s->s.npp > 0; ntmp += (uint64_t)s->s.ntemp; nfp16 += (uint64_t)s->s.npp;
            if (nps == 20 || nps == 200 || nps == 1000 || nps % 5000 == 0)
                glog("simplified calculation: %llu pixel shaders, %llu with FP16 temps; %llu of %llu temps FP16 (_pp)\n", (unsigned long long)nps,
                     (unsigned long long)npp, (unsigned long long)nfp16, (unsigned long long)ntmp);
        }
    }
    else { GS.shfail++; static int n; if (n++ < 40) glog("%s_%d_%d %08x not translated: %s\n", o->type == T_VS ? "vs" : "ps", s->s.major, s->s.minor, o->crc, s->s.err); }
    return s;
}
static void dump_text(const char* name, const char* a, const char* b, const char* c) {
    if (!g_glsldump) return;
    char path[1200]; snprintf(path, sizeof path, "%s/%s", g_glsldump, name);
    FILE* f = fopen(path, "w"); if (!f) return;
    if (a) fputs(a, f);
    if (b) { fputs("\n// ---- fragment shader ----\n", f); fputs(b, f); }
    if (c) { fputs("\n// ---- log ----\n", f); fputs(c, f); }
    fclose(f);
}
static GLuint compile_sh(GLenum type, const char* src, char* log, int logn) {
    GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { log[0] = 0; glGetShaderInfoLog(s, logn, NULL, log); glDeleteShader(s); return 0; }
    return s;
}
static uint64_t prog_key(const Obj* vs, const Obj* ps) {
    /* v0.10.5: fixed cache version instead of __DATE__/__TIME__ - the build timestamp invalidated every cached
     * shader on each new APK. BUMP SHADER_CACHE_VER by hand whenever sm3_glsl.c / vk_glsl() output changes. */
    static const char stamp[] = "ME1SHDR7-2026-11-04";
    uint64_t h = fnv(0xcbf29ce484222325ull, stamp, sizeof stamp);
    h = fnv(h, &sm3_float_emu, sizeof sm3_float_emu);
    h = fnv(h, &sm3_invariant, sizeof sm3_invariant);
    if (sm3_lite) h = fnv(h, "lite", 4);   /* unchanged keys (and cached binaries) for the default mode */
    h = fnv(h, &vs->crc, 4); h = fnv(h, &vs->bloblen, 4); h = fnv(h, &ps->crc, 4); h = fnv(h, &ps->bloblen, 4);
    if (vs->blob) h = fnv(h, vs->blob, vs->bloblen);
    if (ps->blob) h = fnv(h, ps->blob, ps->bloblen);
    return h;
}
static int bin_load(GLuint p, uint64_t key) {
    if (!GX.nbin) return 0;
    char path[1000]; snprintf(path, sizeof path, "%s/%016llx.bin", g_bincache, (unsigned long long)key);
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    uint32_t h[4]; uint8_t* b = NULL; int ok = 0;
    if (fread(h, 4, 4, f) == 4 && h[0] == 0x5031454Du && h[2] && h[2] < (32u << 20) && (b = malloc(h[2])) && fread(b, 1, h[2], f) == h[2]
        && rt_crc32(b, h[2]) == h[3]) {
        glProgramBinary(p, h[1], b, (GLsizei)h[2]);
        GLint st = 0; glGetProgramiv(p, GL_LINK_STATUS, &st); ok = st != 0;
        while (glGetError() != GL_NO_ERROR) {}
    }
    free(b); fclose(f);
    if (!ok) unlink(path);
    return ok;
}
static void bin_store(GLuint p, uint64_t key) {
    if (!GX.nbin) return;
    GLint n = 0; glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &n); if (n <= 0) return;
    uint8_t* b = malloc((size_t)n); GLenum fmt = 0; GLsizei got = 0;
    glGetProgramBinary(p, n, &got, &fmt, b);
    if (got > 0) {
        char path[1000], tmp[1010]; snprintf(path, sizeof path, "%s/%016llx.bin", g_bincache, (unsigned long long)key); snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE* f = fopen(tmp, "wb");
        if (f) {
            uint32_t h[4] = { 0x5031454Du, fmt, (uint32_t)got, rt_crc32(b, (size_t)got) };   /* "ME1P" */
            int ok = fwrite(h, 4, 4, f) == 4 && fwrite(b, 1, (size_t)got, f) == (size_t)got; ok &= fclose(f) == 0;
            if (ok) rename(tmp, path); else unlink(tmp);
        }
    }
    free(b);
}
static GlProg* prog_get(GlCtx* C, Obj* vso, Obj* pso) {
    GlSh* vs = vso->gx; GlSh* ps = pso->gx;
    if (!vs || !ps) return NULL;
    if (!vs->done) gl_shader_ready(vso);
    if (!ps->done) gl_shader_ready(pso);
    uint32_t hsh = (vs->serial * 2654435761u ^ ps->serial * 40503u) & (PHASH - 1);
    for (GlProg* p = g_prog[hsh]; p; p = p->next) if (p->vs == vs->serial && p->ps == ps->serial) return p;
    GlProg* P = calloc(1, sizeof *P); P->vs = vs->serial; P->ps = ps->serial; P->next = g_prog[hsh]; g_prog[hsh] = P;
    if (!vs->ok || !ps->ok) return P;
    double t0 = mono();
    uint64_t key = prog_key(vso, pso);
    P->p = glCreateProgram();
    int fromcache = bin_load(P->p, key);
    if (!fromcache) {
        char* vsrc = sm3_glsl_vs(&vs->s, &ps->s); char* psrc = sm3_glsl_ps(&ps->s);
        static char log[8192]; GLuint a = compile_sh(GL_VERTEX_SHADER, vsrc, log, sizeof log), b = 0;
        const char* what = "vertex";
        if (a) { what = "fragment"; b = compile_sh(GL_FRAGMENT_SHADER, psrc, log, sizeof log); }
        if (a && b) {
            glAttachShader(P->p, a); glAttachShader(P->p, b);
            if (GX.nbin && glProgramParameteri) glProgramParameteri(P->p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
            glLinkProgram(P->p);
            GLint ok = 0; glGetProgramiv(P->p, GL_LINK_STATUS, &ok);
            if (!ok) { what = "link"; log[0] = 0; glGetProgramInfoLog(P->p, sizeof log, NULL, log); }
            else what = NULL;
        }
        if (a) glDeleteShader(a);
        if (b) glDeleteShader(b);
        char nm[96]; snprintf(nm, sizeof nm, "prog_%08x_%08x%s.glsl", vso->crc, pso->crc, what ? "_FAILED" : "");
        if (what) {
            GS.progfail++;
            static int n; if (n++ < 12) {
                glog("program vs %08x + ps %08x: %s error:\n%.1500s\n", vso->crc, pso->crc, what, log);
                if (n <= 2 && !g_glsldump) glog("source (%s):\n%.3000s\n", what[0] == 'v' ? "vertex" : "fragment", what[0] == 'v' ? vsrc : psrc);
            }
            dump_text(nm, vsrc, psrc, log);
            free(vsrc); free(psrc);
            glDeleteProgram(P->p); P->p = 0; return P;
        }
        if (g_glsldump && g_gldbg) dump_text(nm, vsrc, psrc, NULL);
        free(vsrc); free(psrc);
        bin_store(P->p, key);
    } else GS.progbin++;
    P->ok = 1; GS.progs++;
    if (!fromcache) GS.tbuilt = mono();
    GLuint p = P->p;
    P->l_vc = glGetUniformLocation(p, "vc"); P->l_vi = glGetUniformLocation(p, "vi"); P->l_vb = glGetUniformLocation(p, "vb");
    P->l_pc = glGetUniformLocation(p, "pc"); P->l_pi = glGetUniformLocation(p, "pi"); P->l_pb = glGetUniformLocation(p, "pb");
    P->l_fix = glGetUniformLocation(p, "dx_fix"); P->l_bgra = glGetUniformLocation(p, "dx_bgra"); P->l_at = glGetUniformLocation(p, "dx_at");
    P->l_srgb = glGetUniformLocation(p, "dx_srgb"); P->l_srgbw = glGetUniformLocation(p, "dx_srgbw");
    P->l_vp = glGetUniformLocation(p, "dx_vp"); P->vpu[0] = -1e30f;
    glUseProgram(p); if (C) C->st[S_PROG] = p;
    for (int k = 0; k < 16; k++) {
        char nm[16]; GLint l;
        snprintf(nm, sizeof nm, "ps_s%d", k); if ((l = glGetUniformLocation(p, nm)) >= 0) glUniform1i(l, k);
        if (k < 4) { snprintf(nm, sizeof nm, "vs_s%d", k); if ((l = glGetUniformLocation(p, nm)) >= 0) glUniform1i(l, 16 + k); }
    }
    P->bgra = P->srgb = P->srgbw = -1; P->fix[0] = -1e30f; P->at[0] = -1;
    double dt = mono() - t0;
    if (g_log || dt > 0.25 || GS.progs <= 3) glog("program %llu: vs %08x + ps %08x %s in %.1f ms\n", (unsigned long long)GS.progs, vso->crc, pso->crc, fromcache ? "loaded" : "built", dt * 1e3);
    glcheck("program");
    return P;
}
static void prog_drop(uint32_t serial) {   /* a shader died: delete its programs */
    for (int i = 0; i < PHASH; i++) for (GlProg** pp = &g_prog[i]; *pp;) {
        GlProg* p = *pp;
        if (p->vs == serial || p->ps == serial) {
            *pp = p->next;
            if (p->p) { for (int k = 0; k < g_nctx; k++) if (g_ctxs[k]->st[S_PROG] == p->p) g_ctxs[k]->st[S_PROG] = 0xEEEEEEEEu; glDeleteProgram(p->p); }
            free(p);
        } else pp = &p->next;
    }
}
/* constants: uploaded when the device's set changed since the program last saw it */
static void upload_consts(Dev* d, GlProg* P, GlSh* vs, GlSh* ps) {
    if (P->dev != d) { P->dev = d; P->vver = d->cver[0] - 1; P->pver = d->cver[1] - 1; }
    if (P->vver != d->cver[0]) {
        P->vver = d->cver[0];
        const Sm3* s = &vs->s;
        if (P->l_vc >= 0 && s->nconst) {
            if (s->crel && s->ndef) {
                static float tmpc[256][4]; memcpy(tmpc, d->vsf, (size_t)s->nconst * 16);
                for (int k = 0; k < s->ndef; k++) if (s->def[k].reg < s->nconst) memcpy(tmpc[s->def[k].reg], s->def[k].v, 16);
                glUniform4fv(P->l_vc, s->nconst, &tmpc[0][0]);
            } else glUniform4fv(P->l_vc, s->nconst, &d->vsf[0][0]);
        }
        if (P->l_vi >= 0) glUniform4iv(P->l_vi, 16, &d->vsi[0][0]);
        if (P->l_vb >= 0) { int m = 0; for (int k = 0; k < 16; k++) if (d->vsb[k]) m |= 1 << k; glUniform1i(P->l_vb, m); }
    }
    if (P->pver != d->cver[1]) {
        P->pver = d->cver[1];
        const Sm3* s = &ps->s;
        if (P->l_pc >= 0 && s->nconst) {
            if (s->crel && s->ndef) {
                static float tmpc[224][4]; memcpy(tmpc, d->psf, (size_t)s->nconst * 16);
                for (int k = 0; k < s->ndef; k++) if (s->def[k].reg < s->nconst) memcpy(tmpc[s->def[k].reg], s->def[k].v, 16);
                glUniform4fv(P->l_pc, s->nconst, &tmpc[0][0]);
            } else glUniform4fv(P->l_pc, s->nconst, &d->psf[0][0]);
        }
        if (P->l_pi >= 0) glUniform4iv(P->l_pi, 16, &d->psi[0][0]);
        if (P->l_pb >= 0) { int m = 0; for (int k = 0; k < 16; k++) if (d->psb[k]) m |= 1 << k; glUniform1i(P->l_pb, m); }
    }
}

/* ---- samplers ---- */
static GLenum wrap_gl(uint32_t a) {
    switch (a) {
    case 2: return GL_MIRRORED_REPEAT;
    case 3: return GL_CLAMP_TO_EDGE;
    case 4: return GX.border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
    case 5: return GX.mirror ? XMIRRORCLAMP : GL_CLAMP_TO_EDGE;
    default: return GL_REPEAT;
    }
}
static GLuint sampler_get(const uint32_t* ss, const GlRes* g, const Obj* t) {
    uint32_t u = ss[1] & 7, v = ss[2] & 7, w = ss[3] & 7, mag = ss[5] & 7, mn = ss[6] & 7, mip = ss[7] & 7;
    int nearest = (is_float_fmt(g->ifmt) && !GX.flin);
    uint32_t an = 1; if ((mn == 3 || mag == 3) && GX.aniso) { an = ss[10] ? ss[10] : 1; if (an > (uint32_t)GX.maxaniso) an = (uint32_t)GX.maxaniso; if (an > 16) an = 16; if (sm3_lite && an > 2) an = 2; }
    int lod = (int)(t->lod > ss[9] ? t->lod : ss[9]) - g->drop; if (lod < 0) lod = 0; if (lod > g->levels - g->drop - 1) lod = g->levels - g->drop - 1; if (lod > 15) lod = 15;
    if (g->levels - g->drop <= 1) mip = 0;
    uint32_t border = (u == 4 || v == 4 || w == 4) ? ss[4] : 0;
    uint64_t k = u | v << 3 | w << 6 | mag << 9 | mn << 12 | mip << 15 | (uint64_t)an << 18 | (uint64_t)nearest << 23 | (uint64_t)lod << 24;
    uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40 ^ border * 2654435761u) & (NSMP - 1);
    for (int i = 0; i < NSMP; i++, h = (h + 1) & (NSMP - 1)) {
        SmpEnt* e = &g_smp[h];
        if (e->s && e->k == k && e->border == border) return e->s;
        if (!e->s) {
            if (g_nsmp >= NSMP * 3 / 4) break;
            e->k = k; e->border = border; glGenSamplers(1, &e->s); g_nsmp++;
            GLuint s = e->s;
            glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (GLint)wrap_gl(u)); glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (GLint)wrap_gl(v));
            glSamplerParameteri(s, GL_TEXTURE_WRAP_R, (GLint)wrap_gl(w));
            int lin_mag = mag >= 2 && !nearest, lin_min = mn >= 2 && !nearest;
            GLenum fmin = lin_min ? GL_LINEAR : GL_NEAREST;
            if (mip == 1 || (mip && nearest)) fmin = lin_min ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
            else if (mip >= 2) fmin = lin_min ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
            glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, lin_mag ? GL_LINEAR : GL_NEAREST);
            glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, (GLint)fmin);
            if (lod) glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (float)lod);
            if (an > 1) glSamplerParameterf(s, XANISO, (float)an);
            if (border && GX.border) {
                float c[4] = { (border >> 16 & 255) / 255.f, (border >> 8 & 255) / 255.f, (border & 255) / 255.f, (border >> 24) / 255.f };
                glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, c);
            }
            glcheck("sampler");
            return s;
        }
    }
    static GLuint fallback; if (!fallback) { glGenSamplers(1, &fallback); glSamplerParameteri(fallback, GL_TEXTURE_MIN_FILTER, GL_LINEAR); }
    return fallback;
}
/* texture + sampler of D3D sampler slot idx on GL unit; want: 2 2D, 3 cube, 4 volume. Returns 1 if sRGB reads are on */
static int bind_stage(GlCtx* C, Dev* d, int unit, int idx, int want) {
    GLenum tg = want == 3 ? GL_TEXTURE_CUBE_MAP : want == 4 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
    Obj* t = d->tex[idx]; GlRes* g = t ? tex_res(C, t) : NULL;
    GLuint smp;
    if (!g || g->kind != 1 || g->target != tg) {
        s_tex(C, unit, tg, g_dummy[want == 3 ? 1 : want == 4 ? 2 : 0]);
        static GLuint ds; if (!ds) { glGenSamplers(1, &ds); glSamplerParameteri(ds, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glSamplerParameteri(ds, GL_TEXTURE_MAG_FILTER, GL_NEAREST); }
        smp = ds;
        if (t && g && g->kind == 1) { static int n; if (n++ < 10) glog("sampler %d: %s texture bound where the shader samples %s\n", idx, g->target == GL_TEXTURE_CUBE_MAP ? "cube" : g->target == GL_TEXTURE_3D ? "volume" : "2D", want == 3 ? "cube" : want == 4 ? "volume" : "2D"); }
    } else { s_tex(C, unit, tg, g->name); smp = sampler_get(d->ss[idx], g, t); }
    if (C->smp[unit] != smp) { glBindSampler((GLuint)unit, smp); C->smp[unit] = smp; }
    return g && d->ss[idx][11] != 0;
}

/* ---- render states ---- */
static GLenum blend_gl(uint32_t b) {
    static const GLenum t[16] = { GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
        GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_COLOR,
        GL_ONE_MINUS_CONSTANT_COLOR };
    return b < 16 ? t[b] : GL_ONE;
}
static void blend_pair(uint32_t s, uint32_t d, GLenum* gs, GLenum* gd) {
    if (s == 12) { *gs = GL_SRC_ALPHA; *gd = GL_ONE_MINUS_SRC_ALPHA; return; }           /* BOTHSRCALPHA */
    if (s == 13) { *gs = GL_ONE_MINUS_SRC_ALPHA; *gd = GL_SRC_ALPHA; return; }           /* BOTHINVSRCALPHA */
    *gs = blend_gl(s); *gd = blend_gl(d);
}
static GLenum beq_gl(uint32_t o) {
    switch (o) { case 2: return GL_FUNC_SUBTRACT; case 3: return GL_FUNC_REVERSE_SUBTRACT; case 4: return GL_MIN; case 5: return GL_MAX; default: return GL_FUNC_ADD; }
}
static GLenum cmp_gl(uint32_t c) { return c >= 1 && c <= 8 ? GL_NEVER + (c - 1) : GL_ALWAYS; }
static GLenum sop_gl(uint32_t o) {
    static const GLenum t[9] = { GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
    return o < 9 ? t[o] : GL_KEEP;
}
static uint32_t ffbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static int no_alpha_fmt(uint32_t f) { return f == 22 || f == 24 || f == 33; }   /* X8R8G8B8, X1R5G5B5, X8B8G8R8: destination alpha reads 1 */
/* viewport (and depth range); returns the viewport size */
static void apply_viewport(GlCtx* C, Dev* d, uint32_t* vw, uint32_t* vh) {
    int x = (int)d->vp[0], y = (int)d->vp[1], w = (int)d->vp[2], h = (int)d->vp[3];
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    s_viewport(C, x, y, w, h);
    int a = chg(C, S_DR0, d->vp[4]), b = chg(C, S_DR1, d->vp[5]);
    if (a | b) glDepthRangef(bitsf(d->vp[4]), bitsf(d->vp[5]));
    *vw = (uint32_t)w; *vh = (uint32_t)h;
}
static void apply_rs(GlCtx* C, Dev* d, int tf) {
    const uint32_t* rs = d->rs;
    /* blending */
    int be = rs[27] != 0;
    s_enable(C, S_BLEND, GL_BLEND, be);
    if (be) {
        uint32_t sb = rs[19] & 255, db = rs[20] & 255, sa = rs[206] ? rs[207] & 255 : sb, da = rs[206] ? rs[208] & 255 : db;
        if (chg(C, S_BFUNC, sb | db << 8 | sa << 16 | da << 24)) {
            GLenum s1, d1, s2, d2; blend_pair(sb, db, &s1, &d1); blend_pair(sa, da, &s2, &d2); glBlendFuncSeparate(s1, d1, s2, d2);
        }
        GLenum e1 = beq_gl(rs[171]), e2 = rs[206] ? beq_gl(rs[209]) : e1;
        if (chg(C, S_BEQ, (uint32_t)e1 << 16 | e2)) glBlendEquationSeparate(e1, e2);
        if (chg(C, S_BCOL, rs[193])) { uint32_t c = rs[193]; glBlendColor((c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f); }
    }
    /* color write masks (X8 formats keep their alpha at 1) */
    uint32_t m[4] = { rs[168] & 15, rs[190] & 15, rs[191] & 15, rs[192] & 15 };
    for (int i = 0; i < 4; i++) if (d->rt[i] && no_alpha_fmt(d->rt[i]->fmt)) m[i] &= 7;
    int same = 1; for (int i = 1; i < 4; i++) if (d->rt[i] && m[i] != m[0]) same = 0;
    if (same || !GX.maski) s_cmask(C, m[0]); else for (int i = 0; i < 4; i++) s_cmaski(C, i, m[i]);
    /* depth */
    int hasds = (tf & 2) != 0, zt = hasds && rs[7] != 0;
    s_enable(C, S_DEPTH, GL_DEPTH_TEST, zt);
    if (zt) { s_dmask(C, rs[14] != 0); if (chg(C, S_DFUNC, rs[23])) glDepthFunc(cmp_gl(rs[23])); }
    /* stencil: GL front faces = D3D clockwise = the "normal" ops, back = CCW ops */
    int st = (tf & 4) && rs[52];
    s_enable(C, S_STENCIL, GL_STENCIL_TEST, st);
    if (st) {
        int two = rs[185] != 0;
        uint32_t ref = rs[57] & 255, msk = rs[58] & 255;
        uint32_t ff = rs[56] & 15, bf = two ? rs[189] & 15 : ff;
        if (chg(C, S_SFRONT, ff | ref << 4 | msk << 12)) glStencilFuncSeparate(GL_FRONT, cmp_gl(ff), (GLint)ref, msk);
        if (chg(C, S_SBACK, bf | ref << 4 | msk << 12)) glStencilFuncSeparate(GL_BACK, cmp_gl(bf), (GLint)ref, msk);
        uint32_t of = (rs[53] & 15) | (rs[54] & 15) << 4 | (rs[55] & 15) << 8;
        uint32_t ob = two ? (rs[186] & 15) | (rs[187] & 15) << 4 | (rs[188] & 15) << 8 : of;
        if (chg(C, S_SOPF, of)) glStencilOpSeparate(GL_FRONT, sop_gl(of & 15), sop_gl(of >> 4 & 15), sop_gl(of >> 8));
        if (chg(C, S_SOPB, ob)) glStencilOpSeparate(GL_BACK, sop_gl(ob & 15), sop_gl(ob >> 4 & 15), sop_gl(ob >> 8));
        s_swmask(C, rs[59] & 255);
    }
    /* culling */
    uint32_t cm = rs[22];
    s_enable(C, S_CULL, GL_CULL_FACE, cm == 2 || cm == 3);
    if ((cm == 2 || cm == 3) && chg(C, S_CULLF, cm)) glCullFace(cm == 2 ? GL_FRONT : GL_BACK);
    /* depth bias */
    float bias = bitsf(rs[195]), slope = bitsf(rs[175]);
    int po = hasds && (bias != 0.f || slope != 0.f);
    s_enable(C, S_POFS, GL_POLYGON_OFFSET_FILL, po);
    if (po) { int a = chg(C, S_PO1, rs[175]), b = chg(C, S_PO2, rs[195]); if (a | b) glPolygonOffset(slope, bias * 16777215.f); }
    /* scissor */
    int sc = rs[174] != 0;
    s_enable(C, S_SCISSOR, GL_SCISSOR_TEST, sc);
    if (sc) {
        int l = (int)d->scissor[0], t = (int)d->scissor[1], r = (int)d->scissor[2], b = (int)d->scissor[3];
        s_scissor(C, l, t, r > l ? r - l : 0, b > t ? b - t : 0);
    }
}

/* ---- vertex input ---- */
typedef struct { uint16_t stream, off; uint8_t type, method, usage, index; } VElem;
static const struct { uint8_t n; GLenum t; uint8_t norm, bytes; } g_vtype[17] = {
    { 1, GL_FLOAT, 0, 4 }, { 2, GL_FLOAT, 0, 8 }, { 3, GL_FLOAT, 0, 12 }, { 4, GL_FLOAT, 0, 16 }, { 4, GL_UNSIGNED_BYTE, 1, 4 },
    { 4, GL_UNSIGNED_BYTE, 0, 4 }, { 2, GL_SHORT, 0, 4 }, { 4, GL_SHORT, 0, 8 }, { 4, GL_UNSIGNED_BYTE, 1, 4 }, { 2, GL_SHORT, 1, 4 },
    { 4, GL_SHORT, 1, 8 }, { 2, GL_UNSIGNED_SHORT, 1, 4 }, { 4, GL_UNSIGNED_SHORT, 1, 8 }, { 4, GL_UNSIGNED_INT_2_10_10_10_REV, 0, 4 },
    { 4, GL_INT_2_10_10_10_REV, 1, 4 }, { 2, GL_HALF_FLOAT, 0, 4 }, { 4, GL_HALF_FLOAT, 0, 8 } };
static int fvf_decl(uint32_t fvf, VElem* e) {
    int n = 0; uint16_t off = 0;
#define FE(ty, us, ix) do { e[n].stream = 0; e[n].off = off; e[n].type = (ty); e[n].method = 0; e[n].usage = (us); e[n].index = (ix); off += g_vtype[ty].bytes; n++; } while (0)
    uint32_t pos = fvf & 0x400E;
    if (pos == 0x2) FE(2, 0, 0);
    else if (pos == 0x4) FE(3, 9, 0);
    else if (pos == 0x4002) FE(3, 0, 0);
    else if (pos >= 0x6 && pos <= 0xE) {
        FE(2, 0, 0); int nb = (int)(pos - 4) / 2;   /* XYZB1..5: blend weights (the last one may be indices) */
        int last = (fvf & 0x1000) ? 5 : (fvf & 0x8000) ? 4 : -1, nw = last >= 0 ? nb - 1 : nb;
        if (nw > 0) FE(nw - 1, 1, 0);
        if (last >= 0) FE(last, 2, 0);
    }
    if (fvf & 0x10) FE(2, 3, 0);
    if (fvf & 0x20) FE(0, 4, 0);
    if (fvf & 0x40) FE(4, 10, 0);
    if (fvf & 0x80) FE(4, 10, 1);
    int nt = (int)(fvf >> 8 & 15);
    for (int i = 0; i < nt && i < 8; i++) { uint32_t f = fvf >> (16 + 2 * i) & 3; FE(f == 0 ? 1 : f == 1 ? 2 : f == 2 ? 3 : 0, 5, i); }
#undef FE
    return n;
}
/* Pre-transformed vertices (D3DDECLUSAGE_POSITIONT / D3DFVF_XYZRHW) bypass vertex processing: D3D9 ignores the bound
 * vertex shader for them (wined3d: use_vs() is false when the declaration is position_transformed). Bink movies
 * (FBinkGearsMovieSupport) draw that way with whatever vertex shader UE3 left bound, so honouring the leftover shader put
 * the movie in a small corner of the screen. They get a built-in pass-through vertex shader instead. */
static int draw_rhw(const Dev* d) {
    if (d->decl && d->decl->blob) {
        const uint8_t* b = d->decl->blob;
        for (int i = 0; i < 64 && b[8 * i] != 0xFF; i++) if (b[8 * i + 6] == 9) return 1;
        return 0;
    }
    return d->fvf && (d->fvf & 0x400E) == 0x4;
}
static Obj* rhw_vs(void) {
    static Obj o; static int init;
    if (!init) {
        init = 1;
        /* vs_3_0: dcl v0 POSITION, v1 COLOR0, v2 COLOR1, v3..v10 TEXCOORD0..7; the same outputs; mov o#, v# */
        static uint32_t t[1 + 11 * 3 * 2 + 11 * 3 + 1]; int n = 0;
        static const uint8_t us[11] = { 0, 10, 10, 5, 5, 5, 5, 5, 5, 5, 5 }, ix[11] = { 0, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7 };
        t[n++] = 0xFFFE0300u;
        for (int r = 0; r < 11; r++) { t[n++] = 0x0200001Fu; t[n++] = 0x80000000u | us[r] | (uint32_t)ix[r] << 16; t[n++] = 0x900F0000u | (uint32_t)r; }
        for (int r = 0; r < 11; r++) { t[n++] = 0x0200001Fu; t[n++] = 0x80000000u | us[r] | (uint32_t)ix[r] << 16; t[n++] = 0xE00F0000u | (uint32_t)r; }
        for (int r = 0; r < 11; r++) { t[n++] = 0x02000001u; t[n++] = 0xE00F0000u | (uint32_t)r; t[n++] = 0x90E40000u | (uint32_t)r; }
        t[n++] = 0x0000FFFFu;
        o.type = T_VS; o.blob = (uint8_t*)t; o.bloblen = 4u * (uint32_t)n; o.crc = 0x52485721u;   /* "!WHR" */
        gl_shader_new(&o);
        GlSh* s = gl_shader_ready(&o);
        if (s && s->ok) s->s.rhw = 1;
        else glog("built-in XYZRHW vertex shader not translated: %s\n", s ? s->s.err : "no GL");
    }
    return &o;
}
/* streaming ring buffers (0: vertices, 1: indices). minoff: the returned offset is >= minoff, (offset - minoff) % 16 == 0 */
static uint32_t ring_put(GlCtx* C, int w, const void* src, uint32_t len, uint32_t minoff) {
    GLenum tg = w ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    uint64_t need = (uint64_t)len + minoff + 64;
    if (!C->ring[w] || need > C->rsize[w]) {
        uint32_t sz = 8u << 20; while (sz < need * 2 && sz < (1u << 30)) sz *= 2;
        if (!C->ring[w]) glGenBuffers(1, &C->ring[w]);
        if (w) s_ebuf(C, C->ring[w]); else s_abuf(C, C->ring[w]);
        glBufferData(tg, sz, NULL, GL_STREAM_DRAW); C->rsize[w] = sz; C->roff[w] = 0;
    } else if (w) s_ebuf(C, C->ring[w]); else s_abuf(C, C->ring[w]);
    uint32_t base = C->roff[w] > minoff ? C->roff[w] - minoff : 0;
    uint32_t off = ((base + 15) & ~15u) + minoff;
    if ((uint64_t)off + len > C->rsize[w]) { glBufferData(tg, C->rsize[w], NULL, GL_STREAM_DRAW); off = minoff; }   /* orphan */
    void* p = len ? glMapBufferRange(tg, off, len, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT) : NULL;
    if (p) { memcpy(p, src, len); glUnmapBuffer(tg); } else if (len) glBufferSubData(tg, off, len, src);
    C->roff[w] = off + len; GS.ring += len;
    return off;
}
typedef struct {
    uint32_t pt, prims, count; int indexed;
    int32_t base; uint32_t minv, nv, start;    /* DrawIndexedPrimitive: base vertex, vertex range, start index; DrawPrimitive: start vertex */
    uint32_t upv, upstride, upi, upifmt;       /* *UP draws: guest pointers */
    int up; uint32_t ninst;
} DrawCall;
/* attributes for the vertex shader's inputs; returns the dx_bgra mask, or -1 to skip the draw */
static int setup_vertices(GlCtx* C, Dev* d, const Sm3* vs, const DrawCall* dc) {
    VElem el[65]; int ne = 0;
    if (d->decl && d->decl->blob) {
        const uint8_t* b = d->decl->blob;
        for (; ne < 64 && b[8 * ne] != 0xFF; ne++) {
            VElem* e = &el[ne]; memcpy(&e->stream, b + 8 * ne, 2); memcpy(&e->off, b + 8 * ne + 2, 2);
            e->type = b[8 * ne + 4]; e->method = b[8 * ne + 5]; e->usage = b[8 * ne + 6]; e->index = b[8 * ne + 7];
        }
    } else if (d->fvf) ne = fvf_decl(d->fvf, el);
    int64_t rebase = dc->up ? 0 : dc->indexed ? dc->base : (int64_t)dc->start;
    uint32_t lo, hi;   /* D3D vertex range of per-vertex streams */
    if (dc->indexed) { lo = (uint32_t)((int64_t)dc->minv + (dc->up ? 0 : dc->base)); hi = lo + dc->nv; } else { lo = dc->up ? 0 : dc->start; hi = lo + dc->count; }
    int64_t sptr[16]; GLuint sbuf[16]; uint32_t sdone = 0;
    uint32_t used = 0; int bgra = 0;
    for (int k = 0; k < vs->nin; k++) {
        const Sm3Sem* in = &vs->in[k]; int loc = in->reg & 15;
        const VElem* e = NULL;
        for (int i = 0; i < ne; i++) if (el[i].usage == in->usage && el[i].index == in->index && el[i].type <= 16) { e = &el[i]; break; }
        if (!e && in->usage == 0) for (int i = 0; i < ne; i++) if (el[i].usage == 9 && el[i].index == in->index && el[i].type <= 16) { e = &el[i]; break; }
        int s = e ? e->stream & 15 : 0;
        Obj* vb = dc->up ? NULL : (e ? d->vb[s] : NULL);
        if (!e || (!dc->up && !vb)) {   /* fixed function: a missing diffuse color is white */
            float one = vs->rhw && in->usage == 10 && in->index == 0 ? 1.f : 0.f;
            s_attr(C, loc, 0); glVertexAttrib4f((GLuint)loc, one, one, one, 1.f); continue;
        }
        uint32_t stride = dc->up ? dc->upstride : d->vbstride[s];
        int inst = !dc->up && dc->ninst > 1 && (d->vbfreq[s] & 0x80000000u);
        uint32_t div = 0;
        if (!(sdone >> s & 1)) {
            sdone |= 1u << s;
            uint32_t off0 = dc->up ? 0 : d->vboff[s];
            uint32_t slo = lo, shi = hi; int64_t srb = rebase;
            if (inst) { div = d->vbfreq[s] & 0x3FFFFFFFu; if (!div) div = 1; slo = 0; shi = (dc->ninst + div - 1) / div; srb = 0; }
            if (!stride) { slo = 0; shi = 1; srb = 0; }
            GlRes* g = (!dc->up && !vb->shadow) ? buf_res(C, vb) : NULL;
            if (g) { sbuf[s] = g->name; sptr[s] = (int64_t)off0 + srb * stride; }
            else {
                uint32_t src = dc->up ? dc->upv : vb->shadow;
                uint64_t a = (uint64_t)off0 + (uint64_t)slo * (stride ? stride : 0), len = (uint64_t)(shi - slo) * (stride ? stride : 0);
                if (!stride) len = 256;
                if (!dc->up) { if (a >= vb->size) return -1; if (a + len > vb->size) len = vb->size - a; }
                uint32_t minoff = (uint32_t)((int64_t)slo - srb) * stride;
                uint32_t r = ring_put(C, 0, M + src + a, (uint32_t)len, minoff);
                sbuf[s] = C->ring[0]; sptr[s] = (int64_t)r - (int64_t)minoff;
            }
        }
        if (inst) { div = d->vbfreq[s] & 0x3FFFFFFFu; if (!div) div = 1; }
        if (!stride) div = 0x7FFFFFFFu;
        int64_t ptr = sptr[s] + e->off;
        if (ptr < 0) { static int n; if (n++ < 10) glog("%s: negative vertex offset (base vertex %d) - draw skipped\n", g_cur, dc->base); return -1; }
        s_abuf(C, sbuf[s]);
        glVertexAttribPointer((GLuint)loc, g_vtype[e->type].n, g_vtype[e->type].t, g_vtype[e->type].norm, (GLsizei)stride, (const void*)(uintptr_t)ptr);
        s_attr(C, loc, 1); s_div(C, loc, div);
        used |= 1u << loc;
        if (e->type == 4) bgra |= 1 << loc;
    }
    for (int loc = 0; loc < 16; loc++) if ((C->attr_on >> loc & 1) && !(used >> loc & 1)) s_attr(C, loc, 0);
    return bgra;
}
static const GLenum g_mode[7] = { GL_POINTS, GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN };
static uint32_t prim_verts(uint32_t pt, uint32_t n) {
    switch (pt) { case 1: return n; case 2: return 2 * n; case 3: return n ? n + 1 : 0; case 4: return 3 * n; case 5: case 6: return n ? n + 2 : 0; default: return 0; }
}
/* v0.10.2: CPU cost of a draw call, in stages; measured on every 16th draw (the log line costs too much for every call).
 * The GLES and the Vulkan path (hle_d3d9_vk.h) both accumulate into g_gp and gp_report appends the totals to the
 * frame-times suffix of "[d3d] frame ...". Shared by both backends - this header is included first. */
enum { GP_PROG, GP_TGT, GP_CONST, GP_TEX, GP_STATE, GP_VERT, GP_DRAW, GP_NST };
static struct { uint64_t ns[GP_NST]; uint32_t n; } g_gp;
static void gp_report(char* o, size_t cap, size_t* k) {
    if (!g_gp.n) return;
    double us[GP_NST], tot = 0;
    for (int i = 0; i < GP_NST; i++) { us[i] = g_gp.ns[i] / 1e3 / g_gp.n; tot += us[i]; }
    if (*k < cap) *k += (size_t)snprintf(o + *k, cap - *k, ", CPU %.1f us/draw (program %.1f, targets %.1f, constants %.1f, textures %.1f, states %.1f, vertices %.1f, draw %.1f, %u samples)",
                                         tot, us[GP_PROG], us[GP_TGT], us[GP_CONST], us[GP_TEX], us[GP_STATE], us[GP_VERT], us[GP_DRAW], g_gp.n);
    memset(&g_gp, 0, sizeof g_gp);
}
/* ---- v1.1.3: occlusion queries. GLES 3 has no sample counts: GL_ANY_SAMPLES_PASSED_CONSERVATIVE says visible or hidden,
 * a visible query reports 0.5% of the render target (q_area). One GL query per D3D query, begun at its first draw (in the
 * drawing thread's context), ended at Issue(END); GetData flushes when polled again and gives up after 0.25 s. ---- */
#define XANY_SAMPLES 0x8D6A   /* GL_ANY_SAMPLES_PASSED_CONSERVATIVE */
static void gq_draw(GlCtx* C, uint32_t W, uint32_t H) {
    QRes* q = g_qopen->gx; q->rec++;
    if (!q->rtw) { q->rtw = W; q->rth = H; }
    if (q->over || q->n) return;
    if (!glGenQueries || !glBeginQuery) { q->over = 1; return; }
    if (!q->glq[0] || q->ctx != C) { GLuint id = 0; glGenQueries(1, &id); q->glq[0] = id; q->ctx = C; }   /* names are per context */
    if (!q->glq[0]) { q->over = 1; return; }
    glBeginQuery(XANY_SAMPLES, q->glq[0]); q->n = 1;
}
static void gles_query_begin(Obj* o) { (void)o; }
static void gles_query_end(Obj* o) {
    QRes* q = o->gx;
    if (!q->n) return;
    if (q->ctx == gl_cur()) glEndQuery(XANY_SAMPLES); else q->over = 1;
}
static int gles_query_data(Obj* o, uint32_t flags, uint32_t* out) {
    QRes* q = o->gx; (void)flags;
    if (!q->n || q->ctx != gl_cur() || !glGetQueryObjectuiv) { q_done(q, q_visible(q), 0); *out = q->res; return 1; }
    GLuint av = 0; glGetQueryObjectuiv(q->glq[0], 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &av);
    if (av) {
        GLuint r = 0; glGetQueryObjectuiv(q->glq[0], 0x8866 /* GL_QUERY_RESULT */, &r);
        q_done(q, r ? q_area(q) : 0, 1); *out = q->res; return 1;
    }
    int poll = q->polls++;
    if (poll == 1) { glFlush(); g_qs.flushes++; }
    if (poll) g_qs.waits++;
    if (!q->t0) q->t0 = mono();
    else if (mono() - q->t0 > 0.25) { q_done(q, q_visible(q), 0); *out = q->res; return 1; }
    return 0;
}
static void gles_query_free(QRes* q) {
    if (q && q->glq[0] && q->ctx && q->ctx == gl_cur() && glDeleteQueries) glDeleteQueries(1, &q->glq[0]);
}
static void gl_draw(Dev* d, DrawCall* dc) {
    GlCtx* C = gl_cur(); if (!C) return;
    dc->count = prim_verts(dc->pt, dc->prims);
    if (!dc->count || dc->pt < 1 || dc->pt > 6) return;
    int rhw = draw_rhw(d);
    Obj* vso = rhw ? rhw_vs() : d->vs;
    if (!vso || !d->ps || !vso->gx || !d->ps->gx) { GS.skipped++; return; }
    GlSh* vs = vso->gx; GlSh* ps = d->ps->gx;
    int prof = (g_gp.n = g_gp.n + 1, (g_gp.n & 15) == 0);
    double ts[GP_NST + 1]; if (prof) ts[0] = mono();
    GlProg* P = prog_get(C, vso, d->ps);
    if (!P || !P->ok) { GS.skipped++; return; }
    uint32_t W, H; int tf; uint32_t vw, vh;
    float fix[4], at[2]; int srgb, sw;
    if (prof) {
        ts[GP_PROG + 1] = mono();
        tf = bind_targets(C, d, &W, &H);
        ts[GP_TGT + 1] = mono();
    } else {
        tf = bind_targets(C, d, &W, &H);
    }
    if (!tf) { GS.skipped++; return; }
    if (dc->indexed && !dc->up && !d->ib) { GS.skipped++; return; }
    s_prog(C, P->p);
    upload_consts(d, P, vs, ps);
    apply_viewport(C, d, &vw, &vh);
    fix[0] = 1.f / (float)vw; fix[1] = 1.f / (float)vh; fix[2] = -1.f; fix[3] = 0.f;
    if (P->l_fix >= 0 && memcmp(fix, P->fix, sizeof fix)) { memcpy(P->fix, fix, sizeof fix); glUniform4fv(P->l_fix, 1, fix); }
    if (rhw) {
        GS.rhw++;
        float vpu[4] = { (float)(int)d->vp[0], (float)(int)d->vp[1], 2.f / (float)vw, 2.f / (float)vh };
        if (P->l_vp >= 0 && memcmp(vpu, P->vpu, sizeof vpu)) { memcpy(P->vpu, vpu, sizeof vpu); glUniform4fv(P->l_vp, 1, vpu); }
    }
    if (prof) ts[GP_CONST + 1] = mono();
    srgb = 0;
    for (int k = 0; k < 16; k++) if (ps->s.samp[k]) srgb |= bind_stage(C, d, k, k, ps->s.samp[k]) << k;
    for (int k = 0; k < 4; k++) if (vs->s.samp[k]) bind_stage(C, d, 16 + k, 17 + k, vs->s.samp[k]);
    if (prof) ts[GP_TEX + 1] = mono();
    apply_rs(C, d, tf);
    at[0] = d->rs[15] ? (float)(d->rs[25] & 15) : 0.f; at[1] = (float)(d->rs[24] & 255) / 255.f;
    if (P->l_at >= 0 && (at[0] != P->at[0] || at[1] != P->at[1])) { P->at[0] = at[0]; P->at[1] = at[1]; float v4[4] = { at[0], at[1], 0.f, 0.f }; glUniform4fv(P->l_at, 1, v4); }
    if (P->l_srgb >= 0 && srgb != P->srgb) { P->srgb = srgb; glUniform1i(P->l_srgb, srgb); }
    sw = d->rs[194] != 0;
    if (P->l_srgbw >= 0 && sw != P->srgbw) { P->srgbw = sw; glUniform1i(P->l_srgbw, sw); }
    if (prof) ts[GP_STATE + 1] = mono();
    dc->ninst = 0;
    if (dc->indexed && !dc->up && (d->vbfreq[0] & 0x40000000u)) { dc->ninst = d->vbfreq[0] & 0x3FFFFFFFu; if (!dc->ninst) dc->ninst = 1; }
    int bgra = setup_vertices(C, d, &vs->s, dc);
    if (bgra < 0) { GS.skipped++; return; }
    if (P->l_bgra >= 0 && bgra != P->bgra) { P->bgra = bgra; glUniform1i(P->l_bgra, bgra); }
    if (prof) ts[GP_VERT + 1] = mono();
    if (g_qopen) gq_draw(C, W, H);   /* v1.1.3: inside an occlusion query */
    GLenum mode = g_mode[dc->pt];
    if (!dc->indexed) glDrawArrays(mode, 0, (GLsizei)dc->count);
    else {
        uint32_t ifmt = dc->up ? dc->upifmt : d->ib->fmt, isz = ifmt == 102 ? 4 : 2;
        GLenum it = isz == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        uint32_t off;
        if (dc->up) off = ring_put(C, 1, M + dc->upi, dc->count * isz, 0);
        else {
            Obj* ib = d->ib;
            uint64_t a = (uint64_t)dc->start * isz, len = (uint64_t)dc->count * isz;
            if (a + len > ib->size) { GS.skipped++; return; }
            GlRes* g = ib->shadow ? NULL : buf_res(C, ib);
            if (g) { s_ebuf(C, g->name); off = (uint32_t)a; }
            else off = ring_put(C, 1, M + ib->shadow + a, (uint32_t)len, 0);
        }
        if (dc->ninst > 1) glDrawElementsInstanced(mode, (GLsizei)dc->count, it, (const void*)(uintptr_t)off, (GLsizei)dc->ninst);
        else glDrawElements(mode, (GLsizei)dc->count, it, (const void*)(uintptr_t)off);
    }
    if (prof) {
        ts[GP_DRAW + 1] = mono();
        for (int i = 0; i < GP_NST; i++) g_gp.ns[i] += (uint64_t)((ts[i + 1] - ts[i]) * 1e9);
    }
    GS.draws++;
    glcheck("draw");
}
static void gl_draw_x(Dev* d, uint32_t pt, uint32_t prims, int indexed, int32_t base, uint32_t minv, uint32_t nv, uint32_t start,
                      int up, uint32_t upv, uint32_t upstride, uint32_t upi, uint32_t upifmt) {
    DrawCall dc; memset(&dc, 0, sizeof dc);
    dc.pt = pt; dc.prims = prims; dc.indexed = indexed; dc.base = base; dc.minv = minv; dc.nv = nv; dc.start = start;
    dc.up = up; dc.upv = upv; dc.upstride = upstride; dc.upi = upi; dc.upifmt = upifmt;
    gl_draw(d, &dc);
}

/* ---- clears, copies, readback ---- */
static void rect_and(int* r, int x0, int y0, int x1, int y1) {
    if (r[0] < x0) r[0] = x0;
    if (r[1] < y0) r[1] = y0;
    if (r[2] > x1) r[2] = x1;
    if (r[3] > y1) r[3] = y1;
}
static void gl_clear(Dev* d, uint32_t n, uint32_t rects, uint32_t flags, uint32_t color, uint32_t zbits, uint32_t stencil) {
    GlCtx* C = gl_cur(); if (!C) return;
    uint32_t W, H; int tf = bind_targets(C, d, &W, &H);
    if (!tf) return;
    GLbitfield mask = 0;
    if (flags & 1) {
        mask |= GL_COLOR_BUFFER_BIT; s_cmask(C, 15);
        float a = (color >> 24) / 255.f; if (no_alpha_fmt(d->rt[0]->fmt)) a = 1.f;
        glClearColor((color >> 16 & 255) / 255.f, (color >> 8 & 255) / 255.f, (color & 255) / 255.f, a);
    }
    if ((flags & 2) && (tf & 2)) { mask |= GL_DEPTH_BUFFER_BIT; s_dmask(C, 1); float z = bitsf(zbits); glClearDepthf(z < 0.f ? 0.f : z > 1.f ? 1.f : z); }
    if ((flags & 4) && (tf & 4)) { mask |= GL_STENCIL_BUFFER_BIT; s_swmask(C, 0xFF); glClearStencil((GLint)(stencil & 255)); }
    if (!mask) return;
    int area[4] = { (int)d->vp[0], (int)d->vp[1], (int)(d->vp[0] + d->vp[2]), (int)(d->vp[1] + d->vp[3]) };
    rect_and(area, 0, 0, (int)W, (int)H);
    if (d->rs[174]) rect_and(area, (int)d->scissor[0], (int)d->scissor[1], (int)d->scissor[2], (int)d->scissor[3]);
    int nr = rects ? (int)n : 0; if (nr > 256) nr = 256;
    for (int i = 0; i < (nr ? nr : 1); i++) {
        int r[4] = { area[0], area[1], area[2], area[3] };
        if (nr) rect_and(r, (int)rd32(rects + 16 * i), (int)rd32(rects + 16 * i + 4), (int)rd32(rects + 16 * i + 8), (int)rd32(rects + 16 * i + 12));
        if (r[2] <= r[0] || r[3] <= r[1]) continue;
        int full = r[0] == 0 && r[1] == 0 && r[2] == (int)W && r[3] == (int)H;
        s_enable(C, S_SCISSOR, GL_SCISSOR_TEST, !full);
        if (!full) s_scissor(C, r[0], r[1], r[2] - r[0], r[3] - r[1]);
        glClear(mask);
    }
    GS.clears++;
    glcheck("clear");
}
static void rect_of(uint32_t p, uint32_t w, uint32_t h, int* r) {
    r[0] = 0; r[1] = 0; r[2] = (int)w; r[3] = (int)h;
    if (p) { r[0] = (int)rd32(p); r[1] = (int)rd32(p + 4); r[2] = (int)rd32(p + 8); r[3] = (int)rd32(p + 12); }
}
static void scratch_begin(GlCtx* C, const Img* src, const Img* dst, GLenum ap) {
    s_enable(C, S_SCISSOR, GL_SCISSOR_TEST, 0);
    C->st[S_FBO] = 0xEEEEEEEEu;
    if (src) { glBindFramebuffer(GL_READ_FRAMEBUFFER, C->scr[0]); attach(GL_READ_FRAMEBUFFER, ap, src); }
    if (dst) { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, C->scr[1]); attach(GL_DRAW_FRAMEBUFFER, ap, dst); }
}
static void scratch_end(GlCtx* C, int src, int dst, GLenum ap) {
    if (src) { glBindFramebuffer(GL_READ_FRAMEBUFFER, C->scr[0]); detach(GL_READ_FRAMEBUFFER, ap); }
    if (dst) { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, C->scr[1]); detach(GL_DRAW_FRAMEBUFFER, ap); }
    C->st[S_FBO] = 0xEEEEEEEEu;
}
static void gl_stretch(Obj* src, uint32_t sr, Obj* dst, uint32_t dr, uint32_t filter) {
    GlCtx* C = gl_cur(); if (!C) return;
    Img a, b;
    if (!img_of(C, src, &a) || !img_of(C, dst, &b) || a.g->kind != b.g->kind) { static int n; if (n++ < 10) glog("StretchRect: unsupported source/destination\n"); return; }
    uint32_t sw, sh, dw, dh; img_size(&a, &sw, &sh); img_size(&b, &dw, &dh);
    int s[4], t[4]; rect_of(sr, sw, sh, s); rect_of(dr, dw, dh, t);
    int depth = a.g->kind == 2;
    GLenum ap = depth ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0;
    scratch_begin(C, &a, &b, ap);
    glBlitFramebuffer(s[0], s[1], s[2], s[3], t[0], t[1], t[2], t[3], depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT,
                      depth || filter < 2 ? GL_NEAREST : GL_LINEAR);
    scratch_end(C, 1, 1, ap);
    GS.blits++;
    glcheck("StretchRect");
}
static void gl_colorfill(Obj* surf, uint32_t rect, uint32_t color) {
    GlCtx* C = gl_cur(); if (!C) return;
    Img b; if (!img_of(C, surf, &b) || b.g->kind != 1) return;
    uint32_t w, h; img_size(&b, &w, &h); int r[4]; rect_of(rect, w, h, r); rect_and(r, 0, 0, (int)w, (int)h);
    if (r[2] <= r[0] || r[3] <= r[1]) return;
    scratch_begin(C, NULL, &b, GL_COLOR_ATTACHMENT0);
    s_cmask(C, 15);
    s_enable(C, S_SCISSOR, GL_SCISSOR_TEST, 1); s_scissor(C, r[0], r[1], r[2] - r[0], r[3] - r[1]);
    float c[4] = { (color >> 16 & 255) / 255.f, (color >> 8 & 255) / 255.f, (color & 255) / 255.f, (color >> 24) / 255.f };
    glClearBufferfv(GL_COLOR, 0, c);
    scratch_end(C, 0, 1, GL_COLOR_ATTACHMENT0);
    glcheck("ColorFill");
}
/* sub-resource memory of a CPU-side surface (system memory pools keep their lock buffers) */
static uint32_t sub_mem(Obj* s, Obj** rootp, uint32_t* sub) {
    Obj* r = s->parent ? s->parent : s; uint32_t i = s->parent ? s->face * r->levels + s->level : 0, lvl = s->parent ? s->level : 0;
    if (!r->lockv) r->lockv = calloc(sub_count(r), 4);
    if (!r->lockv[i]) { uint32_t n = level_bytes(r, lvl); r->lockv[i] = galloc(n); if (r->lockv[i]) memset(M + r->lockv[i], 0, n); }
    *rootp = r; *sub = i;
    return r->lockv[i];
}
static void gl_update_surface(Obj* src, uint32_t sr, Obj* dst, uint32_t dpt) {
    GlCtx* C = gl_cur(); if (!C || src->fmt != dst->fmt) return;
    Img b; if (!img_of(C, dst, &b) || b.g->kind != 1) return;
    Obj* sroot; uint32_t si; uint32_t mem = sub_mem(src, &sroot, &si); if (!mem) return;
    uint32_t sw = src->w, sh = src->h; int s[4]; rect_of(sr, sw, sh, s); rect_and(s, 0, 0, (int)sw, (int)sh);
    if (s[2] <= s[0] || s[3] <= s[1]) return;
    uint32_t dx = dpt ? rd32(dpt) : 0, dy = dpt ? rd32(dpt + 4) : 0;
    uint32_t pitch, rows, unit; int blk; layout(src->fmt, sw, sh, &pitch, &rows, &unit, &blk);
    uint32_t k = blk ? 4 : 1;
    const uint8_t* p = M + mem + (size_t)((uint32_t)s[1] / k) * pitch + (size_t)((uint32_t)s[0] / k) * unit;
    tex_upload(C, b.root, b.g, b.face, b.level, dx, dy, 0, (uint32_t)(s[2] - s[0]), (uint32_t)(s[3] - s[1]), 1, p, pitch, pitch * rows);
}
static void gl_update_texture(Obj* src, Obj* dst) {
    GlCtx* C = gl_cur(); if (!C || src->fmt != dst->fmt || src->type != dst->type) return;
    GlRes* g = tex_res(C, dst); if (!g || g->kind != 1 || !src->lockv) return;
    uint32_t faces = dst->type == T_CUBE ? 6 : 1;
    for (uint32_t l = 0; l < dst->levels; l++) {
        uint32_t w = dimv(dst->w, l), h = dimv(dst->h, l), dd = dst->type == T_VOLTEX ? dimv(dst->d, l) : 1, sl;
        for (sl = 0; sl < src->levels; sl++) if (dimv(src->w, sl) == w && dimv(src->h, sl) == h) break;
        if (sl == src->levels) continue;
        uint32_t pitch, rows, unit; int blk; layout(dst->fmt, w, h, &pitch, &rows, &unit, &blk);
        for (uint32_t f = 0; f < faces; f++) {
            uint32_t m = src->lockv[f * src->levels + sl]; if (!m) continue;
            tex_upload(C, dst, g, f, l, 0, 0, 0, w, h, dd, M + m, pitch, pitch * rows);
        }
    }
}
/* read (x, y, w, h) of a color image into guest memory in the D3D layout of fmt */
static int img_read(GlCtx* C, const Img* im, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* dst, uint32_t pitch, uint32_t fmt) {
    if (!im->g || im->g->kind != 1 || !w || !h) return 0;
    int fl = is_float_fmt(im->g->ifmt) || is_half_fmt(im->g->ifmt);
    uint8_t* buf = tmpbuf((size_t)w * h * (fl ? 16 : 4));
    scratch_begin(C, im, NULL, GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, GL_RGBA, fl ? GL_FLOAT : GL_UNSIGNED_BYTE, buf);
    scratch_end(C, 1, 0, GL_COLOR_ATTACHMENT0);
    glcheck("readback");
    for (uint32_t r = 0; r < h; r++) {
        uint8_t* o = dst + (size_t)r * pitch;
        for (uint32_t c = 0; c < w; c++) {
            float v[4]; uint8_t b[4];
            if (fl) { memcpy(v, buf + ((size_t)r * w + c) * 16, 16); for (int i = 0; i < 4; i++) { float t = v[i] < 0 ? 0 : v[i] > 1 ? 1 : v[i]; b[i] = (uint8_t)(t * 255.f + .5f); } }
            else { memcpy(b, buf + ((size_t)r * w + c) * 4, 4); for (int i = 0; i < 4; i++) v[i] = b[i] / 255.f; }
            uint16_t u16; uint32_t u32;
            switch (fmt) {
            case 21: case 22: o[4 * c] = b[2]; o[4 * c + 1] = b[1]; o[4 * c + 2] = b[0]; o[4 * c + 3] = b[3]; break;
            case 32: case 33: memcpy(o + 4 * c, b, 4); break;
            case 23: u16 = (uint16_t)((b[0] >> 3) << 11 | (b[1] >> 2) << 5 | b[2] >> 3); memcpy(o + 2 * c, &u16, 2); break;
            case 24: case 25: u16 = (uint16_t)((b[3] >> 7) << 15 | (b[0] >> 3) << 10 | (b[1] >> 3) << 5 | b[2] >> 3); memcpy(o + 2 * c, &u16, 2); break;
            case 26: u16 = (uint16_t)((b[3] >> 4) << 12 | (b[0] >> 4) << 8 | (b[1] >> 4) << 4 | b[2] >> 4); memcpy(o + 2 * c, &u16, 2); break;
            case 31: u32 = (uint32_t)(v[0] * 1023.f + .5f) | (uint32_t)(v[1] * 1023.f + .5f) << 10 | (uint32_t)(v[2] * 1023.f + .5f) << 20 | (uint32_t)(v[3] * 3.f + .5f) << 30; memcpy(o + 4 * c, &u32, 4); break;
            case 35: u32 = (uint32_t)(v[2] * 1023.f + .5f) | (uint32_t)(v[1] * 1023.f + .5f) << 10 | (uint32_t)(v[0] * 1023.f + .5f) << 20 | (uint32_t)(v[3] * 3.f + .5f) << 30; memcpy(o + 4 * c, &u32, 4); break;
            case 28: o[c] = b[3]; break;
            case 50: o[c] = b[0]; break;
            case 51: o[2 * c] = b[0]; o[2 * c + 1] = b[3]; break;
            case 111: u16 = f2h(v[0]); memcpy(o + 2 * c, &u16, 2); break;
            case 112: for (int i = 0; i < 2; i++) { u16 = f2h(v[i]); memcpy(o + 4 * c + 2 * i, &u16, 2); } break;
            case 113: for (int i = 0; i < 4; i++) { u16 = f2h(v[i]); memcpy(o + 8 * c + 2 * i, &u16, 2); } break;
            case 114: memcpy(o + 4 * c, v, 4); break;
            case 115: memcpy(o + 8 * c, v, 8); break;
            case 116: memcpy(o + 16 * c, v, 16); break;
            case 34: for (int i = 0; i < 2; i++) { u16 = (uint16_t)(v[i] < 0 ? 0 : v[i] > 1 ? 65535 : v[i] * 65535.f + .5f); memcpy(o + 4 * c + 2 * i, &u16, 2); } break;
            case 36: for (int i = 0; i < 4; i++) { u16 = (uint16_t)(v[i] < 0 ? 0 : v[i] > 1 ? 65535 : v[i] * 65535.f + .5f); memcpy(o + 8 * c + 2 * i, &u16, 2); } break;
            default: return 0;
            }
        }
    }
    return 1;
}
static void gl_get_rt_data(Obj* rt, Obj* dst) {
    GlCtx* C = gl_cur(); if (!C || rt->w != dst->w || rt->h != dst->h) return;
    Img a; if (!img_of(C, rt, &a)) return;
    Obj* r; uint32_t si; uint32_t mem = sub_mem(dst, &r, &si); if (!mem) return;
    uint32_t pitch, rows, unit; int blk; layout(dst->fmt, dst->w, dst->h, &pitch, &rows, &unit, &blk);
    if (!img_read(C, &a, 0, 0, dst->w, dst->h, M + mem, pitch, dst->fmt)) { static int n; if (n++ < 5) glog("GetRenderTargetData: %s readback not supported\n", fmt_name(dst->fmt)); }
}
/* LockRect of a render target: its current contents */
static void gl_rt_lock(Obj* r, uint32_t s, uint32_t lvl, uint32_t mem) {
    GlCtx* C = gl_cur(); if (!C) return;
    GlRes* g = tex_res(C, r); if (!g || g->kind != 1) return;
    Img im = { g, r, r->type == T_CUBE ? s / r->levels : 0, lvl };
    if ((int)lvl < g->drop) return;
    uint32_t pitch, rows, unit; int blk; layout(r->fmt, dimv(r->w, lvl), dimv(r->h, lvl), &pitch, &rows, &unit, &blk);
    img_read(C, &im, 0, 0, dimv(r->w, lvl), dimv(r->h, lvl), M + mem, pitch, r->fmt);
}

/* ---- PNG (stored deflate) ---- */
static uint32_t g_crctab[256];
static uint32_t png_crc(uint32_t c, const uint8_t* p, size_t n) {
    if (!g_crctab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; g_crctab[i] = k; }
    c = ~c; for (size_t i = 0; i < n; i++) c = g_crctab[(c ^ p[i]) & 255] ^ (c >> 8); return ~c;
}
static void be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void png_chunk(FILE* f, const char* t, const uint8_t* d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = png_crc(0, h + 4, 4); c = png_crc(c, d, n); be32(h, c); fwrite(h, 1, 4, f);
}
/* rgba: w*h*4 bytes, rows top first unless flip */
static int png_write(const char* path, const uint8_t* rgba, int w, int h, int flip) {
    FILE* f = fopen(path, "wb"); if (!f) return 0;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 }; fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    png_chunk(f, "IHDR", ih, 13);
    size_t row = (size_t)w * 3 + 1, raw = row * (size_t)h, nblk = (raw + 65534) / 65535;
    size_t zn = 2 + raw + 5 * nblk + 4; uint8_t* z = malloc(zn); uint8_t* rbuf = malloc(raw);
    for (int y = 0; y < h; y++) {
        const uint8_t* s = rgba + (size_t)(flip ? h - 1 - y : y) * w * 4; uint8_t* o = rbuf + (size_t)y * row; o[0] = 0;
        for (int x = 0; x < w; x++) { o[1 + 3 * x] = s[4 * x]; o[2 + 3 * x] = s[4 * x + 1]; o[3 + 3 * x] = s[4 * x + 2]; }
    }
    size_t k = 0; z[k++] = 0x78; z[k++] = 0x01;
    uint32_t a1 = 1, a2 = 0;
    for (size_t i = 0; i < raw; i++) { a1 += rbuf[i]; if (a1 >= 65521) a1 -= 65521; a2 += a1; if (a2 >= 65521) a2 -= 65521; }
    for (size_t off = 0; off < raw || off == 0; ) {
        size_t n = raw - off > 65535 ? 65535 : raw - off;
        z[k++] = off + n >= raw ? 1 : 0; z[k++] = (uint8_t)n; z[k++] = (uint8_t)(n >> 8); z[k++] = (uint8_t)~n; z[k++] = (uint8_t)(~n >> 8);
        memcpy(z + k, rbuf + off, n); k += n; off += n; if (!n) break;
    }
    be32(z + k, a2 << 16 | a1); k += 4;
    png_chunk(f, "IDAT", z, (uint32_t)k); png_chunk(f, "IEND", NULL, 0);
    free(z); free(rbuf);
    return fclose(f) == 0;
}

/* ---- present ---- */
typedef struct { GLuint tex; int w, h, state; GLsync wfence, rfence; uint64_t frame; } Slot;   /* state 0 free, 1 ready, 2 presenting, 3 writing */
static Slot g_slot[3];
static pthread_mutex_t g_pmx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pcv = PTHREAD_COND_INITIALIZER;
static void* g_win_pending; static int g_win_gen, g_win_ack, g_presenter, g_haswin;
static int g_view[6];   /* letterbox x, y, w, h in the window (top-left origin), frame w, h */
static uint64_t g_pframes, g_shown, g_dropped;
static GLsync g_throttle[2];
static int g_fakew, g_fakeh;
static void dump_image(GlCtx* C, const Img* im, uint32_t w, uint32_t h, const char* path) {
    uint8_t* buf = malloc((size_t)w * h * 4);
    scratch_begin(C, im, NULL, GL_COLOR_ATTACHMENT0); glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    scratch_end(C, 1, 0, GL_COLOR_ATTACHMENT0);
    if (!png_write(path, buf, (int)w, (int)h, 0)) { static int n; if (n++ < 3) glog("cannot write %s\n", path); }
    free(buf);
}
/* v0.9.9: where the frame time goes. GPU time per frame (GL_EXT_disjoint_timer_query: one TIME_ELAPSED query per presented
 * frame on the device context, read back a few frames later, never blocking) and the time Present waits for the GPU
 * (the two-frames-ahead throttle). Both are printed in the "[d3d] frame N: fps" line; ME1_GPU_TIMER=0 disables the query. */
typedef void (*PFQGEN)(GLsizei, GLuint*); typedef void (*PFQBEGIN)(GLenum, GLuint); typedef void (*PFQEND)(GLenum);
typedef void (*PFQGETU)(GLuint, GLenum, GLuint*); typedef void (*PFQGET64)(GLuint, GLenum, uint64_t*);
static struct { PFQGEN gen; PFQBEGIN begin; PFQEND end; PFQGETU getu; PFQGET64 get64; GlCtx* ctx; GLuint q[4];
                uint64_t issued, read, ns, nf; int active; double wait, waitf; } GT;
static void gt_init(const char* ex) {
    const char* e = getenv("ME1_GPU_TIMER");
    if ((e && *e == '0') || !has_ext(ex, "GL_EXT_disjoint_timer_query")) return;
    GT.gen = (PFQGEN)p_eglGetProcAddress("glGenQueriesEXT"); GT.begin = (PFQBEGIN)p_eglGetProcAddress("glBeginQueryEXT");
    GT.end = (PFQEND)p_eglGetProcAddress("glEndQueryEXT"); GT.getu = (PFQGETU)p_eglGetProcAddress("glGetQueryObjectuivEXT");
    GT.get64 = (PFQGET64)p_eglGetProcAddress("glGetQueryObjectui64vEXT");
    if (!GT.gen || !GT.begin || !GT.end || !GT.getu || !GT.get64) GT.begin = NULL;
}
static void gt_frame(GlCtx* C) {   /* at Present: close this frame's query, collect finished ones, open the next */
    if (!GT.begin || (GT.ctx && GT.ctx != C)) return;
    if (!GT.ctx) { GT.ctx = C; GT.gen(4, GT.q); }
    if (GT.active) { GT.end(0x88BF /* GL_TIME_ELAPSED_EXT */); GT.active = 0; GT.issued++; }
    while (GT.read < GT.issued) {
        GLuint av = 0; GT.getu(GT.q[GT.read & 3], 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &av);
        if (!av) break;
        uint64_t ns = 0; GT.get64(GT.q[GT.read & 3], 0x8866 /* GL_QUERY_RESULT */, &ns); GT.read++;
        GLint dj = 0; glGetIntegerv(0x8FBB /* GL_GPU_DISJOINT_EXT */, &dj);
        if (!dj && ns < 10000000000ull) { GT.ns += ns; GT.nf++; }
    }
    if (GT.issued - GT.read < 4) { GT.begin(0x88BF, GT.q[GT.issued & 3]); GT.active = 1; }
}
/* -> printable " | GPU 23.1 ms/frame, Present waits 12.0 ms" for the frames since the last call ("" if nothing measured) */
static void gl_frame_times(char* o, size_t cap, uint64_t frames) {
    size_t k = 0; o[0] = 0;
    if (GT.nf) k += (size_t)snprintf(o + k, cap - k, ", GPU %.1f ms/frame", GT.ns / 1e6 / GT.nf);
    if (frames && k < cap) k += (size_t)snprintf(o + k, cap - k, ", Present waits %.1f ms/frame", (GT.wait - GT.waitf) * 1e3 / frames);
    gp_report(o, cap, &k);
    GT.ns = GT.nf = 0; GT.waitf = GT.wait;
}
static void gl_present(Obj* bb) {
    GlCtx* C = gl_cur(); if (!C || !bb) return;
    g_primary = C;
    Img im; if (!img_of(C, bb, &im) || im.g->kind != 1) return;
    uint32_t w, h; img_size(&im, &w, &h);
    g_pframes++;
    if (g_fdump && (g_pframes == 1 || g_pframes % (uint64_t)g_dumpevery == 0)) {
        char path[1100]; snprintf(path, sizeof path, "%s/frame_%06llu.png", g_fdump, (unsigned long long)g_pframes);
        dump_image(C, &im, w, h, path);
        if (g_pframes == 1 || g_log) glog("frame %llu saved: %s\n", (unsigned long long)g_pframes, path);
    }
    Slot* sl = NULL;
    pthread_mutex_lock(&g_pmx);
    if (g_presenter && g_haswin) {
        for (int i = 0; i < 3; i++) if (g_slot[i].state == 0 && (!sl || g_slot[i].frame < sl->frame)) sl = &g_slot[i];
        if (!sl) for (int i = 0; i < 3; i++) if (g_slot[i].state == 1 && (!sl || g_slot[i].frame < sl->frame)) sl = &g_slot[i];
        if (sl) { if (sl->state == 1) g_dropped++; sl->state = 3; }
    }
    pthread_mutex_unlock(&g_pmx);
    if (sl) {
        if (sl->rfence) { glWaitSync(sl->rfence, 0, GL_TIMEOUT_IGNORED); glDeleteSync(sl->rfence); sl->rfence = NULL; }
        if (sl->wfence) { glDeleteSync(sl->wfence); sl->wfence = NULL; }
        if (!sl->tex || sl->w != (int)w || sl->h != (int)h) {
            if (sl->tex) glDeleteTextures(1, &sl->tex);
            glGenTextures(1, &sl->tex); s_scratch_tex(C, GL_TEXTURE_2D, sl->tex); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            sl->w = (int)w; sl->h = (int)h;
        }
        GlRes tg; memset(&tg, 0, sizeof tg); tg.kind = 1; tg.name = sl->tex; tg.target = GL_TEXTURE_2D;
        Obj tr; memset(&tr, 0, sizeof tr); tr.w = w; tr.h = h; tr.levels = 1;
        Img dst = { &tg, &tr, 0, 0 };
        scratch_begin(C, &im, &dst, GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(0, 0, (GLint)w, (GLint)h, 0, 0, (GLint)w, (GLint)h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        scratch_end(C, 1, 1, GL_COLOR_ATTACHMENT0);
        sl->wfence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        pthread_mutex_lock(&g_pmx); sl->state = 1; sl->frame = g_pframes; pthread_cond_broadcast(&g_pcv); pthread_mutex_unlock(&g_pmx);
    }
    /* keep the CPU at most two frames ahead of the GPU */
    GLsync f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (g_throttle[0]) { double tw = mono(); glClientWaitSync(g_throttle[0], GL_SYNC_FLUSH_COMMANDS_BIT, 250000000ull); GT.wait += mono() - tw; glDeleteSync(g_throttle[0]); }
    g_throttle[0] = g_throttle[1]; g_throttle[1] = f;
    gt_frame(C);
    glcheck("present");
}
static void set_view(int ww, int wh, int fw, int fh) {
    int vw = ww, vh = (int)((int64_t)ww * fh / (fw ? fw : 1));
    if (vh > wh) { vh = wh; vw = (int)((int64_t)wh * fw / (fh ? fh : 1)); }
    g_view[0] = (ww - vw) / 2; g_view[1] = (wh - vh) / 2; g_view[2] = vw; g_view[3] = vh; g_view[4] = fw; g_view[5] = fh;
}
/* window position -> frame (back buffer) position; returns 0 outside the picture */
int me1gl_get_view(int* x, int* y, int* w, int* h, int* fw, int* fh) {
    pthread_mutex_lock(&g_pmx);
    *x = g_view[0]; *y = g_view[1]; *w = g_view[2]; *h = g_view[3]; *fw = g_view[4]; *fh = g_view[5];
    pthread_mutex_unlock(&g_pmx);
    return *w > 0;
}
#ifndef __ANDROID__
/* v1.1: desktop windows. X11: the Window itself; Wayland: a wl_egl_window on the surface (libwayland-egl, dlopen),
 * resized by the presenter when the front end reports a new size */
static struct { void* (*create)(void*, int, int); void (*destroy)(void*); void (*resize)(void*, int, int, int, int); int tried; } WLE;
static void* g_wlwin; static int g_wlw, g_wlh; static Me1Window* g_pwin;
static int wle_load(void) {
    if (!WLE.tried) {
        WLE.tried = 1;
        void* h = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL); if (!h) h = dlopen("libwayland-egl.so", RTLD_NOW | RTLD_LOCAL);
        if (h) { *(void**)&WLE.create = dlsym(h, "wl_egl_window_create"); *(void**)&WLE.destroy = dlsym(h, "wl_egl_window_destroy"); *(void**)&WLE.resize = dlsym(h, "wl_egl_window_resize"); }
        if (!WLE.create || !WLE.destroy || !WLE.resize) glog("libwayland-egl not found: no Wayland window output\n");
    }
    return WLE.create && WLE.destroy && WLE.resize;
}
static void ws_release(void) { if (g_wlwin) { WLE.destroy(g_wlwin); g_wlwin = NULL; } g_pwin = NULL; }
static void ws_resize(void) {   /* presenter, before drawing: follow the window size (Wayland only; X11 surfaces track it) */
    Me1Window* w = g_pwin;
    if (!w || !g_wlwin) return;
    int ww = w->w, wh = w->h;
    if (ww > 0 && wh > 0 && (ww != g_wlw || wh != g_wlh)) { WLE.resize(g_wlwin, ww, wh, 0, 0); g_wlw = ww; g_wlh = wh; }
}
static EGLSurface ws_surface(Me1Window* w) {
    EGLSurface s = EGL_NO_SURFACE;
    ws_release();
    if (w->kind == ME1_WS_WAYLAND) {
        if (!wle_load() || !w->surface) return EGL_NO_SURFACE;
        g_wlw = w->w > 0 ? w->w : 1280; g_wlh = w->h > 0 ? w->h : 720;
        g_wlwin = WLE.create(w->surface, g_wlw, g_wlh);
        if (!g_wlwin) { glog("wl_egl_window_create failed\n"); return EGL_NO_SURFACE; }
        s = p_eglCreateWindowSurface(g_dpy, g_cfg, (EGLNativeWindowType)g_wlwin, NULL);
    } else s = p_eglCreateWindowSurface(g_dpy, g_cfg, (EGLNativeWindowType)(uintptr_t)w->xwindow, NULL);   /* legacy entry: the XID itself */
    if (s == EGL_NO_SURFACE) { glog("eglCreateWindowSurface (%s) failed: 0x%x\n", w->kind == ME1_WS_WAYLAND ? "Wayland" : "X11", p_eglGetError()); ws_release(); }
    else g_pwin = w;
    return s;
}
#endif
static EGLSurface win_surface(void* win, int* fake) {
    *fake = 0;
    if (!win) {
        if (!g_fakew) return EGL_NO_SURFACE;
        *fake = 1; return mk_pb(g_fakew, g_fakeh);
    }
#ifndef __ANDROID__
    if (g_ws_kind) return ws_surface((Me1Window*)win);
#endif
#ifdef __ANDROID__
    EGLint vid = 0; p_eglGetConfigAttrib(g_dpy, g_cfg, EGL_NATIVE_VISUAL_ID, &vid);
    typedef int32_t (*SBG)(void*, int32_t, int32_t, int32_t);
    static SBG sbg; if (!sbg) { void* h = dlopen("libandroid.so", RTLD_NOW); if (h) sbg = (SBG)dlsym(h, "ANativeWindow_setBuffersGeometry"); }
    if (sbg && vid) sbg(win, 0, 0, vid);
#endif
    EGLSurface s = p_eglCreateWindowSurface(g_dpy, g_cfg, (EGLNativeWindowType)win, NULL);
    if (s == EGL_NO_SURFACE) glog("eglCreateWindowSurface failed: 0x%x\n", p_eglGetError());
    return s;
}
/* CPU presentation (Mesa zink, ME1_GL_PRESENT=cpu): the frame is read back straight into the locked window buffer.
 * The buffer gets the window's aspect ratio (letterbox bars = black), the compositor scales it to the window. */
typedef struct { int32_t width, height, stride, format; void* bits; uint32_t reserved[6]; } AnwBuf;   /* = ANativeWindow_Buffer */
static struct {
    int32_t (*geom)(void*, int32_t, int32_t, int32_t); int32_t (*lock)(void*, AnwBuf*, void*); int32_t (*post)(void*);
    int32_t (*getw)(void*); int32_t (*geth)(void*);
} g_anw;
static uint64_t g_cpucopy_us;
/* tests: replace ANativeWindow_setBuffersGeometry / _lock / _unlockAndPost / _getWidth / _getHeight */
void me1gl_test_window_api(void* geom, void* lock, void* post, void* getw, void* geth) {
    g_anw.geom = geom; g_anw.lock = lock; g_anw.post = post; g_anw.getw = getw; g_anw.geth = geth;
}
static int anw_load(void) {
#ifdef __ANDROID__
    if (!g_anw.lock) {
        void* h = dlopen("libandroid.so", RTLD_NOW);
        if (h) {
            g_anw.geom = (__typeof__(g_anw.geom))dlsym(h, "ANativeWindow_setBuffersGeometry"); g_anw.lock = (__typeof__(g_anw.lock))dlsym(h, "ANativeWindow_lock");
            g_anw.post = (__typeof__(g_anw.post))dlsym(h, "ANativeWindow_unlockAndPost");
            g_anw.getw = (__typeof__(g_anw.getw))dlsym(h, "ANativeWindow_getWidth"); g_anw.geth = (__typeof__(g_anw.geth))dlsym(h, "ANativeWindow_getHeight");
        }
    }
#endif
    return g_anw.geom && g_anw.lock && g_anw.post && g_anw.getw && g_anw.geth;
}
static void cpu_present(void* win, const Slot* s, int ww, int wh, int* gw, int* gh, GLuint rfbo) {
    int fw = s->w, fh = s->h, bw = fw, bh = fh;
    if ((int64_t)ww * fh > (int64_t)wh * fw) bw = (int)(((int64_t)fh * ww + wh / 2) / wh); else bh = (int)(((int64_t)fw * wh + ww / 2) / ww);
    bw = (bw + 1) & ~1; if (bw < fw) bw = fw; if (bh < fh) bh = fh;
    if (bw != *gw || bh != *gh) { g_anw.geom(win, bw, bh, 2 /* WINDOW_FORMAT_RGBX_8888 */); *gw = bw; *gh = bh; }
    pthread_mutex_lock(&g_pmx); set_view(ww, wh, fw, fh); pthread_mutex_unlock(&g_pmx);
    AnwBuf b; memset(&b, 0, sizeof b);
    if (g_anw.lock(win, &b, NULL) != 0 || !b.bits) { static int k; if (k++ < 5) glog("presenter: ANativeWindow_lock failed\n"); return; }
    double t0 = mono();
    if (b.width >= fw && b.height >= fh && b.stride >= b.width) {
        int ox = (b.width - fw) / 2, oy = (b.height - fh) / 2, rx = b.width - ox - fw; uint8_t* px = b.bits; size_t st = (size_t)b.stride * 4;
        for (int y = 0; y < b.height; y++) {
            uint8_t* row = px + st * (size_t)y;
            if (y < oy || y >= oy + fh) memset(row, 0, (size_t)b.width * 4);
            else { if (ox) memset(row, 0, (size_t)ox * 4); if (rx > 0) memset(row + (size_t)(ox + fw) * 4, 0, (size_t)rx * 4); }
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, rfbo); glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ROW_LENGTH, b.stride);
        glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px + (size_t)oy * st + (size_t)ox * 4);   /* row 0 = top of the frame */
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    } else { static int k; if (k++ < 5) glog("presenter: window buffer %dx%d (stride %d) smaller than the frame %dx%d\n", b.width, b.height, b.stride, fw, fh); }
    g_cpucopy_us += (uint64_t)((mono() - t0) * 1e6);
    g_anw.post(win);
}
static void* presenter_main(void* arg) {
    (void)arg;
    EGLContext ctx = mk_ctx(g_ctx0);
    if (ctx == EGL_NO_CONTEXT) { glog("presenter: eglCreateContext failed 0x%x\n", p_eglGetError()); return NULL; }
    EGLSurface surf = EGL_NO_SURFACE; int gen = -1, fake = 0, sw = 0, sh = 0; GLuint rfbo = 0; uint64_t n = 0;
    void* cwin = NULL; EGLSurface cpb = EGL_NO_SURFACE; int ww = 0, wh = 0, gw = -1, gh = -1;   /* CPU presentation */
    for (;;) {
        pthread_mutex_lock(&g_pmx);
        int ready = -1;
        for (;;) {
            if (g_win_gen != gen) break;
            for (int i = 0; i < 3; i++) if (g_slot[i].state == 1 && (ready < 0 || g_slot[i].frame > g_slot[ready].frame)) ready = i;
            if (ready >= 0) break;
            pthread_cond_wait(&g_pcv, &g_pmx);
        }
        if (g_win_gen != gen) {
            gen = g_win_gen; void* w = g_win_pending; g_haswin = 0;
            pthread_mutex_unlock(&g_pmx);
            if (surf != EGL_NO_SURFACE) { p_eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); p_eglDestroySurface(g_dpy, surf); surf = EGL_NO_SURFACE; }
#ifndef __ANDROID__
            if (!fake) ws_release();
#endif
            int ok = 0;
            if (g_cpupresent) {   /* no EGL window surface: lock the window and copy */
                cwin = NULL; gw = gh = -1;
                if (w && anw_load()) { g_anw.geom(w, 0, 0, 2); ww = g_anw.getw(w); wh = g_anw.geth(w); if (ww > 0 && wh > 0) cwin = w; }
                if (cwin && cpb == EGL_NO_SURFACE && !g_surfless) cpb = mk_pb(16, 16);
                if (cwin && p_eglMakeCurrent(g_dpy, cpb, cpb, ctx)) {
                    ok = 1; if (!rfbo) glGenFramebuffers(1, &rfbo);
                    glog("presenter: window %dx%d, CPU copy (%s)\n", ww, wh, g_zink ? "Mesa zink" : "ME1_GL_PRESENT=cpu");
                } else if (w) glog("presenter: CPU copy impossible for window %p (%s)\n", w, anw_load() ? "eglMakeCurrent failed" : "no ANativeWindow API");
                pthread_mutex_lock(&g_pmx);
                g_haswin = ok; g_win_ack = gen;
                if (!ok) for (int i = 0; i < 3; i++) if (g_slot[i].state == 1) g_slot[i].state = 0;
                pthread_cond_broadcast(&g_pcv); pthread_mutex_unlock(&g_pmx);
                continue;
            }
            surf = win_surface(w, &fake);
            if (surf != EGL_NO_SURFACE && p_eglMakeCurrent(g_dpy, surf, surf, ctx)) {
                ok = 1; { const char* vs = getenv("ME1_VSYNC"); p_eglSwapInterval(g_dpy, fake ? 0 : vs && *vs ? atoi(vs) : 1); }
                p_eglQuerySurface(g_dpy, surf, EGL_WIDTH, &sw); p_eglQuerySurface(g_dpy, surf, EGL_HEIGHT, &sh);
                if (!rfbo) glGenFramebuffers(1, &rfbo);
                glog("presenter: %s %dx%d\n", fake ? "fake window (pbuffer)" : "window", sw, sh);
            } else if (w || fake) glog("presenter: no surface for window %p (0x%x)\n", w, p_eglGetError());
            pthread_mutex_lock(&g_pmx);
            g_haswin = ok; g_win_ack = gen;
            if (!ok) for (int i = 0; i < 3; i++) if (g_slot[i].state == 1) g_slot[i].state = 0;
            pthread_cond_broadcast(&g_pcv); pthread_mutex_unlock(&g_pmx);
            continue;
        }
        for (int i = 0; i < 3; i++) if (i != ready && g_slot[i].state == 1) { g_slot[i].state = 0; g_dropped++; }
        Slot* s = &g_slot[ready]; s->state = 2;
        pthread_mutex_unlock(&g_pmx);
        GLsync rf = NULL;
        if (g_cpupresent && cwin) {   /* synchronous read-back: the slot is free again when this returns */
            if (s->wfence) glWaitSync(s->wfence, 0, GL_TIMEOUT_IGNORED);
            cpu_present(cwin, s, ww, wh, &gw, &gh, rfbo); n++;
        } else if (surf != EGL_NO_SURFACE) {
            if (s->wfence) glWaitSync(s->wfence, 0, GL_TIMEOUT_IGNORED);
#ifndef __ANDROID__
            if (!fake) ws_resize();
#endif
            if (!fake) { p_eglQuerySurface(g_dpy, surf, EGL_WIDTH, &sw); p_eglQuerySurface(g_dpy, surf, EGL_HEIGHT, &sh); }
            pthread_mutex_lock(&g_pmx); set_view(sw, sh, s->w, s->h); int vx = g_view[0], vy = g_view[1], vw = g_view[2], vh = g_view[3]; pthread_mutex_unlock(&g_pmx);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1);
            glViewport(0, 0, sw, sh); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, rfbo); glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
            /* frame row 0 is the top: window y grows upwards -> flip */
            int y0 = sh - vy - vh;
            glBlitFramebuffer(0, 0, s->w, s->h, vx, y0 + vh, vx + vw, y0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            rf = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            n++;
            if (fake && g_fdump && (n == 1 || n % (uint64_t)g_dumpevery == 0)) {
                uint8_t* buf = malloc((size_t)sw * sh * 4);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
                char path[1100]; snprintf(path, sizeof path, "%s/win_%06llu.png", g_fdump, (unsigned long long)n);
                png_write(path, buf, sw, sh, 1); free(buf);
            }
            if (!p_eglSwapBuffers(g_dpy, surf)) { static int k; if (k++ < 5) glog("eglSwapBuffers failed: 0x%x\n", p_eglGetError()); }
            if (g_gldbg) { GLenum e = glGetError(); if (e) glog("presenter: GL error 0x%04x\n", e); }
        }
        pthread_mutex_lock(&g_pmx);
        if (s->rfence) glDeleteSync(s->rfence);
        s->rfence = rf; s->state = 0; g_shown++;
        pthread_cond_broadcast(&g_pcv); pthread_mutex_unlock(&g_pmx);
    }
    return NULL;
}
static void presenter_start(void) {
    const char* e = getenv("ME1_GL_FAKEWIN");
    if (e && sscanf(e, "%dx%d", &g_fakew, &g_fakeh) == 2 && g_fakew > 0 && g_fakeh > 0) {}
    else g_fakew = g_fakeh = 0;
#ifndef __ANDROID__
    if (!g_fakew && !g_cpupresent && !g_ws_kind) return;   /* v1.1: desktop window -> presenter */
#endif
    if (!g_winbit && !g_fakew && !g_cpupresent) { glog("EGL config without window support: no presenter\n"); return; }
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_mutex_lock(&g_pmx);
    if (pthread_create(&t, &a, presenter_main, NULL) == 0) { g_presenter = 1; if (g_fakew) g_win_gen++; }
    pthread_cond_broadcast(&g_pcv);
    pthread_mutex_unlock(&g_pmx);
}
/* Android: the activity's surface (ANativeWindow*), NULL when it is destroyed; waits until the presenter switched */
void me1gl_set_window(void* anw) {
    pthread_mutex_lock(&g_pmx);
    g_win_pending = anw; int gen = ++g_win_gen;
    pthread_cond_broadcast(&g_pcv);
    if (g_presenter) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 3;
        while (g_win_ack < gen) if (pthread_cond_timedwait(&g_pcv, &g_pmx, &ts) == ETIMEDOUT) { glog("me1gl_set_window: presenter did not answer in 3 s\n"); break; }
    }
    pthread_mutex_unlock(&g_pmx);
}

/* ---- object lifetime, statistics ---- */
static void forget_name(int tex, GLuint name) {
    for (int i = 0; i < g_nctx; i++) {
        GlCtx* X = g_ctxs[i];
        if (tex) { for (int u = 0; u < 24; u++) if (X->tex[u] == name) X->tex[u] = 0xFFFFFFFFu; }
        else { if (X->st[S_ABUF] == name) X->st[S_ABUF] = 0xEEEEEEEEu; if (X->st[S_EBUF] == name) X->st[S_EBUF] = 0xEEEEEEEEu; }
    }
}
static void gl_free(Obj* o) {
    if (!o->gx) return;
    GlCtx* C = gl_cur();
    if (o->type == T_VS || o->type == T_PS) {
        GlSh* s = o->gx;
        if (C) prog_drop(s->serial);
        sm3_free(&s->s); free(s);
    } else {
        GlRes* g = o->gx;
        if (C && g->name) {
            if (g->kind == 1 || g->kind == 2) { fbo_purge(C, g->serial); stale_all(g->serial); }
            if (g->kind == 1) { forget_name(1, g->name); glDeleteTextures(1, &g->name); }
            else if (g->kind == 2) glDeleteRenderbuffers(1, &g->name);
            else if (g->kind == 3) { forget_name(0, g->name); glDeleteBuffers(1, &g->name); }
        }
        GS.texmem -= g->bytes;
        free(g);
    }
    o->gx = NULL;
}
static void gl_report(void) {
    if (g_gl != 1) return;
    glog("report: %llu draws (%llu skipped, %llu pre-transformed), %llu clears, %llu blits, %llu programs (%llu from cache, %llu failed), %llu shaders translated (%llu not), "
         "%llu framebuffers, %llu texture uploads (%llu MB, %llu CPU-decoded), %llu buffer uploads (%llu MB), %llu MB streamed, %llu MB GPU memory; "
         "%llu frames, %llu shown, %llu dropped, %d contexts\n",
         (unsigned long long)GS.draws, (unsigned long long)GS.skipped, (unsigned long long)GS.rhw, (unsigned long long)GS.clears, (unsigned long long)GS.blits,
         (unsigned long long)GS.progs, (unsigned long long)GS.progbin, (unsigned long long)GS.progfail, (unsigned long long)GS.shtrans, (unsigned long long)GS.shfail,
         (unsigned long long)GS.fbos, (unsigned long long)GS.texup, (unsigned long long)(GS.texbytes >> 20), (unsigned long long)GS.decoded,
         (unsigned long long)GS.bufup, (unsigned long long)(GS.bufbytes >> 20), (unsigned long long)(GS.ring >> 20), (unsigned long long)(GS.texmem >> 20),
         (unsigned long long)g_pframes, (unsigned long long)g_shown, (unsigned long long)g_dropped, g_nctx);
    if (g_cpupresent && g_shown) glog("CPU present: %.2f ms per frame (read-back + copy)\n", g_cpucopy_us / 1e3 / (double)g_shown);
}
/* tests: the "[d3d] frame" timing suffix; 1 if the GPU timer query is available */
int me1gl_frame_times(char* o, size_t cap, uint64_t frames) { gl_frame_times(o, cap, frames); return GT.begin != NULL; }
/* counters for tests: draws, skipped draws, programs linked, failed programs, shaders not translated */
void me1gl_stats(uint64_t out[5]) { out[0] = GS.draws; out[1] = GS.skipped; out[2] = GS.progs; out[3] = GS.progfail; out[4] = GS.shfail; }
/* GameActivity's start-up / shader compilation screen (polled a few times a second from the UI thread; plain counters,
 * torn reads are harmless): [0] frames presented, [1] D3DXCompileShader calls, [2] of them from the HLSL cache,
 * [3] a compile is running, [4] its duration in ms, [5] GL programs linked, [6] of them from the binary cache,
 * [7] shaders translated to GLSL, [8] distinct D3D shaders created, [9] ms since the last GL program was built (-1: none) */
void me1_status(int64_t o[10]) {
    double t = mono();
    pthread_mutex_lock(&g_dxmx);
    o[1] = (int64_t)g_dxs[DX_COMPILE].n; o[2] = (int64_t)g_dxs[DX_COMPILE].hits;
    o[3] = g_dxc.active; o[4] = g_dxc.active ? (int64_t)((t - g_dxc.t0) * 1e3) : 0;
    pthread_mutex_unlock(&g_dxmx);
    o[0] = (int64_t)g_st.frames; o[5] = (int64_t)GS.progs; o[6] = (int64_t)GS.progbin; o[7] = (int64_t)GS.shtrans;
    o[8] = (int64_t)g_nshcrc; o[9] = GS.tbuilt > 0 ? (int64_t)((t - GS.tbuilt) * 1e3) : -1;
}
