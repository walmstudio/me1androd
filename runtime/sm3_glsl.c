/* D3D9 shader bytecode -> GLSL ES 3.00 (see sm3_glsl.h). Shader model 2.0/2.x/3.0 vertex and pixel shaders, as
 * produced by the D3DX HLSL compiler (and vs_1_1 from D3DXAssembleShader: no instruction lengths in the tokens,
 * "mov a0" floors), arithmetic, matrix ops, texld/texldp/texldb/texldl/texldd, texkill, static and
 * dynamic flow control (if/ifc/else, rep, loop with aL, break/breakc/breakp, call/callnz/label/ret), predication
 * (setp, predicated instructions), relative addressing of constants (a0, aL), def/defi/defb.
 * Every D3D register is a vec4 (temps r#, inputs v#/t#, outputs o#/oC#); an instruction computes a vec4 value and
 * stores its write-masked components. */
#include "sm3_glsl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

typedef struct { char* p; size_t n, cap; } Sb;
static void sb_put(Sb* b, const char* s) {
    size_t n = strlen(s);
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 256; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n + 1); b->n += n;
}
static void sb_printf(Sb* b, const char* fmt, ...) {
    char tmp[2048]; va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < (int)sizeof tmp) { sb_put(b, tmp); return; }
    char* big = malloc(n + 1); va_start(ap, fmt); vsnprintf(big, n + 1, fmt, ap); va_end(ap); sb_put(b, big); free(big);
}
static char* sb_take(Sb* b) { if (!b->p) { b->p = malloc(1); b->p[0] = 0; } char* r = b->p; b->p = NULL; b->n = b->cap = 0; return r; }

enum { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_ADDR = 3, R_TEXTURE = 3, R_RASTOUT = 4, R_ATTROUT = 5, R_TEXCRDOUT = 6, R_OUTPUT = 6,
       R_CONSTINT = 7, R_COLOROUT = 8, R_DEPTHOUT = 9, R_SAMPLER = 10, R_CONST2 = 11, R_CONST3 = 12, R_CONST4 = 13,
       R_CONSTBOOL = 14, R_LOOP = 15, R_TEMPF16 = 16, R_MISC = 17, R_LABEL = 18, R_PRED = 19 };
enum { O_NOP, O_MOV, O_ADD, O_SUB, O_MAD, O_MUL, O_RCP, O_RSQ, O_DP3, O_DP4, O_MIN, O_MAX, O_SLT, O_SGE, O_EXP, O_LOG,
       O_LIT, O_DST, O_LRP, O_FRC, O_M4x4, O_M4x3, O_M3x4, O_M3x3, O_M3x2, O_CALL, O_CALLNZ, O_LOOP, O_RET, O_ENDLOOP,
       O_LABEL, O_DCL, O_POW, O_CRS, O_SGN, O_ABS, O_NRM, O_SINCOS, O_REP, O_ENDREP, O_IF, O_IFC, O_ELSE, O_ENDIF,
       O_BREAK, O_BREAKC, O_MOVA, O_DEFB, O_DEFI,
       O_TEXCOORD = 64, O_TEXKILL, O_TEX, O_EXPP = 78, O_LOGP, O_CND, O_DEF, O_CMP = 88, O_BEM, O_DP2ADD, O_DSX, O_DSY,
       O_TEXLDD, O_SETP, O_TEXLDL, O_BREAKP };
static const char* const usage_names[14] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
    "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };
int sm3_vs_uniform_vectors = 256 + 64, sm3_ps_uniform_vectors = 224 + 64;
/* D3D9 float semantics (like DXVK's d3d9.floatEmulation): D3D9 hardware treats 0 * inf as 0, and shaders rely on it:
 * pow(x, y) compiles to log/mul/exp, so pow(0, 0) = exp(0 * log 0) = 1 and normalize(0) = 0 * rsq(0) = 0, where IEEE
 * gives NaN (a NaN pixel is written as black: dark texels of skin/specular masks turned into black blotches).
 * 1: rcp/rsq/log/exp/nrm results are clamped to +-FLT_MAX (0 * FLT_MAX = 0) and pow is exp2(y * clamped log2|x|);
 *    v0.10: without ever computing an infinity - "max(log2(0), -FLT_MAX)" is a no-op for a compiler that assumes no inf/NaN
 *    (Vulkan drivers do, SPIR-V without float controls): the input is tested instead (d3d_rcp/d3d_rsq/d3d_log/d3d_exp);
 * 2: also mul/mad/dp* with an exact 0 operand give 0 (slower); 0: plain IEEE GLSL. */
int sm3_float_emu = 1;
/* invariant gl_Position: UE3 draws a mesh several times (depth pre-pass, base pass, one additive pass per light,
 * shadows) with depth test LESSEQUAL/EQUAL against the depth of the first pass. D3D9 hardware computes the same
 * position bit-exactly in every pass; a GLSL compiler may schedule the same expressions differently per program
 * (fused multiply-add here, not there), and the lighting passes then z-fight: speckled black "crumbs" on skinned
 * meshes, worst on faces at grazing angles. 0: off (ME1_INVARIANT=0). */
int sm3_invariant = 1;
int sm3_lite = 0;
#define FMAX_ "3.4028234e38"
const char* sm3_usage_name(int u) { return u >= 0 && u < 14 ? usage_names[u] : "UNKNOWN"; }
static int rtype(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
static int rnum(uint32_t t) { return (int)(t & 0x7FF); }

typedef struct {
    Sm3* s; const uint32_t* t; uint32_t n;
    int vs, sm;                     /* sm: major version */
    Sb mainb, fnb; Sb* cur; int in_sub, main_done, depth;
    int maxr, a0, aL, p0, lit, srgb, inarr, fmul, femu;
    uint8_t rpp[64], rfull[64];     /* temp r# written by _pp / by full-precision instructions (lite mode) */
    uint8_t isdef[256]; int defslot[256];
    int idef[16]; int idefv[16][4]; int bdef[16]; int bdefv[16];
    uint32_t iused, bused;
    int loopstk[64], nloop, uid;
    uint8_t outdone[32];            /* VS output pseudo-registers already listed */
    int labels[64], nlabels;
    /* current instruction */
    uint32_t op, ctrl, dst, dstrel, pred; int hasdst, haspred, hasdstrel;
    uint32_t src[6], srel[6]; int hasrel[6], nsrc;
    int err;
} Tr;
static void fail(Tr* T, const char* fmt, ...) {
    if (T->err) return;
    T->err = 1; va_list ap; va_start(ap, fmt); vsnprintf(T->s->err, sizeof T->s->err, fmt, ap); va_end(ap);
}
static void ind(Tr* T) { for (int i = 0; i <= T->depth; i++) sb_put(T->cur, "  "); }
static void fmtf(char* o, float f) {
    if (f != f) f = 0; if (f > 3.4e38f) f = 3.4e38f; if (f < -3.4e38f) f = -3.4e38f;
    char b[48]; snprintf(b, sizeof b, "%.9g", (double)f);
    if (!strchr(b, '.') && !strchr(b, 'e') && !strchr(b, 'n') && !strchr(b, 'i')) strcat(b, ".0");
    strcpy(o, b);
}
static const char* cname(Tr* T) { return T->vs ? "vc" : "pc"; }
static const char* iname(Tr* T) { return T->vs ? "vi" : "pi"; }
static const char* bname(Tr* T) { return T->vs ? "vb" : "pb"; }
static void note_const(Tr* T, int idx) { if (idx + 1 > T->s->nconst) T->s->nconst = idx + 1; }

/* VS output pseudo registers for vs_2: oPos 16, oFog 17, oPts 18, oD0-1 19-20, oT0-7 21-28 */
static void vs2_out(Tr* T, int reg, int usage, int index) {
    if (T->outdone[reg]) return;
    T->outdone[reg] = 1;
    Sm3* s = T->s; if (s->nout >= 16) return;
    Sm3Sem* o = &s->out[s->nout++]; o->reg = (uint8_t)reg; o->usage = (uint8_t)usage; o->index = (uint8_t)index; o->mask = 15;
}
/* register name (vec4-typed expression, no swizzle); off: register offset (matrix rows) */
static void regname(Tr* T, uint32_t tok, uint32_t rel, int hasrel, int off, char* o, size_t on, int dst) {
    int ty = rtype(tok), n = rnum(tok) + off;
    char ri[64] = "";
    if (hasrel) {
        int rt = rtype(rel);
        if (rt == R_LOOP) { snprintf(ri, sizeof ri, "aL"); T->aL = 1; }
        else { snprintf(ri, sizeof ri, "a0.%c", "xyzw"[(rel >> 16) & 3]); T->a0 = 1; }
    }
    switch (ty) {
    case R_TEMP: case R_TEMPF16:
        if (n > T->maxr) T->maxr = n;
        snprintf(o, on, "r%d", n); break;
    case R_INPUT:
        if (hasrel) { if (T->vs) fail(T, "relative input in vs"); else { T->inarr = 1; snprintf(o, on, "vin[clamp(%s + %d, 0, VIN_LAST)]", ri, n); } }   /* v0.10.12 (P1-5): OOB index is UB in GLSL/SPIR-V */
        else if (!T->vs && T->sm >= 3) snprintf(o, on, "vin[%d]", n);
        else snprintf(o, on, "v%d", n);
        break;
    case R_CONST:
        if (hasrel) { T->s->crel = 1; snprintf(o, on, "%s[clamp(%s + %d, 0, %s)]", cname(T), ri, n, T->vs ? "VC_LAST" : "PC_LAST"); }
        else if (n < 256 && T->isdef[n]) snprintf(o, on, "k%d", n);
        else { note_const(T, n); snprintf(o, on, "%s[%d]", cname(T), n); }
        break;
    case R_CONST2: case R_CONST3: case R_CONST4: fail(T, "constant register c%d+ not supported", 2048 * (ty - R_CONST2 + 1)); snprintf(o, on, "vec4(0.0)"); break;
    case 3:   /* R_ADDR (vs) / R_TEXTURE (ps) */
        if (T->vs) { if (dst) snprintf(o, on, "a0"); else snprintf(o, on, "vec4(a0)"); T->a0 = 1; }
        else snprintf(o, on, "t%d", n);
        break;
    case R_RASTOUT:
        if (!T->vs) { fail(T, "rastout in ps"); break; }
        vs2_out(T, 16 + n, n == 0 ? 0 : n == 1 ? 11 : 4, 0); snprintf(o, on, "o%d", 16 + n); break;
    case R_ATTROUT: vs2_out(T, 19 + n, 10, n); snprintf(o, on, "o%d", 19 + n); break;
    case 6:   /* R_TEXCRDOUT (vs_2) / R_OUTPUT (vs_3) */
        if (hasrel) fail(T, "relative output register");
        if (T->sm >= 3) snprintf(o, on, "o%d", n); else { vs2_out(T, 21 + n, 5, n); snprintf(o, on, "o%d", 21 + n); }
        break;
    case R_CONSTINT:
        T->iused |= 1u << (n & 15);
        if (T->idef[n & 15]) snprintf(o, on, "vec4(ivec4(%d, %d, %d, %d))", T->idefv[n & 15][0], T->idefv[n & 15][1], T->idefv[n & 15][2], T->idefv[n & 15][3]);
        else snprintf(o, on, "vec4(%s[%d])", iname(T), n);
        break;
    case R_COLOROUT: T->s->outmask |= 1 << (n & 3); snprintf(o, on, "oC%d", n); break;
    case R_DEPTHOUT: T->s->depth = 1; snprintf(o, on, "oDepth"); break;
    case R_LOOP: T->aL = 1; snprintf(o, on, "vec4(float(aL))"); break;
    case R_MISC:
        if (n == 0) { T->s->vpos = 1; snprintf(o, on, "vpos"); } else { T->s->vface = 1; snprintf(o, on, "vface"); }
        break;
    case R_PRED: T->p0 = 1; snprintf(o, on, dst ? "p0" : "vec4(p0)"); break;
    case R_CONSTBOOL:
        T->bused |= 1u << (n & 15);
        if (T->bdef[n & 15]) snprintf(o, on, "vec4(%s)", T->bdefv[n & 15] ? "1.0" : "0.0");
        else snprintf(o, on, "vec4(((%s >> %d) & 1) != 0 ? 1.0 : 0.0)", bname(T), n & 15);
        break;
    default: fail(T, "register type %d not supported", ty); snprintf(o, on, "vec4(0.0)");
    }
}
static void swz_str(uint32_t swz, char* o) {   /* "" for .xyzw */
    if (swz == 0xE4) { o[0] = 0; return; }
    o[0] = '.'; for (int i = 0; i < 4; i++) o[1 + i] = "xyzw"[(swz >> (2 * i)) & 3];
    o[5] = 0;
}
static void apply_mod(int mod, const char* x, char* o, size_t on) {
    switch (mod) {
    case 0: snprintf(o, on, "%s", x); break;
    case 1: snprintf(o, on, "(-%s)", x); break;
    case 2: snprintf(o, on, "(%s - 0.5)", x); break;
    case 3: snprintf(o, on, "(0.5 - %s)", x); break;
    case 4: snprintf(o, on, "(2.0 * %s - 1.0)", x); break;
    case 5: snprintf(o, on, "(1.0 - 2.0 * %s)", x); break;
    case 6: snprintf(o, on, "(1.0 - %s)", x); break;
    case 7: snprintf(o, on, "(2.0 * %s)", x); break;
    case 8: snprintf(o, on, "(-2.0 * %s)", x); break;
    case 11: snprintf(o, on, "abs(%s)", x); break;
    case 12: snprintf(o, on, "(-abs(%s))", x); break;
    default: snprintf(o, on, "%s", x);   /* dz/dw (ps_1_4), not (bools: handled by the callers) */
    }
}
/* source i as vec4 */
static void S(Tr* T, int i, int off, char* o, size_t on) {
    char base[160], sw[8], x[200];
    regname(T, T->src[i], T->srel[i], T->hasrel[i], off, base, sizeof base, 0);
    swz_str((T->src[i] >> 16) & 0xFF, sw);
    snprintf(x, sizeof x, "%s%s", base, sw);
    apply_mod((T->src[i] >> 24) & 0xF, x, o, on);
}
/* source i as float: component at swizzle position pos (3: the w rule of scalar ops; replicate swizzles agree) */
static void Sc(Tr* T, int i, int pos, char* o, size_t on) {
    char base[160], x[200];
    regname(T, T->src[i], T->srel[i], T->hasrel[i], 0, base, sizeof base, 0);
    int ty = rtype(T->src[i]);
    char c = "xyzw"[(T->src[i] >> (16 + 2 * pos)) & 3];
    if (ty == R_LOOP) snprintf(x, sizeof x, "float(aL)");
    else snprintf(x, sizeof x, "%s.%c", base, c);
    apply_mod((T->src[i] >> 24) & 0xF, x, o, on);
}
static const char* mask_str(int m, char* o) {
    int k = 0; if (m == 15) { o[0] = 0; return o; }
    o[k++] = '.'; for (int i = 0; i < 4; i++) if (m >> i & 1) o[k++] = "xyzw"[i];
    o[k] = 0; return o;
}
/* predicate of a predicated instruction (bvec4) */
static void P(Tr* T, char* o, size_t on) {
    char sw[8]; swz_str((T->pred >> 16) & 0xFF, sw);
    T->p0 = 1;
    if (((T->pred >> 24) & 0xF) == 13) snprintf(o, on, "not(p0%s)", sw); else snprintf(o, on, "p0%s", sw);
}
static void assign(Tr* T, const char* v) {
    char nm[160], ms[8], e[1200];
    int ty = rtype(T->dst), m = (T->dst >> 16) & 15, sat = (T->dst >> 20) & 1;
    if (!m) m = 15;
    regname(T, T->dst, T->dstrel, T->hasdstrel, 0, nm, sizeof nm, 1);
    if ((ty == R_TEMP || ty == R_TEMPF16) && rnum(T->dst) < 64) { if ((T->dst >> 21) & 1) T->rpp[rnum(T->dst)] = 1; else T->rfull[rnum(T->dst)] = 1; }
    if (ty == 3 && T->vs) {   /* a0: mova rounds to nearest, vs_1 "mov a0" floors */
        snprintf(e, sizeof e, T->sm < 2 ? "ivec4(floor(%s))" : "ivec4(floor((%s) + 0.5))", v);
        mask_str(m, ms);
        ind(T); if (m == 15) sb_printf(T->cur, "a0 = %s;\n", e); else sb_printf(T->cur, "a0%s = (%s)%s;\n", ms, e, ms);
        return;
    }
    if (sat) snprintf(e, sizeof e, "clamp(%s, 0.0, 1.0)", v); else snprintf(e, sizeof e, "%s", v);
    if (T->haspred) { char p[64], e2[1300]; P(T, p, sizeof p); snprintf(e2, sizeof e2, "mix(%s, %s, %s)", nm, e, p); strcpy(e, e2); }
    mask_str(m, ms);
    ind(T);
    if (m == 15) sb_printf(T->cur, "%s = %s;\n", nm, e);
    else sb_printf(T->cur, "%s%s = (%s)%s;\n", nm, ms, e, ms);
}
static const char* cmp_op(int c) { static const char* const t[8] = { "?", ">", "==", ">=", "<", "!=", "<=", "?" }; return t[c & 7]; }
static const char* cmp_fn(int c) {
    static const char* const t[8] = { "?", "greaterThan", "equal", "greaterThanEqual", "lessThan", "notEqual", "lessThanEqual", "?" };
    return t[c & 7];
}
static void ivec(Tr* T, int n, char* o, size_t on) {
    n &= 15; T->iused |= 1u << n;
    if (T->idef[n]) snprintf(o, on, "ivec4(%d, %d, %d, %d)", T->idefv[n][0], T->idefv[n][1], T->idefv[n][2], T->idefv[n][3]);
    else snprintf(o, on, "%s[%d]", iname(T), n);
}
/* boolean condition of if/callnz/breakp source i */
static void cond(Tr* T, int i, char* o, size_t on) {
    uint32_t t = T->src[i]; int ty = rtype(t), n = rnum(t), nt = ((t >> 24) & 0xF) == 13;
    if (ty == R_PRED) { T->p0 = 1; snprintf(o, on, "%sp0.%c", nt ? "!" : "", "xyzw"[(t >> 16) & 3]); }
    else if (ty == R_CONSTBOOL) {
        T->bused |= 1u << (n & 15);
        if (T->bdef[n & 15]) snprintf(o, on, "%s", (T->bdefv[n & 15] != 0) != nt ? "true" : "false");
        else snprintf(o, on, "(((%s >> %d) & 1) %s 0)", bname(T), n & 15, nt ? "==" : "!=");
    } else { char x[200]; Sc(T, i, 0, x, sizeof x); snprintf(o, on, "(%s != 0.0)", x); }
}
static void tex(Tr* T, char* o, size_t on) {
    int sn = rnum(T->src[1]) & 15, st = T->s->samp[sn] ? T->s->samp[sn] : 2;
    if (!T->s->samp[sn]) T->s->samp[sn] = 2;
    char sname[16], c[220], a[220], sw[8], e[800];
    snprintf(sname, sizeof sname, "%s_s%d", T->vs ? "vs" : "ps", sn);
    S(T, 0, 0, a, sizeof a);
    snprintf(c, sizeof c, st == 2 ? "(%s).xy" : "(%s).xyz", a);
    if (T->op == O_TEX) {
        int k = (int)T->ctrl & 3;
        if (k == 1) { if (st == 3) snprintf(e, sizeof e, "texture(%s, (%s).xyz / (%s).w)", sname, a, a); else snprintf(e, sizeof e, "textureProj(%s, %s)", sname, a); }
        else if (k == 2 && !T->vs) snprintf(e, sizeof e, "texture(%s, %s, (%s).w)", sname, c, a);
        else if (T->vs) snprintf(e, sizeof e, "textureLod(%s, %s, 0.0)", sname, c);
        else snprintf(e, sizeof e, "texture(%s, %s)", sname, c);
    } else if (T->op == O_TEXLDL) snprintf(e, sizeof e, "textureLod(%s, %s, (%s).w)", sname, c, a);
    else {   /* texldd */
        char dx[220], dy[220]; S(T, 2, 0, dx, sizeof dx); S(T, 3, 0, dy, sizeof dy);
        const char* k = st == 2 ? "xy" : "xyz";
        if (T->vs) snprintf(e, sizeof e, "textureLod(%s, %s, 0.0)", sname, c);
        else snprintf(e, sizeof e, "textureGrad(%s, %s, (%s).%s, (%s).%s)", sname, c, dx, k, dy, k);
    }
    swz_str((T->src[1] >> 16) & 0xFF, sw);
    if (!T->vs) { T->srgb = 1; snprintf(o, on, "d3d_srgb(%s, %d)%s", e, sn, sw); }
    else snprintf(o, on, "%s%s", e, sw);
}
/* parse the instruction at token index i (SM2+: length in bits 24-27); returns the next index */
static const signed char op_ndst[97] = {
    0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0, 1,1,1,1,1,1,0,0,0,0,0,0,0,0,1,1, 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 0 };
/* shader model 1 tokens carry no instruction length: parameter count by opcode (vs_1_x instruction set); -1 unknown */
static int sm1_len(uint32_t op) {
    switch (op) {
    case O_NOP: return 0;
    case O_MOV: case O_RCP: case O_RSQ: case O_EXP: case O_LOG: case O_LIT: case O_FRC: case O_EXPP: case O_LOGP: case O_ABS:
    case O_NRM: case O_MOVA: case O_DCL: return 2;
    case O_ADD: case O_SUB: case O_MUL: case O_DP3: case O_DP4: case O_MIN: case O_MAX: case O_SLT: case O_SGE: case O_DST:
    case O_M4x4: case O_M4x3: case O_M3x4: case O_M3x3: case O_M3x2: case O_POW: case O_CRS: return 3;
    case O_MAD: case O_LRP: case O_CND: case O_CMP: case O_DP2ADD: case O_SGN: return 4;
    case O_DEF: return 5;
    }
    return -1;
}
static void parse(Tr* T, uint32_t i, uint32_t len) {
    uint32_t tk = T->t[i], end = i + 1 + len;
    T->op = tk & 0xFFFF; T->ctrl = (tk >> 16) & 0xFF; T->haspred = (tk >> 28) & 1;
    T->hasdst = T->op < 97 ? op_ndst[T->op] : 0; T->hasdstrel = 0; T->nsrc = 0;
    uint32_t k = i + 1;
    int relok = T->vs ? T->sm >= 2 : T->sm >= 3;
    if (T->hasdst && k < end) {
        T->dst = T->t[k++];
        if ((T->dst >> 13 & 1) && relok && k < end) { T->dstrel = T->t[k++]; T->hasdstrel = 1; }
    }
    if (T->haspred && k < end) T->pred = T->t[k++];
    while (k < end && T->nsrc < 6) {
        int j = T->nsrc++;
        T->src[j] = T->t[k++]; T->hasrel[j] = 0;
        if ((T->src[j] >> 13 & 1) && relok && k < end) { T->srel[j] = T->t[k++]; T->hasrel[j] = 1; }
        else if (T->src[j] >> 13 & 1) { T->srel[j] = 0x80000000u | (3u << 28); T->hasrel[j] = 1; }   /* vs_1: a0.x */
    }
}
static void pre_dcl(Tr* T) {   /* dcl: usage token T->dst? no - raw tokens */
    (void)T;
}
static void do_dcl(Tr* T, uint32_t i) {
    uint32_t u = T->t[i + 1], d = T->t[i + 2];
    int ty = rtype(d), n = rnum(d), m = (d >> 16) & 15; if (!m) m = 15;
    Sm3* s = T->s;
    int usage = u & 31, index = (u >> 16) & 15;
    if (ty == R_SAMPLER) { int tt = (u >> 27) & 15; s->samp[n & 15] = (uint8_t)(tt == 3 ? 3 : tt == 4 ? 4 : 2); return; }
    if (ty == R_MISC) { if (n == 0) s->vpos = 1; else s->vface = 1; return; }
    if (T->vs && ty == R_INPUT) {
        if (s->nin < 16) { Sm3Sem* e = &s->in[s->nin++]; e->reg = (uint8_t)n; e->usage = (uint8_t)usage; e->index = (uint8_t)index; e->mask = (uint8_t)m; }
        return;
    }
    if (T->vs && ty == R_OUTPUT) {
        if (s->nout < 16) { Sm3Sem* e = &s->out[s->nout++]; e->reg = (uint8_t)n; e->usage = (uint8_t)usage; e->index = (uint8_t)index; e->mask = (uint8_t)m; }
        if (usage == 4) s->psize = 1;
        return;
    }
    if (!T->vs && (ty == R_INPUT || ty == R_TEXTURE)) {
        if (T->sm < 3) { usage = ty == R_INPUT ? 10 : 5; index = n; if (ty == R_TEXTURE) n += 16; }
        if (s->nin < 16) { s->inpp[s->nin] = (uint8_t)((d >> 21) & 1); Sm3Sem* e = &s->in[s->nin++]; e->reg = (uint8_t)n; e->usage = (uint8_t)usage; e->index = (uint8_t)index; e->mask = (uint8_t)m; }
        return;
    }
}
static void emit(Tr* T, uint32_t i) {
    char a[400], b[400], c[400], v[1600], x[400];
    switch (T->op) {
    case O_NOP: case O_DCL: case O_DEF: case O_DEFI: case O_DEFB: case O_TEXCOORD: break;
    case O_MOV: S(T, 0, 0, a, sizeof a); assign(T, a); break;
    case O_MOVA: S(T, 0, 0, a, sizeof a); assign(T, a); break;
    case O_ADD: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); snprintf(v, sizeof v, "(%s + %s)", a, b); assign(T, v); break;
    case O_SUB: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); snprintf(v, sizeof v, "(%s - %s)", a, b); assign(T, v); break;
    case O_MUL: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b);
        if (sm3_float_emu > 1) { T->fmul = 1; snprintf(v, sizeof v, "d3d_mul(%s, %s)", a, b); } else snprintf(v, sizeof v, "(%s * %s)", a, b);
        assign(T, v); break;
    case O_MAD: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); S(T, 2, 0, c, sizeof c);
        if (sm3_float_emu > 1) { T->fmul = 1; snprintf(v, sizeof v, "(d3d_mul(%s, %s) + %s)", a, b, c); } else snprintf(v, sizeof v, "(%s * %s + %s)", a, b, c);
        assign(T, v); break;
    case O_MIN: case O_MAX: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); snprintf(v, sizeof v, "%s(%s, %s)", T->op == O_MIN ? "min" : "max", a, b); assign(T, v); break;
    case O_SLT: case O_SGE: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); snprintf(v, sizeof v, "vec4(%s(%s, %s))", T->op == O_SLT ? "lessThan" : "greaterThanEqual", a, b); assign(T, v); break;
    case O_RCP: Sc(T, 0, 3, a, sizeof a);
        if (sm3_float_emu) { T->femu = 1; snprintf(v, sizeof v, "vec4(d3d_rcp(%s))", a); } else snprintf(v, sizeof v, "vec4(1.0 / %s)", a);
        assign(T, v); break;
    case O_RSQ: Sc(T, 0, 3, a, sizeof a);
        if (sm3_float_emu) { T->femu = 1; snprintf(v, sizeof v, "vec4(d3d_rsq(%s))", a); } else snprintf(v, sizeof v, "vec4(inversesqrt(abs(%s)))", a);
        assign(T, v); break;
    case O_EXP: case O_EXPP: Sc(T, 0, 3, a, sizeof a);
        if (sm3_float_emu) { T->femu = 1; snprintf(v, sizeof v, "vec4(d3d_exp(%s))", a); } else snprintf(v, sizeof v, "vec4(exp2(%s))", a);
        assign(T, v); break;
    case O_LOG: case O_LOGP: Sc(T, 0, 3, a, sizeof a);
        if (sm3_float_emu) { T->femu = 1; snprintf(v, sizeof v, "vec4(d3d_log(%s))", a); } else snprintf(v, sizeof v, "vec4(log2(abs(%s)))", a);
        assign(T, v); break;
    case O_POW: Sc(T, 0, 3, a, sizeof a); Sc(T, 1, 3, b, sizeof b);
        if (sm3_float_emu) { T->femu = 1; snprintf(v, sizeof v, "vec4(d3d_pow(%s, %s))", a, b); }
        else snprintf(v, sizeof v, "vec4(pow(abs(%s), %s))", a, b);
        assign(T, v); break;
    case O_DP3: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b);
        if (sm3_float_emu > 1) { T->fmul = 1; snprintf(v, sizeof v, "vec4(dot(d3d_mul(%s, %s).xyz, vec3(1.0)))", a, b); }
        else snprintf(v, sizeof v, "vec4(dot((%s).xyz, (%s).xyz))", a, b);
        assign(T, v); break;
    case O_DP4: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b);
        if (sm3_float_emu > 1) { T->fmul = 1; snprintf(v, sizeof v, "vec4(dot(d3d_mul(%s, %s), vec4(1.0)))", a, b); }
        else snprintf(v, sizeof v, "vec4(dot(%s, %s))", a, b);
        assign(T, v); break;
    case O_DP2ADD: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); Sc(T, 2, 3, c, sizeof c); snprintf(v, sizeof v, "vec4(dot((%s).xy, (%s).xy) + %s)", a, b, c); assign(T, v); break;
    case O_LIT: S(T, 0, 0, a, sizeof a); T->lit = 1; snprintf(v, sizeof v, "d3d_lit(%s)", a); assign(T, v); break;
    case O_DST: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); snprintf(v, sizeof v, "vec4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a, b, a, b); assign(T, v); break;
    case O_LRP: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); S(T, 2, 0, c, sizeof c); snprintf(v, sizeof v, "mix(%s, %s, %s)", c, b, a); assign(T, v); break;
    case O_FRC: S(T, 0, 0, a, sizeof a); snprintf(v, sizeof v, "fract(%s)", a); assign(T, v); break;
    case O_ABS: S(T, 0, 0, a, sizeof a); snprintf(v, sizeof v, "abs(%s)", a); assign(T, v); break;
    case O_SGN: S(T, 0, 0, a, sizeof a); snprintf(v, sizeof v, "sign(%s)", a); assign(T, v); break;
    case O_CRS: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); snprintf(v, sizeof v, "vec4(cross((%s).xyz, (%s).xyz), 0.0)", a, b); assign(T, v); break;
    case O_NRM: S(T, 0, 0, a, sizeof a);
        if (sm3_float_emu) { T->femu = 1; snprintf(v, sizeof v, "(%s * d3d_rsq(dot((%s).xyz, (%s).xyz)))", a, a, a); }
        else snprintf(v, sizeof v, "(%s * inversesqrt(dot((%s).xyz, (%s).xyz)))", a, a, a);
        assign(T, v); break;
    case O_SINCOS: Sc(T, 0, 3, a, sizeof a); snprintf(v, sizeof v, "vec4(cos(%s), sin(%s), 0.0, 0.0)", a, a); assign(T, v); break;
    case O_CMP: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); S(T, 2, 0, c, sizeof c); snprintf(v, sizeof v, "mix(%s, %s, greaterThanEqual(%s, vec4(0.0)))", c, b, a); assign(T, v); break;
    case O_CND: S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b); S(T, 2, 0, c, sizeof c); snprintf(v, sizeof v, "mix(%s, %s, greaterThan(%s, vec4(0.5)))", c, b, a); assign(T, v); break;
    case O_DSX: case O_DSY: S(T, 0, 0, a, sizeof a); snprintf(v, sizeof v, "%s(%s)", T->op == O_DSX ? "dFdx" : "dFdy", a); assign(T, v); break;
    case O_M4x4: case O_M4x3: case O_M3x4: case O_M3x3: case O_M3x2: {
        int rows = T->op == O_M4x4 || T->op == O_M3x4 ? 4 : T->op == O_M3x2 ? 2 : 3, three = T->op >= O_M3x4;
        char r[4][200]; uint32_t sv = T->src[1]; T->src[1] = (sv & ~0x00FF0000u) | 0x00E40000u;   /* rows: no swizzle */
        S(T, 0, 0, a, sizeof a);
        for (int k = 0; k < rows; k++) S(T, 1, k, r[k], sizeof r[k]);
        T->src[1] = sv;
        int n = snprintf(v, sizeof v, "vec4(");
        for (int k = 0; k < 4; k++) {
            if (k < rows) n += snprintf(v + n, sizeof v - n, three ? "dot((%s).xyz, (%s).xyz)" : "dot(%s, %s)", a, r[k]);
            else n += snprintf(v + n, sizeof v - n, "0.0");
            n += snprintf(v + n, sizeof v - n, k < 3 ? ", " : ")");
        }
        assign(T, v); break;
    }
    case O_TEX: case O_TEXLDL: case O_TEXLDD:
        if (T->sm < 2) { fail(T, "ps_1_x tex"); break; }
        tex(T, v, sizeof v); assign(T, v); break;
    case O_TEXKILL: {
        char nm[160], ms[8]; int m = (T->dst >> 16) & 15; if (!m) m = 15;
        regname(T, T->dst, 0, 0, 0, nm, sizeof nm, 1);
        int nc = 0; for (int k = 0; k < 4; k++) nc += m >> k & 1;
        mask_str(m, ms);
        ind(T);
        if (nc == 1) sb_printf(T->cur, "if (%s%s < 0.0) discard;\n", nm, ms);
        else sb_printf(T->cur, "if (any(lessThan(%s%s, vec%d(0.0)))) discard;\n", nm, ms, nc);
        break;
    }
    case O_SETP: {
        S(T, 0, 0, a, sizeof a); S(T, 1, 0, b, sizeof b);
        int m = (T->dst >> 16) & 15; if (!m) m = 15; char ms[8]; mask_str(m, ms); T->p0 = 1;
        ind(T);
        if (m == 15) sb_printf(T->cur, "p0 = %s(%s, %s);\n", cmp_fn(T->ctrl), a, b);
        else sb_printf(T->cur, "p0%s = %s(%s, %s)%s;\n", ms, cmp_fn(T->ctrl), a, b, ms);
        break;
    }
    case O_IF: cond(T, 0, a, sizeof a); ind(T); sb_printf(T->cur, "if (%s) {\n", a); T->depth++; break;
    case O_IFC: Sc(T, 0, 0, a, sizeof a); Sc(T, 1, 0, b, sizeof b); ind(T); sb_printf(T->cur, "if (%s %s %s) {\n", a, cmp_op(T->ctrl), b); T->depth++; break;
    case O_ELSE: T->depth--; ind(T); sb_put(T->cur, "} else {\n"); T->depth++; break;
    case O_ENDIF: T->depth--; ind(T); sb_put(T->cur, "}\n"); break;
    case O_REP: {
        ivec(T, rnum(T->src[0]), x, sizeof x);
        int id = T->uid++; if (T->nloop < 64) T->loopstk[T->nloop++] = -1 - id;
        ind(T); sb_printf(T->cur, "for (int rep%d = 0; rep%d < (%s).x; rep%d++) {\n", id, id, x, id); T->depth++;
        break;
    }
    case O_LOOP: {
        ivec(T, rnum(T->src[1]), x, sizeof x); T->aL = 1;
        int id = T->uid++; if (T->nloop < 64) T->loopstk[T->nloop++] = id;
        ind(T); sb_printf(T->cur, "{ int aLs%d = aL; aL = (%s).y;\n", id, x);
        ind(T); sb_printf(T->cur, "for (int lp%d = 0; lp%d < (%s).x; lp%d++, aL += (%s).z) {\n", id, id, x, id, x); T->depth++;
        break;
    }
    case O_ENDREP: case O_ENDLOOP: {
        int id = T->nloop ? T->loopstk[--T->nloop] : -1;
        T->depth--; ind(T);
        if (id >= 0) sb_printf(T->cur, "} aL = aLs%d; }\n", id); else sb_put(T->cur, "}\n");
        break;
    }
    case O_BREAK: ind(T); sb_put(T->cur, "break;\n"); break;
    case O_BREAKC: Sc(T, 0, 0, a, sizeof a); Sc(T, 1, 0, b, sizeof b); ind(T); sb_printf(T->cur, "if (%s %s %s) break;\n", a, cmp_op(T->ctrl), b); break;
    case O_BREAKP: cond(T, 0, a, sizeof a); ind(T); sb_printf(T->cur, "if (%s) break;\n", a); break;
    case O_CALL: ind(T); sb_printf(T->cur, "sub_%d();\n", rnum(T->src[0])); break;
    case O_CALLNZ: cond(T, 1, a, sizeof a); ind(T); sb_printf(T->cur, "if (%s) sub_%d();\n", a, rnum(T->src[0])); break;
    case O_LABEL:
        if (!T->main_done) { sb_put(&T->mainb, "}\n"); T->main_done = 1; }
        else if (T->in_sub) sb_put(&T->fnb, "}\n");
        T->cur = &T->fnb; T->in_sub = 1; T->depth = 0;
        sb_printf(&T->fnb, "void sub_%d() {\n", rnum(T->src[0]));
        break;
    case O_RET:
        if (T->depth > 0) { ind(T); sb_put(T->cur, "return;\n"); break; }
        if (!T->main_done && !T->in_sub) { sb_put(&T->mainb, "}\n"); T->main_done = 1; T->cur = &T->fnb; }
        else if (T->in_sub) { sb_put(&T->fnb, "}\n"); T->in_sub = 0; }
        break;
    default: fail(T, "opcode %u not supported", T->op);
    }
    (void)i;
}
int sm3_translate(const uint32_t* tok, uint32_t ntok, Sm3* s) {
    memset(s, 0, sizeof *s);
    if (ntok < 2) { snprintf(s->err, sizeof s->err, "empty shader"); return -1; }
    Tr Tz; Tr* T = &Tz; memset(T, 0, sizeof *T);
    T->s = s; T->t = tok; T->n = ntok;
    uint32_t ver = tok[0];
    s->ps = (ver >> 16) == 0xFFFF; T->vs = !s->ps;
    s->major = (ver >> 8) & 0xFF; s->minor = ver & 0xFF; T->sm = s->major;
    if ((ver >> 16) != 0xFFFE && (ver >> 16) != 0xFFFF) { snprintf(s->err, sizeof s->err, "bad version token %08x", ver); return -1; }
    if (s->major < (s->ps ? 2 : 1)) { snprintf(s->err, sizeof s->err, "%s_%d_%d not supported (ps_2_0+ / vs_1_1+ only)", s->ps ? "ps" : "vs", s->major, s->minor); return -1; }
    T->maxr = -1;
    /* pass 1: declarations, constants, labels */
    for (uint32_t i = 1; i < ntok;) {
        uint32_t t = tok[i], op = t & 0xFFFF;
        if (t == 0x0000FFFFu) break;
        if (op == 0xFFFE) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
        int l1 = T->sm >= 2 ? (int)((t >> 24) & 0xF) : sm1_len(op);
        if (l1 < 0) { snprintf(s->err, sizeof s->err, "%s_%d_%d: opcode %u not supported", s->ps ? "ps" : "vs", s->major, s->minor, op); return -1; }
        uint32_t len = (uint32_t)l1;
        if (i + len >= ntok) break;
        if (op == O_DCL) do_dcl(T, i);
        else if (op == O_DEF) {
            int r = rnum(tok[i + 1]);
            if (r < 256) { T->isdef[r] = 1; if (s->ndef < 256) { s->def[s->ndef].reg = r; memcpy(s->def[s->ndef].v, &tok[i + 2], 16); s->ndef++; } }
        } else if (op == O_DEFI) { int r = rnum(tok[i + 1]) & 15; T->idef[r] = 1; memcpy(T->idefv[r], &tok[i + 2], 16); }
        else if (op == O_DEFB) { int r = rnum(tok[i + 1]) & 15; T->bdef[r] = 1; T->bdefv[r] = (int)tok[i + 2]; }
        else if (op == O_LABEL && T->nlabels < 64) T->labels[T->nlabels++] = rnum(tok[i + 1]);
        i += 1 + len;
    }
    (void)pre_dcl;
    /* pass 2: code */
    T->cur = &T->mainb;
    sb_put(&T->mainb, "void d3d_main() {\n");
    uint32_t i = 1;
    for (; i < ntok && !T->err;) {
        uint32_t t = tok[i], op = t & 0xFFFF;
        if (t == 0x0000FFFFu) break;
        if (op == 0xFFFE) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
        if (op == 0xFFFD) { fail(T, "phase (ps_1_4)"); break; }
        uint32_t len = T->sm >= 2 ? (t >> 24) & 0xF : (uint32_t)sm1_len(op);
        if (i + len >= ntok) { fail(T, "truncated instruction at token %u", i); break; }
        if (op != O_DCL && op != O_DEF && op != O_DEFI && op != O_DEFB) { parse(T, i, len); emit(T, i); }
        i += 1 + len;
    }
    if (!T->main_done) { sb_put(&T->mainb, "}\n"); T->main_done = 1; }
    else if (T->in_sub) sb_put(&T->fnb, "}\n");
    if (T->err) { free(T->mainb.p); free(T->fnb.p); return -1; }
    /* declarations shared by every program using this shader */
    Sb d = { 0 };
    if (s->crel) {   /* relative addressing: the whole register file, as far as the GL uniform budget allows */
        int extra = 2 + (T->iused ? 16 : 0) + (T->bused ? 1 : 0) + 2, cap = (T->vs ? sm3_vs_uniform_vectors : sm3_ps_uniform_vectors) - extra;
        int full = T->vs ? 256 : 224;
        s->nconst = cap < full ? (cap > s->nconst ? cap : s->nconst) : full;
        sb_printf(&d, "#define %s %d\n", T->vs ? "VC_LAST" : "PC_LAST", s->nconst - 1);
    }
    if (s->nconst) sb_printf(&d, "uniform vec4 %s[%d];\n", cname(T), s->nconst);
    if (T->iused & ~0u) { uint32_t u = 0; for (int k = 0; k < 16; k++) if ((T->iused >> k & 1) && !T->idef[k]) u = 1; if (u) sb_printf(&d, "uniform ivec4 %s[16];\n", iname(T)); }
    if (T->bused) { uint32_t u = 0; for (int k = 0; k < 16; k++) if ((T->bused >> k & 1) && !T->bdef[k]) u = 1; if (u) sb_printf(&d, "uniform int %s;\n", bname(T)); }
    for (int k = 0; k < 16; k++) if (s->samp[k])
        sb_printf(&d, "uniform %s %s_s%d;\n", s->samp[k] == 3 ? "samplerCube" : s->samp[k] == 4 ? "sampler3D" : "sampler2D", T->vs ? "vs" : "ps", k);
    for (int k = 0; k <= T->maxr; k++) {   /* lite: a temp the shader only ever writes at partial precision is FP16 */
        int lo = sm3_lite && !T->vs && k < 64 && T->rpp[k] && !T->rfull[k]; s->npp += lo; s->ntemp++;
        sb_printf(&d, "%svec4 r%d = vec4(0.0);\n", lo ? "mediump " : "", k);
    }
    if (T->a0) sb_put(&d, "ivec4 a0 = ivec4(0);\n");
    if (T->aL || T->vs) sb_put(&d, "int aL = 0;\n");
    if (T->p0) sb_put(&d, "bvec4 p0 = bvec4(false);\n");
    for (int k = 0; k < s->ndef; k++) {
        char f[4][48]; for (int j = 0; j < 4; j++) fmtf(f[j], s->def[k].v[j]);
        sb_printf(&d, "const vec4 k%d = vec4(%s, %s, %s, %s);\n", s->def[k].reg, f[0], f[1], f[2], f[3]);
    }
    if (T->vs) {
        for (int k = 0; k < s->nin; k++) sb_printf(&d, "layout(location = %d) in vec4 dx_in%d;\nvec4 v%d = vec4(0.0);\n", s->in[k].reg, s->in[k].reg, s->in[k].reg);
        /* every v# read must exist even without dcl */
        uint8_t seen[32] = { 0 }; for (int k = 0; k < s->nout; k++) seen[s->out[k].reg] = 1;
        for (int k = 0; k < s->nout; k++) sb_printf(&d, "vec4 o%d = vec4(0.0);\n", s->out[k].reg);
        sb_put(&d, "uniform vec4 dx_fix;\nuniform int dx_bgra;\n");
    } else {
        int ni = 0; for (int k = 0; k < s->nin; k++) if (s->in[k].reg < 16 && s->in[k].reg + 1 > ni) ni = s->in[k].reg + 1;
        if (T->sm >= 3) sb_printf(&d, "vec4 vin[%d];\n#define VIN_LAST (%d)\n", ni ? ni : 1, (ni ? ni : 1) - 1);
        else {
            for (int k = 0; k < 2; k++) sb_printf(&d, "vec4 v%d = vec4(0.0);\n", k);
            for (int k = 0; k < 8; k++) sb_printf(&d, "vec4 t%d = vec4(0.0);\n", k);
        }
        for (int k = 0; k < s->nin; k++)
            sb_printf(&d, "in %svec4 V_%s%d;\n", sm3_lite && s->inpp[k] ? "mediump " : "", sm3_usage_name(s->in[k].usage), s->in[k].index);
        for (int k = 0; k < 4; k++) if (s->outmask >> k & 1) sb_printf(&d, "layout(location = %d) out vec4 oC%d;\n", k, k);
        if (s->depth) sb_put(&d, "vec4 oDepth = vec4(0.0);\n");
        if (s->vpos) sb_put(&d, "vec4 vpos;\n");
        if (s->vface) sb_put(&d, "vec4 vface;\n");
        sb_put(&d, "uniform vec4 dx_at;\nuniform int dx_srgb;\nuniform int dx_srgbw;\n");
        if (T->srgb && sm3_lite) sb_put(&d, "vec4 d3d_srgb(vec4 c, int n) { return ((dx_srgb >> n) & 1) != 0 ? vec4(c.rgb * c.rgb, c.a) : c; }\n");
        else if (T->srgb) sb_put(&d, "vec4 d3d_srgb(vec4 c, int n) { return ((dx_srgb >> n) & 1) != 0 ? vec4(pow(max(c.rgb, vec3(0.0)), vec3(2.2)), c.a) : c; }\n");
    }
    if (T->fmul) sb_put(&d, "vec4 d3d_mul(vec4 a, vec4 b) { return mix(a * b, vec4(0.0), bvec4(vec4(equal(a, vec4(0.0))) + vec4(equal(b, vec4(0.0))))); }\n");
    if (T->femu) sb_put(&d, "float d3d_rcp(float x) { return abs(x) < 1.1754944e-38 ? (x < 0.0 ? -" FMAX_ " : " FMAX_ ") : 1.0 / x; }\n"
                            "float d3d_rsq(float x) { return abs(x) < 1.1754944e-38 ? " FMAX_ " : inversesqrt(abs(x)); }\n"
                            "float d3d_log(float x) { return abs(x) < 1.1754944e-38 ? -" FMAX_ " : log2(abs(x)); }\n"
                            "float d3d_exp(float x) { return exp2(min(x, 127.99998)); }\n"
                            "float d3d_pow(float x, float y) { return exp2(clamp(y * d3d_log(x), -150.0, 127.99998)); }\n");
    if (T->lit) sb_put(&d, "vec4 d3d_lit(vec4 s) { vec4 r = vec4(1.0, 0.0, 0.0, 1.0); if (s.x > 0.0) { r.y = s.x; "
                           "r.z = s.y > 0.0 ? pow(s.y, clamp(s.w, -127.9961, 127.9961)) : 0.0; } return r; }\n");
    for (int k = 0; k < T->nlabels; k++) sb_printf(&d, "void sub_%d();\n", T->labels[k]);
    s->decl = sb_take(&d);
    sb_put(&T->mainb, T->fnb.p ? T->fnb.p : "");
    free(T->fnb.p);
    s->body = sb_take(&T->mainb);
    return 0;
}
void sm3_free(Sm3* s) { free(s->decl); free(s->body); s->decl = s->body = NULL; }

static const char* HDR = "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler3D;\n";
/* components of mask m in order -> swizzle string */
static void comps(int m, char* o) { int k = 0; o[k++] = '.'; for (int i = 0; i < 4; i++) if (m >> i & 1) o[k++] = "xyzw"[i]; o[k] = 0; }
char* sm3_glsl_vs(const Sm3* vs, const Sm3* ps) {
    Sb b = { 0 };
    sb_put(&b, HDR);
    if (sm3_invariant) sb_put(&b, "invariant gl_Position;\n");
    sb_put(&b, vs->decl);
    if (vs->rhw) sb_put(&b, "uniform vec4 dx_vp;\n");
    /* varyings: one per pixel shader input semantic, data at the PS's declared components */
    if (ps) for (int k = 0; k < ps->nin; k++) sb_printf(&b, "out vec4 V_%s%d;\n", sm3_usage_name(ps->in[k].usage), ps->in[k].index);
    sb_put(&b, vs->body);
    sb_put(&b, "void main() {\n");
    for (int k = 0; k < vs->nin; k++) {
        int r = vs->in[k].reg;
        sb_printf(&b, "  v%d = dx_in%d; if (((dx_bgra >> %d) & 1) != 0) v%d = v%d.zyxw;\n", r, r, r, r, r);
    }
    sb_put(&b, "  d3d_main();\n");
    int pos = -1, psz = -1;
    for (int k = 0; k < vs->nout; k++) {
        if (vs->out[k].usage == 0 && vs->out[k].index == 0 && pos < 0) pos = vs->out[k].reg;
        if (vs->out[k].usage == 4 && psz < 0) psz = vs->out[k].reg;
    }
    if (pos >= 0) sb_printf(&b, "  vec4 P = o%d;\n", pos); else sb_put(&b, "  vec4 P = vec4(0.0, 0.0, 0.0, 1.0);\n");
    if (vs->rhw)   /* D3DFVF_XYZRHW / POSITIONT: render target pixels (D3D9 pixel centers at integers), z in [0, 1], w = 1 / rhw;
                    * dx_vp = (viewport x, y, 2 / width, 2 / height). The rows are stored top-down, like the projected path. */
        sb_put(&b, "  float W = P.w != 0.0 ? 1.0 / P.w : 1.0;\n"
                   "  gl_Position = vec4(((P.xy + 0.5 - dx_vp.xy) * dx_vp.zw - 1.0) * W, (2.0 * P.z - 1.0) * W, W);\n");
    else sb_put(&b, "  P.y *= dx_fix.z; P.xy += dx_fix.xy * P.w; P.z = 2.0 * P.z - P.w;\n  gl_Position = P;\n");
    if (psz >= 0) sb_printf(&b, "  gl_PointSize = max(o%d.x, 1.0);\n", psz);
    if (ps) for (int k = 0; k < ps->nin; k++) {
        const Sm3Sem* in = &ps->in[k];
        char vn[48]; snprintf(vn, sizeof vn, "V_%s%d", sm3_usage_name(in->usage), in->index);
        int found = -1;
        for (int j = 0; j < vs->nout; j++) if (vs->out[j].usage == in->usage && vs->out[j].index == in->index) { found = j; break; }
        if (found < 0) { sb_printf(&b, "  %s = vec4(0.0);\n", vn); continue; }
        const Sm3Sem* o = &vs->out[found];
        int om = o->mask ? o->mask : 15, im = in->mask ? in->mask : 15;
        const char* clampc = vs->major < 3 && o->usage == 10 ? "clamp(" : "";
        const char* clampe = vs->major < 3 && o->usage == 10 ? ", 0.0, 1.0)" : "";
        if (om == im || om == 15) {
            if (im == 15) sb_printf(&b, "  %s = %so%d%s;\n", vn, clampc, o->reg, clampe);
            else { char c[8]; comps(im, c); sb_printf(&b, "  %s = vec4(0.0); %s%s = %so%d%s%s;\n", vn, vn, c, clampc, o->reg, clampe, c); }
        } else {   /* packed: k-th component of the VS mask -> k-th component of the PS mask */
            char ci[8], co[8]; int ni = 0, no = 0;
            ci[ni++] = '.'; co[no++] = '.';
            for (int x = 0, y = 0; x < 4; x++) if (im >> x & 1) {
                while (y < 4 && !(om >> y & 1)) y++;
                if (y >= 4) break;
                ci[ni++] = "xyzw"[x]; co[no++] = "xyzw"[y]; y++;
            }
            ci[ni] = 0; co[no] = 0;
            if (ni > 1) sb_printf(&b, "  %s = vec4(0.0); %s%s = %so%d%s%s;\n", vn, vn, ci, clampc, o->reg, clampe, co);
            else sb_printf(&b, "  %s = vec4(0.0);\n", vn);
        }
    }
    sb_put(&b, "}\n");
    return sb_take(&b);
}
char* sm3_glsl_ps(const Sm3* ps) {
    Sb b = { 0 };
    sb_put(&b, HDR); sb_put(&b, ps->decl); sb_put(&b, ps->body);
    sb_put(&b, "void main() {\n");
    for (int k = 0; k < ps->nin; k++) {
        const Sm3Sem* in = &ps->in[k]; int m = in->mask ? in->mask : 15;
        char vn[48], c[8]; snprintf(vn, sizeof vn, "V_%s%d", sm3_usage_name(in->usage), in->index); comps(m, c);
        char dst[16];
        if (ps->major >= 3) snprintf(dst, sizeof dst, "vin[%d]", in->reg); else if (in->reg >= 16) snprintf(dst, sizeof dst, "t%d", in->reg - 16); else snprintf(dst, sizeof dst, "v%d", in->reg);
        if (m == 15) sb_printf(&b, "  %s = %s;\n", dst, vn); else sb_printf(&b, "  %s%s = %s%s;\n", dst, c, vn, c);
    }
    if (ps->vpos) sb_put(&b, "  vpos = vec4(gl_FragCoord.xy - vec2(0.5), 0.0, 0.0);\n");
    if (ps->vface) sb_put(&b, "  vface = vec4(gl_FrontFacing ? 1.0 : -1.0);\n");
    sb_put(&b, "  d3d_main();\n");
    if (ps->outmask & 1) {
        sb_put(&b, "  if (dx_at.x != 0.0) { float a = oC0.a, r = dx_at.y; int f = int(dx_at.x); bool ok = f == 8 || (f == 2 && a < r) || "
                   "(f == 3 && a == r) || (f == 4 && a <= r) || (f == 5 && a > r) || (f == 6 && a != r) || (f == 7 && a >= r); if (!ok) discard; }\n");
        sb_put(&b, sm3_lite ? "  if (dx_srgbw != 0) oC0.rgb = sqrt(max(oC0.rgb, vec3(0.0)));\n"
                            : "  if (dx_srgbw != 0) oC0.rgb = pow(max(oC0.rgb, vec3(0.0)), vec3(1.0 / 2.2));\n");
    }
    if (ps->depth) sb_put(&b, "  gl_FragDepth = clamp(oDepth.x, 0.0, 1.0);\n");
    sb_put(&b, "}\n");
    return sb_take(&b);
}
