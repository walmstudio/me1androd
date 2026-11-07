/* FISTP/FISTTP vs runtime, x86/x86-64 (SSE3 for FISTTP), masked exceptions.
 * gcc -O1 -fwrapv -fno-strict-aliasing -ffunction-sections -Wl,--gc-sections
 * -Iruntime tests/recomp/test_fist_native.c runtime/rt_fp.c -lm -o /tmp/test_fist && /tmp/test_fist
 * Compares stored integers, invalid and precision; not all exception flags. */
#include "rt.h"
#include <math.h>
#include <fenv.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static int n, fail;
#define X87_FIST(op,resv) asm volatile("fninit; fldcw %[cw]; fldl %[x]; " op " %[out]; fnstsw %[sw]" : [out] "=m"(resv), [sw] "=m"(sw) : [cw] "m"(cw), [x] "m"(x) : "st", "memory")
static void test(double x,int rc,int host,int size,int t) {
    unsigned short cw=(unsigned short)(0x37f | rc<<10),sw=0; int64_t expected=0,got=0;
    if(size==16){int16_t v; if(t){X87_FIST("fisttps",v);}else{X87_FIST("fistps",v);} expected=v;}
    else if(size==32){int32_t v; if(t){X87_FIST("fisttpl",v);}else{X87_FIST("fistpl",v);} expected=v;}
    else {int64_t v; if(t){X87_FIST("fisttpll",v);}else{X87_FIST("fistpll",v);} expected=v;}
    int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    if(fesetround(modes[host])){fail++;return;}
    CPU cpu;memset(&cpu,0,sizeof cpu);cpu.fcw=cw;
    if(size==16)got=rt_fist16(&cpu,x,t);else if(size==32)got=rt_fist32(&cpu,x,t);else got=rt_fist64(&cpu,x,t);
    int ok=expected==got && !((sw ^ cpu.fsw) & 0x21);n++;
    if(!ok){if(fail<15)printf("FAIL x=%a rc=%d host=%d size=%d t=%d expected=%lld got=%lld sw=%04x got=%04x\n",x,rc,host,size,t,(long long)expected,(long long)got,sw,(unsigned)cpu.fsw);fail++;}
}
int main(void){
 double xs[]={0.,-0.,0.1,-0.1,0.5,-0.5,1.5,-1.5,2.5,-2.5,32767.5,-32768.5,2147483647.5,-2147483648.5,
  0x1.fffffffffffffp62,-0x1p63,0x1p63,-0x1.0000000000001p63,1e100,-1e100,INFINITY,-INFINITY,NAN};
 for(unsigned i=0;i<sizeof xs/sizeof *xs;i++)for(int r=0;r<4;r++)for(int h=0;h<4;h++)for(int s=16;s<=64;s*=2)for(int t=0;t<2;t++)test(xs[i],r,h,s,t);
 uint32_t rng=0x5427ab19u;for(int i=0;i<1024;i++){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;double x=((double)rng/4294967295.0*2.-1.)*1e6;
  for(int r=0;r<4;r++)for(int h=0;h<4;h++)for(int s=16;s<=64;s*=2)for(int t=0;t<2;t++)test(x,r,h,s,t);}
 fesetround(FE_TONEAREST);printf("fist: %d checked, %d failed\n",n,fail);return fail!=0;
}
