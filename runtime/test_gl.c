/* Rendering test of the GLES backend (hle_d3d9_gl.h): draws through the guest D3D9 vtables (as the game does) and reads
 * the pixels back with GetRenderTargetData. Runs on any EGL/GLES 3 driver (host: Mesa llvmpipe, surfaceless).
 * usage: test_gl      env: ME1_GL_NOS3TC / ME1_GL_NORGTC (CPU decode paths), ME1_FRAME_DUMP=dir (Present -> PNG) */
#include "rt.h"
#include "hle.h"
#include "sm3_glsl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>

static int g_fail, g_ok;
#define CHECK(cond, ...) do { if (cond) g_ok++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static uint32_t call(uint32_t t, int n, const uint32_t* a, const char* what) {
    uint32_t sp = g_cpu.esp, r = rt_call(&g_cpu, t, n, a);
    if (g_cpu.esp != sp) { fprintf(stderr, "FAIL: %s: esp %08x != %08x (wrong arg count)\n", what, g_cpu.esp, sp); g_fail++; g_cpu.esp = sp; }
    return r;
}
static uint32_t api(const char* dll, const char* name, int n, ...) {
    uint32_t t = hle_resolve_impl(dll, name), a[24]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    if (!t) { fprintf(stderr, "FAIL: %s!%s not implemented\n", dll, name); g_fail++; return 0; }
    return call(t, n, a, name);
}
static uint32_t com(uint32_t o, int k, int n, ...) {
    uint32_t a[24]; va_list ap; va_start(ap, n);
    a[0] = o; for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t vt = rd32(o), t = rd32(vt + 4 * k);
    if (!hle_is_thunk(t)) { fprintf(stderr, "FAIL: vtable slot %d of %08x is %08x\n", k, o, t); g_fail++; return 0xFFFFFFFFu; }
    char w[32]; snprintf(w, sizeof w, "method %d", k);
    return call(t, n + 1, a, w);
}
static uint32_t gbuf(uint32_t n) { uint32_t a = ghalloc(n, 1); memset(M + a, 0, n); return a; }
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void wrfl(uint32_t a, float f) { wr32(a, fb(f)); }

/* ---- tiny D3D9 shader assembler ---- */
enum { R_TEMP = 0, R_IN = 1, R_CONST = 2, R_TEX = 3, R_OUT = 6, R_COLOROUT = 8, R_SAMP = 10 };
#define RTY(t) ((((uint32_t)(t) & 7) << 28) | ((((uint32_t)(t) >> 3) & 3) << 11))
#define DST(t, n) (0x800F0000u | RTY(t) | (n))
#define DSTM(t, n, m) (0x80000000u | ((uint32_t)(m) << 16) | RTY(t) | (n))
#define SRC(t, n) (0x80E40000u | RTY(t) | (n))
#define SRCS(t, n, s) (0x80000000u | ((uint32_t)(s) << 16) | RTY(t) | (n))
#define OP(o, len) ((uint32_t)(o) | ((uint32_t)(len) << 24))
#define DCL(usage, idx, t, n) OP(0x1F, 2), 0x80000000u | (usage) | ((uint32_t)(idx) << 16), DST(t, n)
#define DCL2D(n) OP(0x1F, 2), 0x90000000u, DST(R_SAMP, n)
#define MOV(d, s) OP(0x01, 2), d, s
#define ADD(d, a, b) OP(0x02, 3), d, a, b
#define MUL(d, a, b) OP(0x05, 3), d, a, b
#define TEXLD(d, c, s) OP(0x42, 3), d, c, s
#define END 0x0000FFFFu
enum { U_POS = 0, U_TC = 5, U_COLOR = 10 };

static uint32_t dev, sysrb, out;
static uint32_t pix[64 * 64];
static uint32_t shader(int k, const uint32_t* t, int n) {
    uint32_t c = gbuf(4 * n); for (int i = 0; i < n; i++) wr32(c + 4 * i, t[i]);
    CHECK(com(dev, k, 2, c, out) == 0 && rd32(out), "create shader (%s)", k == 91 ? "vs" : "ps");
    return rd32(out);
}
/* read a 64x64 render target back into pix[] (0xAARRGGBB) */
static void grab(uint32_t rt) {
    memset(pix, 0xCD, sizeof pix);
    uint32_t hr = com(dev, 32, 2, rt, sysrb);
    CHECK(hr == 0, "GetRenderTargetData -> %08x", hr);
    uint32_t lr = gbuf(8);
    CHECK(com(sysrb, 13, 3, lr, 0u, 0x10u) == 0, "LockRect readback surface");
    uint32_t pitch = rd32(lr), bits = rd32(lr + 4);
    for (int y = 0; y < 64; y++) memcpy(pix + 64 * y, M + bits + pitch * y, 256);
    com(sysrb, 14, 0);
}
static int near(uint32_t a, uint32_t b, int tol, int alpha) {
    for (int i = 0; i < (alpha ? 32 : 24); i += 8) { int x = (int)(a >> i & 255), y = (int)(b >> i & 255); if (abs(x - y) > tol) return 0; }
    return 1;
}
#define PX(x, y) pix[(y) * 64 + (x)]
#define EXPECT(x, y, col, what) do { uint32_t p_ = PX(x, y); CHECK(near(p_, (col), 2, 0), "%s: pixel (%d,%d) = %08x, expected %08x", what, x, y, p_, (uint32_t)(col)); } while (0)
/* quadrant check: TL, TR, BL, BR */
static void quads(uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br, const char* what) {
    EXPECT(8, 8, tl, what); EXPECT(55, 8, tr, what); EXPECT(8, 55, bl, what); EXPECT(55, 55, br, what);
    EXPECT(30, 30, tl, what); EXPECT(33, 30, tr, what); EXPECT(30, 33, bl, what); EXPECT(33, 33, br, what);
}

/* vertex: float3 pos, float2 uv, D3DCOLOR (24 bytes) */
static uint32_t vtx;
static void quad_at(uint32_t at, float x0, float y0, float x1, float y1, float z, uint32_t col) {
    float v[4][5] = { { x0, y0, z, 0, 0 }, { x1, y0, z, 1, 0 }, { x0, y1, z, 0, 1 }, { x1, y1, z, 1, 1 } };   /* strip order */
    for (int i = 0; i < 4; i++) { for (int k = 0; k < 5; k++) wrfl(at + 24 * i + 4 * k, v[i][k]); wr32(at + 24 * i + 20, col); }
}
static void quad(float x0, float y0, float x1, float y1, float z, uint32_t col) {   /* clip coordinates: y0 top */
    quad_at(vtx, x0, y0, x1, y1, z, col);
    CHECK(com(dev, 83, 4, 5u, 2u, vtx, 24u) == 0, "DrawPrimitiveUP");
}
static void full(uint32_t col) { quad(-1, 1, 1, -1, 0.5f, col); }
static void rs(uint32_t s, uint32_t v) { com(dev, 57, 2, s, v); }
static void clear(uint32_t col) { CHECK(com(dev, 43, 6, 0u, 0u, 7u, col, fb(1.0f), 0u) == 0, "Clear"); }
static uint32_t tex(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
    uint32_t hr = com(dev, 23, 8, w, h, levels, usage, fmt, pool, out, 0u);
    CHECK(hr == 0 && rd32(out), "CreateTexture %ux%u fmt %08x -> %08x", w, h, fmt, hr);
    return rd32(out);
}
static void fill(uint32_t t, uint32_t level, const void* data, uint32_t rowbytes, uint32_t rows) {
    uint32_t lr = gbuf(8);
    CHECK(com(t, 19, 4, level, lr, 0u, 0u) == 0, "LockRect level %u", level);
    for (uint32_t y = 0; y < rows; y++) memcpy(M + rd32(lr + 4) + rd32(lr) * y, (const uint8_t*)data + rowbytes * y, rowbytes);
    CHECK(com(t, 20, 1, level) == 0, "UnlockRect");
}
static void point(int s) { com(dev, 69, 3, (uint32_t)s, 5u, 1u); com(dev, 69, 3, (uint32_t)s, 6u, 1u); com(dev, 69, 3, (uint32_t)s, 7u, 0u); }

/* --cpu-present: a fake ANativeWindow (200x100) for the presenter's CPU copy path (Mesa zink on Android) */
typedef struct { int32_t width, height, stride, format; void* bits; uint32_t reserved[6]; } TAnwBuf;
static uint8_t *fw_px, *fw_last; static int fw_gw, fw_gh, fw_fmt, fw_posts, fw_lw, fw_lh, fw_ls;
static int32_t fw_geom(void* w, int32_t a, int32_t b, int32_t f) { (void)w; fw_gw = a; fw_gh = b; fw_fmt = f; return 0; }
static int32_t fw_getw(void* w) { (void)w; return fw_gw ? fw_gw : 200; }
static int32_t fw_geth(void* w) { (void)w; return fw_gh ? fw_gh : 100; }
static int32_t fw_lock(void* w, TAnwBuf* b, void* r) {
    (void)r; b->width = fw_getw(w); b->height = fw_geth(w); b->stride = b->width + 8; b->format = fw_fmt;
    free(fw_px); fw_px = malloc((size_t)b->stride * b->height * 4); memset(fw_px, 0x55, (size_t)b->stride * b->height * 4);   /* garbage */
    b->bits = fw_px; fw_lw = b->width; fw_lh = b->height; fw_ls = b->stride; return 0;
}
static int32_t fw_post(void* w) { (void)w; free(fw_last); fw_last = fw_px; fw_px = NULL; fw_posts++; return 0; }
static const uint8_t* fw_at(int x, int y) { return fw_last + ((size_t)y * fw_ls + x) * 4; }
void me1gl_test_window_api(void* geom, void* lock, void* post, void* getw, void* geth);
void me1gl_set_window(void* anw);
int me1gl_get_view(int* x, int* y, int* w, int* h, int* fw, int* fh);

int main(int argc, char** argv) {
    rt_init();
    setenv("ME1_SCREEN", "1280x720", 0);
    int cpu = argc > 1 && !strcmp(argv[1], "--cpu-present");
    if (cpu) { setenv("ME1_GL_PRESENT", "cpu", 1); me1gl_test_window_api((void*)fw_geom, (void*)fw_lock, (void*)fw_post, (void*)fw_getw, (void*)fw_geth); }
    out = gbuf(64); vtx = gbuf(24 * 64);
    uint32_t d3d = api("d3d9.dll", "Direct3DCreate9", 1, 32u);
    uint32_t pp = gbuf(56);
    wr32(pp, 64); wr32(pp + 4, 64); wr32(pp + 8, 22); wr32(pp + 12, 1); wr32(pp + 24, 1); wr32(pp + 28, 0x10000); wr32(pp + 32, 1);
    wr32(pp + 36, 1); wr32(pp + 40, 75);
    CHECK(com(d3d, 16, 6, 0u, 1u, 0x10000u, 0x40u, pp, out) == 0 && rd32(out), "CreateDevice");
    dev = rd32(out);
    if (cpu) me1gl_set_window((void*)&fw_px);
    CHECK(com(dev, 18, 4, 0u, 0u, 0u, out) == 0, "GetBackBuffer"); uint32_t bb = rd32(out);
    CHECK(com(dev, 36, 6, 64u, 64u, 21u, 2u, out, 0u) == 0, "CreateOffscreenPlainSurface SYSTEMMEM"); sysrb = rd32(out);

    /* shaders */
    static const uint32_t vs_t[] = { 0xFFFE0300u, DCL(U_POS, 0, R_IN, 0), DCL(U_TC, 0, R_IN, 1), DCL(U_COLOR, 0, R_IN, 2),
        DCL(U_POS, 0, R_OUT, 0), DCL(U_TC, 0, R_OUT, 1), DCL(U_COLOR, 0, R_OUT, 2),
        ADD(DST(R_OUT, 0), SRC(R_IN, 0), SRC(R_CONST, 0)), MOV(DST(R_OUT, 1), SRC(R_IN, 1)), MOV(DST(R_OUT, 2), SRC(R_IN, 2)), END };
    static const uint32_t ps_tex[] = { 0xFFFF0300u, DCL(U_TC, 0, R_IN, 0), DCL(U_COLOR, 0, R_IN, 1), DCL2D(0),
        TEXLD(DST(R_TEMP, 0), SRC(R_IN, 0), SRC(R_SAMP, 0)), MUL(DST(R_COLOROUT, 0), SRC(R_TEMP, 0), SRC(R_IN, 1)), END };
    static const uint32_t ps_col[] = { 0xFFFF0300u, DCL(U_COLOR, 0, R_IN, 0), MOV(DST(R_COLOROUT, 0), SRC(R_IN, 0)), END };
    static const uint32_t ps_c0[] = { 0xFFFF0300u, MOV(DST(R_COLOROUT, 0), SRC(R_CONST, 0)), END };
    static const uint32_t ps2_tex[] = { 0xFFFF0200u, OP(0x1F, 2), 0x80000000u, DSTM(R_TEX, 0, 3), DCL2D(0),
        TEXLD(DST(R_TEMP, 0), SRC(R_TEX, 0), SRC(R_SAMP, 0)), MOV(DST(R_COLOROUT, 0), SRC(R_TEMP, 0)), END };
    uint32_t vs = shader(91, vs_t, sizeof vs_t / 4), pst = shader(106, ps_tex, sizeof ps_tex / 4);
    uint32_t psc = shader(106, ps_col, sizeof ps_col / 4), ps0 = shader(106, ps_c0, sizeof ps_c0 / 4);
    uint32_t ps2 = shader(106, ps2_tex, sizeof ps2_tex / 4);
    uint32_t de = gbuf(32);
    static const uint8_t decl[32] = { 0,0, 0,0, 2,0,0,0,   0,0, 12,0, 1,0,5,0,   0,0, 20,0, 4,0,10,0,   0xFF,0, 0,0, 17,0,0,0 };
    memcpy(M + de, decl, 32);
    CHECK(com(dev, 86, 2, de, out) == 0, "CreateVertexDeclaration"); uint32_t vd = rd32(out);
    uint32_t c0 = gbuf(16);
    com(dev, 87, 1, vd); com(dev, 92, 1, vs); com(dev, 94, 3, 0u, c0, 1u);
    rs(22, 1);   /* CULL_NONE */
    rs(7, 0);    /* ZENABLE off */
    com(dev, 41, 0);

    /* 1. clears */
    clear(0xFFFF0000u); grab(bb); quads(0xFF0000, 0xFF0000, 0xFF0000, 0xFF0000, "clear red");
    uint32_t rc = gbuf(16); wr32(rc, 0); wr32(rc + 4, 0); wr32(rc + 8, 32); wr32(rc + 12, 32);
    CHECK(com(dev, 43, 6, 1u, rc, 1u, 0xFF0000FFu, fb(1.0f), 0u) == 0, "Clear rect");
    grab(bb); quads(0x0000FF, 0xFF0000, 0xFF0000, 0xFF0000, "clear rect");

    /* 2. vertex color (D3DCOLOR -> bgra swizzle), semantic matching, UP draw */
    com(dev, 107, 1, psc);
    clear(0xFF000000u); full(0xFF102030u); grab(bb); quads(0x102030, 0x102030, 0x102030, 0x102030, "vertex color");
    clear(0xFF000000u); quad(-1, 1, 0, -1, 0.5f, 0xFF00FF00u); grab(bb); quads(0x00FF00, 0, 0x00FF00, 0, "left half");
    clear(0xFF000000u); quad(-1, 1, 1, 0, 0.5f, 0xFFFFFF00u); grab(bb); quads(0xFFFF00, 0xFFFF00, 0, 0, "top half (y up = top)");
    /* vs constant: shift right by 1 (half screen) */
    wrfl(c0, 1.0f); com(dev, 94, 3, 0u, c0, 1u);
    clear(0xFF000000u); quad(-1, 1, 0, -1, 0.5f, 0xFF00FF00u); grab(bb); quads(0, 0x00FF00, 0, 0x00FF00, "vs constant c0");
    wrfl(c0, 0.0f); com(dev, 94, 3, 0u, c0, 1u);
    /* ps constant */
    com(dev, 107, 1, ps0); uint32_t pc = gbuf(16); wrfl(pc, 0.25f); wrfl(pc + 4, 0.5f); wrfl(pc + 8, 1.0f); wrfl(pc + 12, 1.0f);
    com(dev, 109, 3, 0u, pc, 1u); clear(0xFF000000u); full(0); grab(bb); quads(0x4080FF, 0x4080FF, 0x4080FF, 0x4080FF, "ps constant");
    {   /* D3D9 float semantics (sm3_float_emu): exp(0 * log 0) = 1, 0 * rcp(0) = 0, nrm(0) = 0, pow(0, 0) = 1 */
        static const uint32_t ps_fe[] = { 0xFFFF0300u,
            OP(0x0F, 2), DST(R_TEMP, 0), SRCS(R_CONST, 0, 0x00),                 /* log r0, c0.x   (-inf)     */
            MUL(DST(R_TEMP, 0), SRC(R_TEMP, 0), SRC(R_CONST, 0)),               /* r0 = -inf * 0             */
            OP(0x0E, 2), DST(R_TEMP, 1), SRCS(R_TEMP, 0, 0x00),                 /* exp r1, r0.x   (1)        */
            OP(0x06, 2), DST(R_TEMP, 2), SRCS(R_CONST, 0, 0x00),                /* rcp r2, c0.x   (inf)      */
            MUL(DST(R_TEMP, 2), SRC(R_TEMP, 2), SRC(R_CONST, 0)),               /* 0                         */
            OP(0x24, 2), DST(R_TEMP, 3), SRC(R_CONST, 0),                       /* nrm r3, c0     (0)        */
            OP(0x20, 3), DST(R_TEMP, 4), SRCS(R_CONST, 0, 0x00), SRCS(R_CONST, 0, 0x00),   /* pow r4, 0, 0 (1) */
            ADD(DST(R_TEMP, 1), SRC(R_TEMP, 1), SRC(R_TEMP, 2)),
            ADD(DST(R_TEMP, 1), SRC(R_TEMP, 1), SRC(R_TEMP, 3)),
            MUL(DST(R_COLOROUT, 0), SRC(R_TEMP, 1), SRC(R_TEMP, 4)), END };
        uint32_t psf = shader(106, ps_fe, sizeof ps_fe / 4);
        com(dev, 107, 1, psf); for (int k = 0; k < 4; k++) wrfl(pc + 4 * k, 0.0f); com(dev, 109, 3, 0u, pc, 1u);
        clear(0xFF000000u); full(0); grab(bb);
        if (sm3_float_emu) quads(0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, "D3D9 float semantics (0 * inf = 0)");
        com(dev, 107, 1, ps0);
    }
    {   /* v0.9.9 simplified shaders (sm3_lite): temps written only by _pp instructions -> mediump, _pp inputs -> mediump
         * varyings, cheap sRGB; the picture stays the same (the host GL computes mediump at full precision anyway) */
#define PP 0x00200000u
        static const uint32_t ps_pp[] = { 0xFFFF0300u, OP(0x1F, 2), 0x80000000u | U_COLOR, DST(R_IN, 0) | PP, DCL2D(0),
            MUL(DST(R_TEMP, 0) | PP, SRC(R_CONST, 0), SRC(R_CONST, 0)),     /* mul_pp r0, c0, c0  -> mediump r0     */
            ADD(DST(R_TEMP, 1), SRC(R_CONST, 0), SRC(R_CONST, 0)),          /* add r1 (full)                        */
            MUL(DST(R_TEMP, 1) | PP, SRC(R_TEMP, 1), SRC(R_CONST, 0)),      /* mul_pp r1: mixed -> stays highp      */
            TEXLD(DST(R_TEMP, 2) | PP, SRC(R_CONST, 0), SRC(R_SAMP, 0)),     /* texld_pp r2 (no texture: 0,0,0,1)    */
            MUL(DST(R_TEMP, 2) | PP, SRC(R_TEMP, 2), SRC(R_IN, 0)),
            ADD(DST(R_TEMP, 0) | PP, SRC(R_TEMP, 0), SRC(R_TEMP, 2)),
            ADD(DST(R_COLOROUT, 0), SRC(R_TEMP, 0), SRC(R_TEMP, 1)), END };
        Sm3 t; sm3_lite = 1;
        int tr = sm3_translate(ps_pp, sizeof ps_pp / 4, &t);
        char* g = tr ? NULL : sm3_glsl_ps(&t);
        CHECK(!tr && g && strstr(g, "mediump vec4 r0 ") && !strstr(g, "mediump vec4 r1 ") && strstr(g, "mediump vec4 r2 ") && t.npp == 2
              && strstr(g, "in mediump vec4 V_COLOR0") && strstr(g, "c.rgb * c.rgb") && strstr(g, "sqrt(max(oC0.rgb"),
              "lite translation: _pp temps/inputs mediump, mixed highp, cheap sRGB\n%s", g ? g : t.err);
        free(g); sm3_free(&t);
        uint64_t st0[5], st1[5]; me1gl_stats(st0);
        uint32_t psl = shader(106, ps_pp, sizeof ps_pp / 4);      /* translated (lazily) while sm3_lite = 1 */
        com(dev, 107, 1, psl); wrfl(pc, 0.25f); wrfl(pc + 4, 0.5f); wrfl(pc + 8, 1.0f); wrfl(pc + 12, 1.0f); com(dev, 109, 3, 0u, pc, 1u);
        clear(0xFF000000u); full(0xFF808080u); grab(bb); me1gl_stats(st1);
        /* r0 = c0^2 = (.0625, .25, 1, 1), r1 = 2 c0^2 = (.125, .5, 2, 2), r2 = 0 -> (.1875, .75, 1) */
        quads(0x30BFFF, 0x30BFFF, 0x30BFFF, 0x30BFFF, "simplified shaders (lite) render the same");
        CHECK(st1[3] == st0[3] && st1[2] > st0[2], "lite program linked (programs %llu -> %llu, failed %llu -> %llu)",
              (unsigned long long)st0[2], (unsigned long long)st1[2], (unsigned long long)st0[3], (unsigned long long)st1[3]);
        sm3_lite = 0;
        uint32_t psn = shader(106, ps_pp, sizeof ps_pp / 4);
        com(dev, 107, 1, psn); clear(0xFF000000u); full(0xFF808080u); grab(bb);
        quads(0x30BFFF, 0x30BFFF, 0x30BFFF, 0x30BFFF, "same _pp shader, default precision");
        tr = sm3_translate(ps_pp, sizeof ps_pp / 4, &t); g = tr ? NULL : sm3_glsl_ps(&t);
        CHECK(g && !strstr(g, "mediump") && strstr(g, "pow(max(c.rgb"), "default mode: no mediump, exact sRGB");
        free(g); sm3_free(&t);
        com(dev, 107, 1, ps0);
#undef PP
    }

    /* 3. vertex buffer + DrawPrimitive, index buffer + DrawIndexedPrimitive (base vertex) */
    com(dev, 107, 1, psc);
    CHECK(com(dev, 26, 6, 24u * 16, 0x208u, 0u, 0u, out, 0u) == 0, "CreateVertexBuffer"); uint32_t vb = rd32(out);
    uint32_t pv = gbuf(4);
    CHECK(com(vb, 11, 4, 0u, 0u, pv, 0x2000u) == 0, "VB Lock DISCARD");
    quad_at(rd32(pv), -1, 1, 0, -1, 0.5f, 0xFFFF0000u);           /* verts 0-3: left half red */
    quad_at(rd32(pv) + 96, 0, 1, 1, -1, 0.5f, 0xFF0000FFu);       /* verts 4-7: right half blue */
    CHECK(com(vb, 12, 0) == 0, "VB Unlock");
    com(dev, 100, 4, 0u, vb, 0u, 24u);
    clear(0xFF000000u); CHECK(com(dev, 81, 3, 5u, 4u, 2u) == 0, "DrawPrimitive"); grab(bb); quads(0, 0xFF, 0, 0xFF, "DrawPrimitive start 4");
    CHECK(com(dev, 27, 6, 64u, 8u, 101u, 0u, out, 0u) == 0, "CreateIndexBuffer"); uint32_t ib = rd32(out);
    CHECK(com(ib, 11, 4, 0u, 0u, pv, 0u) == 0, "IB Lock");
    static const uint16_t idx[] = { 0, 1, 2, 2, 1, 3 }; memcpy(M + rd32(pv), idx, sizeof idx);
    com(ib, 12, 0); com(dev, 104, 1, ib);
    clear(0xFF000000u); CHECK(com(dev, 82, 6, 4u, 0u, 0u, 4u, 0u, 2u) == 0, "DrawIndexedPrimitive"); grab(bb); quads(0xFF0000, 0, 0xFF0000, 0, "DIP base 0");
    clear(0xFF000000u); CHECK(com(dev, 82, 6, 4u, 4u, 0u, 4u, 0u, 2u) == 0, "DrawIndexedPrimitive base 4"); grab(bb); quads(0, 0xFF, 0, 0xFF, "DIP base 4");
    /* DrawIndexedPrimitiveUP */
    quad_at(vtx, -1, 1, 1, 0, 0.5f, 0xFF00FFFFu); uint32_t ix = gbuf(16); memcpy(M + ix, idx, sizeof idx);
    clear(0xFF000000u); CHECK(com(dev, 84, 8, 4u, 0u, 4u, 2u, ix, 101u, vtx, 24u) == 0, "DrawIndexedPrimitiveUP"); grab(bb);
    quads(0x00FFFF, 0x00FFFF, 0, 0, "DIP UP");
    com(dev, 100, 4, 0u, vb, 0u, 24u); com(dev, 104, 1, ib);

    /* 4. textures */
    com(dev, 107, 1, pst); point(0);
    uint32_t t2 = tex(2, 2, 1, 0, 21, 1);
    static const uint32_t t2d[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
    fill(t2, 0, t2d, 8, 2); com(dev, 65, 2, 0u, t2);
    clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); quads(0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, "A8R8G8B8 texture");
    clear(0xFF000000u); full(0xFF808080u); grab(bb); quads(0x800000, 0x008000, 0x000080, 0x808080, "texture * vertex color");
    /* DXT1 4x4: rows 0,2,3 = c0 (red), row 1 = c1 (blue) */
    uint32_t td = tex(4, 4, 1, 0, 0x31545844u, 1);
    static const uint8_t dxt1[8] = { 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x55, 0x00, 0x00 };
    fill(td, 0, dxt1, 8, 1); com(dev, 65, 2, 0u, td);
    clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); EXPECT(10, 4, 0xFF0000, "DXT1 row 0"); EXPECT(40, 20, 0x0000FF, "DXT1 row 1"); EXPECT(40, 60, 0xFF0000, "DXT1 row 3");
    /* DXT5 4x4: alpha block 0x80, color white -> with alpha blending over black = gray */
    uint32_t t5 = tex(4, 4, 1, 0, 0x35545844u, 1);
    static const uint8_t dxt5[16] = { 0x80, 0x80, 0,0,0,0,0,0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };
    fill(t5, 0, dxt5, 16, 1); com(dev, 65, 2, 0u, t5);
    rs(27, 1); rs(19, 5); rs(20, 6);
    clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); quads(0x808080, 0x808080, 0x808080, 0x808080, "DXT5 alpha blend");
    rs(27, 0);
    /* ATI2 (3Dc): D3D9 returns red = second block, green = first block, blue = 1 */
    uint32_t ta = tex(4, 4, 1, 0, 0x32495441u, 1);
    static const uint8_t ati2[16] = { 0x40, 0x40, 0,0,0,0,0,0, 0xC0, 0xC0, 0,0,0,0,0,0 };
    fill(ta, 0, ati2, 16, 1); com(dev, 65, 2, 0u, ta);
    clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); quads(0xC040FF, 0xC040FF, 0xC040FF, 0xC040FF, "ATI2");
    uint32_t t1 = tex(4, 4, 1, 0, 0x31495441u, 1);
    static const uint8_t ati1[8] = { 0x80, 0x80, 0,0,0,0,0,0 };
    fill(t1, 0, ati1, 8, 1); com(dev, 65, 2, 0u, t1);
    clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); quads(0x808080, 0x808080, 0x808080, 0x808080, "ATI1");
    /* mip levels: 4x4 red level 0, 2x2 green level 1, MAXMIPLEVEL 1 forces level 1 */
    uint32_t tm = tex(4, 4, 2, 0, 21, 1);
    uint32_t l0[16], l1[4]; for (int i = 0; i < 16; i++) l0[i] = 0xFFFF0000u; for (int i = 0; i < 4; i++) l1[i] = 0xFF00FF00u;
    fill(tm, 0, l0, 16, 4); fill(tm, 1, l1, 8, 2); com(dev, 65, 2, 0u, tm);
    com(dev, 69, 3, 0u, 7u, 1u);
    clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); EXPECT(32, 32, 0xFF0000, "mip level 0 (magnified)");
    com(dev, 69, 3, 0u, 9u, 1u);
    clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); EXPECT(32, 32, 0x00FF00, "MAXMIPLEVEL 1");
    com(dev, 69, 3, 0u, 9u, 0u); point(0);
    /* D3DXLoadSurfaceFromMemory into D3DPOOL_DEFAULT textures (v0.9.3: how the Scaleform renderer fills its bitmaps and
     * glyph cache; the GL upload must cover exactly the destination rectangle) */
    {
        uint32_t tg = tex(2, 2, 1, 0, 21, 0); CHECK(com(tg, 18, 2, 0u, out) == 0, "GetSurfaceLevel DEFAULT"); uint32_t sg = rd32(out);
        uint32_t src = gbuf(64), sr = gbuf(16), dr = gbuf(16);
        static const uint32_t abgr[4] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu };   /* A8B8G8R8: red, green, blue, white */
        memcpy(M + src, abgr, 16); wr32(sr, 0); wr32(sr + 4, 0); wr32(sr + 8, 2); wr32(sr + 12, 2);
        CHECK(api("d3dx9_31.dll", "D3DXLoadSurfaceFromMemory", 10, sg, 0u, 0u, src, 32u, 8u, 0u, sr, 0xFFFFFFFFu, 0u) == 0, "D3DXLoadSurfaceFromMemory A8B8G8R8");
        com(dev, 65, 2, 0u, tg); point(0);
        clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); quads(0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, "DEFAULT texture from D3DXLoadSurfaceFromMemory");
        static const uint32_t yel = 0xFF00FFFFu;   /* A8B8G8R8 yellow into the bottom-right texel only */
        memcpy(M + src, &yel, 4); wr32(sr, 0); wr32(sr + 4, 0); wr32(sr + 8, 1); wr32(sr + 12, 1);
        wr32(dr, 1); wr32(dr + 4, 1); wr32(dr + 8, 2); wr32(dr + 12, 2);
        CHECK(api("d3dx9_31.dll", "D3DXLoadSurfaceFromMemory", 10, sg, 0u, dr, src, 32u, 4u, 0u, sr, 0xFFFFFFFFu, 0u) == 0, "D3DXLoadSurfaceFromMemory sub-rect");
        clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); quads(0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, "sub-rect update keeps the other texels");
        /* A8 glyph texture, sampled through the alpha channel: blend white text over blue */
        uint32_t t8 = tex(2, 2, 1, 0, 28, 0); com(t8, 18, 2, 0u, out); uint32_t s8 = rd32(out);
        static const uint8_t a8[4] = { 0xFF, 0x00, 0x00, 0x80 };
        memcpy(M + src, a8, 4); wr32(sr, 0); wr32(sr + 4, 0); wr32(sr + 8, 2); wr32(sr + 12, 2);
        CHECK(api("d3dx9_31.dll", "D3DXLoadSurfaceFromMemory", 10, s8, 0u, 0u, src, 28u, 2u, 0u, sr, 0xFFFFFFFFu, 0u) == 0, "D3DXLoadSurfaceFromMemory A8");
        com(dev, 65, 2, 0u, t8); rs(27, 1); rs(19, 5); rs(20, 6);
        clear(0xFF0000FFu); full(0xFFFFFFFFu); grab(bb); quads(0x000000, 0x0000FF, 0x0000FF, 0x00007F, "A8 texture alpha (black glyph over blue)");
        rs(27, 0);
        com(tg, 2, 0); com(t8, 2, 0);
    }
    /* ps_2_0 texld t0 */
    com(dev, 107, 1, ps2); com(dev, 65, 2, 0u, t2);
    clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); quads(0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, "ps_2_0 texture");
    com(dev, 107, 1, pst);

    /* 5. render to texture, then sample it (y orientation of FBO textures) */
    uint32_t rtt = tex(64, 64, 1, 1, 21, 0);
    CHECK(com(rtt, 18, 2, 0u, out) == 0, "GetSurfaceLevel"); uint32_t rts = rd32(out);
    CHECK(com(dev, 37, 2, 0u, rts) == 0, "SetRenderTarget texture");
    com(dev, 65, 2, 0u, t2); clear(0xFF000000u); full(0xFFFFFFFFu);
    grab(rts); quads(0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, "render to texture readback");
    CHECK(com(dev, 37, 2, 0u, bb) == 0, "SetRenderTarget backbuffer");
    com(dev, 65, 2, 0u, rtt); clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); quads(0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, "sample RT texture");
    /* culling: D3D default CULL_CCW removes counter-clockwise (screen) triangles, in both kinds of targets */
    com(dev, 107, 1, psc); rs(22, 3);
    for (int pass = 0; pass < 2; pass++) {
        uint32_t tgt = pass ? rts : bb; com(dev, 37, 2, 0u, tgt);
        clear(0xFF000000u);
        float cw[3][5] = { { -1, 1, .5f }, { 1, 1, .5f }, { -1, -1, .5f } };           /* TL, TR, BL: clockwise on screen */
        float ccw[3][5] = { { 1, 1, .5f }, { -1, -1, .5f }, { 1, -1, .5f } };          /* TR, BL, BR: counter-clockwise */
        for (int i = 0; i < 3; i++) { for (int k = 0; k < 5; k++) { wrfl(vtx + 24 * i + 4 * k, cw[i][k]); wrfl(vtx + 72 + 24 * i + 4 * k, ccw[i][k]); }
            wr32(vtx + 24 * i + 20, 0xFFFFFFFFu); wr32(vtx + 72 + 24 * i + 20, 0xFFFFFFFFu); }
        CHECK(com(dev, 83, 4, 4u, 2u, vtx, 24u) == 0, "DrawPrimitiveUP list");
        grab(tgt); EXPECT(8, 8, 0xFFFFFF, pass ? "cull: CW drawn (RT)" : "cull: CW drawn"); EXPECT(55, 55, 0, pass ? "cull: CCW culled (RT)" : "cull: CCW culled");
    }
    rs(22, 1); com(dev, 37, 2, 0u, bb);

    /* 6. depth test */
    rs(7, 1); rs(14, 1); rs(23, 4);
    clear(0xFF000000u); quad(-1, 1, 1, -1, 0.3f, 0xFF00FF00u); quad(-1, 1, 1, -1, 0.6f, 0xFFFF0000u); grab(bb);
    quads(0x00FF00, 0x00FF00, 0x00FF00, 0x00FF00, "depth: far quad hidden");
    quad(-1, 1, 0, -1, 0.1f, 0xFF0000FFu); grab(bb); quads(0xFF, 0x00FF00, 0xFF, 0x00FF00, "depth: nearer quad drawn");
    rs(23, 8); rs(14, 0); quad(0, 1, 1, -1, 0.9f, 0xFFFFFFFFu); rs(23, 4); quad(-1, 1, 1, -1, 0.35f, 0xFFFF00FFu); grab(bb);
    quads(0xFF, 0xFFFFFF, 0xFF, 0xFFFFFF, "depth: ZWRITE off keeps depth");
    rs(7, 0); rs(14, 1);
    /* 7. alpha test, blending, color write mask, scissor, viewport */
    rs(15, 1); rs(25, 5); rs(24, 0x80);
    clear(0xFF000000u); quad(-1, 1, 0, -1, .5f, 0x40FFFFFFu); quad(0, 1, 1, -1, .5f, 0xC0FFFFFFu); grab(bb); quads(0, 0xFFFFFF, 0, 0xFFFFFF, "alpha test");
    rs(15, 0);
    rs(27, 1); rs(19, 5); rs(20, 6);
    clear(0xFF0000FFu); full(0x80FF0000u); grab(bb); quads(0x80007F, 0x80007F, 0x80007F, 0x80007F, "alpha blend");
    rs(19, 2); rs(20, 2); clear(0xFF0000FFu); full(0xFF00FF00u); grab(bb); quads(0x00FFFF, 0x00FFFF, 0x00FFFF, 0x00FFFF, "additive");
    rs(27, 0);
    rs(168, 1); clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); quads(0xFF0000, 0xFF0000, 0xFF0000, 0xFF0000, "COLORWRITEENABLE red");
    rs(168, 15);
    uint32_t sc = gbuf(16); wr32(sc, 0); wr32(sc + 4, 0); wr32(sc + 8, 32); wr32(sc + 12, 64); com(dev, 75, 1, sc);
    clear(0xFF000000u); rs(174, 1); full(0xFFFFFFFFu); grab(bb); quads(0xFFFFFF, 0, 0xFFFFFF, 0, "scissor");
    rs(174, 0);
    rs(174, 1); clear(0xFFFFFFFFu); grab(bb); quads(0xFFFFFF, 0, 0xFFFFFF, 0, "Clear respects the scissor rect"); rs(174, 0);
    uint32_t vp = gbuf(24); wr32(vp, 32); wr32(vp + 4, 0); wr32(vp + 8, 32); wr32(vp + 12, 32); wrfl(vp + 16, 0); wrfl(vp + 20, 1);
    com(dev, 47, 1, vp); clear(0xFF000000u);
    wr32(vp, 0); wr32(vp + 8, 64); wr32(vp + 12, 64);
    grab(bb); quads(0xFFFFFF, 0, 0xFFFFFF, 0, "clear limited to viewport");
    wr32(vp, 32); wr32(vp + 8, 32); wr32(vp + 12, 32); com(dev, 47, 1, vp); full(0xFFFFFFFFu); grab(bb);
    quads(0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0, "viewport top right");
    wr32(vp, 0); wr32(vp + 8, 64); wr32(vp + 12, 64); com(dev, 47, 1, vp);

    /* 8. ColorFill, StretchRect, UpdateSurface */
    clear(0xFF000000u);
    CHECK(com(dev, 35, 3, bb, rc, 0xFF00FF00u) == 0, "ColorFill"); grab(bb); quads(0x00FF00, 0, 0, 0, "ColorFill");
    com(dev, 37, 2, 0u, rts); com(dev, 107, 1, pst); com(dev, 65, 2, 0u, t2); full(0xFFFFFFFFu); com(dev, 37, 2, 0u, bb);
    CHECK(com(dev, 34, 5, rts, 0u, bb, 0u, 0u) == 0, "StretchRect"); grab(bb); quads(0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, "StretchRect");
    uint32_t dr = gbuf(16); wr32(dr, 32); wr32(dr + 4, 32); wr32(dr + 8, 64); wr32(dr + 12, 64);
    clear(0xFF000000u); CHECK(com(dev, 34, 5, rts, 0u, bb, dr, 0u) == 0, "StretchRect to rect"); grab(bb);
    EXPECT(36, 36, 0xFF0000, "StretchRect rect TL"); EXPECT(60, 60, 0xFFFFFF, "StretchRect rect BR"); EXPECT(10, 10, 0, "StretchRect outside");
    /* UpdateSurface: system memory 2x2 -> default pool texture */
    CHECK(com(dev, 36, 6, 2u, 2u, 21u, 2u, out, 0u) == 0, "sysmem surface"); uint32_t ss = rd32(out);
    { uint32_t lr = gbuf(8); com(ss, 13, 3, lr, 0u, 0u); static const uint32_t q[4] = { 0xFFFFFFFFu, 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u };
      for (int y = 0; y < 2; y++) memcpy(M + rd32(lr + 4) + rd32(lr) * y, q + 2 * y, 8); com(ss, 14, 0); }
    uint32_t tdef = tex(2, 2, 1, 0, 21, 0); CHECK(com(tdef, 18, 2, 0u, out) == 0, ""); uint32_t tds = rd32(out);
    CHECK(com(dev, 30, 4, ss, 0u, tds, 0u) == 0, "UpdateSurface"); com(dev, 65, 2, 0u, tdef);
    clear(0xFF000000u); full(0xFFFFFFFFu); grab(bb); quads(0xFFFFFF, 0x0000FF, 0x00FF00, 0xFF0000, "UpdateSurface texture");

    /* 8b. pre-transformed vertices (POSITIONT declaration / D3DFVF_XYZRHW): D3D9 ignores the bound vertex shader; the
     * coordinates are render target pixels with pixel centers at integers. A leftover shader with a big c0 offset stays bound. */
    {
        float junk[4] = { 0.7f, -0.6f, 0.f, 0.f }; uint32_t cj = gbuf(16); for (int k = 0; k < 4; k++) wrfl(cj + 4 * k, junk[k]);
        com(dev, 94, 3, 0u, cj, 1u);
        uint32_t dt = gbuf(32);
        static const uint8_t declt[32] = { 0,0, 0,0, 3,0,9,0,   0,0, 16,0, 1,0,5,0,   0,0, 24,0, 4,0,10,0,   0xFF,0, 0,0, 17,0,0,0 };
        memcpy(M + dt, declt, 32);
        CHECK(com(dev, 86, 2, dt, out) == 0, "CreateVertexDeclaration POSITIONT"); uint32_t vdt = rd32(out);
        com(dev, 107, 1, psc);
        for (int pass = 0; pass < 2; pass++) {
            uint32_t col = pass ? 0xFF00FF00u : 0xFFFF0000u, want = col & 0xFFFFFF;
            /* pixels x 16..47, y 8..23 (edges at -0.5) */
            float q[4][2] = { { 15.5f, 7.5f }, { 47.5f, 7.5f }, { 15.5f, 23.5f }, { 47.5f, 23.5f } };
            uint32_t tv = gbuf(4 * 28);
            for (int i = 0; i < 4; i++) {
                uint32_t a = tv + 28 * i; wrfl(a, q[i][0]); wrfl(a + 4, q[i][1]); wrfl(a + 8, 0.5f); wrfl(a + 12, 1.0f);
                if (pass == 0) { wrfl(a + 16, 0); wrfl(a + 20, 0); wr32(a + 24, col); }
                else { wr32(a + 16, col); wrfl(a + 20, 0); wrfl(a + 24, 0); }   /* FVF order: XYZRHW, DIFFUSE, TEX1 */
            }
            if (pass == 0) com(dev, 87, 1, vdt); else CHECK(com(dev, 89, 1, 0x144u) == 0, "SetFVF XYZRHW|DIFFUSE|TEX1");
            clear(0xFF000000u);
            CHECK(com(dev, 83, 4, 5u, 2u, tv, 28u) == 0, "DrawPrimitiveUP pre-transformed");
            grab(bb);
            const char* w = pass ? "XYZRHW FVF" : "POSITIONT declaration";
            EXPECT(16, 8, want, w); EXPECT(47, 8, want, w); EXPECT(16, 23, want, w); EXPECT(47, 23, want, w); EXPECT(30, 15, want, w);
            EXPECT(15, 8, 0, w); EXPECT(48, 23, 0, w); EXPECT(16, 7, 0, w); EXPECT(47, 24, 0, w); EXPECT(55, 55, 0, w);
        }
        com(dev, 87, 1, vd); com(dev, 94, 3, 0u, c0, 1u);
        CHECK(com(dev, 89, 1, 0u) == 0, "SetFVF 0");
    }

    /* 9. float render target + Present */
    uint32_t tf = tex(64, 64, 1, 1, 113, 0); CHECK(com(tf, 18, 2, 0u, out) == 0, ""); uint32_t tfs = rd32(out);
    com(dev, 37, 2, 0u, tfs); com(dev, 107, 1, ps0); wrfl(pc, 2.0f); wrfl(pc + 4, 0.5f); wrfl(pc + 8, 0.0f); wrfl(pc + 12, 1.0f); com(dev, 109, 3, 0u, pc, 1u);
    full(0); com(dev, 37, 2, 0u, bb); com(dev, 107, 1, pst); com(dev, 65, 2, 0u, tf); point(0);
    /* result * vertex color 0.5: red 2.0*0.5 = 1.0 proves the float target kept the value above 1 */
    clear(0xFF000000u); full(0xFF808080u); grab(bb); quads(0xFF4000, 0xFF4000, 0xFF4000, 0xFF4000, "A16B16G16R16F render target");
    /* last frame: the quadrant texture (frame dumps / fake window show TL red, TR green, BL blue, BR white) */
    com(dev, 65, 2, 0u, t2); clear(0xFF000000u); full(0xFFFFFFFFu);
    com(dev, 42, 0);
    CHECK(com(dev, 17, 4, 0u, 0u, 0u, 0u) == 0, "Present");
    int frames = getenv("ME1_GL_FAKEWIN") || cpu ? 8 : 1;
    for (int i = 0; i < frames; i++) { usleep(30000); com(dev, 41, 0); clear(0xFF000000u); full(0xFFFFFFFFu); com(dev, 42, 0);
        CHECK(com(dev, 17, 4, 0u, 0u, 0u, 0u) == 0, "Present %d", i + 2); }
    {   /* v0.9.9 frame timing: GPU time (timer query, if the GL has EXT_disjoint_timer_query) and the Present wait */
        int me1gl_frame_times(char* o, size_t cap, uint64_t frames);
        char ft[320]; int has = me1gl_frame_times(ft, sizeof ft, 0);   /* reset */
        for (int i = 0; i < 6; i++) { com(dev, 41, 0); clear(0xFF000000u); full(0xFFFFFFFFu); com(dev, 42, 0); com(dev, 17, 4, 0u, 0u, 0u, 0u); usleep(20000); }
        me1gl_frame_times(ft, sizeof ft, 6);
        fprintf(stderr, "frame times: timer query %s%s\n", has ? "available" : "not available", ft);
        CHECK(strstr(ft, "Present waits") && (!has || strstr(ft, "GPU ")), "frame timing suffix '%s'", ft);
        CHECK(strstr(ft, " us/draw"), "CPU per-draw measurement in the frame suffix '%s'", ft);   /* v0.10.2 */
    }
    usleep(100000);
    if (getenv("ME1_FRAME_DUMP")) { char p[512]; snprintf(p, sizeof p, "%s/frame_000001.png", getenv("ME1_FRAME_DUMP")); CHECK(access(p, 0) == 0, "frame dump %s", p); }
    if (cpu) {   /* window 200x100, frame 64x64 -> buffer 128x64, picture at x 32..95; TL red, TR green, BL blue, BR white */
        CHECK(fw_posts >= 2 && fw_last, "CPU present: %d frames posted", fw_posts);
        CHECK(fw_gw == 128 && fw_gh == 64 && fw_fmt == 2, "CPU present: buffer geometry %dx%d format %d (want 128x64 RGBX)", fw_gw, fw_gh, fw_fmt);
        if (fw_last) {
            static const struct { int x, y; uint8_t r, g, b; const char* what; } P[] = {
                { 48, 16, 255, 0, 0, "top-left red" }, { 80, 16, 0, 255, 0, "top-right green" }, { 48, 48, 0, 0, 255, "bottom-left blue" },
                { 80, 48, 255, 255, 255, "bottom-right white" }, { 4, 30, 0, 0, 0, "left bar black" }, { 124, 30, 0, 0, 0, "right bar black" },
                { 31, 63, 0, 0, 0, "bar edge" } };
            for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) {
                const uint8_t* q = fw_at(P[i].x, P[i].y);
                CHECK(abs(q[0] - P[i].r) < 8 && abs(q[1] - P[i].g) < 8 && abs(q[2] - P[i].b) < 8, "CPU present: %s at %d,%d = %d %d %d",
                      P[i].what, P[i].x, P[i].y, q[0], q[1], q[2]);
            }
        }
        int vx, vy, vw, vh, ffw, ffh; me1gl_get_view(&vx, &vy, &vw, &vh, &ffw, &ffh);
        CHECK(vx == 50 && vy == 0 && vw == 100 && vh == 100 && ffw == 64 && ffh == 64, "CPU present: touch view %d,%d %dx%d of %dx%d", vx, vy, vw, vh, ffw, ffh);
    }
    {   /* the launcher's start-up screen counters */
        extern void me1_status(int64_t o[10]); int64_t st[10]; me1_status(st);
        CHECK(st[0] >= 2 && st[5] >= 6 && st[6] <= st[5] && st[7] >= 7 && st[8] >= 6 && st[9] >= 0 && st[3] == 0,
              "me1_status: frames %lld, programs %lld (cached %lld), translated %lld, shaders %lld, last build %lld ms ago",
              (long long)st[0], (long long)st[5], (long long)st[6], (long long)st[7], (long long)st[8], (long long)st[9]);
    }
    hle_d3d9_report();
    printf("test_gl: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail != 0;
}
