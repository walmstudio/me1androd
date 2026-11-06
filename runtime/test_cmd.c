/* Stress test of the D3D9 command stream (v1.2.1, ME1_D3D_CMD): renders frames that change everything the recording
 * must capture - render/sampler states, constants, dynamic buffers locked DISCARD / NOOVERWRITE / overwrite between
 * draws, *UP draws, textures relocked mid-frame (small: copied, big: by reference), render target switches, StretchRect,
 * ColorFill, UpdateTexture with the source changed right after, GetRenderTargetData feeding later draws, objects
 * released right after their draw, occlusion queries. Compare the ME1_FRAME_DUMP PNGs of ME1_D3D_CMD=0/1/2.
 * usage: test_cmd [frames] [perf [work_ms] | dxt]   (perf: 2000 small draws per frame, prints ms/frame;
 *        work_ms: simulated game CPU work per frame, shows the overlap of ME1_D3D_CMD=2;
 *        dxt: two 1024x1024 DXT textures re-uploaded per frame - with ME1_TEX_BC=0 the CPU decoder on the GEARMASS
 *        workers, ME1_GM_WORKERS=0..8). v1.2.1: every frame also refills a DXT1 and a DXT5 texture (256x256): the final
 *        crc must not depend on ME1_GM_WORKERS / ME1_TEX_BC=0 */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

static int g_fail, g_ok;
#define CHECK(cond, ...) do { if (cond) g_ok++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static uint32_t call(uint32_t t, int n, const uint32_t* a) { return rt_call(&g_cpu, t, n, a); }
static uint32_t api(const char* dll, const char* name, int n, ...) {
    uint32_t t = hle_resolve_impl(dll, name), a[24]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap); return call(t, n, a);
}
static uint32_t com(uint32_t o, int k, int n, ...) {
    uint32_t a[24]; va_list ap; va_start(ap, n);
    a[0] = o; for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap); return call(rd32(rd32(o) + 4 * k), n + 1, a);
}
static uint32_t gbuf(uint32_t n) { uint32_t a = ghalloc(n, 1); memset(M + a, 0, n); return a; }
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void wrfl(uint32_t a, float f) { wr32(a, fb(f)); }
enum { R_TEMP = 0, R_IN = 1, R_CONST = 2, R_OUT = 6, R_COLOROUT = 8, R_SAMP = 10 };
#define RTY(t) ((((uint32_t)(t) & 7) << 28) | ((((uint32_t)(t) >> 3) & 3) << 11))
#define DST(t, n) (0x800F0000u | RTY(t) | (n))
#define SRC(t, n) (0x80E40000u | RTY(t) | (n))
#define OP(o, len) ((uint32_t)(o) | ((uint32_t)(len) << 24))
#define DCL(usage, idx, t, n) OP(0x1F, 2), 0x80000000u | (usage) | ((uint32_t)(idx) << 16), DST(t, n)
#define DCL2D(n) OP(0x1F, 2), 0x90000000u, DST(R_SAMP, n)
#define MOV(d, s) OP(0x01, 2), d, s
#define ADD(d, a, b) OP(0x02, 3), d, a, b
#define MUL(d, a, b) OP(0x05, 3), d, a, b
#define TEXLD(d, c, s) OP(0x42, 3), d, c, s
#define END 0x0000FFFFu
enum { U_POS = 0, U_TC = 5, U_COLOR = 10 };
static uint32_t dev, out, vtx;
static uint32_t shader(int k, const uint32_t* t, int n) {
    uint32_t c = gbuf(4 * n); for (int i = 0; i < n; i++) wr32(c + 4 * i, t[i]);
    CHECK(com(dev, k, 2, c, out) == 0 && rd32(out), "create shader"); return rd32(out);
}
/* a quad (triangle strip, 24-byte vertices) in grid cell (cx, cy) of 8x8, inset by `in` */
static void quad_at(uint32_t at, int cx, int cy, float in, uint32_t col) {
    float x0 = -1 + cx * 0.25f + in, x1 = -1 + (cx + 1) * 0.25f - in, y0 = 1 - cy * 0.25f - in, y1 = 1 - (cy + 1) * 0.25f + in;
    float v[4][5] = { { x0, y0, 0.5f, 0, 0 }, { x1, y0, 0.5f, 1, 0 }, { x0, y1, 0.5f, 0, 1 }, { x1, y1, 0.5f, 1, 1 } };
    for (int i = 0; i < 4; i++) { for (int k = 0; k < 5; k++) wrfl(at + 24 * i + 4 * k, v[i][k]); wr32(at + 24 * i + 20, col); }
}
static void up(int cx, int cy, uint32_t col) { quad_at(vtx, cx, cy, 0.01f, col); com(dev, 83, 4, 5u, 2u, vtx, 24u); }
static void rs(uint32_t s, uint32_t v) { com(dev, 57, 2, s, v); }
static uint32_t tex(uint32_t w, uint32_t h, uint32_t usage, uint32_t fmt, uint32_t pool) {
    CHECK(com(dev, 23, 8, w, h, 1u, usage, fmt, pool, out, 0u) == 0, "CreateTexture"); return rd32(out);
}
static void tfill(uint32_t t, uint32_t rect, uint32_t col, uint32_t w, uint32_t h) {   /* A8R8G8B8 */
    uint32_t lr = gbuf(8);
    CHECK(com(t, 19, 4, 0u, lr, rect, 0u) == 0, "LockRect");
    uint32_t pitch = rd32(lr), b = rd32(lr + 4);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) wr32(b + pitch * y + 4 * x, col ^ (x * 0x10 + y * 0x1000));
    com(t, 20, 1, 0u);
}
/* v1.2.1: random DXT blocks (LCG per frame): the CPU decoder (ME1_TEX_BC=0) runs on the GEARMASS workers */
static void dxtfill(uint32_t t, uint32_t w, uint32_t h, uint32_t bsz, uint32_t seed) {
    uint32_t lr = gbuf(8);
    CHECK(com(t, 19, 4, 0u, lr, 0u, 0u) == 0, "LockRect DXT");
    uint32_t pitch = rd32(lr), b = rd32(lr + 4), x = seed * 2654435761u + 1;
    for (uint32_t y = 0; y < h / 4; y++) for (uint32_t i = 0; i < (w / 4) * bsz; i++) { x = x * 1664525u + 1013904223u; M[b + pitch * y + i] = (uint8_t)(x >> 24); }
    com(t, 20, 1, 0u);
}
static uint32_t rgb(int f, int k) { return 0xFF000000u | (uint32_t)((f * 37 + k * 71) & 255) << 16 | (uint32_t)((f * 11 + k * 113) & 255) << 8 | (uint32_t)((k * 29 + f * 5) & 255); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char** argv) {
    rt_init();
    int frames = argc > 1 ? atoi(argv[1]) : 40, perf = argc > 2 && !strcmp(argv[2], "perf"), dxt = argc > 2 && !strcmp(argv[2], "dxt");
    double work_ms = argc > 3 ? atof(argv[3]) : 0;   /* perf: simulated game-thread CPU work per frame */
    out = gbuf(64); vtx = gbuf(24 * 64);
    uint32_t d3d = api("d3d9.dll", "Direct3DCreate9", 1, 32u), pp = gbuf(56);
    uint32_t W = perf || dxt ? 640 : 128, H = perf || dxt ? 480 : 128;
    wr32(pp, W); wr32(pp + 4, H); wr32(pp + 8, 22); wr32(pp + 12, 1); wr32(pp + 24, 1); wr32(pp + 28, 0x10000); wr32(pp + 32, 1);
    wr32(pp + 36, 1); wr32(pp + 40, 75);
    CHECK(com(d3d, 16, 6, 0u, 1u, 0x10000u, 0x40u, pp, out) == 0, "CreateDevice"); dev = rd32(out);
    com(dev, 18, 4, 0u, 0u, 0u, out); uint32_t bb = rd32(out);
    static const uint32_t vs_t[] = { 0xFFFE0300u, DCL(U_POS, 0, R_IN, 0), DCL(U_TC, 0, R_IN, 1), DCL(U_COLOR, 0, R_IN, 2),
        DCL(U_POS, 0, R_OUT, 0), DCL(U_TC, 0, R_OUT, 1), DCL(U_COLOR, 0, R_OUT, 2),
        ADD(DST(R_OUT, 0), SRC(R_IN, 0), SRC(R_CONST, 0)), MOV(DST(R_OUT, 1), SRC(R_IN, 1)), MOV(DST(R_OUT, 2), SRC(R_IN, 2)), END };
    static const uint32_t ps_tex[] = { 0xFFFF0300u, DCL(U_TC, 0, R_IN, 0), DCL(U_COLOR, 0, R_IN, 1), DCL2D(0),
        TEXLD(DST(R_TEMP, 0), SRC(R_IN, 0), SRC(R_SAMP, 0)), MUL(DST(R_COLOROUT, 0), SRC(R_TEMP, 0), SRC(R_IN, 1)), END };
    static const uint32_t ps_col[] = { 0xFFFF0300u, DCL(U_COLOR, 0, R_IN, 0), MOV(DST(R_COLOROUT, 0), SRC(R_IN, 0)), END };
    static const uint32_t ps_c0[] = { 0xFFFF0300u, MOV(DST(R_COLOROUT, 0), SRC(R_CONST, 0)), END };
    uint32_t vs = shader(91, vs_t, sizeof vs_t / 4), pst = shader(106, ps_tex, sizeof ps_tex / 4);
    uint32_t psc = shader(106, ps_col, sizeof ps_col / 4), ps0 = shader(106, ps_c0, sizeof ps_c0 / 4);
    uint32_t de = gbuf(32);
    static const uint8_t decl[32] = { 0,0, 0,0, 2,0,0,0,   0,0, 12,0, 1,0,5,0,   0,0, 20,0, 4,0,10,0,   0xFF,0, 0,0, 17,0,0,0 };
    memcpy(M + de, decl, 32);
    com(dev, 86, 2, de, out); uint32_t vd = rd32(out);
    uint32_t c0 = gbuf(16), pc = gbuf(16);
    com(dev, 87, 1, vd); com(dev, 92, 1, vs); com(dev, 94, 3, 0u, c0, 1u);
    rs(22, 1); rs(7, 0);
    /* resources */
    CHECK(com(dev, 26, 6, 96u * 1024u, 0x208u, 0u, 0u, out, 0u) == 0, "dynamic VB"); uint32_t dvb = rd32(out);
    com(dev, 26, 6, 96u, 0u, 0u, 1u, out, 0u); uint32_t svb = rd32(out);
    com(dev, 27, 6, 12u, 0u, 101u, 1u, out, 0u); uint32_t sib = rd32(out);
    { uint32_t lr = gbuf(4); com(sib, 11, 4, 0u, 0u, lr, 0u); static const uint16_t ix[6] = { 0, 1, 2, 2, 1, 3 }; memcpy(M + rd32(lr), ix, 12); com(sib, 12, 0); }
    uint32_t tA = tex(16, 16, 0, 21, 1), tBig = tex(1024, 512, 0, 21, 1), tSys = tex(16, 16, 0, 21, 2), tDef = tex(16, 16, 0, 21, 0);
    uint32_t tRT = tex(64, 64, 1, 21, 0); com(tRT, 18, 2, 0u, out); uint32_t sRT = rd32(out);
    uint32_t dxw = dxt ? 1024 : 256, tD1 = tex(dxw, dxw, 0, 0x31545844u, 1), tD5 = tex(dxw, dxw, 0, 0x35545844u, 1);   /* DXT1, DXT5 */
    com(dev, 36, 6, 64u, 64u, 21u, 2u, out, 0u); uint32_t sysrb = rd32(out);
    com(dev, 118, 2, 9u, out); uint32_t q = rd32(out);
    uint32_t rect = gbuf(16), rect2 = gbuf(16), lr = gbuf(8);
    uint32_t fbcol = 0xFF808080u;
    double t0 = now();
    for (int f = 0; f < frames; f++) {
        com(dev, 41, 0);
        com(dev, 43, 6, 0u, 0u, 7u, rgb(f, 0) & 0xFF3F3F3Fu, fb(1.0f), 0u);
        if (dxt) {   /* v1.2.1: two 1024x1024 DXT textures re-uploaded every frame: the cost of CPU decoding (ME1_TEX_BC=0) */
            com(dev, 107, 1, pst);
            dxtfill(tD1, dxw, dxw, 8, (uint32_t)f); com(dev, 65, 2, 0u, tD1); up(0, 0, 0xFFFFFFFFu);
            dxtfill(tD5, dxw, dxw, 16, (uint32_t)f + 1000); com(dev, 65, 2, 0u, tD5); up(1, 0, 0xFFFFFFFFu);
            com(dev, 42, 0); com(dev, 17, 4, 0u, 0u, 0u, 0u);
            continue;
        }
        if (perf) {   /* many small draws with a constant change each: the per-draw cost of the front end + renderer */
            com(dev, 107, 1, ps0);
            /* a 4x4 scissor keeps the (software) rasterizer out of the measurement: what is left is the per-draw CPU cost */
            wr32(rect, 0); wr32(rect + 4, 0); wr32(rect + 8, 4); wr32(rect + 12, 4); com(dev, 75, 1, rect); rs(174, 1);
            for (int i = 0; i < 2000; i++) {
                wrfl(pc, (float)(i & 255) / 255.f); wrfl(pc + 4, (float)(f & 255) / 255.f); wrfl(pc + 8, 0.5f); wrfl(pc + 12, 1.f);
                com(dev, 109, 3, 0u, pc, 1u);
                rs(27, i & 1);
                up(i & 7, (i >> 3) & 7, 0);
            }
            rs(174, 0);
            com(dev, 42, 0);
            if (work_ms > 0) { double tw = now() + work_ms / 1e3; volatile uint32_t sink = 0; while (now() < tw) for (int k = 0; k < 1000; k++) sink += k; }
            com(dev, 17, 4, 0u, 0u, 0u, 0u);
            continue;
        }
        /* 1. dynamic VB: DISCARD, NOOVERWRITE, then an overwrite of data the first draw uses (copy-on-write) */
        com(dev, 107, 1, psc); com(dev, 100, 4, 0u, dvb, 0u, 24u);
        com(dvb, 11, 4, 0u, 96u, lr, 0x2000u); quad_at(rd32(lr), 0, 0, 0.01f, rgb(f, 1)); com(dvb, 12, 0);
        com(dev, 81, 3, 5u, 0u, 2u);
        com(dvb, 11, 4, 96u, 96u, lr, 0x1000u); quad_at(rd32(lr), 1, 0, 0.01f, rgb(f, 2)); com(dvb, 12, 0);
        com(dev, 81, 3, 5u, 4u, 2u);
        com(dvb, 11, 4, 0u, 96u, lr, 0u); quad_at(rd32(lr), 2, 0, 0.01f, rgb(f, 3)); com(dvb, 12, 0);
        com(dev, 81, 3, 5u, 0u, 2u);
        /* 2. static VB + IB, vertex shader constant */
        { uint32_t l2 = gbuf(4); com(svb, 11, 4, 0u, 0u, l2, 0u); quad_at(rd32(l2), 3, 0, 0.02f, rgb(f, 4)); com(svb, 12, 0); }
        wrfl(c0, (f & 1) ? 0.25f : 0.f); com(dev, 94, 3, 0u, c0, 1u);
        com(dev, 100, 4, 0u, svb, 0u, 24u); com(dev, 104, 1, sib);
        com(dev, 82, 6, 4u, 0u, 0u, 4u, 0u, 2u);
        wrfl(c0, 0.f); com(dev, 94, 3, 0u, c0, 1u);
        /* 3. *UP draws, blend changes, the indexed UP draw */
        for (int i = 0; i < 8; i++) { rs(27, i == 5); rs(19, 5); rs(20, 6); up(i, 1, (rgb(f, 10 + i) & 0x00FFFFFFu) | 0x80000000u); }
        rs(27, 0);
        { uint32_t ix = gbuf(12); static const uint16_t x6[6] = { 0, 1, 2, 2, 1, 3 }; memcpy(M + ix, x6, 12); quad_at(vtx, 6, 0, 0.03f, rgb(f, 5));
          com(dev, 84, 8, 4u, 0u, 4u, 2u, ix, 101u, vtx, 24u); }
        /* 4. a small texture relocked between draws (copied into the stream) */
        com(dev, 107, 1, pst); com(dev, 65, 2, 0u, tA);
        tfill(tA, 0, rgb(f, 20), 16, 16); up(0, 2, 0xFFFFFFFFu);
        tfill(tA, 0, rgb(f, 21), 16, 16); up(1, 2, 0xFFFFFFFFu);
        wr32(rect, 4); wr32(rect + 4, 4); wr32(rect + 8, 12); wr32(rect + 12, 12); tfill(tA, rect, rgb(f, 22), 8, 8); up(2, 2, 0xFFFFFFFFu);
        /* 5. a big texture (2 MB: uploaded by reference, the next Lock waits) */
        com(dev, 65, 2, 0u, tBig); tfill(tBig, 0, rgb(f, 23), 1024, 512); up(3, 2, 0xFFFFFFFFu);
        /* 6. render target switch, the RT as a texture, StretchRect, ColorFill */
        com(dev, 37, 2, 0u, sRT); com(dev, 43, 6, 0u, 0u, 1u, rgb(f, 30), fb(1.0f), 0u);
        com(dev, 107, 1, psc); up(2, 2, rgb(f, 31)); up(5, 5, rgb(f, 32));
        com(dev, 37, 2, 0u, bb);
        com(dev, 107, 1, pst); com(dev, 65, 2, 0u, tRT); up(0, 3, 0xFFFFFFFFu);
        wr32(rect, 0); wr32(rect + 4, 0); wr32(rect + 8, 64); wr32(rect + 12, 64);
        wr32(rect2, 16); wr32(rect2 + 4, 48); wr32(rect2 + 8, 32); wr32(rect2 + 12, 64);
        com(dev, 34, 5, sRT, rect, bb, rect2, 2u);
        wr32(rect2, 32); wr32(rect2 + 4, 48); wr32(rect2 + 8, 48); wr32(rect2 + 12, 64); com(dev, 35, 3, bb, rect2, rgb(f, 33));
        /* 7. a texture and a vertex buffer released right after their draws */
        { uint32_t t = tex(8, 8, 0, 21, 1); tfill(t, 0, rgb(f, 40), 8, 8); com(dev, 65, 2, 0u, t); up(3, 3, 0xFFFFFFFFu);
          com(dev, 65, 2, 0u, 0u); com(t, 2, 0); }
        { com(dev, 26, 6, 96u, 0x208u, 0u, 0u, out, 0u); uint32_t v = rd32(out); com(dev, 107, 1, psc);
          com(v, 11, 4, 0u, 0u, lr, 0x2000u); quad_at(rd32(lr), 4, 3, 0.01f, rgb(f, 41)); com(v, 12, 0);
          com(dev, 100, 4, 0u, v, 0u, 24u); com(dev, 81, 3, 5u, 0u, 2u); com(dev, 100, 4, 0u, 0u, 0u, 0u); com(v, 2, 0); }
        /* 8. occlusion query around a draw (result not used: timing-dependent with ME1_OCCLUSION=2) */
        com(q, 6, 1, 2u); up(5, 3, rgb(f, 42)); com(q, 6, 1, 1u); com(q, 7, 3, out, 4u, 0u);
        /* 9. readback feeding a later draw (every 5th frame) */
        if (f % 5 == 0) {
            CHECK(com(dev, 32, 2, sRT, sysrb) == 0, "GetRenderTargetData");
            CHECK(com(sysrb, 13, 3, lr, 0u, 0x10u) == 0, "LockRect readback");
            fbcol = rd32(rd32(lr + 4) + rd32(lr) * 40 + 4 * 40) | 0xFF000000u; com(sysrb, 14, 0);
        }
        com(dev, 107, 1, psc); up(6, 3, fbcol);
        /* 10. UpdateTexture, then the source changes at once (must not reach the GPU copy) */
        tfill(tSys, 0, rgb(f, 50), 16, 16); com(dev, 31, 2, tSys, tDef); tfill(tSys, 0, 0xFFFF00FFu, 16, 16);
        com(dev, 107, 1, pst); com(dev, 65, 2, 0u, tDef); up(7, 3, 0xFFFFFFFFu);
        /* 11. scissor and viewport, ps constants per draw */
        com(dev, 107, 1, ps0);
        wr32(rect, 0); wr32(rect + 4, 64); wr32(rect + 8, 40 + f % 20); wr32(rect + 12, 128); com(dev, 75, 1, rect); rs(174, 1);
        for (int i = 0; i < 4; i++) { wrfl(pc, i * 0.25f); wrfl(pc + 4, (f % 10) * 0.1f); wrfl(pc + 8, 0.5f); wrfl(pc + 12, 1.f); com(dev, 109, 3, 0u, pc, 1u); up(i, 4, 0); }
        rs(174, 0);
        { uint32_t vp = gbuf(24); wr32(vp, 64); wr32(vp + 4, 64); wr32(vp + 8, 64); wr32(vp + 12, 64); wrfl(vp + 16, 0); wrfl(vp + 20, 1);
          com(dev, 47, 1, vp); wrfl(pc, 1.f); wrfl(pc + 4, 0.f); com(dev, 109, 3, 0u, pc, 1u); up(f % 8, 6, 0);
          wr32(vp, 0); wr32(vp + 4, 0); wr32(vp + 8, 128); wr32(vp + 12, 128); com(dev, 47, 1, vp); }
        /* 12. v1.2.1: DXT1 / DXT5 textures refilled every frame (CPU decoding with ME1_TEX_BC=0: GEARMASS workers) */
        com(dev, 107, 1, pst);
        dxtfill(tD1, dxw, dxw, 8, (uint32_t)f); com(dev, 65, 2, 0u, tD1); up(0, 7, 0xFFFFFFFFu);
        dxtfill(tD5, dxw, dxw, 16, (uint32_t)f + 1000); com(dev, 65, 2, 0u, tD5); up(1, 7, 0xFFFFFFFFu);
        com(dev, 42, 0);
        com(dev, 17, 4, 0u, 0u, 0u, 0u);
    }
    double dt = now() - t0;
    /* final readback of the back buffer */
    com(dev, 36, 6, W, H, 21u, 2u, out, 0u); uint32_t sbb = rd32(out);
    CHECK(com(dev, 32, 2, bb, sbb) == 0, "GetRenderTargetData back buffer");
    com(sbb, 13, 3, lr, 0u, 0x10u);
    uint32_t crc = 0; for (uint32_t y = 0; y < H; y++) crc = rt_crc32(M + rd32(lr + 4) + rd32(lr) * y, W * 4) ^ (crc * 31);
    com(sbb, 14, 0);
    printf("test_cmd: %d frames in %.3f s (%.2f ms/frame), final frame crc %08x, %d ok, %d failed\n", frames, dt, dt * 1e3 / frames, crc, g_ok, g_fail);
    hle_d3d9_report();
    return g_fail != 0;
}
