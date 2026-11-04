/* ME1 recomp runtime: x87 / SSE helpers that are too large to inline. */
#include "rt.h"
#include <math.h>

/* ---- 80-bit extended <-> double ---- */
double rt_rdf80(uint32_t a) {
    uint64_t m = rd64(a); uint16_t se = rd16(a + 8);
    int e = se & 0x7fff; double r;
    if (e == 0x7fff) r = (m << 1) ? NAN : INFINITY;     /* sign applied below */
    else if (m == 0) r = 0.0;
    else r = ldexp((double)m, e - 16383 - 63);   /* (double)m rounds 64 -> 53 bits */
    return (se & 0x8000) ? -r : r;
}
void rt_wrf80(uint32_t a, double v) {
    uint64_t bits; memcpy(&bits, &v, 8);
    unsigned sign = (unsigned)(bits >> 63), e = (bits >> 52) & 0x7ff; uint64_t f = bits & ((1ull << 52) - 1), m; unsigned E;
    if (e == 0x7ff) { E = 0x7fff; m = f ? (0xC000000000000000ull | (f << 11)) : 0x8000000000000000ull; }
    else if (e == 0) {
        if (!f) { E = 0; m = 0; }
        else { int s = __builtin_clzll(f); m = f << s; E = 15372 - s; }
    } else { E = e - 1023 + 16383; m = (1ull << 63) | (f << 11); }
    wr64(a, m); wr16(a + 8, (uint16_t)((sign << 15) | E));
}

/* ---- rounding / integer stores ---- */
double rt_frnd(CPU* c, double x) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(x);
    case 1: return floor(x);
    case 2: return ceil(x);
    default: return trunc(x);
    }
}
#define FIST_BODY(T, LO, HI, INDEF) \
    double r = t ? trunc(x) : rt_frnd(c, x); \
    if (!(r >= (LO) && r <= (HI))) { c->fsw |= 1; return (T)(INDEF); } \
    if (r != x) c->fsw |= 0x20; \
    return (T)r;
int16_t rt_fist16(CPU* c, double x, int t) { FIST_BODY(int16_t, -32768.0, 32767.0, INT16_MIN) }
int32_t rt_fist32(CPU* c, double x, int t) { FIST_BODY(int32_t, -2147483648.0, 2147483647.0, INT32_MIN) }
int64_t rt_fist64(CPU* c, double x, int t) {
    double r = t ? trunc(x) : rt_frnd(c, x);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) { c->fsw |= 1; return INT64_MIN; }
    if (r != x) c->fsw |= 0x20;
    return (int64_t)r;
}

/* ---- misc x87 ---- */
void rt_fxam(CPU* c) {
    double x = FST(0); uint32_t sw = c->fsw & ~0x4700u;
    if (signbit(x)) sw |= 0x200;                          /* C1 = sign */
    if (c->fempty & (1u << c->ftop)) sw |= 0x4100;        /* empty */
    else if (isnan(x)) sw |= 0x0100;
    else if (isinf(x)) sw |= 0x0500;
    else if (x == 0) sw |= 0x4000;
    else sw |= 0x0400;          /* normal (double subnormals are normal in 80-bit format) */
    c->fsw = sw;
}
void rt_fprem(CPU* c, int ieee) {
    double a = FST(0), b = FST(1);
    c->fsw &= ~0x4700u;          /* C2 = 0: reduction always completes in one step */
    if (isnan(a) || isnan(b)) { FST(0) = isnan(a) ? a : b; return; }
    if (isinf(a) || b == 0) { FST(0) = -NAN; c->fsw |= 1; return; }   /* real indefinite */
    if (isinf(b)) return;
    double r = ieee ? remainder(a, b) : fmod(a, b);
    double q = fabs(nearbyint((a - r) / b));
    uint64_t qi = q < 18446744073709551616.0 ? (uint64_t)q : 0;
    FST(0) = FN64(r);
    if (qi & 1) c->fsw |= 0x0200;   /* C1 = Q0 */
    if (qi & 2) c->fsw |= 0x4000;   /* C3 = Q1 */
    if (qi & 4) c->fsw |= 0x0100;   /* C0 = Q2 */
}
void rt_fxtract(CPU* c) {
    double x = FST(0);
    if (isnan(x)) { fpush(c, x); return; }
    if (x == 0) { FST(0) = -INFINITY; fpush(c, x); c->fsw |= 4; return; }
    if (isinf(x)) { FST(0) = INFINITY; fpush(c, x); return; }
    int e; double m = frexp(x, &e);
    FST(0) = (double)(e - 1); fpush(c, m * 2.0);
}
void rt_fscale(CPU* c) {
    double s = trunc(FST(1));
    if (isnan(s)) { FST(0) = s; return; }
    if (s > 100000) s = 100000;
    if (s < -100000) s = -100000;
    FST(0) = ldexp(FST(0), (int)s);
}
void rt_fninit(CPU* c) { c->fcw = 0x37F; c->fsw = 0; c->ftop = 0; c->fempty = 0xFF; }

static uint32_t ftw_full(CPU* c) {
    uint32_t t = 0;
    for (int r = 0; r < 8; r++) {
        unsigned tag;
        if (c->fempty & (1u << r)) tag = 3;
        else { double v = c->st[r]; tag = v == 0 ? 1 : (isnan(v) || isinf(v)) ? 2 : 0; }
        t |= tag << (2 * r);
    }
    return t;
}
void rt_fnstenv(CPU* c, uint32_t a) {
    wr32(a, 0xFFFF0000u | c->fcw); wr32(a + 4, 0xFFFF0000u | fnstsw_val(c)); wr32(a + 8, 0xFFFF0000u | ftw_full(c));
    wr32(a + 12, 0); wr32(a + 16, 0); wr32(a + 20, 0); wr32(a + 24, 0xFFFF0000u);
    c->fcw |= 0x3F;              /* FNSTENV masks all exceptions afterwards */
}
void rt_fldenv(CPU* c, uint32_t a) {
    c->fcw = rd16(a); uint32_t sw = rd16(a + 4); c->ftop = (sw >> 11) & 7; c->fsw = sw & ~0x3800u;
    uint32_t tw = rd16(a + 8); c->fempty = 0;
    for (int r = 0; r < 8; r++) if (((tw >> (2 * r)) & 3) == 3) c->fempty |= 1u << r;
}
void rt_fnsave(CPU* c, uint32_t a) {
    rt_fnstenv(c, a);
    for (int i = 0; i < 8; i++) rt_wrf80(a + 28 + 10 * i, FST(i));
    rt_fninit(c);
}
void rt_frstor(CPU* c, uint32_t a) {
    rt_fldenv(c, a);
    for (int i = 0; i < 8; i++) FST(i) = rt_rdf80(a + 28 + 10 * i);
}
void rt_fxsave(CPU* c, uint32_t a) {
    memset(M + a, 0, 512);
    wr16(a, (uint16_t)c->fcw); wr16(a + 2, (uint16_t)fnstsw_val(c));
    uint8_t ab = 0; for (int r = 0; r < 8; r++) if (!(c->fempty & (1u << r))) ab |= 1u << r;
    wr8(a + 4, ab); wr32(a + 24, c->mxcsr); wr32(a + 28, 0xFFFF);
    for (int i = 0; i < 8; i++) rt_wrf80(a + 32 + 16 * i, FST(i));
    for (int i = 0; i < 8; i++) wrx(a + 160 + 16 * i, c->xmm[i]);
}
void rt_fxrstor(CPU* c, uint32_t a) {
    c->fcw = rd16(a); uint32_t sw = rd16(a + 2); c->ftop = (sw >> 11) & 7; c->fsw = sw & ~0x3800u;
    uint8_t ab = rd8(a + 4); c->fempty = (uint8_t)~ab; c->mxcsr = rd32(a + 24);
    for (int i = 0; i < 8; i++) FST(i) = rt_rdf80(a + 32 + 16 * i);
    for (int i = 0; i < 8; i++) c->xmm[i] = rdx(a + 160 + 16 * i);
}

/* ---- SSE conversions ---- */
int32_t rt_cvt_f2i32(CPU* c, double x, int t) {
    double r;
    if (t) r = trunc(x);
    else switch ((c->mxcsr >> 13) & 3) {
        case 0: r = nearbyint(x); break;
        case 1: r = floor(x); break;
        case 2: r = ceil(x); break;
        default: r = trunc(x);
    }
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN;
    return (int32_t)r;
}
float rt_sse_i2f(CPU* c, int32_t x) { (void)c; return (float)x; }
