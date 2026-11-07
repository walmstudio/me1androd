/* FRNDINT vs native x87, with every guest and host rounding-mode combination.
 * gcc -O1 -fwrapv -fno-strict-aliasing -ffunction-sections -Wl,--gc-sections
 *   -Iruntime tests/recomp/test_frnd_native.c runtime/rt_fp.c -lm -o /tmp/test_frnd && /tmp/test_frnd
 * Tests values (including signed zero), precision and C1; not all exception flags. */
#include "rt.h"
#include <math.h>
#include <fenv.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static int n, fail;
static void test(double x, int rc, int host) {
    unsigned short cw = (unsigned short)(0x37f | (rc << 10)), sw; double expected;
    asm volatile("fninit; fldcw %[cw]; fldl %[x]; frndint; fnstsw %[sw]; fstpl %[out]"
        : [out] "=m"(expected), [sw] "=m"(sw) : [cw] "m"(cw), [x] "m"(x) : "st", "memory");
    if (fesetround(host)) { puts("cannot set host rounding mode"); fail++; return; }
    CPU cpu; memset(&cpu, 0, sizeof cpu); cpu.fcw = cw;
    double got = rt_frnd(&cpu, x);
    int ok = !( (sw ^ cpu.fsw) & 0x220 ) && ((isnan(expected) && isnan(got)) || (expected == got && (expected != 0 || !!signbit(expected)==!!signbit(got))));
    n++; if (!ok) { if(fail < 15) printf("FAIL x=%a guest=%d host=%x expected=%a got=%a sw=%04x got=%04x\n",x,rc,host,expected,got,sw,(unsigned)cpu.fsw); fail++; }
}
int main(void) {
    double xs[] = {0., -0., 0.1, -0.1, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 3.5, -3.5,
      0x1.fffffffffffffp-2, -0x1.fffffffffffffp-2, 0x1.0000000000001p-1, -0x1.0000000000001p-1,
      0x1.fffffffffffffp51, -0x1.fffffffffffffp51, 0x1p52, -0x1p52,
      0x1.fffffffffffffp1023, -0x1.fffffffffffffp1023, INFINITY, -INFINITY, NAN};
    int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    for(unsigned i=0;i<sizeof(xs)/sizeof(*xs);i++) for(int rc=0;rc<4;rc++) for(int h=0;h<4;h++) test(xs[i],rc,modes[h]);
    uint32_t rng=0xf1a732d1u;
    for(int i=0;i<4096;i++) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;
        double x=((double)rng/4294967295.0*2.-1.)*1e6;
        for(int rc=0;rc<4;rc++) for(int h=0;h<4;h++) test(x,rc,modes[h]);
    }
    fesetround(FE_TONEAREST);
    /* Sticky exceptions must survive; C1 must reflect this instruction. */
    { CPU c; memset(&c, 0, sizeof c); c.fcw=0x37f; c.fsw=0x221;
      (void)rt_frnd(&c, 2.0); n++; if ((c.fsw & 0x221) != 0x21) { puts("FAIL sticky/clear C1"); fail++; }
      c.fsw=1; (void)rt_frnd(&c, 1.75); n++; if ((c.fsw & 0x221) != 0x221) { puts("FAIL sticky/set C1"); fail++; }
    }
    printf("frndint: %d checked, %d failed\n",n,fail); return fail!=0;
}
