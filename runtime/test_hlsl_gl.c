/* HLSL -> D3D9 bytecode (recompiled d3dx9_31.dll, as the game compiles its shaders) -> sm3_glsl -> GLES program, drawn
 * through the D3D9 HLE with pixel read-back. Covers what UE3 shaders use: matrices, skinning with relative constant
 * addressing, loops over int constants, bool/dynamic branches, clip, tex2Dlod/bias/proj/grad, texCUBE, VPOS/VFACE,
 * MRT, depth output, ps_2_0. usage: test_hlsl_gl <msvcr80.dll|image.bin> <d3dx9_31.dll|image.bin> */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

extern const FuncEntry crt_funcs[]; extern const unsigned crt_nfuncs;
extern const ImportEntry crt_imports[]; extern const unsigned crt_nimports;
extern const ExportEntry crt_exports[]; extern const uint32_t crt_base, crt_entry, crt_image_size, crt_image_crc;
extern const FuncEntry dx_funcs[]; extern const unsigned dx_nfuncs;
extern const ImportEntry dx_imports[]; extern const unsigned dx_nimports;
extern const ExportEntry dx_exports[]; extern const uint32_t dx_base, dx_entry, dx_image_size, dx_image_crc;
void me1gl_stats(uint64_t out[5]);
static Module crt, dx;
static int g_fail, g_ok;
#define CHECK(cond, ...) do { if (cond) g_ok++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static uint32_t call(uint32_t t, int n, const uint32_t* a, const char* what) {
    uint32_t sp = g_cpu.esp, r = rt_call(&g_cpu, t, n, a);
    if (g_cpu.esp != sp) { fprintf(stderr, "FAIL %s: esp\n", what); g_fail++; g_cpu.esp = sp; }
    return r;
}
static uint32_t dxf(const char* name, int n, ...) {
    uint32_t t = rt_find_export(&dx, name), a[16]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return t ? call(t, n, a, name) : 0xFFFFFFFFu;
}
static uint32_t crtf(const char* name, int n, ...) {
    uint32_t t = rt_find_export(&crt, name), a[16]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return t ? call(t, n, a, name) : 0xFFFFFFFFu;
}
/* v0.9.2: msvcr80 memmove/memcpy tail copies use `neg ecx; jmp [ecx*4+table]` (table entries before the
 * displacement); the recompiler used to emit only 4 of the 7 targets -> "dispatch: no function at target".
 * Exhaustive check: every length 0..80, src/dst misalignment 0..3, forward/backward overlap, disjoint. */
static void test_crt_memmove(void) {
    uint32_t g = gbuf(512); uint8_t ref[512]; int bad = 0, n_ok = 0;
    const char* fn[2] = { "memmove", "memcpy" };
    for (int f = 0; f < 2; f++)
    for (int len = 0; len <= 80; len++)
    for (int sa = 0; sa < 4; sa++)
    for (int d = -12; d <= 12; d++) {
        if (f == 1 && d > -len && d < len && d != 0) continue;   /* memcpy: overlap undefined (but MS memcpy == memmove) */
        int src = 200 + sa, dst = src + d;
        for (int i = 0; i < 512; i++) M[g + i] = ref[i] = (uint8_t)(i * 7 + 3);
        memmove(ref + dst, ref + src, (size_t)len);
        uint32_t r = crtf(fn[f], 3, g + (uint32_t)dst, g + (uint32_t)src, (uint32_t)len);
        if (r != g + (uint32_t)dst || memcmp(M + g, ref, 512)) { if (bad++ < 5) fprintf(stderr, "FAIL %s len=%d sa=%d d=%d\n", fn[f], len, sa, d); }
        else n_ok++;
    }
    CHECK(bad == 0, "crt memmove/memcpy: %d bad cases (%d ok)", bad, n_ok);
}
static uint32_t api(const char* dll, const char* name, int n, ...) {
    uint32_t t = hle_resolve_impl(dll, name), a[24]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return t ? call(t, n, a, name) : 0;
}
static uint32_t com(uint32_t o, int k, int n, ...) {
    uint32_t a[24]; va_list ap; va_start(ap, n);
    a[0] = o; for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return call(rd32(rd32(o) + 4 * k), n + 1, a, "method");
}
static uint32_t gbuf(uint32_t n) { uint32_t a = ghalloc(n, 1); memset(M + a, 0, n); return a; }
static uint32_t gstr(const char* s) { uint32_t n = (uint32_t)strlen(s) + 1, g = ghalloc(n, 1); memcpy(M + g, s, n); return g; }
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void wrfl(uint32_t a, float f) { wr32(a, fb(f)); }

static uint32_t dev, out, sysrb, bb;
static uint32_t pix[64 * 64];
/* compile HLSL with the recompiled D3DX and create the D3D9 shader */
static uint32_t shader(const char* name, const char* src, const char* prof) {
    uint32_t o = gbuf(12);
    uint32_t hr = dxf("D3DXCompileShader", 10, gstr(src), (uint32_t)strlen(src), 0u, 0u, gstr("Main"), gstr(prof), 0u, o, o + 4, 0u);
    if (hr || !rd32(o)) {
        uint32_t e = rd32(o + 4);
        fprintf(stderr, "FAIL compile %s (%s): %08x %s\n", name, prof, hr, e ? (char*)M + com(e, 3, 0) : ""); g_fail++; return 0;
    }
    uint32_t code = com(rd32(o), 3, 0);
    int ps = prof[0] == 'p';
    CHECK(com(dev, ps ? 106 : 91, 2, code, out) == 0 && rd32(out), "Create%sShader %s", ps ? "Pixel" : "Vertex", name);
    return rd32(out);
}
static uint32_t asmsh(const char* name, const char* src, int ps) {
    /* D3DXAssembleShader(src, len, defines, include, flags, &shader, &errors) */
    uint32_t o = gbuf(8);
    uint32_t hr = dxf("D3DXAssembleShader", 7, gstr(src), (uint32_t)strlen(src), 0u, 0u, 0u, o, o + 4);
    CHECK(hr == 0 && rd32(o), "assemble %s: %08x %s", name, hr, rd32(o + 4) ? (char*)M + com(rd32(o + 4), 3, 0) : "");
    if (!rd32(o)) return 0;
    CHECK(com(dev, ps ? 106 : 91, 2, com(rd32(o), 3, 0), out) == 0, "Create%sShader %s", ps ? "Pixel" : "Vertex", name);
    return rd32(out);
}
static uint32_t asmps(const char* name, const char* src) { return asmsh(name, src, 1); }
static void grab(uint32_t rt) {
    memset(pix, 0xCD, sizeof pix);
    CHECK(com(dev, 32, 2, rt, sysrb) == 0, "GetRenderTargetData");
    uint32_t lr = gbuf(8); com(sysrb, 13, 3, lr, 0u, 0x10u);
    for (int y = 0; y < 64; y++) memcpy(pix + 64 * y, M + rd32(lr + 4) + rd32(lr) * y, 256);
    com(sysrb, 14, 0);
}
static int near(uint32_t a, uint32_t b, int tol) { for (int i = 0; i < 24; i += 8) if (abs((int)(a >> i & 255) - (int)(b >> i & 255)) > tol) return 0; return 1; }
#define EXPECT(x, y, col, tol, what) do { uint32_t p_ = pix[(y) * 64 + (x)]; CHECK(near(p_, (col), tol), "%s: pixel (%d,%d) = %06x, expected %06x", what, x, y, p_ & 0xFFFFFF, (uint32_t)(col)); } while (0)
static uint32_t vtx;
/* vertex: float3 pos, float3 normal, float2 uv, ubyte4 indices, ubyte4n weights (40 bytes) */
static void quad(float x0, float y0, float x1, float y1, float z, int bone) {
    float v[4][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } }, uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; i++) {
        uint32_t a = vtx + 40 * i;
        wrfl(a, v[i][0]); wrfl(a + 4, v[i][1]); wrfl(a + 8, z); wrfl(a + 12, 0); wrfl(a + 16, 0); wrfl(a + 20, 1);
        wrfl(a + 24, uv[i][0]); wrfl(a + 28, uv[i][1]);
        wr32(a + 32, (uint32_t)bone | (uint32_t)(bone + 1) << 8); wr32(a + 36, 0x000000FFu);   /* weight 1 on the first bone */
    }
    CHECK(com(dev, 83, 4, 5u, 2u, vtx, 40u) == 0, "DrawPrimitiveUP");
}
static void vsf(uint32_t start, const float* f, uint32_t n4) { uint32_t b = gbuf(16 * n4); for (uint32_t i = 0; i < 4 * n4; i++) wrfl(b + 4 * i, f[i]); com(dev, 94, 3, start, b, n4); }
static void psf(uint32_t start, const float* f, uint32_t n4) { uint32_t b = gbuf(16 * n4); for (uint32_t i = 0; i < 4 * n4; i++) wrfl(b + 4 * i, f[i]); com(dev, 109, 3, start, b, n4); }
static void clear(uint32_t col) { com(dev, 43, 6, 0u, 0u, 3u, col, fb(1.0f), 0u); }

/* ---- shaders (UE3 style) ---- */
static const char* VS_BASIC =
    "float4x4 ViewProjection;\n"
    "void Main(float4 P : POSITION, float3 N : NORMAL, float2 UV : TEXCOORD0, out float4 OP : POSITION, out float2 OUV : TEXCOORD0,\n"
    "          out float3 ON : TEXCOORD1, out float4 OW : TEXCOORD2)\n"
    "{ OP = mul(P, ViewProjection); OUV = UV; ON = N; OW = P; }\n";
static const char* VS_SKIN =   /* UE3 GPU skinning: float4x3 bone matrices indexed by BLENDINDICES (relative addressing) */
    "float4x3 Bones[75] : register(c0);\n"
    "float4x4 ViewProjection : register(c225);\n"
    "void Main(float4 P : POSITION, float3 N : NORMAL, float2 UV : TEXCOORD0, int4 BI : BLENDINDICES, float4 BW : BLENDWEIGHT,\n"
    "          out float4 OP : POSITION, out float2 OUV : TEXCOORD0, out float3 ON : TEXCOORD1, out float4 OW : TEXCOORD2)\n"
    "{\n"
    "  float4x3 M = Bones[BI.x] * BW.x + Bones[BI.y] * BW.y + Bones[BI.z] * BW.z + Bones[BI.w] * BW.w;\n"
    "  float3 W = mul(P, M);\n"
    "  OP = mul(float4(W, 1), ViewProjection); OUV = UV; ON = mul(N, (float3x3)M); OW = float4(W, 1);\n"
    "}\n";
static const char* VS_LIGHTS =   /* loop over an int constant (rep/loop in SM3) */
    "float4x4 ViewProjection : register(c0);\n"
    "int NumLights : register(i0);\n"
    "float4 LightColors[8] : register(c4);\n"
    "void Main(float4 P : POSITION, float3 N : NORMAL, float2 UV : TEXCOORD0, out float4 OP : POSITION, out float2 OUV : TEXCOORD0,\n"
    "          out float3 ON : TEXCOORD1, out float4 OW : TEXCOORD2)\n"
    "{\n"
    "  float3 c = 0;\n"
    "  for (int i = 0; i < NumLights; i++) c += LightColors[i].rgb;\n"
    "  OP = mul(P, ViewProjection); OUV = UV; ON = c; OW = P;\n"
    "}\n";
static const char* PS_COLOR =   /* color from the vertex shader's TEXCOORD1 */
    "float4 Main(float2 UV : TEXCOORD0, float3 N : TEXCOORD1) : COLOR0 { return float4(N, 1); }\n";
static const char* PS_MATH =
    "float4 Params;\n"
    "float4 Main(float2 UV : TEXCOORD0) : COLOR0 {\n"
    "  float a = pow(Params.x, 2.0);\n"                       /* 0.25 */
    "  float b = frac(Params.y * 3.0);\n"                     /* frac(0.75*3)=0.25 */
    "  float c = saturate(exp2(Params.z) - 1.5);\n"           /* exp2(1)-1.5 = 0.5 */
    "  return float4(a, b, c, 1) + float4(sin(0) , cos(0) - 1, 0, 0) + normalize(float3(0, 0, 2)).xyxx * 0;\n"
    "}\n";
static const char* PS_BRANCH =   /* dynamic branch on a varying + bool constant + clip */
    "bool bInvert : register(b0);\n"
    "float4 Main(float2 UV : TEXCOORD0) : COLOR0 {\n"
    "  float4 c;\n"
    "  if (UV.x > 0.5) c = float4(1, 0, 0, 1); else c = float4(0, 1, 0, 1);\n"
    "  if (bInvert) c.rgb = 1 - c.rgb;\n"
    "  clip(UV.y - 0.25);\n"
    "  return c;\n"
    "}\n";
static const char* PS_TEX =
    "sampler2D Tex : register(s0);\n"
    "samplerCUBE Cube : register(s1);\n"
    "float4 Main(float2 UV : TEXCOORD0, float4 W : TEXCOORD2) : COLOR0 {\n"
    "  float4 a = tex2Dlod(Tex, float4(UV, 0, 0));\n"
    "  float4 b = tex2Dbias(Tex, float4(UV, 0, 0.5));\n"
    "  float4 c = tex2Dproj(Tex, float4(UV * 2, 0, 2));\n"
    "  float4 d = tex2Dgrad(Tex, UV, ddx(UV), ddy(UV));\n"
    "  float4 e = texCUBE(Cube, float3(1, 0, 0));\n"
    "  return (a + b + c + d) * 0.25 * e;\n"
    "}\n";
static const char* PS_VPOS =
    "float4 Main(float2 UV : TEXCOORD0, float2 Pos : VPOS, float Face : VFACE) : COLOR0 {\n"
    "  return float4(Pos.x < 32 ? 1 : 0, Face > 0 ? 1 : 0, 0, 1);\n"
    "}\n";
static const char* PS_MRT =
    "void Main(float2 UV : TEXCOORD0, out float4 C0 : COLOR0, out float4 C1 : COLOR1, out float D : DEPTH) {\n"
    "  C0 = float4(0, 0, 1, 1); C1 = float4(1, 1, 0, 1); D = 0.25;\n"
    "}\n";
static const char* PS_LOOP =   /* loop with aL-relative constant reads in a pixel shader */
    "float4 Params[4] : register(c0);\n"
    "int Count;\n"
    "float4 Main(float2 UV : TEXCOORD0) : COLOR0 { float4 s = 0; for (int i = 0; i < Count; i++) s += Params[i]; return s; }\n";
static const char* PS_LOOP_ASM =   /* real loop aL, i0 (i0 from SetPixelShaderConstantI; ps_3_0 cannot index c# by aL) */
    "ps_3_0\n"
    "def c10, 0, 0, 0, 0\n"
    "mov r0, c10\n"
    "loop aL, i0\n"
    "  add r0, r0, c1\n"
    "endloop\n"
    "mov oC0, r0\n";
static const char* PS_UNWRITTEN =   /* reads a varying the vertex shader does not write (must still link) */
    "float4 Main(float2 UV : TEXCOORD0, float4 X : TEXCOORD5, float4 C : COLOR1) : COLOR0 { return float4(0, 0.5, 0, 1) + X * 0 + C * 0; }\n";
static const char* PS_PRED_ASM =   /* predicated instructions (setp / (p0) / if_pred) */
    "ps_3_0\n"
    "def c0, 1, 0, 0, 1\n"
    "def c1, 0, 1, 0, 1\n"
    "def c2, 0.5, 0.75, 0, 0\n"
    "dcl_texcoord0 v0.xy\n"
    "mov r0, c1\n"
    "setp_gt p0.xy, v0.xyxx, c2.xyxx\n"
    "(p0.x) mov r0, c0\n"
    "if p0.y\n"
    "  mov r0.b, c0.x\n"
    "endif\n"
    "mov oC0, r0\n";
static const char* VS11_ASM =   /* vs_1_1 as Scaleform/UE3 assemble it: no instruction lengths; mov a0.x floors */
    "vs_1_1\n"
    "dcl_position v0\n"
    "dcl_texcoord0 v2\n"
    "m4x4 oPos, v0, c0\n"
    "mov a0.x, c4.x\n"
    "mov oT1, c[a0.x + 5]\n"
    "mul r0, v2.xyxy, c4.yyyy\n"
    "mov oT0, r0\n";
static const char* PS2_TEX =
    "sampler2D Tex;\n"
    "float4 Tint;\n"
    "float4 Main(float2 UV : TEXCOORD0) : COLOR0 { return tex2D(Tex, UV) * Tint; }\n";

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <msvcr80.dll|image.bin> <d3dx9_31.dll|image.bin>\n", argv[0]); return 2; }
    rt_init();
    hle_fs_init("/tmp", "/tmp", NULL);
    crt.name = "msvcr80.dll"; crt.base = crt_base; crt.size = crt_image_size; crt.entry = crt_entry;
    crt.funcs = crt_funcs; crt.nfuncs = crt_nfuncs; crt.imports = crt_imports; crt.nimports = crt_nimports; crt.exports = crt_exports;
    rt_expect_image_crc(crt_image_crc); hle_register_module(&crt, "C:\\MassEffect\\Binaries\\msvcr80.dll"); rt_load_module(&crt, argv[1]);
    dx.name = "d3dx9_31.dll"; dx.base = dx_base; dx.size = dx_image_size; dx.entry = dx_entry;
    dx.funcs = dx_funcs; dx.nfuncs = dx_nfuncs; dx.imports = dx_imports; dx.nimports = dx_nimports; dx.exports = dx_exports;
    rt_expect_image_crc(dx_image_crc); hle_register_module(&dx, "C:\\Windows\\System32\\d3dx9_31.dll"); rt_load_module(&dx, argv[2]);
    CHECK(hle_dll_attach(&crt) == 1 && hle_dll_attach(&dx) == 1, "DllMain");
    test_crt_memmove();
    dxf("D3DXDebugMute", 1, 1u);
    out = gbuf(64); vtx = gbuf(40 * 8);
    uint32_t d3d = api("d3d9.dll", "Direct3DCreate9", 1, 32u), pp = gbuf(56);
    wr32(pp, 64); wr32(pp + 4, 64); wr32(pp + 8, 22); wr32(pp + 12, 1); wr32(pp + 24, 1); wr32(pp + 28, 0x10000); wr32(pp + 32, 1);
    wr32(pp + 36, 1); wr32(pp + 40, 75);
    CHECK(com(d3d, 16, 6, 0u, 1u, 0x10000u, 0x40u, pp, out) == 0, "CreateDevice"); dev = rd32(out);
    com(dev, 18, 4, 0u, 0u, 0u, out); bb = rd32(out);
    com(dev, 36, 6, 64u, 64u, 21u, 2u, out, 0u); sysrb = rd32(out);
    static const uint8_t decl[48] = { 0,0, 0,0, 2,0,0,0,  0,0, 12,0, 2,0,3,0,  0,0, 24,0, 1,0,5,0,  0,0, 32,0, 5,0,2,0,  0,0, 36,0, 8,0,1,0,
                                      0xFF,0, 0,0, 17,0,0,0 };
    uint32_t de = gbuf(48); memcpy(M + de, decl, 48);
    CHECK(com(dev, 86, 2, de, out) == 0, "CreateVertexDeclaration"); com(dev, 87, 1, rd32(out));
    com(dev, 57, 2, 22u, 1u); com(dev, 57, 2, 7u, 0u);
    static const float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    uint32_t vs = shader("VS_BASIC", VS_BASIC, "vs_3_0"), vskin = shader("VS_SKIN", VS_SKIN, "vs_3_0");
    uint32_t vsl = shader("VS_LIGHTS", VS_LIGHTS, "vs_3_0");
    uint32_t pcol = shader("PS_COLOR", PS_COLOR, "ps_3_0"), pmath = shader("PS_MATH", PS_MATH, "ps_3_0");
    uint32_t pbr = shader("PS_BRANCH", PS_BRANCH, "ps_3_0"), ptex = shader("PS_TEX", PS_TEX, "ps_3_0");
    uint32_t pvp = shader("PS_VPOS", PS_VPOS, "ps_3_0"), pmrt = shader("PS_MRT", PS_MRT, "ps_3_0");
    uint32_t p2 = shader("PS2_TEX", PS2_TEX, "ps_2_0");
    uint32_t ploop = shader("PS_LOOP", PS_LOOP, "ps_3_0"), punw = shader("PS_UNWRITTEN", PS_UNWRITTEN, "ps_3_0");
    uint32_t ppred = asmps("PS_PRED_ASM", PS_PRED_ASM), plasm = asmps("PS_LOOP_ASM", PS_LOOP_ASM);
    uint32_t v11 = asmsh("VS11_ASM", VS11_ASM, 0);
    /* constants: D3DX puts the first declared constant at c0; ViewProjection is the only/first in VS_BASIC/VS_LIGHTS,
     * in VS_SKIN Bones c0..c224 and ViewProjection after them (c225). Transposed (column-major) by default. */
    com(dev, 41, 0);

    /* basic transform + vertex -> pixel varyings (normal as color) */
    com(dev, 92, 1, vs); com(dev, 107, 1, pcol); vsf(0, ident, 4);
    clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0);
    grab(bb); EXPECT(32, 32, 0x0000FF, 2, "VS_BASIC + PS_COLOR (normal 0,0,1)");
    /* PS math */
    com(dev, 107, 1, pmath); { float p[4] = { 0.5f, 0.75f, 1.0f, 0 }; psf(0, p, 1); }
    clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb); EXPECT(32, 32, 0x404080, 3, "PS_MATH pow/frac/exp2/saturate");
    /* branches + bool + clip */
    com(dev, 107, 1, pbr); { uint32_t b = gbuf(4); wr32(b, 0); com(dev, 113, 3, 0u, b, 1u); }
    clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb);
    EXPECT(48, 40, 0xFF0000, 2, "branch: right red"); EXPECT(16, 40, 0x00FF00, 2, "branch: left green"); EXPECT(16, 4, 0, 2, "clip: top quarter killed");
    { uint32_t b = gbuf(4); wr32(b, 1); com(dev, 113, 3, 0u, b, 1u); }
    clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb); EXPECT(48, 40, 0x00FFFF, 2, "bool constant: inverted");
    /* skinning: bone 2 translates x by +1 (half screen to the right) */
    com(dev, 92, 1, vskin); com(dev, 107, 1, pcol);
    { float bones[75 * 12]; memset(bones, 0, sizeof bones);
      for (int b = 0; b < 75; b++) { float* m = bones + 12 * b; m[0] = 1; m[5] = 1; m[10] = 1; if (b == 2) m[3] = 1.0f; }   /* rows of the transposed 4x3 */
      vsf(0, bones, 225); vsf(225, ident, 4); }
    clear(0xFF000000u); quad(-1, 1, 0, -1, .5f, 2); grab(bb);
    EXPECT(48, 32, 0x0000FF, 2, "skinning: bone 2 moved the quad right"); EXPECT(16, 32, 0, 2, "skinning: left empty");
    clear(0xFF000000u); quad(-1, 1, 0, -1, .5f, 0); grab(bb); EXPECT(16, 32, 0x0000FF, 2, "skinning: bone 0 identity");
    /* loop over int constant */
    com(dev, 92, 1, vsl); vsf(0, ident, 4);
    { float lc[8 * 4]; memset(lc, 0, sizeof lc); lc[0] = 0.25f; lc[4] = 0.25f; lc[5] = 0.5f; lc[9] = 1.0f; lc[14] = 1.0f; vsf(4, lc, 8);
      uint32_t ic = gbuf(16); wr32(ic, 2); wr32(ic + 4, 0); wr32(ic + 8, 1); com(dev, 96, 3, 0u, ic, 1u); }
    clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb); EXPECT(32, 32, 0x808000, 3, "loop: 2 lights (0.5, 0.5, 0)");
    /* textures: 2x2 white texture + white cube (sampler registers by D3DX: Tex s0, Cube s1) */
    com(dev, 92, 1, vs); com(dev, 107, 1, ptex);
    com(dev, 23, 8, 2u, 2u, 1u, 0u, 21u, 1u, out, 0u); uint32_t t = rd32(out);
    { uint32_t lr = gbuf(8); com(t, 19, 4, 0u, lr, 0u, 0u); for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) wr32(rd32(lr + 4) + rd32(lr) * y + 4 * x, 0xFF808080u); com(t, 20, 1, 0u); }
    com(dev, 25, 7, 1u, 1u, 0u, 21u, 1u, out, 0u); uint32_t cube = rd32(out);
    for (int f = 0; f < 6; f++) { uint32_t lr = gbuf(8); com(cube, 19, 5, (uint32_t)f, 0u, lr, 0u, 0u); wr32(rd32(lr + 4), f == 0 ? 0xFFFFFFFFu : 0xFF000000u); com(cube, 20, 2, (uint32_t)f, 0u); }
    com(dev, 65, 2, 0u, t); com(dev, 65, 2, 1u, cube);
    clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb); EXPECT(32, 32, 0x808080, 3, "tex2Dlod/bias/proj/grad + texCUBE(+X face)");
    /* VPOS / VFACE */
    com(dev, 107, 1, pvp); clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb);
    EXPECT(16, 32, 0xFFFF00, 2, "VPOS.x < 32, front face"); EXPECT(48, 32, 0x00FF00, 2, "VPOS.x >= 32");
    /* MRT + depth output */
    com(dev, 23, 8, 64u, 64u, 1u, 1u, 21u, 0u, out, 0u); uint32_t rt1 = rd32(out); com(rt1, 18, 2, 0u, out); uint32_t rs1 = rd32(out);
    com(dev, 37, 2, 1u, rs1); com(dev, 107, 1, pmrt);
    com(dev, 57, 2, 7u, 1u); com(dev, 57, 2, 23u, 4u);
    clear(0xFF000000u); com(dev, 43, 6, 0u, 0u, 1u, 0xFF000000u, fb(1.0f), 0u);
    quad(-1, 1, 1, -1, .9f, 0); grab(bb); EXPECT(32, 32, 0x0000FF, 2, "MRT oC0");
    grab(rs1); EXPECT(32, 32, 0xFFFF00, 2, "MRT oC1");
    com(dev, 37, 2, 1u, 0u);
    com(dev, 107, 1, pmath); quad(-1, 1, 1, -1, .3f, 0); grab(bb); EXPECT(32, 32, 0x0000FF, 2, "oDepth 0.25: a quad at z 0.3 stays hidden");
    quad(-1, 1, 1, -1, .2f, 0); grab(bb); EXPECT(32, 32, 0x404080, 3, "a quad at z 0.2 is drawn");
    com(dev, 57, 2, 7u, 0u);
    /* ps_2_0 */
    com(dev, 107, 1, p2); { float tint[4] = { 1, 0.5f, 0, 1 }; psf(0, tint, 1); } com(dev, 65, 2, 0u, t);
    clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb); EXPECT(32, 32, 0x804000, 3, "ps_2_0 tex2D * Tint");
    /* pixel shader loop: the 2006 D3DX compiler unrolls it and keeps `int Count` as a FLOAT constant (c4, after Params) */
    com(dev, 107, 1, ploop); { float pr[20] = { 0.25f, 0, 0, 1, 0.25f, 0.5f, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 3, 0, 0, 0 }; psf(0, pr, 5);
      uint32_t ic = gbuf(16); wr32(ic, 3); wr32(ic + 4, 0); wr32(ic + 8, 1); com(dev, 111, 3, 0u, ic, 1u); }
    clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb); EXPECT(32, 32, 0x8080FF, 3, "ps loop over 3 constants (unrolled, Count in c4)");
    if (plasm) {   /* real loop aL, i0 = {3, 0, 1} set above */
        com(dev, 107, 1, plasm); clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb); EXPECT(32, 32, 0xC0FF00, 3, "ps loop aL, i0 = 3 iterations");
        uint32_t ic = gbuf(16); wr32(ic, 2); wr32(ic + 4, 1); wr32(ic + 8, 1); com(dev, 111, 3, 0u, ic, 1u);   /* 2 iterations */
        clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb); EXPECT(32, 32, 0x80FF00, 3, "ps loop aL, i0 = 2 iterations");
    }
    com(dev, 107, 1, punw); clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb); EXPECT(32, 32, 0x008000, 3, "PS reads unwritten varyings");
    if (v11) {   /* c4.x = 1.7: floor -> a0.x = 1 -> oT1 = c6 (rounding would pick c7) */
        com(dev, 92, 1, v11); com(dev, 107, 1, pcol); vsf(0, ident, 4);
        float c4[16] = { 1.7f, 1, 0, 0,  0, 1, 0, 1,  1, 0.5f, 0, 1,  0, 0, 1, 1 }; vsf(4, c4, 4);
        clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb); EXPECT(32, 32, 0xFF8000, 3, "vs_1_1: m4x4 + c[a0.x + 5] (mov a0 floors)");
        com(dev, 92, 1, vs);
    }
    if (ppred) {
        com(dev, 107, 1, ppred); clear(0xFF000000u); quad(-1, 1, 1, -1, .5f, 0); grab(bb);
        EXPECT(16, 8, 0x00FF00, 2, "predicate: left top green"); EXPECT(48, 8, 0xFF0000, 2, "predicate: right top red");
        EXPECT(16, 60, 0x00FFFF, 2, "if_pred: left bottom +blue"); EXPECT(48, 60, 0xFF00FF, 2, "if_pred: right bottom +blue");
    }
    com(dev, 42, 0);
    uint64_t st[5]; me1gl_stats(st);
    CHECK(st[1] == 0 && st[3] == 0 && st[4] == 0, "GL: %llu draws, %llu skipped, %llu programs, %llu failed, %llu shaders not translated",
          (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2], (unsigned long long)st[3], (unsigned long long)st[4]);
    hle_d3d9_report();
    printf("test_hlsl_gl: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail != 0;
}
