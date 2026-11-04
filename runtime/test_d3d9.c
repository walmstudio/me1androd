/* Test of the D3D9 HLE (hle_d3d9.c) through the real thunks and guest vtables, exactly as the game calls it.
 * usage: test_d3d9 [dump_dir] */
#include "rt.h"
#include "hle.h"
#include "me1settings.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

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
/* COM call: method index k of object o (this + n args) */
static uint32_t com(uint32_t o, int k, int n, ...) {
    uint32_t a[24]; va_list ap; va_start(ap, n);
    a[0] = o; for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t vt = rd32(o), t = rd32(vt + 4 * k);
    if (!hle_is_thunk(t)) { fprintf(stderr, "FAIL: vtable slot %d of %08x is %08x\n", k, o, t); g_fail++; return 0xFFFFFFFFu; }
    char w[32]; snprintf(w, sizeof w, "method %d", k);
    return call(t, n + 1, a, w);
}
static uint32_t gbuf(uint32_t n) { return ghalloc(n, 1); }
static float rdf(uint32_t a) { float f; uint32_t u = rd32(a); memcpy(&f, &u, 4); return f; }
static void wrfl(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(a, u); }
#define DXT1 0x31545844u
#define DXT5 0x35545844u
#define INTZ 0x5A544E49u
static uint32_t guid(uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t* d4) {
    uint32_t g = gbuf(16); wr32(g, d1); wr16(g + 4, d2); wr16(g + 6, d3); memcpy(M + g + 8, d4, 8); return g;
}

int main(int argc, char** argv) {
    rt_init();
    if (argc > 1) setenv("ME1_D3D_DUMP", argv[1], 1);
    setenv("ME1_SCREEN", "1600x900", 1);
    uint32_t out = gbuf(64), out2 = gbuf(64), tmp = gbuf(4096);

    /* IDirect3D9 */
    uint32_t d3d = api("d3d9.dll", "Direct3DCreate9", 1, 32u);
    CHECK(d3d && hle_is_thunk(rd32(rd32(d3d))), "Direct3DCreate9 -> %08x", d3d);
    CHECK(com(d3d, 4, 0) == 1, "GetAdapterCount");
    CHECK(com(d3d, 5, 3, 0u, 0u, tmp) == 0 && strstr((char*)M + tmp + 512, "GeForce"), "GetAdapterIdentifier");
    uint32_t caps = gbuf(304);
    CHECK(com(d3d, 14, 3, 0u, 1u, caps) == 0 && rd32(caps + 196) == 0xFFFE0300u && rd32(caps + 204) == 0xFFFF0300u, "GetDeviceCaps");
    CHECK(rd32(caps + 88) == 4096 && rd32(caps + 240) == 4, "caps: max texture %u, MRT %u", rd32(caps + 88), rd32(caps + 240));
    CHECK(com(d3d, 10, 6, 0u, 1u, 22u, 0u, 3u, DXT1) == 0, "CheckDeviceFormat DXT1 texture");
    CHECK(com(d3d, 10, 6, 0u, 1u, 22u, 2u, 1u, 75u) == 0, "CheckDeviceFormat D24S8 surface");
    CHECK(com(d3d, 10, 6, 0u, 1u, 22u, 1u, 3u, 113u) == 0, "CheckDeviceFormat A16B16G16R16F render target");
    CHECK(com(d3d, 10, 6, 0u, 1u, 22u, 2u, 3u, INTZ) != 0, "CheckDeviceFormat INTZ must be unavailable");
    CHECK(com(d3d, 10, 6, 0u, 1u, 22u, 1u, 3u, DXT1) != 0, "CheckDeviceFormat DXT1 render target must fail");
    CHECK(com(d3d, 10, 6, 0u, 1u, 22u, 0x80000u, 3u, 113u) == 0, "CheckDeviceFormat FP16 blending");
    uint32_t nm = com(d3d, 6, 2, 0u, 22u);
    CHECK(nm >= 3, "GetAdapterModeCount %u", nm);
    CHECK(com(d3d, 7, 4, 0u, 22u, nm - 1, out) == 0 && rd32(out) == 1600 && rd32(out + 4) == 900, "EnumAdapterModes last %ux%u", rd32(out), rd32(out + 4));
    CHECK(com(d3d, 8, 2, 0u, out) == 0 && rd32(out) == 1600 && rd32(out + 12) == 22, "GetAdapterDisplayMode");
    CHECK(com(d3d, 11, 6, 0u, 1u, 21u, 1u, 4u, out) == 0 && rd32(out) == 1, "CheckDeviceMultiSampleType 4x");
    CHECK(com(d3d, 11, 6, 0u, 1u, 21u, 1u, 8u, out) != 0, "CheckDeviceMultiSampleType 8x unsupported");
    static const uint8_t d4ex[8] = { 0xA1, 0x46, 0x7A, 0x27, 0x9C, 0x21, 0x8C, 0x9F };   /* IDirect3D9Ex {02177241-69FC-400C-8FF1-...} */
    CHECK(com(d3d, 0, 2, guid(0x02177241u, 0x69FC, 0x400C, d4ex), out) == 0x80004002u && rd32(out) == 0, "QueryInterface(IDirect3D9Ex) must fail");
    static const uint8_t d4unk[8] = { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    CHECK(com(d3d, 0, 2, guid(0, 0, 0, d4unk), out) == 0 && rd32(out) == d3d, "QueryInterface(IUnknown)");
    CHECK(com(d3d, 2, 0) == 1, "Release after QI -> 1");

    /* device */
    uint32_t pp = gbuf(56);
    wr32(pp + 8, 0); wr32(pp + 12, 1); wr32(pp + 24, 1); wr32(pp + 28, 0x10000); wr32(pp + 32, 1); wr32(pp + 36, 1); wr32(pp + 40, 75);
    CHECK(com(d3d, 16, 6, 0u, 1u, 0x10000u, 0x40u, pp, out) == 0 && rd32(out), "CreateDevice");
    uint32_t dev = rd32(out);
    CHECK(rd32(pp) == 1600 && rd32(pp + 4) == 900 && rd32(pp + 8) == 22, "present params filled: %ux%u fmt %u", rd32(pp), rd32(pp + 4), rd32(pp + 8));
    CHECK(com(dev, 3, 0) == 0, "TestCooperativeLevel");
    CHECK(com(dev, 4, 0) == 512u << 20, "GetAvailableTextureMem");
    CHECK(com(dev, 18, 4, 0u, 0u, 0u, out) == 0 && rd32(out), "GetBackBuffer");
    uint32_t bb = rd32(out);
    CHECK(com(bb, 12, 1, tmp) == 0 && rd32(tmp) == 22 && rd32(tmp + 8) == 1 && rd32(tmp + 24) == 1600 && rd32(tmp + 28) == 900, "back buffer desc");
    CHECK(com(dev, 38, 2, 0u, out2) == 0 && rd32(out2) == bb, "GetRenderTarget(0) == back buffer");
    com(bb, 2, 0); com(bb, 2, 0);
    CHECK(rd32(bb + 4) == 0x39443344u, "back buffer survives release (owned by the swap chain)");
    CHECK(com(dev, 40, 1, out) == 0 && rd32(out), "GetDepthStencilSurface (auto depth)");
    CHECK(com(rd32(out), 12, 1, tmp) == 0 && rd32(tmp) == 75 && rd32(tmp + 8) == 2, "auto depth desc fmt %u", rd32(tmp));
    com(rd32(out), 2, 0);
    CHECK(com(dev, 48, 1, tmp) == 0 && rd32(tmp + 8) == 1600 && rdf(tmp + 20) == 1.0f, "default viewport");
    CHECK(com(dev, 58, 2, 22u, out) == 0 && rd32(out) == 3, "default CULLMODE = CCW");
    CHECK(com(dev, 57, 2, 22u, 1u) == 0 && com(dev, 58, 2, 22u, out) == 0 && rd32(out) == 1, "Set/GetRenderState");
    CHECK(com(dev, 68, 3, 0u, 1u, out) == 0 && rd32(out) == 1, "default ADDRESSU = WRAP");
    CHECK(com(dev, 69, 3, 257u, 5u, 2u) == 0 && com(dev, 68, 3, 257u, 5u, out) == 0 && rd32(out) == 2, "vertex texture sampler state");

    /* textures */
    CHECK(com(dev, 23, 8, 256u, 256u, 0u, 0u, DXT1, 1u, out, 0u) == 0 && rd32(out), "CreateTexture DXT1 256 full chain");
    uint32_t tex = rd32(out);
    CHECK(com(tex, 13, 0) == 9, "GetLevelCount = 9 (%u)", com(tex, 13, 0));
    CHECK(com(tex, 10, 0) == 3, "GetType = TEXTURE");
    CHECK(com(tex, 17, 2, 3u, tmp) == 0 && rd32(tmp + 24) == 32 && rd32(tmp) == DXT1, "GetLevelDesc(3)");
    uint32_t lr = gbuf(8);
    CHECK(com(tex, 19, 4, 0u, lr, 0u, 0u) == 0 && rd32(lr) == 512 && rd32(lr + 4), "LockRect DXT1 level 0: pitch %u", rd32(lr));
    uint32_t bits = rd32(lr);
    bits = rd32(lr + 4); memset(M + bits, 0xAB, 512 * 64);
    uint32_t rect = gbuf(16); wr32(rect, 8); wr32(rect + 4, 16); wr32(rect + 8, 16); wr32(rect + 12, 24);
    CHECK(com(tex, 19, 4, 0u, lr, rect, 0u) == 0 && rd32(lr + 4) == bits + 4 * 512 + 2 * 8, "LockRect with rect offset");
    CHECK(com(tex, 20, 1, 0u) == 0 && com(tex, 20, 1, 0u) == 0, "UnlockRect (twice is harmless)");
    CHECK(com(tex, 18, 2, 1u, out) == 0 && rd32(out), "GetSurfaceLevel(1)");
    uint32_t s1 = rd32(out);
    CHECK(com(s1, 12, 1, tmp) == 0 && rd32(tmp + 24) == 128 && rd32(tmp + 28) == 128, "level 1 desc");
    CHECK(com(s1, 13, 3, lr, 0u, 0u) == 0 && rd32(lr) == 256, "surface LockRect pitch %u", rd32(lr));
    CHECK(com(s1, 14, 0) == 0, "surface UnlockRect");
    CHECK(com(s1, 11, 2, 0u, out2) == 0 && rd32(out2) == tex, "GetContainer");
    CHECK(com(tex, 2, 0) == 2, "refcount forwarded to the texture");
    CHECK(com(dev, 65, 2, 0u, tex) == 0, "SetTexture");
    CHECK(com(s1, 2, 0) == 1 && com(tex, 2, 0) == 0, "release surface + container ref -> texture refs 0");
    CHECK(rd32(tex + 4) == 0x39443344u, "texture bound to a stage stays alive");
    CHECK(com(dev, 64, 2, 0u, out) == 0 && rd32(out) == tex, "GetTexture");
    com(tex, 2, 0);
    CHECK(com(dev, 65, 2, 0u, 0u) == 0 && rd32(tex + 4) == 0xDEADD3D9u, "unbinding destroys it");
    CHECK(com(dev, 23, 8, 64u, 64u, 1u, 0x400u, 21u, 0u, out, 0u) == 0 && com(rd32(out), 13, 0) == 1, "AUTOGENMIPMAP -> 1 level");
    com(rd32(out), 2, 0);
    CHECK(com(dev, 23, 8, 64u, 64u, 1u, 0u, INTZ, 0u, out, 0u) != 0 && rd32(out) == 0, "CreateTexture INTZ fails");
    CHECK(com(dev, 25, 7, 64u, 0u, 0u, 21u, 1u, out, 0u) == 0, "CreateCubeTexture");
    uint32_t cube = rd32(out);
    CHECK(com(cube, 13, 0) == 7 && com(cube, 18, 3, 3u, 2u, out) == 0 && com(rd32(out), 12, 1, tmp) == 0 && rd32(tmp + 24) == 16, "cube face 3 level 2");
    com(rd32(out), 2, 0);
    CHECK(com(cube, 19, 5, 5u, 6u, lr, 0u, 0u) == 0 && rd32(lr) == 4 && com(cube, 20, 2, 5u, 6u) == 0, "cube LockRect 1x1");
    CHECK(com(cube, 2, 0) == 0 && rd32(cube + 4) == 0xDEADD3D9u, "cube released");
    CHECK(com(dev, 24, 9, 32u, 32u, 16u, 0u, 0u, 21u, 1u, out, 0u) == 0, "CreateVolumeTexture");
    uint32_t vt = rd32(out), lb = gbuf(12);
    CHECK(com(vt, 13, 0) == 6 && com(vt, 19, 4, 1u, lb, 0u, 0u) == 0 && rd32(lb) == 64 && rd32(lb + 4) == 64 * 16, "LockBox level 1");
    CHECK(com(vt, 20, 1, 1u) == 0 && com(vt, 2, 0) == 0, "volume texture released");

    /* render targets */
    CHECK(com(dev, 28, 8, 512u, 256u, 113u, 0u, 0u, 0u, out, 0u) == 0, "CreateRenderTarget FP16");
    uint32_t rt = rd32(out);
    CHECK(com(dev, 37, 2, 0u, rt) == 0 && com(dev, 48, 1, tmp) == 0 && rd32(tmp + 8) == 512 && rd32(tmp + 12) == 256, "SetRenderTarget resets viewport");
    CHECK(com(dev, 37, 2, 0u, 0u) != 0, "SetRenderTarget(0, NULL) is invalid");
    CHECK(com(dev, 37, 2, 1u, rt) == 0 && com(dev, 37, 2, 1u, 0u) == 0, "MRT slot 1");
    com(rt, 2, 0);
    CHECK(rd32(rt + 4) == 0x39443344u, "bound render target alive");

    /* buffers */
    CHECK(com(dev, 26, 6, 65536u, 0x208u, 0u, 0u, out, 0u) == 0, "CreateVertexBuffer dynamic");
    uint32_t vb = rd32(out), pv = gbuf(4);
    CHECK(com(vb, 11, 4, 0u, 0u, pv, 0x2000u) == 0 && rd32(pv), "VB Lock DISCARD");
    uint32_t p0 = rd32(pv); memset(M + p0, 1, 65536);
    CHECK(com(vb, 12, 0) == 0 && com(vb, 11, 4, 1024u, 64u, pv, 0x1000u) == 0 && rd32(pv) == p0 + 1024, "dynamic VB keeps its memory");
    com(vb, 12, 0);
    CHECK(com(vb, 13, 1, tmp) == 0 && rd32(tmp) == 100 && rd32(tmp + 16) == 65536 && rd32(tmp + 8) == 0x208, "VB GetDesc");
    CHECK(com(dev, 100, 4, 0u, vb, 0u, 32u) == 0 && com(dev, 101, 4, 0u, out, out2, tmp) == 0 && rd32(out) == vb && rd32(tmp) == 32, "Set/GetStreamSource");
    com(vb, 2, 0);
    CHECK(com(dev, 27, 6, 6000u, 8u, 101u, 0u, out, 0u) == 0, "CreateIndexBuffer static");
    uint32_t ib = rd32(out);
    CHECK(com(ib, 11, 4, 0u, 0u, pv, 0u) == 0 && rd32(pv) && com(ib, 12, 0) == 0, "IB Lock/Unlock");
    CHECK(com(ib, 13, 1, tmp) == 0 && rd32(tmp) == 101 && rd32(tmp + 4) == 7, "IB GetDesc");
    CHECK(com(dev, 104, 1, ib) == 0, "SetIndices");
    com(ib, 2, 0);

    /* declarations + shaders */
    uint32_t de = gbuf(32);
    wr16(de, 0); wr16(de + 2, 0); M[de + 4] = 2; M[de + 5] = 0; M[de + 6] = 0; M[de + 7] = 0;          /* FLOAT3 POSITION */
    wr16(de + 8, 0xFF); wr16(de + 10, 0); M[de + 12] = 17;                                              /* D3DDECL_END */
    CHECK(com(dev, 86, 2, de, out) == 0, "CreateVertexDeclaration");
    uint32_t decl = rd32(out);
    CHECK(com(decl, 4, 2, 0u, tmp) == 0 && rd32(tmp) == 2, "GetDeclaration count");
    CHECK(com(dev, 87, 1, decl) == 0 && com(dev, 88, 1, out) == 0 && rd32(out) == decl, "Set/GetVertexDeclaration");
    com(decl, 2, 0); com(decl, 2, 0);
    static const uint32_t vs[] = { 0xFFFE0300u, 0x0003FFFEu, 0x42415443u, 0, 0, 0x0200001Fu, 0x80000000u, 0x900F0000u,
                                   0x0200001Fu, 0x80000000u, 0xE00F0000u, 0x02000001u, 0xE00F0000u, 0x90E40000u, 0x0000FFFFu, 0xDEADBEEFu };
    uint32_t code = gbuf(sizeof vs); memcpy(M + code, vs, sizeof vs);
    CHECK(com(dev, 91, 2, code, out) == 0, "CreateVertexShader vs_3_0");
    uint32_t vso = rd32(out);
    CHECK(com(vso, 4, 2, 0u, tmp) == 0 && rd32(tmp) == 15 * 4, "GetFunction size %u (end token found)", rd32(tmp));
    CHECK(com(dev, 92, 1, vso) == 0, "SetVertexShader");
    static const uint32_t ps[] = { 0xFFFF0200u, 0x0200001Fu, 0x80000000u, 0xB00F0000u, 0x02000001u, 0x800F0800u, 0xB0E40000u, 0x0000FFFFu };
    uint32_t pcode = gbuf(sizeof ps); memcpy(M + pcode, ps, sizeof ps);
    CHECK(com(dev, 106, 2, pcode, out) == 0, "CreatePixelShader ps_2_0");
    uint32_t pso = rd32(out);
    CHECK(com(dev, 106, 2, pcode, out2) == 0, "same pixel shader again");
    com(rd32(out2), 2, 0);
    CHECK(com(dev, 106, 2, code, out2) != 0, "vertex bytecode rejected by CreatePixelShader");
    uint32_t cf = gbuf(64); for (int i = 0; i < 16; i++) wrfl(cf + 4 * i, (float)i);
    CHECK(com(dev, 94, 3, 250u, cf, 4u) == 0 && com(dev, 95, 3, 252u, tmp, 1u) == 0 && rdf(tmp + 4) == 9.0f, "VS constants F");
    CHECK(com(dev, 94, 3, 254u, cf, 4u) != 0, "VS constant range check");
    CHECK(com(dev, 109, 3, 0u, cf, 4u) == 0, "PS constants F");
    com(dev, 107, 1, pso); com(pso, 2, 0); com(vso, 2, 0);

    /* draws, queries, frames */
    CHECK(com(dev, 41, 0) == 0 && com(dev, 43, 6, 0u, 0u, 7u, 0u, 0x3F800000u, 0u) == 0, "BeginScene/Clear");
    CHECK(com(dev, 82, 6, 4u, 0u, 0u, 100u, 0u, 50u) == 0 && com(dev, 81, 3, 4u, 0u, 10u) == 0, "DrawIndexedPrimitive/DrawPrimitive");
    uint32_t upv = gbuf(3 * 16);
    CHECK(com(dev, 83, 4, 4u, 1u, upv, 16u) == 0 && com(dev, 101, 4, 0u, out, out2, tmp) == 0 && rd32(out) == 0, "DrawPrimitiveUP unbinds stream 0");
    CHECK(com(dev, 118, 2, 10u, 0u) != 0, "timestamp queries unavailable");
    CHECK(com(dev, 118, 2, 8u, 0u) == 0 && com(dev, 118, 2, 8u, out) == 0, "CreateQuery(EVENT)");
    uint32_t q = rd32(out);
    CHECK(com(q, 6, 1, 1u) == 0 && com(q, 7, 3, tmp, 4u, 1u) == 0 && rd32(tmp) == 1, "event query signalled");
    CHECK(com(q, 4, 0) == 8 && com(q, 5, 0) == 4, "query type/size");
    com(q, 2, 0);
    CHECK(com(dev, 42, 0) == 0 && com(dev, 17, 4, 0u, 0u, 0u, 0u) == 0, "EndScene/Present");
    CHECK(com(dev, 14, 2, 0u, out) == 0 && rd32(out), "GetSwapChain");
    uint32_t sc = rd32(out);
    CHECK(com(sc, 3, 5, 0u, 0u, 0u, 0u, 0u) == 0 && com(sc, 9, 1, tmp) == 0 && rd32(tmp) == 1600, "swap chain Present / params");
    com(sc, 2, 0);
    CHECK(com(dev, 59, 2, 1u, out) == 0 && com(rd32(out), 4, 0) == 0 && com(rd32(out), 5, 0) == 0 && com(rd32(out), 2, 0) == 0, "state block");
    CHECK(com(dev, 60, 0) == 0 && com(dev, 61, 1, out) == 0 && rd32(out), "Begin/EndStateBlock");
    com(rd32(out), 2, 0);
    com(dev, 80, 0);
    double npatch = g_cpu.st[g_cpu.ftop]; fpop(&g_cpu);
    CHECK(npatch == 0.0, "GetNPatchMode returns a float");
    uint32_t mat = gbuf(64); for (int i = 0; i < 16; i++) wrfl(mat + 4 * i, 0);
    wrfl(mat, 2); wrfl(mat + 20, 4); wrfl(mat + 40, 8); wrfl(mat + 60, 1); wrfl(mat + 48, 3);
    CHECK(com(dev, 44, 2, 2u, mat) == 0 && com(dev, 45, 2, 2u, tmp) == 0 && rdf(tmp + 48) == 3.0f, "Set/GetTransform");

    /* Reset */
    wr32(pp, 1280); wr32(pp + 4, 720);
    CHECK(com(dev, 16, 1, pp) == 0 && com(dev, 18, 4, 0u, 0u, 0u, out) == 0 && com(rd32(out), 12, 1, tmp) == 0 && rd32(tmp + 24) == 1280, "Reset to 1280x720");
    com(rd32(out), 2, 0);
    CHECK(com(dev, 58, 2, 22u, out) == 0 && rd32(out) == 3, "Reset restores default states");
    CHECK(rd32(rt + 4) == 0xDEADD3D9u, "render target unbound by Reset and destroyed");

    /* D3DX */
    uint32_t inv = gbuf(64), det = gbuf(4);
    CHECK(api("d3dx9_31.dll", "D3DXMatrixInverse", 3, inv, det, mat) == inv && rdf(det) == 64.0f, "D3DXMatrixInverse det %g", rdf(det));
    CHECK(fabsf(rdf(inv) - 0.5f) < 1e-6f && fabsf(rdf(inv + 48) + 1.5f) < 1e-6f && fabsf(rdf(inv + 40) - 0.125f) < 1e-6f, "inverse values");
    uint32_t sing = gbuf(64);
    CHECK(api("d3dx9_31.dll", "D3DXMatrixInverse", 3, inv, 0u, sing) == 0, "singular -> NULL");
    uint32_t err = gbuf(4), sh = gbuf(4); wr32(sh, 0x1234);
    CHECK(api("d3dx9_31.dll", "D3DXCompileShader", 10, code, 10u, 0u, 0u, gstrdup("Main"), gstrdup("ps_3_0"), 0u, sh, err, 0u) == 0x80004005u, "D3DXCompileShader fails");
    CHECK(rd32(sh) == 0 && rd32(err) && strstr((char*)M + com(rd32(err), 3, 0), "HLSL"), "error buffer text");
    CHECK(com(rd32(err), 2, 0) == 0, "ID3DXBuffer release");
    CHECK(api("d3dx9_31.dll", "D3DXDebugMute", 1, 1u) == 0, "D3DXDebugMute");

    CHECK(com(dev, 2, 0) == 0, "device release");
    CHECK(com(d3d, 2, 0) == 0, "IDirect3D9 release (device holds no external ref... %s)", "");
    /* DirectInput 8 (hle_input.c), the sequence of MassEffect.exe's input init (0x10d07b75..) + buffered/immediate reads */
    {
        static const uint8_t iid8w[8] = { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 }, sys[8] = { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
        uint32_t pdi = gbuf(4), pm = gbuf(4), pk = gbuf(4), dip = gbuf(20), ev = gbuf(20 * 16), n = gbuf(4), st = gbuf(256);
        CHECK(api("dinput8.dll", "DirectInput8Create", 5, 0x10900000u, 0x800u, guid(0xBF798031u, 0x483A, 0x4DA2, iid8w), pdi, 0u) == 0 && rd32(pdi),
              "DirectInput8Create");
        uint32_t di = rd32(pdi);
        CHECK(com(di, 3, 3, guid(0x6F1D2B60u, 0xD5A0, 0x11CF, sys), pm, 0u) == 0 && rd32(pm), "CreateDevice(GUID_SysMouse)");
        CHECK(com(di, 3, 3, guid(0x6F1D2B61u, 0xD5A0, 0x11CF, sys), pk, 0u) == 0 && rd32(pk) && rd32(pk) != rd32(pm), "CreateDevice(GUID_SysKeyboard)");
        CHECK(com(di, 3, 3, guid(0x12345678u, 0, 0, sys), out, 0u) == 0x80040154u && !rd32(out), "CreateDevice(unknown) -> DIERR_DEVICENOTREG");
        uint32_t m = rd32(pm), k = rd32(pk);
        uint32_t fmt = gbuf(24); wr32(fmt, 24); wr32(fmt + 12, 20);
        CHECK(com(m, 11, 1, fmt) == 0, "mouse SetDataFormat");
        wr32(dip, 20); wr32(dip + 4, 16); wr32(dip + 16, 16);
        CHECK(com(m, 6, 2, 1u, dip) == 0, "SetProperty(DIPROP_BUFFERSIZE, 16)");
        wr32(dip + 16, 1); CHECK(com(m, 6, 2, 2u, dip) == 0, "SetProperty(DIPROP_AXISMODE, REL)");
        wr32(dip + 16, 0); CHECK(com(m, 5, 2, 1u, dip) == 0 && rd32(dip + 16) == 16, "GetProperty(DIPROP_BUFFERSIZE)");
        CHECK(com(di, 4, 4, 4u, 0x1093c7b8u, 0u, 1u) == 0, "EnumDevices(GAMECTRL): no callbacks");
        uint32_t dc = gbuf(44); wr32(dc, 44);
        CHECK(com(m, 3, 1, dc) == 0 && rd32(dc + 4) == 1 && (rd32(dc + 8) & 0xFF) == 0x12 && rd32(dc + 16) == 8, "mouse GetCapabilities");
        CHECK(com(m, 13, 2, 0x10000u, 6u) == 0 && com(m, 7, 0) == 0, "SetCooperativeLevel + Acquire");
        hle_input_mouse(5, -3, 0, 1);
        wr32(n, 16);
        CHECK(com(m, 10, 4, 20u, ev, n, 0u) == 0 && rd32(n) == 3, "GetDeviceData: %u events", rd32(n));
        CHECK(rd32(ev) == 0 && rd32(ev + 4) == 5 && rd32(ev + 20) == 4 && rd32(ev + 24) == 0xFFFFFFFDu && rd32(ev + 40) == 12 && rd32(ev + 44) == 0x80,
              "events: X +5, Y -3, button 0 down");
        wr32(n, 16); CHECK(com(m, 10, 4, 20u, ev, n, 0u) == 0 && rd32(n) == 0, "ring drained");
        CHECK(com(m, 9, 2, 20u, st) == 0 && rd32(st) == 5 && rd32(st + 4) == 0xFFFFFFFDu && M[st + 12] == 0x80, "GetDeviceState(DIMOUSESTATE2)");
        CHECK(com(m, 9, 2, 20u, st) == 0 && rd32(st) == 0 && M[st + 12] == 0x80, "deltas reset, button still down");
        hle_input_key(0x1E, 1);
        CHECK(com(k, 9, 2, 256u, st) == 0 && M[st + 0x1E] == 0x80 && M[st + 0x1F] == 0, "keyboard GetDeviceState: DIK_A down");
        CHECK(com(m, 8, 0) == 0 && com(m, 2, 0) == 0 && com(k, 2, 0) == 0 && com(di, 2, 0) <= 1, "Unacquire + Release");
    }
    {   /* user32 window manager (hle_exe.c) */
        uint32_t wc = gbuf(48), r = gbuf(16), pid = gbuf(4), name = hle_wstr("UnrealWindow"), title = hle_wstr("Mass Effect");
        uint32_t mbox = hle_resolve_impl("user32.dll", "MessageBoxW");
        wr32(wc + 4, mbox); wr32(wc + 36, name);   /* WNDCLASSW: a thunk as window procedure (like DefWindowProcW) */
        uint32_t atom = api("user32.dll", "RegisterClassW", 1, wc);
        CHECK(atom >= 0xC000, "RegisterClassW -> atom %x", atom);
        CHECK(api("user32.dll", "RegisterClassW", 1, wc) == 0, "RegisterClassW twice fails");
        CHECK(api("user32.dll", "GetForegroundWindow", 0) == 0, "no foreground window before creation");
        uint32_t h = api("user32.dll", "CreateWindowExW", 12, 0x40000u, hle_wstr("unrealwindow"), title, 0x00CF0000u, 10u, 20u, 1600u, 720u, 0u, 0u, 0x10900000u, 0u);
        CHECK(h != 0 && api("user32.dll", "IsWindow", 1, h), "CreateWindowExW -> %08x", h);
        CHECK(api("user32.dll", "GetClientRect", 2, h, r) && rd32(r) == 0 && rd32(r + 8) == 1600 && rd32(r + 12) == 720, "GetClientRect 1600x720");
        CHECK(api("user32.dll", "GetWindowRect", 2, h, r) && rd32(r) == 10 && rd32(r + 4) == 20 && rd32(r + 8) == 1610 && rd32(r + 12) == 740, "GetWindowRect");
        CHECK(api("user32.dll", "ShowWindow", 2, h, 5u) == 0 && api("user32.dll", "ShowWindow", 2, h, 5u) == 1, "ShowWindow returns previous visibility");
        CHECK(api("user32.dll", "GetForegroundWindow", 0) == h && api("user32.dll", "GetActiveWindow", 0) == h && api("user32.dll", "GetFocus", 0) == h,
              "shown window is foreground/active/focus");
        wr32(pid, 0);
        CHECK(api("user32.dll", "GetWindowThreadProcessId", 2, h, pid) == api("kernel32.dll", "GetCurrentThreadId", 0) &&
              rd32(pid) == api("kernel32.dll", "GetCurrentProcessId", 0), "GetWindowThreadProcessId: our thread/process");
        CHECK(api("user32.dll", "SetWindowLongW", 3, h, (uint32_t)-21, 0x1234u) == 0 && api("user32.dll", "GetWindowLongW", 2, h, (uint32_t)-21) == 0x1234u,
              "GWL_USERDATA");
        CHECK(api("user32.dll", "SetWindowLongW", 3, h, (uint32_t)-16, 0x16CF0000u) == 0x00CF0000u, "SetWindowLongW(GWL_STYLE) -> old style");
        CHECK(api("user32.dll", "SetWindowPos", 7, h, 0u, 0u, 0u, 1280u, 600u, 0x2u) && api("user32.dll", "GetClientRect", 2, h, r) &&
              rd32(r + 8) == 1280 && rd32(r + 12) == 600, "SetWindowPos resizes (SWP_NOMOVE)");
        CHECK(api("user32.dll", "SendMessageW", 4, h, hle_wstr("SendMessage reached the window procedure"), hle_wstr("[test]"), 4u) == 6,
              "SendMessageW calls the window procedure with (hwnd, msg, wp, lp)");
        {   /* front-end input: pointer in back buffer pixels -> client/screen coordinates, queued messages, key state */
            uint32_t msg = gbuf(28), pt = gbuf(8);
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0u, 0u, 1u) == 0, "PeekMessage: empty queue");
            hle_ui_pointer(400, 300, 800, 600, 1);
            CHECK(api("user32.dll", "GetCursorPos", 1, pt) && rd32(pt) == 650 && rd32(pt + 4) == 320, "GetCursorPos after touch: %u,%u", rd32(pt), rd32(pt + 4));
            CHECK(api("user32.dll", "GetAsyncKeyState", 1, 1u) & 0x8000u, "GetAsyncKeyState(VK_LBUTTON) down");
            CHECK(!(api("user32.dll", "GetKeyState", 1, 1u) & 0x8000u), "GetKeyState(VK_LBUTTON) waits for WM_LBUTTONDOWN");
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0u, 0u, 0u) == 1 && rd32(msg + 4) == 0x200 && rd32(msg) == h, "PM_NOREMOVE: WM_MOUSEMOVE");
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0x201u, 0x201u, 1u) == 0, "WM_LBUTTONDOWN held back until the cursor jump was seen");
            { struct timespec ts = { 0, 410000000 }; nanosleep(&ts, NULL); }   /* no frames are presented here: the 400 ms limit */
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0x201u, 0x201u, 1u) == 1 && rd32(msg + 4) == 0x201 && rd32(msg + 12) == ((300u << 16) | 640u),
                  "filtered PeekMessage: WM_LBUTTONDOWN at client 640,300 (lParam %08x)", rd32(msg + 12));
            CHECK(api("user32.dll", "GetKeyState", 1, 1u) & 0x8000u, "GetKeyState(VK_LBUTTON) down once WM_LBUTTONDOWN was retrieved");
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0u, 0u, 1u) == 1 && rd32(msg + 4) == 0x200, "then the WM_MOUSEMOVE");
            CHECK(api("user32.dll", "DispatchMessageW", 1, msg) == 1, "DispatchMessageW calls the window procedure");
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0u, 0u, 1u) == 0, "queue drained");
            hle_ui_pointer(400, 300, 800, 600, 2);
            CHECK(!(api("user32.dll", "GetAsyncKeyState", 1, 1u) & 0x8000u), "VK_LBUTTON released (async)");
            CHECK(api("user32.dll", "GetKeyState", 1, 1u) & 0x8000u, "GetKeyState(VK_LBUTTON) still down until WM_LBUTTONUP");
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0x202u, 0x202u, 1u) == 0, "WM_LBUTTONUP held: the button stays down a few frames");
            { struct timespec ts = { 0, 410000000 }; nanosleep(&ts, NULL); }
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0x202u, 0x202u, 1u) == 1 && rd32(msg + 4) == 0x202, "then WM_LBUTTONUP");
            CHECK(!(api("user32.dll", "GetKeyState", 1, 1u) & 0x8000u), "GetKeyState(VK_LBUTTON) up after WM_LBUTTONUP");
            while (api("user32.dll", "PeekMessageW", 5, msg, 0u, 0u, 0u, 1u)) {}
            hle_ui_key(0x1B, 0x01, 1, 0);
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0x100u, 0x108u, 1u) == 1 && rd32(msg + 4) == 0x100 && rd32(msg + 8) == 0x1B &&
                  (rd32(msg + 12) >> 16 & 255) == 1, "WM_KEYDOWN VK_ESCAPE, scan code 1");
            CHECK(api("user32.dll", "GetKeyState", 1, 0x1Bu) & 0x8000u, "GetKeyState(VK_ESCAPE) down");
            hle_ui_key(0x1B, 0x01, 0, 0);
            while (api("user32.dll", "PeekMessageW", 5, msg, 0u, 0u, 0u, 1u)) {}
            /* on-screen fire button: a click at the current cursor, the cursor does not move; look area: no messages */
            hle_ui_mouse_rel(25, -7);
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0u, 0u, 1u) == 0, "look area: no window messages");
            hle_ui_mouse_button(1);
            CHECK(api("user32.dll", "GetAsyncKeyState", 1, 1u) & 0x8000u, "fire button: VK_LBUTTON down");
            { struct timespec ts = { 0, 410000000 }; nanosleep(&ts, NULL); }
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0x201u, 0x201u, 1u) == 1 && rd32(msg + 12) == ((300u << 16) | 640u),
                  "fire button: WM_LBUTTONDOWN at the cursor (lParam %08x)", rd32(msg + 12));
            CHECK(api("user32.dll", "GetCursorPos", 1, pt) && rd32(pt) == 650 && rd32(pt + 4) == 320, "cursor unchanged");
            hle_ui_mouse_button(2);
            { struct timespec ts = { 0, 410000000 }; nanosleep(&ts, NULL); }
            CHECK(api("user32.dll", "PeekMessageW", 5, msg, 0u, 0x202u, 0x202u, 1u) == 1, "fire button: WM_LBUTTONUP");
            while (api("user32.dll", "PeekMessageW", 5, msg, 0u, 0u, 0u, 1u)) {}
        }
        uint32_t child = api("user32.dll", "CreateWindowExW", 12, 0u, atom, 0u, 0x40000000u, 0u, 0u, 100u, 100u, h, 7u, 0x10900000u, 0u);
        CHECK(child && api("user32.dll", "GetParent", 1, child) == h && api("user32.dll", "GetWindowLongW", 2, child, (uint32_t)-12) == 7,
              "child window by atom: parent + id");
        uint32_t h2 = api("user32.dll", "CreateWindowExA", 12, 0u, gstrdup("STATIC"), 0u, 0x90000000u, 0u, 0u, 0x80000000u, 0u, 0u, 0u, 0u, 0u);
        CHECK(h2 && api("user32.dll", "GetForegroundWindow", 0) == h2 && api("user32.dll", "GetClientRect", 2, h2, r) && rd32(r + 8) > 0,
              "visible popup (system class, CW_USEDEFAULT) becomes foreground");
        CHECK(api("user32.dll", "ShowWindow", 2, h2, 0u) == 1 && api("user32.dll", "GetForegroundWindow", 0) == h,
              "hiding the foreground popup (splash) falls back to the game window");
        CHECK(api("user32.dll", "SetForegroundWindow", 1, h2) && api("user32.dll", "GetForegroundWindow", 0) == h2, "SetForegroundWindow");
        CHECK(api("user32.dll", "DestroyWindow", 1, h2) && api("user32.dll", "GetForegroundWindow", 0) == h && !api("user32.dll", "IsWindow", 1, h2),
              "DestroyWindow: foreground falls back to the game window");
        CHECK(api("user32.dll", "DestroyWindow", 1, h) && !api("user32.dll", "IsWindow", 1, child) && api("user32.dll", "GetForegroundWindow", 0) == 0,
              "DestroyWindow destroys children");
        CHECK(!api("user32.dll", "DestroyWindow", 1, h), "DestroyWindow(invalid) fails");
        CHECK(api("user32.dll", "UnregisterClassW", 2, name, 0u) == 1, "UnregisterClassW");
    }
    {   /* D3DXLoadSurfaceFromMemory (v0.9.3): Scaleform fills all its textures (glyph cache, menu bitmaps) through it */
        #define LSFM(dst, drect, src, fmt, pitch, srect, key) api("d3dx9_31.dll", "D3DXLoadSurfaceFromMemory", 10, dst, 0u, drect, src, fmt, pitch, 0u, srect, 0xFFFFFFFFu, key)
        uint32_t o2 = gbuf(16), lr = gbuf(8), sr = gbuf(16), dr = gbuf(16), src = gbuf(256);
        CHECK(com(dev, 23, 8, 4u, 4u, 1u, 0u, 21u, 2u, o2, 0u) == 0 && rd32(o2), "CreateTexture A8R8G8B8 SYSTEMMEM 4x4");
        uint32_t t = rd32(o2); CHECK(com(t, 18, 2, 0u, o2) == 0, "GetSurfaceLevel"); uint32_t s = rd32(o2);
        for (int i = 0; i < 16; i++) wr32(src + 4 * i, 0x80000000u | (uint32_t)i << 16 | (uint32_t)(i * 2) << 8 | (uint32_t)(i * 3));   /* A8B8G8R8: R in the low byte */
        wr32(sr, 0); wr32(sr + 4, 0); wr32(sr + 8, 4); wr32(sr + 12, 4);
        CHECK(LSFM(s, 0u, src, 32u, 16u, sr, 0u) == 0, "A8B8G8R8 -> A8R8G8B8");
        CHECK(com(t, 19, 4, 0u, lr, 0u, 0x10u) == 0, "LockRect");
        int ok = 1; for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { int i = y * 4 + x; uint32_t v = rd32(rd32(lr + 4) + rd32(lr) * (uint32_t)y + 4u * (uint32_t)x);
            if (v != (0x80000000u | (uint32_t)(i * 3) << 16 | (uint32_t)(i * 2) << 8 | (uint32_t)i)) ok = 0; }
        com(t, 20, 1, 0u); CHECK(ok, "A8B8G8R8 -> A8R8G8B8: R and B swapped, A and G kept");
        CHECK(com(dev, 23, 8, 8u, 8u, 1u, 0u, 28u, 2u, o2, 0u) == 0 && rd32(o2), "CreateTexture A8 SYSTEMMEM 8x8");
        uint32_t ta = rd32(o2); com(ta, 18, 2, 0u, o2); uint32_t sa = rd32(o2);
        CHECK(com(ta, 19, 4, 0u, lr, 0u, 0u) == 0, "LockRect A8"); memset(M + rd32(lr + 4), 0x11, rd32(lr) * 8); com(ta, 20, 1, 0u);
        for (int i = 0; i < 64; i++) M[src + i] = (uint8_t)(0xA0 + i);
        wr32(sr, 1); wr32(sr + 4, 1); wr32(sr + 8, 4); wr32(sr + 12, 3); wr32(dr, 2); wr32(dr + 4, 5); wr32(dr + 8, 5); wr32(dr + 12, 7);
        CHECK(LSFM(sa, dr, src, 28u, 8u, sr, 0u) == 0, "A8 -> A8, sub-rectangles");
        com(ta, 19, 4, 0u, lr, 0u, 0x10u); ok = 1;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { uint8_t v = M[rd32(lr + 4) + rd32(lr) * (uint32_t)y + (uint32_t)x];
            uint8_t e = x >= 2 && x < 5 && y >= 5 && y < 7 ? (uint8_t)(0xA0 + (y - 5 + 1) * 8 + (x - 2 + 1)) : 0x11; if (v != e) ok = 0; }
        com(ta, 20, 1, 0u); CHECK(ok, "A8 sub-rectangle copied, the rest untouched");
        CHECK(com(dev, 23, 8, 4u, 4u, 1u, 0u, 51u, 2u, o2, 0u) == 0, "CreateTexture A8L8"); uint32_t tl = rd32(o2); com(tl, 18, 2, 0u, o2);
        wr32(sr, 0); wr32(sr + 4, 0); wr32(sr + 8, 2); wr32(sr + 12, 2);
        CHECK(LSFM(rd32(o2), 0u, src, 28u, 8u, sr, 0u) == 0, "A8 2x2 -> A8L8 4x4 (point scaling, as the GFx A8L8 fallback)");
        com(tl, 19, 4, 0u, lr, 0u, 0x10u);
        CHECK(M[rd32(lr + 4) + 1] == 0xA0 && M[rd32(lr + 4) + 2 * 3 + 1] == 0xA1 && M[rd32(lr + 4) + rd32(lr) * 3 + 1] == 0xA8 &&
              M[rd32(lr + 4) + rd32(lr) * 3 + 2 * 3 + 1] == 0xA9, "A8L8: alpha from A8, scaled 2x");
        com(tl, 20, 1, 0u);
        wr32(src, 0xFFFF00FFu); wr32(src + 4, 0xFF00FF00u);   /* colour key (A8R8G8B8 source) */
        wr32(sr, 0); wr32(sr + 4, 0); wr32(sr + 8, 2); wr32(sr + 12, 1); wr32(dr, 0); wr32(dr + 4, 0); wr32(dr + 8, 2); wr32(dr + 12, 1);
        CHECK(LSFM(s, dr, src, 21u, 8u, sr, 0xFFFF00FFu) == 0, "colour key");
        com(t, 19, 4, 0u, lr, 0u, 0x10u);
        CHECK(rd32(rd32(lr + 4)) == 0 && rd32(rd32(lr + 4) + 4) == 0xFF00FF00u, "colour key -> transparent black");
        com(t, 20, 1, 0u);
        CHECK(LSFM(s, 0u, src, 21u, 16u, 0u, 0u) == 0x8876086Cu, "no source rectangle -> D3DERR_INVALIDCALL");
        CHECK(LSFM(s, 0u, src, 41u, 16u, sr, 0u) == 0x8876086Cu, "P8 source -> D3DERR_INVALIDCALL");
        com(tl, 2, 0); com(ta, 2, 0); com(t, 2, 0);
        #undef LSFM
    }
    {   /* NxCooking (v0.9.1): NxGetCookingParams() has no hidden return pointer - the stale cdecl argument left on the
         * stack by the game (its PhysX allocator object) must not be overwritten */
        uint32_t obj = gbuf(16); wr32(obj, 0x11a45d2cu); wr32(obj + 4, 0x12345678u);
        uint32_t p = api("NxCooking.dll", "NxGetCookingParams", 1, obj);
        CHECK(rd32(obj) == 0x11a45d2cu && rd32(obj + 4) == 0x12345678u, "NxGetCookingParams leaves [esp+4] alone");
        CHECK(p && p != obj && rd32(p) == 0 && rdf(p + 4) == 0.025f, "NxGetCookingParams -> {PLATFORM_PC, 0.025}");
        uint32_t q = gbuf(16); wr32(q, 0); wrfl(q + 4, 0.05f); wr32(q + 8, 1);
        CHECK(api("NxCooking.dll", "NxSetCookingParams", 1, q) == 1 && rdf(p + 4) == 0.05f && rd32(p + 8) == 1 &&
              api("NxCooking.dll", "NxGetCookingParams", 0) == p, "NxSetCookingParams is seen by NxGetCookingParams");
    }
    {   /* XInput (v0.9.7): pad 0 appears with the first hle_xinput_set (ME1_XINPUT unset = auto), users 1..3 never */
        uint32_t st = gbuf(16), vib = gbuf(4);
        CHECK(api("xinput1_3.dll", "#2", 2, 0u, st) == 1167, "XInputGetState before any pad input -> not connected");
        CHECK(api("xinput1_3.dll", "#3", 2, 0u, vib) == 1167, "XInputSetState without a pad -> not connected");
        hle_xinput_set(0x1000 | 0x0010, 10, 300, 40000, -12345, 0, 32767);
        memset(M + st, 0xAA, 16);
        CHECK(api("xinput1_3.dll", "#2", 2, 0u, st) == 0, "XInputGetState(0) after pad input");
        CHECK(rd16(st + 4) == 0x1010 && M[st + 6] == 10 && M[st + 7] == 255, "XINPUT_GAMEPAD buttons %04x, triggers %u/%u", rd16(st + 4), M[st + 6], M[st + 7]);
        CHECK((int16_t)rd16(st + 8) == 32767 && (int16_t)rd16(st + 10) == -12345 && rd16(st + 12) == 0 && rd16(st + 14) == 32767, "XINPUT_GAMEPAD sticks clamped");
        uint32_t pk = rd32(st);
        hle_xinput_set(0x1010, 10, 255, 32767, -12345, 0, 32767);   /* same state: packet number unchanged */
        api("xinput1_3.dll", "#2", 2, 0u, st);
        CHECK(rd32(st) == pk, "unchanged state keeps the packet number");
        hle_xinput_set(0x2000, 0, 0, 0, 0, 0, 0);
        api("xinput1_3.dll", "#2", 2, 0u, st);
        CHECK(rd32(st) != pk && rd16(st + 4) == 0x2000, "new state -> new packet number");
        CHECK(api("xinput1_3.dll", "#2", 2, 1u, st) == 1167 && api("xinput1_3.dll", "#2", 2, 3u, st) == 1167, "users 1..3 are not connected");
        wr16(vib, 0x8000); wr16(vib + 2, 0x1234);
        CHECK(api("xinput1_3.dll", "#3", 2, 0u, vib) == 0 && hle_xinput_vibration() == (int)(0x8000u << 16 | 0x1234u), "XInputSetState -> rumble %08x", hle_xinput_vibration());
        hle_xinput_connect(0);
        CHECK(api("xinput1_3.dll", "#2", 2, 0u, st) == 1167 && hle_xinput_vibration() == 0, "hle_xinput_connect(0) disconnects + stops rumble");
        hle_xinput_connect(1);
        CHECK(api("xinput1_3.dll", "#2", 2, 0u, st) == 0 && rd16(st + 4) == 0, "reconnected pad starts neutral");
    }
    {   /* launcher "Авто" preset (v0.9.7): device classification */
        static const struct { const char* gpu; const char* soc; int ram, mhz, sw, sh, tier, fps; const char* res; } T[] = {
            { "Mali-G52 MC2", "MT6769", 3700, 2000, 1600, 720, 1, 30, "960x540" },            /* Helio G85 (the reference phone) */
            { "Adreno (TM) 750", "SM8650", 11500, 3300, 2400, 1080, 4, 60, "1920x1080" },      /* Snapdragon 8 Gen 3 */
            { "Adreno (TM) 610", "SM6115", 3800, 2000, 1600, 720, 1, 30, "960x540" },
            { "Adreno (TM) 643", "SM7325", 7500, 2400, 2400, 1080, 2, 30, "1280x720" },
            { "Mali-G610 MC6", "MT6895", 7600, 3100, 2400, 1080, 3, 45, "1600x900" },
            { "PowerVR Rogue GE8320", "MT6765", 2900, 2300, 1520, 720, 0, 30, "640x360" },
            { "Adreno (TM) 740", "SM8550", 3700, 3200, 2400, 1080, 1, 30, "960x540" },         /* RAM cap */
            { "Adreno (TM) 750", "SM8650", 11500, 3300, 1280, 720, 4, 60, "1280x720" },        /* screen cap */
            { "", "kalama", 7500, 3200, 2400, 1080, 4, 60, "1920x1080" },                      /* no GL_RENDERER: SoC */
            { "SwiftShader", "", 0, 0, 0, 0, 1, 30, "960x540" },                               /* unknown: safe default */
        };
        for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
            ME1Device d = { T[i].gpu, T[i].soc, T[i].ram, 8, T[i].mhz, T[i].sw, T[i].sh };
            char rep[512], res[32]; int fps = 0, t = me1cfg_classify(&d, rep, sizeof rep, &fps, res, sizeof res);
            CHECK(t == T[i].tier && fps == T[i].fps && !strcmp(res, T[i].res), "auto preset %s/%s: tier %d fps %d %s (%s)", T[i].gpu, T[i].soc, t, fps, res, rep);
        }
    }
    hle_d3d9_report();
    fprintf(stderr, "test_d3d9: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
