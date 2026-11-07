/* Native x86 comparison. Partial reductions may differ per implementation;
   compare final result after looping, and quotient bits only for a single step. */
#include "rt.h"
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <string.h>
#if !defined(__x86_64__) && !defined(__i386__)
#error This reference test requires x86/x86-64 hardware.
#endif
static double native(double a, double b, int ieee, int loop, unsigned short *sw) {
    double r; unsigned short s;
    if (ieee) {
        if (loop) __asm__ volatile("fninit; fldl %3; fldl %2; 1: fprem1; fnstsw %%ax; test $0x400, %%ax; jnz 1b; fstpl %0; fstp %%st(0)" : "=m"(r), "=a"(s) : "m"(a), "m"(b) : "cc", "st", "memory");
        else __asm__ volatile("fninit; fldl %3; fldl %2; fprem1; fnstsw %%ax; fstpl %0; fstp %%st(0)" : "=m"(r), "=a"(s) : "m"(a), "m"(b) : "st", "memory");
    } else {
        if (loop) __asm__ volatile("fninit; fldl %3; fldl %2; 1: fprem; fnstsw %%ax; test $0x400, %%ax; jnz 1b; fstpl %0; fstp %%st(0)" : "=m"(r), "=a"(s) : "m"(a), "m"(b) : "cc", "st", "memory");
        else __asm__ volatile("fninit; fldl %3; fldl %2; fprem; fnstsw %%ax; fstpl %0; fstp %%st(0)" : "=m"(r), "=a"(s) : "m"(a), "m"(b) : "st", "memory");
    }
    *sw=s; return r;
}
static unsigned rng=124; static unsigned next(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; }
int main(void) {
    int fail=0, checked=0, partial=0;
    for (int i=0; i<20000; i++) for (int ieee=0; ieee<2; ieee++) {
        int e1=(int)(next()%1800)-900, e2=(int)(next()%1800)-900;
        double a=ldexp(1.0+(next()%65536)/65536.0,e1), b=ldexp(1.0+(next()%65536)/65536.0,e2);
        if (next()&1) a=-a;
        if (next()&1) b=-b;
        CPU cpu={0}; CPU* c=&cpu; rt_fninit(c); fpush(c,b); fpush(c,a);
        int steps=0; do { rt_fprem(c,ieee); steps++; if(steps>100) { fail++; break; } } while(c->fsw & 0x400);
        unsigned short sw; double nr=native(a,b,ieee,1,&sw); checked++; if(steps>1) partial++;
        if (FST(0)!=nr || (signbit(FST(0)) != signbit(nr)) || (steps==1 && ((sw^c->fsw)&0x4700))) {
            if(fail++<8) fprintf(stderr,"mismatch a=%a b=%a ieee=%d steps=%d result %a/%a flags %x/%x\n",a,b,ieee,steps,FST(0),nr,c->fsw,sw);
        }
    }
    double edges[][2] = {{DBL_MAX, DBL_TRUE_MIN}, {-DBL_MAX, DBL_TRUE_MIN}, {DBL_TRUE_MIN, DBL_MAX},
                         {0.0, 3.0}, {-0.0, 3.0}, {7.0, -2.0}, {1.0, INFINITY}};
    for (unsigned i=0; i<sizeof edges/sizeof edges[0]; i++) for (int ieee=0; ieee<2; ieee++) {
        CPU cpu={0}; CPU* c=&cpu; rt_fninit(c); fpush(c,edges[i][1]); fpush(c,edges[i][0]);
        int steps=0; do { rt_fprem(c,ieee); steps++; } while((c->fsw & 0x400) && steps<100);
        unsigned short sw; double nr=native(edges[i][0],edges[i][1],ieee,1,&sw); checked++;
        if (steps>=100 || FST(0)!=nr || signbit(FST(0))!=signbit(nr) || (steps==1 && ((sw^c->fsw)&0x4700))) {
            if(fail++<8) fprintf(stderr,"edge mismatch a=%a b=%a ieee=%d steps=%d result %a/%a flags %x/%x\n",edges[i][0],edges[i][1],ieee,steps,FST(0),nr,c->fsw,sw);
        }
    }
    /* Invalid operands: the quotient is not meaningful, so do not compare C0/C1/C3. */
    double invalid[][2] = {{1.0, 0.0}, {0.0, 0.0}, {-1.0, -0.0}, {INFINITY, 2.0}};
    for (unsigned i=0; i<sizeof invalid/sizeof invalid[0]; i++) for (int ieee=0; ieee<2; ieee++) {
        CPU cpu={0}; CPU* c=&cpu; rt_fninit(c); fpush(c,invalid[i][1]); fpush(c,invalid[i][0]);
        rt_fprem(c,ieee); unsigned short sw; double nr=native(invalid[i][0],invalid[i][1],ieee,0,&sw); checked++;
        if (!isnan(FST(0)) || !isnan(nr) || !(c->fsw & 1) || !(sw & 1)) {
            if (fail++ < 8) fprintf(stderr,"invalid operand mismatch: %a / %a ieee=%d sw=%x/%x\n",invalid[i][0],invalid[i][1],ieee,c->fsw,sw);
        }
    }
    printf("fprem: %d checked (%d partial), %d failed\n",checked,partial,fail); return fail ? 1 : 0;
}
