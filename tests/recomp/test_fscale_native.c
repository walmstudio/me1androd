/* Compare rt_fscale with hardware x87 on x86/x86-64. Exceptions masked.
 * Build: gcc -O1 -fwrapv -fno-strict-aliasing -ffunction-sections
 *   -Wl,--gc-sections -Iruntime tests/recomp/test_fscale_native.c runtime/rt_fp.c -lm -o /tmp/test_fscale && /tmp/test_fscale
 * This checks result, sign of zero, TOP and invalid; it does not claim full
 * 80-bit x87 precision or exact overflow/underflow/precision exception flags. */
#include "rt.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
static int n, fail;
static int same(double a, double b) {
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b);
    if (a == 0.0 && b == 0.0) return !!signbit(a) == !!signbit(b);
    return a == b;
}
static void test(double x, double scale) {
    double a, b; unsigned short sw;
    asm volatile("fninit; fldl %[s]; fldl %[x]; fscale; fnstsw %[sw]; fstpl %[a]; fstpl %[b]"
       : [sw] "=m"(sw), [a] "=m"(a), [b] "=m"(b) : [x] "m"(x), [s] "m"(scale) : "st", "memory");
    CPU state; memset(&state, 0, sizeof state); CPU* c = &state;
    c->fempty = 0xff; c->ftop = 0; fpush(c, scale); fpush(c, x);
    rt_fscale(c);
    int ok = same(a, FST(0)) && same(b, FST(1)) && (((sw >> 11) & 7) == c->ftop) && (!!(sw & 1) == !!(c->fsw & 1));
    n++;
    if (!ok) { if (fail < 15) printf("FAIL x=%a scale=%a native=%a got=%a sw=%04x got=%04x\n", x, scale, a, FST(0), sw, (unsigned)fnstsw_val(c)); fail++; }
}
int main(void) {
    double xs[] = { 0., -0., 1., -1., 0.125, -0.125, 0x1p-1022, -0x1p-1022, 0x1p-1074, -0x1p-1074,
                    0x1.fffffffffffffp1023, -0x1.fffffffffffffp1023, INFINITY, -INFINITY, NAN, -NAN };
    double ys[] = { 0., -0., 1., -1., 0.5, -0.5, 1.75, -1.75, 100., -100., 1024., -1024., 1e100, -1e100, INFINITY, -INFINITY, NAN, -NAN };
    for (unsigned i=0; i<sizeof(xs)/sizeof(*xs); i++) for(unsigned j=0; j<sizeof(ys)/sizeof(*ys); j++) test(xs[i],ys[j]);
    uint32_t rng=0x4f31ab91u;
    for(int i=0;i<10000;i++) {
        rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;
        double x=((double)rng / 4294967295.0 * 2.0 - 1.0) * 1e6;
        rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;
        double y=((double)rng / 4294967295.0 * 2.0 - 1.0) * 200.0;
        test(x,y);
    }
    { CPU state; memset(&state,0,sizeof state); CPU* c=&state; c->fempty=0xff; c->fsw=1; fpush(c,1.); fpush(c,2.); rt_fscale(c); n++; if(!(c->fsw&1)) { puts("FAIL sticky invalid lost"); fail++; } }
    printf("fscale: %d checked, %d failed\n",n,fail); return fail!=0;
}
