/* FXTRACT / FSCALE / FXAM vs native x87 (masked exceptions) on special operands and
 * with preset status bits. Compared: TOP, C3 C2 C1 C0, PE UE OE ZE IE, and values
 * (NaN by class). DE (0x02) is not compared: native FLD of a double subnormal sets it
 * and the runtime does not model DE.
 * gcc -O2 -fwrapv -fno-strict-aliasing -ffunction-sections -Wl,--gc-sections -Iruntime \
 *   tests/recomp/test_fpu_misc_native.c runtime/rt_fp.c -lm -o /tmp/t && /tmp/t */
#include "rt.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#if !defined(__x86_64__) && !defined(__i386__)
#error x86 only
#endif
#define MASK 0x7F3Du   /* TOP + C3 C2 C1 C0 + PE UE OE ZE IE */
static void env(unsigned sw0) { uint32_t e[7] = {0x37f, sw0 & ~0x3800u, 0xFFFF, 0, 0, 0, 0};
    asm volatile("fninit; fldenv %0" :: "m"(e) : "memory"); }
static int veq(double a, double b) { return (isnan(a) && isnan(b)) || (a == b && !!signbit(a) == !!signbit(b)); }
static int fail, checked;
static int verbose;
#define NG 256
static struct { char op[8]; unsigned sw0, x, vm; int cnt; double a, b; } grp[NG]; static int ngrp;
static void report(const char* op, unsigned sw0, double a, double b, const char* what, double nv, unsigned ns, double gv, unsigned gs) {
    unsigned x = (ns ^ gs) & MASK, vm = !veq(nv, gv); int k;
    for (k = 0; k < ngrp; k++) if (!strcmp(grp[k].op, op) && grp[k].x == x && grp[k].vm == vm && grp[k].sw0 == sw0) break;
    if (k == ngrp && ngrp < NG) { strcpy(grp[ngrp].op, op); grp[ngrp].x = x; grp[ngrp].vm = vm; grp[ngrp].sw0 = sw0; grp[ngrp].a = a; grp[ngrp].b = b; grp[ngrp].cnt = 0; ngrp++; }
    if (k < NG) grp[k].cnt++;
    if (verbose && fail < 40) printf("%s sw0=%04x a=%a b=%a %s native=%a sw=%04x runtime=%a sw=%04x\n", op, sw0, a, b, what, nv, ns, gv, gs);
    fail++;
}

/* FSCALE exception flags at the 80-bit exponent boundaries, all four guest rounding modes.
 * Values are not compared (the runtime holds a double); status bits are. */
static void fscale_bounds(void) {
    double ms[] = {1.0, 1.5, -1.0, -1.5, 3.0, 0x1.fffffffffffffp0, -0x1.fffffffffffffp0, 0x1.0000000000001p0, 0x1p-1074, 3 * 0x1p-1074, 0x1p-1022, 1e300, -1e-300};
    int ks[64], nk = 0;
    for (int k = 16380; k <= 16388; k++) ks[nk++] = k;
    for (int k = -16452; k <= -16436; k++) ks[nk++] = k;
    int more[] = {-16381, -16382, -16383, 0, 1, -1, 20000, -20000, 65536, -65536, 100000, -100000};
    for (unsigned i = 0; i < sizeof more / sizeof *more; i++) ks[nk++] = more[i];
    for (unsigned d = 0; d < 2; d++) for (unsigned rc = 0; rc < 4; rc++)
    for (unsigned i = 0; i < sizeof ms / sizeof *ms; i++) for (int j = 0; j < nk; j++) {
        double a = ms[i], kk = ks[j]; unsigned short sw; uint32_t e[7] = {0x37f | (rc << 10), d ? 0x4300u : 0u, 0xFFFF, 0, 0, 0, 0};
        asm volatile("fninit; fldenv %1; fldl %2; fldl %3; fscale; fnstsw %0; fstp %%st(0); fstp %%st(0)" : "=m"(sw) : "m"(e), "m"(kk), "m"(a) : "st", "memory");
        CPU cpu, *c = &cpu; memset(&cpu, 0, sizeof cpu); cpu.fcw = 0x37f | (rc << 10); cpu.fempty = 0xFF; cpu.fsw = d ? 0x4300u : 0u;
        fpush(c, kk); fpush(c, a); rt_fscale(c);
        unsigned gs = fnstsw_val(c) & MASK, ns = sw & MASK; checked++;
        if (ns != gs) { if (fail < 40) printf("FSCALEB rc=%u sw0=%04x a=%a k=%d native sw=%04x runtime sw=%04x\n", rc, d ? 0x4300u : 0u, a, ks[j], ns, gs); fail++; }
    }
}
int main(int argc, char** argv) {
    verbose = argc > 1;
    double den = 0x1p-1074, big = 0x1.fffffffffffffp1023;
    double v[] = {0., -0., 1., -1., 3., -3.5, 0.5, 1.5, den, -den, 0x1p-1022, big, -big, 1e300, 1e-300,
                  INFINITY, -INFINITY, NAN, -NAN, 100000., -100000., 2.5, 70000.5};
    int n = sizeof v / sizeof *v;
    static const unsigned dirty[] = {0x0000, 0x4300, 0x4700, 0x0200, 0x0400, 0x0020};
    unsigned nd = sizeof dirty / sizeof *dirty;
    for (unsigned d = 0; d < nd; d++) {
        for (int i = 0; i < n; i++) {
            double a = v[i], r0, r1; unsigned short s;
            /* FXTRACT: ST0 = significand, ST1 = exponent */
            env(dirty[d]);
            asm volatile("fldl %3; fxtract; fnstsw %2; fstpl %0; fstpl %1" : "=m"(r0), "=m"(r1), "=m"(s) : "m"(a) : "st", "memory");
            CPU cpu, *c = &cpu; memset(&cpu, 0, sizeof cpu); cpu.fcw = 0x37f; cpu.fempty = 0xFF; cpu.fsw = dirty[d];
            fpush(c, a); rt_fxtract(c);
            unsigned gs = fnstsw_val(c) & MASK, ns = s & MASK; double g0 = FST(0), g1 = FST(1); checked++;
            if (!veq(r0, g0) || !veq(r1, g1) || ns != gs) report("FXTRACT", dirty[d], a, 0, "st0/st1", r0, ns, g0, gs);
            /* FXAM */
            env(dirty[d]);
            asm volatile("fldl %1; fxam; fnstsw %0; fstp %%st(0)" : "=m"(s) : "m"(a) : "st", "memory");
            memset(&cpu, 0, sizeof cpu); cpu.fcw = 0x37f; cpu.fempty = 0xFF; cpu.fsw = dirty[d];
            fpush(c, a); rt_fxam(c); gs = fnstsw_val(c) & MASK; ns = s & MASK; checked++;
            if (ns != gs) report("FXAM", dirty[d], a, 0, "sw", 0, ns, 0, gs);
            for (int j = 0; j < n; j++) {
                double b = v[j], r; /* FSCALE: ST0 = a, ST1 = b -> ST0 = a * 2^trunc(b) */
                env(dirty[d]);
                asm volatile("fldl %3; fldl %2; fscale; fnstsw %1; fstpl %0; fstp %%st(0)" : "=m"(r), "=m"(s) : "m"(a), "m"(b) : "st", "memory");
                memset(&cpu, 0, sizeof cpu); cpu.fcw = 0x37f; cpu.fempty = 0xFF; cpu.fsw = dirty[d];
                fpush(c, b); fpush(c, a); rt_fscale(c);
                g0 = FST(0); gs = fnstsw_val(c) & MASK; ns = s & MASK; checked++;
                /* 100000 / 70000.5 style scale: double range overflows identically; compare as double */
                if (!veq(r, g0) || ns != gs) report("FSCALE", dirty[d], a, b, "st0", r, ns, g0, gs);
            }
        }
    }
    fscale_bounds();
    for (int k = 0; k < ngrp; k++) printf("%-7s sw0=%04x diffbits=%04x valuemismatch=%u count=%d e.g. a=%a b=%a\n", grp[k].op, grp[k].sw0, grp[k].x, grp[k].vm, grp[k].cnt, grp[k].a, grp[k].b);
    printf("fpu misc: %d checked, %d failed\n", checked, fail);
    return fail != 0;
}
