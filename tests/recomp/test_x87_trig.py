#!/usr/bin/env python3
"""Compare actual generated x87 trig code with native x86, no game build.
Requires capstone, gcc and x86/x86-64 hardware. Tests only masked exceptions;
x87 values in the port are doubles, so large finite argument reduction differs.
"""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.abspath('tools'))
import fpsse
import recomp_x87_fixed
ops = ('fsin', 'fcos', 'fsincos', 'fptan')
emit = '\n'.join('case %d: %s break;' % (i, ' '.join((fpsse.x87 if '--baseline' in sys.argv else recomp_x87_fixed.fixed_x87)(None, None, op, [], None, None))) for i, op in enumerate(ops))
native = '\n'.join('case %d: asm volatile("fninit; fldl %%[marker]; fldl %%[x]; %s; fnstsw %%[sw]; fstpl %%[a]; fstpl %%[b]; fstpl %%[d]" : [sw] "=m"(sw), [a] "=m"(a), [b] "=m"(b), [d] "=m"(d) : [x] "m"(x), [marker] "m"(marker) : "st", "memory"); break;' % (i, op) for i, op in enumerate(ops))
src = r'''
#include "rt.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static int same(double a,double b) {
 if(isnan(a)||isnan(b)) return isnan(a)&&isnan(b);
 if(a==0 && b==0) return !!signbit(a)==!!signbit(b);
 return a==b || (isfinite(a)&&isfinite(b)&&fabs(a-b)<=1e-12*fmax(1.,fmax(fabs(a),fabs(b))));
}
static int test(double x, int op) {
 unsigned short sw=0; double a=0,b=0,d=0,marker=3.125;
 switch(op) { NATIVE }
 CPU state; memset(&state,0,sizeof state); CPU *c=&state;
 c->fempty=0xff; c->ftop=0; fpush(c,marker); fpush(c,x);
 switch(op) { EMIT }
 unsigned top=(sw>>11)&7, c2=!!(sw&0x400), invalid=!!(sw&1);
 int count = ((op>=2 && top==5) ? 3 : 2);
 int ok=(top==c->ftop && c2==!!(c->fsw&0x400) && invalid==!!(c->fsw&1));
 /* x87's approximate pi differs from libm for huge in-range inputs. Check
    numeric results only for modest inputs or non-finite / out-of-range ones. */
 if(!isfinite(x) || fabs(x)<100. || fabs(x)>=0x1p63) {
  double expected[3]={a,b,d};
  for(int k=0;k<count;k++) if(!same(expected[k],FST(k))) ok=0;
 }
 if(!ok) printf("FAIL op=%d x=%a status=%04x got=%04x top=%u got=%u a=%a got=%a b=%a got=%a\n",op,x,sw,c->fsw,top,c->ftop,a,FST(0),b,FST(1));
 return ok;
}
int main(void) {
 double xs[]={0.,-0.,1.,-1.,0.25,-0.25,0x1p63,-0x1p63,
  0x1.fffffffffffffp62,-0x1.fffffffffffffp62,
  0x1.0000000000001p63,-0x1.0000000000001p63,0x1p64,-0x1p64,
  0x1.fffffffffffffp1023,-0x1.fffffffffffffp1023,INFINITY,-INFINITY,NAN,-NAN};
 int ok=0,n=0;
 for(unsigned i=0;i<sizeof(xs)/sizeof(*xs);i++) for(int op=0;op<4;op++){ok+=test(xs[i],op);n++;}
 uint32_t rng=0x1badb002u;
 for(int i=0;i<1024;i++) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  double x=((double)rng / 4294967295.0 * 200.0) - 100.0;
  for(int op=0;op<4;op++){ok+=test(x,op);n++;} }
 printf("native x87 trig: %d/%d passed\n",ok,n); return ok!=n;
}
'''.replace('NATIVE', native).replace('EMIT',emit)
with tempfile.TemporaryDirectory(prefix='me1-x87-trig-') as tmp:
    source = tmp+'/test.c'; binary = tmp+'/test'
    with open(source,'w') as f: f.write(src)
    subprocess.run(['gcc','-O1','-fwrapv','-fno-strict-aliasing','-Iruntime',source,'-lm','-o',binary],check=True)
    subprocess.run([binary],check=True)
