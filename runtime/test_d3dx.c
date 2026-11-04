/* Test of the recompiled d3dx9_31.dll (HLSL compiler, assembler, disassembler, preprocessor, constant table) running
 * on top of the recompiled msvcr80.dll, called exactly as MassEffect.exe calls it (stdcall exports, COM vtables).
 * usage: test_d3dx <msvcr80.dll | image.bin> <d3dx9_31.dll | image.bin> [repeat] */
#include "rt.h"
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

extern const FuncEntry crt_funcs[]; extern const unsigned crt_nfuncs;
extern const ImportEntry crt_imports[]; extern const unsigned crt_nimports;
extern const ExportEntry crt_exports[]; extern const uint32_t crt_base, crt_entry, crt_image_size, crt_image_crc;
extern const FuncEntry dx_funcs[]; extern const unsigned dx_nfuncs;
extern const ImportEntry dx_imports[]; extern const unsigned dx_nimports;
extern const ExportEntry dx_exports[]; extern const uint32_t dx_base, dx_entry, dx_image_size, dx_image_crc;
static Module crt, dx;
static int g_fail, g_ok;
#define CHECK(cond, ...) do { if (cond) { g_ok++; printf("ok   "); } else { g_fail++; printf("FAIL "); } printf(__VA_ARGS__); putchar('\n'); } while (0)
static uint32_t call(uint32_t t, int n, const uint32_t* a, const char* what) {
    uint32_t sp = g_cpu.esp, r = rt_call(&g_cpu, t, n, a);
    if (g_cpu.esp != sp) { printf("FAIL %s: esp %08x != %08x (wrong arg count)\n", what, g_cpu.esp, sp); g_fail++; g_cpu.esp = sp; }
    return r;
}
static uint32_t dxf(const char* name, int n, ...) {
    uint32_t t = rt_find_export(&dx, name), a[16]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    if (!t) { printf("FAIL %s not exported\n", name); g_fail++; return 0xFFFFFFFFu; }
    return call(t, n, a, name);
}
static uint32_t com(uint32_t o, int k, int n, ...) {
    uint32_t a[16]; va_list ap; va_start(ap, n);
    a[0] = o; for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    char w[32]; snprintf(w, sizeof w, "method %d", k);
    return call(rd32(rd32(o) + 4 * k), n + 1, a, w);
}
static uint32_t gstr(const char* s) { uint32_t n = (uint32_t)strlen(s) + 1, g = ghalloc(n, 1); memcpy(M + g, s, n); return g; }
static uint32_t gbuf(uint32_t n) { return ghalloc(n, 1); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void load(Module* m, const char* nm, const char* path, const char* winpath) {
    hle_register_module(m, winpath); rt_load_module(m, path); (void)nm;
}
/* ID3DXBuffer: 3 GetBufferPointer, 4 GetBufferSize, 2 Release */
static uint32_t bptr(uint32_t b) { return com(b, 3, 0); }
static uint32_t bsize(uint32_t b) { return com(b, 4, 0); }

static const char* PS_SIMPLE =
    "float4 Main(float4 Color : COLOR0) : COLOR0 { return Color * 2; }\n";
static const char* VS_MAT =
    "float4x4 LocalToProjection;\n"
    "float4 Tint;\n"
    "void Main(float4 InPosition : POSITION, float2 InUV : TEXCOORD0, out float4 OutPosition : POSITION,\n"
    "          out float2 OutUV : TEXCOORD0, out float4 OutColor : COLOR0)\n"
    "{ OutPosition = mul(InPosition, LocalToProjection); OutUV = InUV; OutColor = Tint; }\n";
static const char* PS_TEX =
    "sampler2D Texture;\n"
    "float4 Params[4];\n"
    "#ifndef NUM_SAMPLES\n#error NUM_SAMPLES not defined\n#endif\n"
    "float4 Main(float2 UV : TEXCOORD0) : COLOR0 {\n"
    "  float4 Sum = 0;\n"
    "  for (int i = 0; i < NUM_SAMPLES; i++) Sum += tex2D(Texture, UV + Params[i % 4].xy * i) * Params[i % 4].w;\n"
    "  float L = dot(Sum.rgb, float3(0.3, 0.59, 0.11));\n"
    "  return float4(lerp(Sum.rgb, L.xxx, saturate(Params[0].z)), pow(abs(Sum.a), 2.2) + exp(L) + log(1 + L) + sin(L));\n"
    "}\n";
static const char* PS_ERR = "float4 Main() : COLOR0 { return undefined_thing; }\n";
static const char* ASM = "ps_3_0\ndef c0, 1, 0.5, 0, 1\ndcl_texcoord0 v0.xy\ndcl_2d s0\ntexld r0, v0, s0\nmul oC0, r0, c0\n";

/* compile; returns shader buffer (0 on failure, errors printed when verbose) */
static uint32_t compile(const char* src, const char* prof, uint32_t defines, uint32_t flags, uint32_t* hr, uint32_t* err, uint32_t* ctab) {
    uint32_t o = gbuf(12);
    *hr = dxf("D3DXCompileShader", 10, gstr(src), (uint32_t)strlen(src), defines, 0u, gstr("Main"), gstr(prof), flags, o, o + 4, ctab ? o + 8 : 0u);
    *err = rd32(o + 4); if (ctab) *ctab = rd32(o + 8);
    return rd32(o);
}
static int valid_bytecode(uint32_t b, uint32_t ver) {
    uint32_t p = bptr(b), n = bsize(b);
    printf("     bytecode crc %08x\n", rt_crc32(M + p, n));   /* must match between x86-64 and arm64 builds */
    return n >= 8 && n % 4 == 0 && rd32(p) == ver && rd32(p + n - 4) == 0x0000FFFFu;
}
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <msvcr80.dll|image.bin> <d3dx9_31.dll|image.bin> [repeat]\n", argv[0]); return 2; }
    int rep = argc > 3 ? atoi(argv[3]) : 5;
    rt_init();
    hle_fs_init("/tmp", "/tmp", NULL);
    crt.name = "msvcr80.dll"; crt.base = crt_base; crt.size = crt_image_size; crt.entry = crt_entry;
    crt.funcs = crt_funcs; crt.nfuncs = crt_nfuncs; crt.imports = crt_imports; crt.nimports = crt_nimports; crt.exports = crt_exports;
    rt_expect_image_crc(crt_image_crc); load(&crt, "crt", argv[1], "C:\\MassEffect\\Binaries\\msvcr80.dll");
    dx.name = "d3dx9_31.dll"; dx.base = dx_base; dx.size = dx_image_size; dx.entry = dx_entry;
    dx.funcs = dx_funcs; dx.nfuncs = dx_nfuncs; dx.imports = dx_imports; dx.nimports = dx_nimports; dx.exports = dx_exports;
    rt_expect_image_crc(dx_image_crc); load(&dx, "dx", argv[2], "C:\\Windows\\System32\\d3dx9_31.dll");
    CHECK(hle_dll_attach(&crt) == 1, "msvcr80 DllMain");
    CHECK(hle_dll_attach(&dx) == 1, "d3dx9_31 DllMain");
    uint32_t hr, err, ctab, b;
    dxf("D3DXDebugMute", 1, 1u);

    b = compile(PS_SIMPLE, "ps_3_0", 0, 0, &hr, &err, &ctab);
    CHECK(hr == 0 && b && valid_bytecode(b, 0xFFFF0300u), "CompileShader ps_3_0 simple: hr %08x, %u bytes", hr, b ? bsize(b) : 0);
    if (hr && err) printf("     errors: %s\n", (char*)M + bptr(err));
    if (b) {
        uint32_t o = gbuf(4);
        uint32_t r = dxf("D3DXDisassembleShader", 4, bptr(b), 0u, 0u, o);
        const char* t = r == 0 ? (const char*)M + bptr(rd32(o)) : "";
        CHECK(r == 0 && strstr(t, "ps_3_0") && strstr(t, "oC0"), "DisassembleShader: hr %08x", r);
        printf("-----\n%s-----\n", t);
    }
    b = compile(VS_MAT, "vs_3_0", 0, 0x8u /*PACKMATRIX_ROWMAJOR*/, &hr, &err, &ctab);
    CHECK(hr == 0 && b && valid_bytecode(b, 0xFFFE0300u), "CompileShader vs_3_0 matrix: hr %08x, %u bytes", hr, b ? bsize(b) : 0);
    if (hr && err) printf("     errors: %s\n", (char*)M + bptr(err));
    if (ctab) {
        uint32_t d = gbuf(12);
        uint32_t r = com(ctab, 5, 1, d);   /* GetDesc */
        CHECK(r == 0 && rd32(d + 8) == 2 && strstr((char*)M + rd32(d), "D3DX9 Shader Compiler"), "ConstantTable GetDesc: hr %08x, %u constants, creator \"%s\"",
              r, rd32(d + 8), r ? "" : (char*)M + rd32(d));
        uint32_t h = com(ctab, 9, 2, 0u, gstr("LocalToProjection"));   /* GetConstantByName */
        uint32_t cd = gbuf(64), cnt = gbuf(4); wr32(cnt, 1);
        r = h ? com(ctab, 6, 3, h, cd, cnt) : 1;                        /* GetConstantDesc: Name, RegisterSet, RegisterIndex, RegisterCount */
        CHECK(r == 0 && rd32(cd + 4) == 2 && rd32(cd + 12) == 4, "GetConstantDesc(LocalToProjection): set %u, reg c%u, count %u", rd32(cd + 4), rd32(cd + 8), rd32(cd + 12));
        CHECK(com(ctab, 2, 0) == 0, "ConstantTable Release");
    }
    uint32_t defs = gbuf(16); wr32(defs, gstr("NUM_SAMPLES")); wr32(defs + 4, gstr("6"));
    double t0 = now_ms();
    b = compile(PS_TEX, "ps_3_0", defs, 0, &hr, &err, &ctab);
    double t1 = now_ms();
    CHECK(hr == 0 && b && valid_bytecode(b, 0xFFFF0300u), "CompileShader ps_3_0 loop/tex2D/pow with D3DXMACRO: hr %08x, %u bytes, %.1f ms", hr, b ? bsize(b) : 0, t1 - t0);
    if (hr && err) printf("     errors: %s\n", (char*)M + bptr(err));
    b = compile(PS_TEX, "ps_2_0", defs, 0, &hr, &err, &ctab);
    CHECK(hr == 0 && b && valid_bytecode(b, 0xFFFF0200u), "CompileShader same as ps_2_0 (unrolled): hr %08x, %u bytes", hr, b ? bsize(b) : 0);
    if (hr && err) printf("     errors: %s\n", (char*)M + bptr(err));
    b = compile(PS_TEX, "ps_3_0", 0, 0, &hr, &err, &ctab);
    CHECK(hr != 0 && !b && err && strstr((char*)M + bptr(err), "NUM_SAMPLES not defined"), "#error without the macro: hr %08x \"%s\"", hr, err ? (char*)M + bptr(err) : "");
    b = compile(PS_ERR, "ps_3_0", 0, 0, &hr, &err, &ctab);
    CHECK(hr == 0x80004005u && !b && err && strstr((char*)M + bptr(err), "undefined_thing"), "syntax error: hr %08x \"%s\"", hr, err ? (char*)M + bptr(err) : "");
    {
        uint32_t o = gbuf(8);
        uint32_t r = dxf("D3DXAssembleShader", 7, gstr(ASM), (uint32_t)strlen(ASM), 0u, 0u, 0u, o, o + 4);
        CHECK(r == 0 && rd32(o) && valid_bytecode(rd32(o), 0xFFFF0300u), "AssembleShader ps_3_0: hr %08x, %u bytes", r, rd32(o) ? bsize(rd32(o)) : 0);
        if (r && rd32(o + 4)) printf("     errors: %s\n", (char*)M + bptr(rd32(o + 4)));
        const char* pp = "#define TWICE(x) ((x)*2)\nfloat f = TWICE(NUM_SAMPLES);\n";
        r = dxf("D3DXPreprocessShader", 6, gstr(pp), (uint32_t)strlen(pp), defs, 0u, o, o + 4);
        CHECK(r == 0 && rd32(o) && strstr((char*)M + bptr(rd32(o)), "( 6 ) * 2"), "PreprocessShader: hr %08x \"%s\"", r, r ? "" : (char*)M + bptr(rd32(o)));
    }
    {   /* the game's path: HLE wrapper (hle_d3d9.c) -> recompiled D3DX, with the compile cache. Pass 0 compiles and stores,
         * pass 1 must be served from the cache with identical bytecode and a working constant table. */
        char dir[] = "/tmp/test_d3dx_cache_XXXXXX"; if (!mkdtemp(dir)) strcpy(dir, "/tmp");
        setenv("ME1_HLSL_CACHE", dir, 1);
        hle_d3dx_attach_module(&dx);
        uint32_t o = gbuf(32), crc[2] = { 0, 0 }, ncon[2] = { 0, 0 }; double dt[2];
        for (int pass = 0; pass < 2; pass++) {
            uint32_t q = o + 12 * pass;
            uint32_t a[10] = { gstr(VS_MAT), (uint32_t)strlen(VS_MAT), 0u, 0u, gstr("Main"), gstr("vs_3_0"), 0x8u, q, q + 4, q + 8 };
            double ta = now_ms();
            uint32_t r = hle_call("d3dx9_31.dll", "D3DXCompileShader", 10, a);
            dt[pass] = now_ms() - ta;
            uint32_t sb = rd32(q), ct = rd32(q + 8);
            if (!r && sb) crc[pass] = rt_crc32(M + bptr(sb), bsize(sb));
            if (!r && ct) { uint32_t d = gbuf(12); if (!com(ct, 5, 1, d)) ncon[pass] = rd32(d + 8);
                            uint32_t h = com(ct, 9, 2, 0u, gstr("LocalToProjection")); if (!h) ncon[pass] = 0; }
            CHECK(r == 0 && sb && ct && rd32(q + 4) == 0, "HLE D3DXCompileShader pass %d: hr %08x, crc %08x, %u constants, %.1f ms", pass, r, crc[pass], ncon[pass], dt[pass]);
        }
        uint64_t hits, stored; hle_d3dx_cache_stats(&hits, &stored);
        CHECK(stored == 1 && hits == 1 && crc[0] == crc[1] && crc[0] && ncon[0] == ncon[1] && ncon[0] == 2,
              "compile cache: stored %llu, hits %llu, same bytecode and constant table (%s)", (unsigned long long)stored, (unsigned long long)hits, dir);
        uint32_t a[10] = { gstr(PS_ERR), (uint32_t)strlen(PS_ERR), 0u, 0u, gstr("Main"), gstr("ps_3_0"), 0u, o + 24, o + 28, 0u };
        uint32_t r = hle_call("d3dx9_31.dll", "D3DXCompileShader", 10, a);
        CHECK(r == 0x80004005u && !rd32(o + 24) && rd32(o + 28), "HLE wrapper passes compile errors through: hr %08x", r);
        unsetenv("ME1_HLSL_CACHE");
    }
    t0 = now_ms();
    int okn = 0;
    for (int i = 0; i < rep; i++) { b = compile(PS_TEX, "ps_3_0", defs, 0, &hr, &err, NULL); okn += hr == 0; if (b) com(b, 2, 0); }
    t1 = now_ms();
    CHECK(okn == rep, "%d compiles: %.1f ms per shader", rep, (t1 - t0) / (rep ? rep : 1));
    printf(g_fail ? "test_d3dx: %d ok, %d FAILED\n" : "test_d3dx: all %d ok\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
