/* FPREM/FPREM1 on special operands vs native x87 (masked exceptions):
 * result value class and status bits C3 C2 C1 C0 + IE/DE/ZE/PE.
 * gcc -O2 -fwrapv -fno-strict-aliasing -ffunction-sections -Wl,--gc-sections -Iruntime \
 *   tests/recomp/test_fprem_special_native.c runtime/rt_fp.c -lm -o /tmp/t && /tmp/t
 * Prints every mismatch; exit 1 if any. Values are compared as double (NaN by class,
 * sign of NaN compared separately and reported as SIGN). */
#include "rt.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#if !defined(__x86_64__) && !defined(__i386__)
#error x86 only
#endif
#define MASK 0x4735u   /* C3 C2 C1 C0 + PE UE OE ZE IE. DE (0x02) is excluded: native FLD of a double
                        * subnormal sets it, the runtime does not model DE anywhere. */
static double native(double a, double b, int ieee, unsigned short *sw, unsigned sw0) {
    double r; unsigned short s;
    uint32_t env[7] = {0x37f, sw0 & ~0x3800u, 0xFFFF, 0, 0, 0, 0};   /* TOP=0, all empty */
    asm volatile("fninit; fldenv %0" :: "m"(env) : "memory");
    if (ieee) asm volatile("fldl %3; fldl %2; fprem1; fnstsw %1; fstpl %0; fstp %%st(0)" : "=m"(r), "=m"(s) : "m"(a), "m"(b) : "st", "memory");
    else      asm volatile("fldl %3; fldl %2; fprem; fnstsw %1; fstpl %0; fstp %%st(0)"  : "=m"(r), "=m"(s) : "m"(a), "m"(b) : "st", "memory");
    *sw = s; return r;
}
int main(void) {
    double den = 0x1p-1074, big = 0x1.fffffffffffffp1023;
    double v[] = {0., -0., 1., -1., 3., -3.5, 0.5, den, -den, 0x1p-1022, big, -big, 1e300, 1e-300,
                  INFINITY, -INFINITY, NAN, -NAN};
    int n = sizeof v / sizeof *v, fail = 0, checked = 0, sign = 0;
    static const unsigned dirty[] = {0x0000, 0x4300, 0x4700, 0x0400, 0x0200, 0x0020};
    for (unsigned d = 0; d < sizeof dirty / sizeof *dirty; d++)
    for (int ieee = 0; ieee < 2; ieee++)
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
        unsigned short nsw; double nr = native(v[i], v[j], ieee, &nsw, dirty[d]);
        CPU cpu, *c = &cpu; memset(&cpu, 0, sizeof cpu); cpu.fcw = 0x37f; cpu.ftop = 0; cpu.fempty = 0xFF; cpu.fsw = dirty[d];
        fpush(c, v[j]); fpush(c, v[i]);   /* ST0 = a (dividend), ST1 = b */
        rt_fprem(c, ieee);
        double gr = FST(0); unsigned gsw = c->fsw & MASK, nw = nsw & MASK;
        int partial = (nw & 0x400u) && (gsw & 0x400u);   /* partial reduction: intermediate is implementation-defined */
        int vok = partial || (isnan(nr) && isnan(gr)) || (nr == gr && signbit(nr) == signbit(gr));
        checked++;
        if (!vok || gsw != nw) {
            if (vok && isnan(nr) && gsw == nw) { sign++; continue; }
            if (fail < 40) printf("sw0=%04x %s a=%a b=%a native r=%a sw=%04x runtime r=%a sw=%04x\n", dirty[d], ieee ? "FPREM1" : "FPREM", v[i], v[j], nr, nw, gr, gsw);
            fail++;
        }
    }
    printf("fprem special: %d checked, %d failed (%d NaN-sign-only diffs ignored)\n", checked, fail, sign);
    return fail != 0;
}
