/* D3D9 shader bytecode (vs/ps 2.0, 2.x, 3.0) -> GLSL ES 3.00 - sm3_glsl.c
 * A shader is translated once (sm3_translate); the final GLSL of a vertex shader depends on the pixel shader it is
 * linked with (varyings are matched by semantic), so the text is produced per program by sm3_glsl_vs / sm3_glsl_ps.
 * Conventions of the generated code (the D3D9 backend sets these uniforms):
 *   VS: vc[] float constants, vi[] int, vb bool bit mask (bit n = b#n), vs_s0..3 samplers (D3DVERTEXTEXTURESAMPLER0..3),
 *       dx_fix = (1/vpW, 1/vpH, y sign, 0) - D3D9 pixel centers and the y flip of render-to-texture,
 *       dx_bgra = bit n: input register vN comes from a D3DCOLOR (stored B,G,R,A) - swizzle .zyxw
 *   PS: pc[] / pi[] / pb (bit mask), ps_s0..15, dx_at = (alpha func D3DCMP_*, ref 0..1, 0, 0) - alpha test (0 = off),
 *       dx_srgb = bit n: sampler n reads sRGB (decode after the fetch), dx_srgbw = 1: sRGB write (encode oC0)
 *   attributes: layout(location = n) for input register vn; fragment outputs: location n = oCn. */
#ifndef SM3_GLSL_H
#define SM3_GLSL_H
#include <stdint.h>
typedef struct { uint8_t reg, usage, index, mask; } Sm3Sem;    /* register, D3DDECLUSAGE, usage index, write mask */
typedef struct { int reg; float v[4]; } Sm3Def;
typedef struct {
    int ps, major, minor;
    int nin; Sm3Sem in[16];              /* VS: input registers; PS: input semantics (ps_2: t# = TEXCOORD#, v# = COLOR#) */
    int nout; Sm3Sem out[16];            /* VS outputs (vs_2: oPos/oFog/oPts/oD#/oT# are given pseudo registers 16+) */
    uint8_t samp[16];                    /* sampler types: 0 unused, 2 = 2D, 3 = cube, 4 = volume */
    int nconst, crel;                    /* float constants used: [0, nconst); crel: relative addressing (array needed) */
    int ndef; Sm3Def def[256];           /* def c#: merged into the uniform upload (relative reads see them) */
    int outmask;                         /* PS: color outputs written (bit n = oCn) */
    int depth, vpos, vface, psize;
    int rhw;                             /* VS: built-in pass-through for pre-transformed vertices (set by the caller after sm3_translate) */
    uint8_t inpp[16];                    /* PS: input k declared _pp (partial precision) */
    int npp, ntemp;                      /* lite mode: temps declared mediump; temps r# declared */
    char* decl; char* body;              /* generated pieces (globals, functions); NULL on error */
    char err[200];
} Sm3;
int sm3_translate(const uint32_t* tok, uint32_t ntok, Sm3* s);   /* 0 ok, -1 error (s->err) */
void sm3_free(Sm3* s);
char* sm3_glsl_vs(const Sm3* vs, const Sm3* ps);                   /* malloc'ed GLSL ES 3.00 source */
char* sm3_glsl_ps(const Sm3* ps);
const char* sm3_usage_name(int usage);
/* GL uniform budget (GL_MAX_*_UNIFORM_VECTORS): sizes vc[]/pc[] of shaders with relative constant addressing */
extern int sm3_vs_uniform_vectors, sm3_ps_uniform_vectors;
/* D3D9 float semantics emulation (0 * inf = 0): 0 off, 1 clamp rcp/rsq/log/exp/nrm/pow (default), 2 + mul/mad/dp */
extern int sm3_float_emu;
/* "invariant gl_Position;" in every vertex shader (multi-pass depth consistency), default 1 */
extern int sm3_invariant;
/* v0.9.9 "simplified shaders" (launcher switch, ME1_SHADER_LITE=1): pixel shaders use FP16 (mediump) wherever the game's
 * shader marks the value as half (_pp: temps written only by _pp instructions, _pp inputs), sRGB decode/encode
 * as x^2 / sqrt(x) instead of pow(x, 2.2); positions, texture coordinates, constants stay highp. Default 0. */
extern int sm3_lite;
#endif
