/* ME1 recomp runtime: x87 / SSE helpers that are too large to inline. */
#include "rt.h"
#include "cpu_profile.h"
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
/* Nearest-even independent of host fenv. modf is exact; when a fractional
 * part exists the integral value and its adjustment by one are representable. */
static double rt_round_nearest(double x) {
    double i, f = modf(x, &i), a = fabs(f);
    if (a > 0.5 || (a == 0.5 && fmod(fabs(i), 2.0) == 1.0)) i += copysign(1.0, x);
    return i;
}
/* floor/ceil built on trunc: libm floor()/ceil() may follow the host rounding
 * mode (with FE_DOWNWARD, glibc returns -0 for floor(0.1) because x - x is -0
 * there). trunc is exact and sign-preserving; the +/-1 adjustment is on an
 * integer below 2^53, so it is exact in every host mode. NaN/inf pass through. */
static double rt_floor_exact(double x) { double r = trunc(x); return r > x ? r - 1.0 : r; }
static double rt_ceil_exact(double x)  { double r = trunc(x); return r < x ? r + 1.0 : r; }
static double rt_round_fcw(CPU* c, double x) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return rt_round_nearest(x);
    case 1: return rt_floor_exact(x);
    case 2: return rt_ceil_exact(x);
    default: return trunc(x);
    }
}
/* FRNDINT sets sticky precision for a changed finite value and C1 when
 * rounding increases its magnitude. Keep FIST's conversion path separate:
 * an out-of-range result raises invalid, not precision. */
double rt_frnd(CPU* c, double x) {
    double r = rt_round_fcw(c, x);
    c->fsw &= ~0x200u;
    if (isfinite(x) && r != x) {
        c->fsw |= 0x20u;
        if (fabs(r) > fabs(x)) c->fsw |= 0x200u;
    }
    return r;
}
#define FIST_BODY(T, LO, HI, INDEF) \
    double r = t ? trunc(x) : rt_round_fcw(c, x); \
    if (!(r >= (LO) && r <= (HI))) { c->fsw |= 1; return (T)(INDEF); } \
    if (r != x) c->fsw |= 0x20; \
    return (T)r;
int16_t rt_fist16(CPU* c, double x, int t) { FIST_BODY(int16_t, -32768.0, 32767.0, INT16_MIN) }
int32_t rt_fist32(CPU* c, double x, int t) { FIST_BODY(int32_t, -2147483648.0, 2147483647.0, INT32_MIN) }
int64_t rt_fist64(CPU* c, double x, int t) {
    double r = t ? trunc(x) : rt_round_fcw(c, x);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) { c->fsw |= 1; return INT64_MIN; }
    if (r != x) c->fsw |= 0x20;
    return (int64_t)r;
}

/* ---- misc x87 ---- */
void rt_fxam(CPU* c) {
    CPU_PROF_START(CPU_PROF_FXAM);
    double x = FST(0); uint32_t sw = c->fsw & ~0x4700u;
    if (signbit(x)) sw |= 0x200;                          /* C1 = sign */
    if (c->fempty & (1u << c->ftop)) sw |= 0x4100;        /* empty */
    else if (isnan(x)) sw |= 0x0100;
    else if (isinf(x)) sw |= 0x0500;
    else if (x == 0) sw |= 0x4000;
    else sw |= 0x0400;          /* normal (double subnormals are normal in 80-bit format) */
    c->fsw = sw;
    CPU_PROF_END(CPU_PROF_FXAM);
}
void rt_fprem(CPU* c, int ieee) {
    CPU_PROF_START(CPU_PROF_FPREM);
    double a = FST(0), b = FST(1);
    /* Hardware (checked against native x87 with preset status bits): NaN and invalid
     * clear C2 and C1 and leave C0/C3 unchanged; a partial reduction sets C2 and clears
     * C0/C1/C3. */
    if (isnan(a) || isnan(b)) { FST(0) = isnan(a) ? a : b; c->fsw &= ~0x600u; CPU_PROF_END(CPU_PROF_FPREM); return; }
    if (isinf(a) || b == 0) { FST(0) = -NAN; c->fsw = (c->fsw & ~0x600u) | 1u; CPU_PROF_END(CPU_PROF_FPREM); return; }
    if (isinf(b) || a == 0) { c->fsw &= ~0x4700u; CPU_PROF_END(CPU_PROF_FPREM); return; }
    int d = ilogb(fabs(a)) - ilogb(fabs(b));
    if (d >= 64) {
        /* Both instructions use truncated partial reduction. N=32 is one
         * permitted implementation choice; quotient flags are undefined. */
        double scaled = scalbn(b, d - 32);
        FST(0) = FN64(fmod(a, scaled));
        c->fsw = (c->fsw & ~0x4300u) | 0x400u;
        CPU_PROF_END(CPU_PROF_FPREM);
        return;
    }
    /* remquo supplies at least the low three quotient bits even when the
     * complete quotient is too large to represent exactly in a double. */
    int q = 0;
    double nearest = remquo(a, b, &q);
    double r = ieee ? nearest : fmod(a, b);
    unsigned qi = q < 0 ? 0u - (unsigned)q : (unsigned)q;
    if (!ieee && r != nearest) qi--; /* nearest quotient was one farther from zero */
    c->fsw &= ~0x4700u;
    FST(0) = FN64(r);
    if (qi & 1) c->fsw |= 0x0200;
    if (qi & 2) c->fsw |= 0x4000;
    if (qi & 4) c->fsw |= 0x0100;
    CPU_PROF_END(CPU_PROF_FPREM);
}

void rt_fprem1(CPU* c) {
    CPU_PROF_START(CPU_PROF_FPREM1);
    double a = FST(0), b = FST(1);
    if (isnan(a) || isnan(b)) { FST(0) = isnan(a) ? a : b; c->fsw &= ~0x600u; CPU_PROF_END(CPU_PROF_FPREM1); return; }
    if (isinf(a) || b == 0) { FST(0) = -NAN; c->fsw = (c->fsw & ~0x600u) | 1u; CPU_PROF_END(CPU_PROF_FPREM1); return; }
    if (isinf(b) || a == 0) { c->fsw &= ~0x4700u; CPU_PROF_END(CPU_PROF_FPREM1); return; }
    int d = ilogb(fabs(a)) - ilogb(fabs(b));
    if (d >= 64) {
        double scaled = scalbn(b, d - 32);
        FST(0) = FN64(fmod(a, scaled));
        c->fsw = (c->fsw & ~0x4300u) | 0x400u;
        CPU_PROF_END(CPU_PROF_FPREM1);
        return;
    }
    int q = 0;
    double nearest = remquo(a, b, &q);
    double r = nearest;
    unsigned qi = q < 0 ? 0u - (unsigned)q : (unsigned)q;
    c->fsw &= ~0x4700u;
    FST(0) = FN64(r);
    if (qi & 1) c->fsw |= 0x0200;
    if (qi & 2) c->fsw |= 0x4000;
    if (qi & 4) c->fsw |= 0x0100;
    CPU_PROF_END(CPU_PROF_FPREM1);
}

void rt_fxtract(CPU* c) {
    double x = FST(0);
    c->fsw &= ~0x200u;           /* push without stack fault clears C1 */
    if (isnan(x)) { fpush(c, x); return; }
    if (x == 0) { FST(0) = -INFINITY; fpush(c, x); c->fsw |= 4; return; }
    if (isinf(x)) { FST(0) = INFINITY; fpush(c, x); return; }
    int e; double m = frexp(x, &e);
    FST(0) = (double)(e - 1); fpush(c, m * 2.0);
}
void rt_fscale(CPU* c) {
    double x = FST(0), s = trunc(FST(1));
    c->fsw &= ~0x200u;           /* C1 is cleared unless rounding/stack fault says otherwise */
    if (isnan(s)) { FST(0) = s; return; }
    /* Masked x87 invalid: 0 * 2^(+inf) and inf * 2^(-inf).
     * Distinguish the infinite scale before clamping; large finite scales
     * must keep zero and infinity unchanged. Quiet NaNs take precedence. */
    if (!isnan(x) && isinf(s) && ((x == 0.0 && s > 0) || (isinf(x) && s < 0))) {
        FST(0) = -NAN; c->fsw |= 1u; return;
    }
    int sinf = isinf(s);          /* x * 2^(+-inf) is exact: no overflow/underflow flags */
    if (s > 100000) s = 100000;
    if (s < -100000) s = -100000;
    int si = (int)s;
    if (!sinf && isfinite(x) && x != 0.0) {
        /* Exception flags follow the 80-bit exponent range, not double's (the value itself
         * is still limited to double). x = m * 2^q with m an odd integer (<= 53 bits). */
        int e2; double f = frexp(fabs(x), &e2);
        uint64_t m = (uint64_t)ldexp(f, 53); int q = e2 - 53, tz = __builtin_ctzll(m);
        m >>= tz; q += tz;
        int rc = (c->fcw >> 10) & 3, neg = signbit(x) != 0;
        if (e2 - 1 + si > 16383) {                       /* overflow: OE|PE, C1 = rounded up */
            c->fsw |= 0x28u;
            if (rc == 0 || (rc == 1 && neg) || (rc == 2 && !neg)) c->fsw |= 0x200u;
        } else if (q + si < -16445) {                    /* inexact denormal/zero: UE|PE */
            c->fsw |= 0x30u;
            int k = -16445 - (q + si);                   /* low bits of m that are dropped */
            int up = 0;
            if (k < 64) {
                uint64_t low = m & ((1ull << k) - 1), half = 1ull << (k - 1);
                if (rc == 0) up = low > half || (low == half && ((m >> k) & 1));
                else if (rc == 1) up = neg && low;
                else if (rc == 2) up = !neg && low;
            } else if (rc == 1) up = neg;
            else if (rc == 2) up = !neg;
            if (up) c->fsw |= 0x200u;
        }
    }
    FST(0) = ldexp(FST(0), si);
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

void rt_fsin(CPU* c) {
    CPU_PROF_START(CPU_PROF_FSIN);
    double x = FST(0);
    c->fsw &= ~0x4700u;
    if (isnan(x)) { c->fsw |= 0x0100u; CPU_PROF_END(CPU_PROF_FSIN); return; }
    if (isinf(x)) { c->fsw |= 0x0100u; FST(0) = -NAN; CPU_PROF_END(CPU_PROF_FSIN); return; }
    if (fabs(x) >= 2e63) { c->fsw |= 0x400u; CPU_PROF_END(CPU_PROF_FSIN); return; } /* C2 set for out-of-range */
    FST(0) = FN64(sin(x));
    CPU_PROF_END(CPU_PROF_FSIN);
}
void rt_fcos(CPU* c) {
    CPU_PROF_START(CPU_PROF_FCOS);
    double x = FST(0);
    c->fsw &= ~0x4700u;
    if (isnan(x)) { c->fsw |= 0x0100u; CPU_PROF_END(CPU_PROF_FCOS); return; }
    if (isinf(x)) { c->fsw |= 0x0100u; FST(0) = -NAN; CPU_PROF_END(CPU_PROF_FCOS); return; }
    if (fabs(x) >= 2e63) { c->fsw |= 0x400u; CPU_PROF_END(CPU_PROF_FCOS); return; } /* C2 set for out-of-range */
    FST(0) = FN64(cos(x));
    CPU_PROF_END(CPU_PROF_FCOS);
}
void rt_fptan(CPU* c) {
    CPU_PROF_START(CPU_PROF_FPTAN);
    double x = FST(0);
    FST(0) = FN64(tan(x));
    fpush(c, (isnan(x) || isinf(x)) ? FST(0) : 1.0);
    c->fsw &= ~0x400u;
    CPU_PROF_END(CPU_PROF_FPTAN);
}
void rt_f2xm1(CPU* c) {
    CPU_PROF_START(CPU_PROF_F2XM1);
    double x = FST(0);
    c->fsw &= ~0x4700u;
    /* Domain: -1 <= x <= 1 */
    if (x < -1.0 || x > 1.0) {
        c->fsw |= 0x0100u; /* IE */
        FST(0) = -NAN;
        CPU_PROF_END(CPU_PROF_F2XM1);
        return;
    }
    FST(0) = FN64(expm1(x * 0.69314718055994530942));
    CPU_PROF_END(CPU_PROF_F2XM1);
}
void rt_fyl2x(CPU* c) {
    CPU_PROF_START(CPU_PROF_FYL2X);
    double x = FST(0), y = FST(1);
    c->fsw &= ~0x4700u;
    FST(1) = FN64(y * log2(x));
    fpop(c);
    CPU_PROF_END(CPU_PROF_FYL2X);
}
void rt_fyl2xp1(CPU* c) {
    CPU_PROF_START(CPU_PROF_FYL2XP1);
    double x = FST(0), y = FST(1);
    c->fsw &= ~0x4700u;
    FST(1) = FN64(y * (log1p(x) / 0.69314718055994530942));
    fpop(c);
    CPU_PROF_END(CPU_PROF_FYL2XP1);
}
/* ---- SSE conversions ---- */
int32_t rt_cvt_f2i32(CPU* c, double x, int t) {
    double r;
    if (t) r = trunc(x);
    else switch ((c->mxcsr >> 13) & 3) {
        case 0: r = rt_round_nearest(x); break;
        case 1: r = floor(x); break;
        case 2: r = ceil(x); break;
        default: r = trunc(x);
    }
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN;
    return (int32_t)r;
}
float rt_sse_i2f(CPU* c, int32_t x) { (void)c; return (float)x; }
