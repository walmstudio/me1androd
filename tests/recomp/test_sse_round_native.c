/* CVTSD2SI/CVTTSD2SI vs rt_cvt_f2i32, masked exceptions, x86/x86-64.
 * Build: gcc -O1 -fwrapv -fno-strict-aliasing -ffunction-sections
 * -Wl,--gc-sections -Iruntime tests/recomp/test_sse_round_native.c runtime/rt_fp.c -lm -o /tmp/test_sse_round && /tmp/test_sse_round
 * This tests values, not MXCSR exception flags or subnormal DAZ behavior. */
#include "rt.h"
#include <math.h>
#include <fenv.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static int n,fail;
static void test(double x,int rc,int host,int truncating) {
    int modes[]={FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}; int expected;
    if (fesetround(modes[rc])) { fail++; return; }
    if (truncating) asm volatile("cvttsd2si %[x], %[out]" : [out] "=r"(expected) : [x] "x"(x));
    else asm volatile("cvtsd2si %[x], %[out]" : [out] "=r"(expected) : [x] "x"(x));
    if (fesetround(modes[host])) { fail++; return; }
    CPU cpu; memset(&cpu,0,sizeof cpu); cpu.mxcsr=0x1f80 | (rc<<13);
    int got=rt_cvt_f2i32(&cpu,x,truncating); n++;
    if (got != expected) { if(fail<15) printf("FAIL x=%a guest=%d host=%d trunc=%d expected=%d got=%d\n",x,rc,host,truncating,expected,got); fail++; }
}
int main(void) {
    double xs[]={0.,-0.,0.1,-0.1,0.5,-0.5,1.5,-1.5,2.5,-2.5,3.5,-3.5,0x1.fffffffffffffp-2,
      0x1.0000000000001p-1,2147483647.,2147483647.5,2147483648.,-2147483648.,-2147483648.5,
      1e100,-1e100,INFINITY,-INFINITY,NAN};
    for(unsigned i=0;i<sizeof(xs)/sizeof(*xs);i++)for(int r=0;r<4;r++)for(int h=0;h<4;h++)for(int t=0;t<2;t++)test(xs[i],r,h,t);
    uint32_t rng=0xa815e072u;
    for(int i=0;i<2048;i++){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;double x=((double)rng/4294967295.0*2.-1.)*1e6;
      for(int r=0;r<4;r++)for(int h=0;h<4;h++)for(int t=0;t<2;t++)test(x,r,h,t);}
    fesetround(FE_TONEAREST); printf("sse conversions: %d checked, %d failed\n",n,fail); return fail!=0;
}
